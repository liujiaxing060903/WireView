#include "windows_video.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <ShellScalingApi.h>
#include <codecapi.h>
#include <d3d10_1.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi1_2.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mftransform.h>
#include <strmif.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <limits>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace wireview::host {
namespace {

using Microsoft::WRL::ComPtr;
using Clock = std::chrono::steady_clock;

constexpr UINT32 kBitRate = 24'000'000U;
constexpr LONGLONG kHundredNanosecondsPerSecond = 10'000'000LL;

// A hardware encoder is allowed to renegotiate its H.264 output type once, right
// after the first frame. Anything beyond that means the encoder keeps restarting
// its stream and the session cannot make progress.
constexpr unsigned kMaxStreamChanges = 4U;
// Intel's Quick Sync encoder answers ProcessOutput with E_UNEXPECTED instead of
// MF_E_TRANSFORM_NEED_MORE_INPUT for a short while after its stream was reset.
// Treat a bounded run of those as "no output yet", not as a fatal error.
constexpr unsigned kMaxUnexpectedOutputFailures = 120U;

[[nodiscard]] std::string hresult_text(HRESULT value) {
    std::ostringstream output;
    output << "0x" << std::hex << std::uppercase
           << static_cast<std::uint32_t>(value);
    return output.str();
}

void check_hresult(HRESULT value, const char* operation) {
    if (FAILED(value)) {
        throw std::runtime_error(
            std::string(operation) + " failed: " + hresult_text(value));
    }
}

// Windows switches away from the user's desktop while a UAC prompt is on the
// secure desktop. DXGI invalidates every desktop-duplication object at that
// point. This is a normal, temporary capture interruption, not a lost Android
// connection; keep it distinct so the active TCP session can survive it.
class DesktopDuplicationLost final : public std::runtime_error {
public:
    explicit DesktopDuplicationLost(HRESULT result)
        : std::runtime_error("DXGI desktop duplication temporarily unavailable: " +
              hresult_text(result)) {
    }
};

[[nodiscard]] std::string utf8(const wchar_t* value) {
    if (value == nullptr || *value == L'\0') {
        return {};
    }
    const int length = WideCharToMultiByte(
        CP_UTF8, 0, value, -1, nullptr, 0, nullptr, nullptr);
    if (length <= 1) {
        return {};
    }
    std::string result(static_cast<std::size_t>(length), '\0');
    const int converted = WideCharToMultiByte(
        CP_UTF8, 0, value, -1, result.data(), length, nullptr, nullptr);
    if (converted != length) {
        throw std::runtime_error("WideCharToMultiByte failed");
    }
    result.pop_back();
    return result;
}

class ComApartment {
public:
    ComApartment() {
        result_ = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (FAILED(result_) && result_ != RPC_E_CHANGED_MODE) {
            check_hresult(result_, "CoInitializeEx");
        }
    }

    ~ComApartment() {
        if (SUCCEEDED(result_)) {
            CoUninitialize();
        }
    }

    ComApartment(const ComApartment&) = delete;
    ComApartment& operator=(const ComApartment&) = delete;

private:
    HRESULT result_{E_FAIL};
};

class MediaFoundationSession {
public:
    MediaFoundationSession() {
        check_hresult(MFStartup(MF_VERSION, MFSTARTUP_FULL), "MFStartup");
    }

    ~MediaFoundationSession() {
        MFShutdown();
    }

    MediaFoundationSession(const MediaFoundationSession&) = delete;
    MediaFoundationSession& operator=(const MediaFoundationSession&) = delete;
};

struct CaptureTarget {
    ComPtr<IDXGIAdapter1> adapter;
    ComPtr<IDXGIOutput1> output;
    DXGI_ADAPTER_DESC1 adapter_description{};
    DXGI_OUTPUT_DESC output_description{};
    bool primary{false};
};

[[nodiscard]] bool contains_desktop_origin(const RECT& bounds) noexcept {
    return bounds.left <= 0 && bounds.right > 0 && bounds.top <= 0 && bounds.bottom > 0;
}

[[nodiscard]] std::vector<CaptureTarget> enumerate_capture_targets() {
    ComPtr<IDXGIFactory1> factory;
    check_hresult(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "CreateDXGIFactory1");

    std::vector<CaptureTarget> targets;
    for (UINT adapter_index = 0;; ++adapter_index) {
        ComPtr<IDXGIAdapter1> adapter;
        const HRESULT adapter_result = factory->EnumAdapters1(adapter_index, &adapter);
        if (adapter_result == DXGI_ERROR_NOT_FOUND) {
            break;
        }
        check_hresult(adapter_result, "IDXGIFactory1::EnumAdapters1");

        DXGI_ADAPTER_DESC1 adapter_description{};
        check_hresult(adapter->GetDesc1(&adapter_description), "IDXGIAdapter1::GetDesc1");
        for (UINT output_index = 0;; ++output_index) {
            ComPtr<IDXGIOutput> output;
            const HRESULT output_result = adapter->EnumOutputs(output_index, &output);
            if (output_result == DXGI_ERROR_NOT_FOUND) {
                break;
            }
            check_hresult(output_result, "IDXGIAdapter1::EnumOutputs");

            DXGI_OUTPUT_DESC output_description{};
            check_hresult(output->GetDesc(&output_description), "IDXGIOutput::GetDesc");
            if (!output_description.AttachedToDesktop) {
                continue;
            }
            ComPtr<IDXGIOutput1> output1;
            check_hresult(output.As(&output1), "IDXGIOutput::QueryInterface(IDXGIOutput1)");
            CaptureTarget target{
                .adapter = adapter,
                .output = output1,
                .adapter_description = adapter_description,
                .output_description = output_description,
                .primary = contains_desktop_origin(output_description.DesktopCoordinates),
            };
            targets.push_back(std::move(target));
        }
    }
    if (targets.empty()) {
        throw std::runtime_error("no attached DXGI desktop output was found");
    }
    return targets;
}

[[nodiscard]] CaptureTarget find_capture_target(std::string_view device_name) {
    const auto targets = enumerate_capture_targets();
    if (device_name.empty()) {
        const auto primary = std::find_if(
            targets.begin(), targets.end(),
            [](const CaptureTarget& target) { return target.primary; });
        return primary != targets.end() ? *primary : targets.front();
    }
    const std::string requested(device_name);
    const auto target = std::find_if(
        targets.begin(), targets.end(),
        [&requested](const CaptureTarget& candidate) {
            return _stricmp(utf8(candidate.output_description.DeviceName).c_str(),
                requested.c_str()) == 0;
        });
    if (target == targets.end()) {
        throw std::runtime_error(
            "requested capture display is unavailable: " + requested +
            ". Run wireview_host --list-displays and ensure Windows is extending it.");
    }
    return *target;
}

struct D3DDevice {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
};

struct DesktopPointer {
    bool visible{false};
    bool system_cursor_shape{false};
    float render_scale_adjustment{1.0F};
    float render_width_adjustment{1.0F};
    LONG x{};
    LONG y{};
    UINT width{};
    UINT height{};
    UINT pitch{};
    UINT hotspot_x{};
    UINT hotspot_y{};
    DXGI_OUTDUPL_POINTER_SHAPE_TYPE type{DXGI_OUTDUPL_POINTER_SHAPE_TYPE_COLOR};
    std::vector<std::uint8_t> bytes;
    std::uint64_t shape_version{};
};

[[nodiscard]] D3DDevice create_video_device(IDXGIAdapter1* adapter) {
    constexpr UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT |
                           D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
    constexpr D3D_FEATURE_LEVEL requested_levels[]{
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
    };

    D3DDevice result;
    D3D_FEATURE_LEVEL selected_level{};
    HRESULT create_result = D3D11CreateDevice(
        adapter,
        D3D_DRIVER_TYPE_UNKNOWN,
        nullptr,
        flags,
        requested_levels,
        static_cast<UINT>(std::size(requested_levels)),
        D3D11_SDK_VERSION,
        &result.device,
        &selected_level,
        &result.context);
    if (create_result == E_INVALIDARG) {
        create_result = D3D11CreateDevice(
            adapter,
            D3D_DRIVER_TYPE_UNKNOWN,
            nullptr,
            flags,
            &requested_levels[1],
            1,
            D3D11_SDK_VERSION,
            &result.device,
            &selected_level,
            &result.context);
    }
    check_hresult(create_result, "D3D11CreateDevice(video adapter)");

    ComPtr<ID3D10Multithread> multithread;
    if (SUCCEEDED(result.device.As(&multithread))) {
        multithread->SetMultithreadProtected(TRUE);
    }
    return result;
}

[[nodiscard]] float monitor_cursor_scale(HMONITOR monitor) {
    UINT dpi_x = 96U;
    UINT dpi_y = 96U;
    if (monitor != nullptr && SUCCEEDED(GetDpiForMonitor(
            monitor, MDT_EFFECTIVE_DPI, &dpi_x, &dpi_y)) && dpi_x >= 96U) {
        return static_cast<float>(dpi_x) / 96.0F;
    }
    return 1.0F;
}

class AcquiredFrame {
public:
    explicit AcquiredFrame(IDXGIOutputDuplication* duplication) noexcept
        : duplication_(duplication) {
    }

    ~AcquiredFrame() {
        if (active_) {
            duplication_->ReleaseFrame();
        }
    }

    void activate() noexcept {
        active_ = true;
    }

    void release() {
        if (active_) {
            check_hresult(duplication_->ReleaseFrame(),
                          "IDXGIOutputDuplication::ReleaseFrame");
            active_ = false;
        }
    }

    AcquiredFrame(const AcquiredFrame&) = delete;
    AcquiredFrame& operator=(const AcquiredFrame&) = delete;

private:
    IDXGIOutputDuplication* duplication_{};
    bool active_{false};
};

class DesktopFrameSource {
public:
    // Probe hook: runs the Win32 cursor fallback used for secondary displays.
    [[nodiscard]] static bool capture_cursor_shape_for_probe(
        HCURSOR cursor, DesktopPointer& captured_pointer) {
        return capture_system_cursor_shape(cursor, captured_pointer);
    }

    DesktopFrameSource(
        IDXGIOutput1* output,
        ID3D11Device* device,
        ID3D11DeviceContext* context,
        const RECT& desktop_bounds,
        bool use_system_cursor_fallback)
        : device_(device),
          context_(context),
          desktop_bounds_(desktop_bounds),
          use_system_cursor_fallback_(use_system_cursor_fallback) {
        const HRESULT duplicate_result = output->DuplicateOutput(device, &duplication_);
        // During a UAC secure-desktop transition the previous duplication is
        // invalidated first (ACCESS_LOST), while attempts to create the next
        // one are denied until Windows returns to the user desktop. Both are
        // transient conditions and must use the same retry path.
        if (duplicate_result == DXGI_ERROR_ACCESS_LOST || duplicate_result == E_ACCESSDENIED) {
            throw DesktopDuplicationLost(duplicate_result);
        }
        check_hresult(duplicate_result, "IDXGIOutput1::DuplicateOutput");
        duplication_->GetDesc(&description_);

        D3D11_TEXTURE2D_DESC texture_description{};
        texture_description.Width = description_.ModeDesc.Width;
        texture_description.Height = description_.ModeDesc.Height;
        texture_description.MipLevels = 1;
        texture_description.ArraySize = 1;
        texture_description.Format = description_.ModeDesc.Format;
        texture_description.SampleDesc.Count = 1;
        texture_description.Usage = D3D11_USAGE_DEFAULT;
        texture_description.BindFlags = 0;
        check_hresult(
            device_->CreateTexture2D(&texture_description, nullptr, &latest_),
            "ID3D11Device::CreateTexture2D(desktop copy)");
    }

    [[nodiscard]] bool refresh(UINT timeout_milliseconds) {
        dxgi_pointer_updated_this_frame_ = false;
        DXGI_OUTDUPL_FRAME_INFO frame_info{};
        ComPtr<IDXGIResource> resource;
        AcquiredFrame guard(duplication_.Get());
        const HRESULT result = duplication_->AcquireNextFrame(
            timeout_milliseconds, &frame_info, &resource);
        if (result == DXGI_ERROR_WAIT_TIMEOUT) {
            return false;
        }
        if (result == DXGI_ERROR_ACCESS_LOST || result == DXGI_ERROR_SESSION_DISCONNECTED) {
            throw DesktopDuplicationLost(result);
        }
        check_hresult(result, "IDXGIOutputDuplication::AcquireNextFrame");
        guard.activate();

        update_pointer(frame_info);

        ComPtr<ID3D11Texture2D> texture;
        check_hresult(resource.As(&texture), "desktop frame QueryInterface(ID3D11Texture2D)");
        context_->CopyResource(latest_.Get(), texture.Get());
        guard.release();
        ++updates_;
        return true;
    }

    [[nodiscard]] ID3D11Texture2D* texture() const noexcept {
        return latest_.Get();
    }

    [[nodiscard]] UINT width() const noexcept {
        return description_.ModeDesc.Width;
    }

    [[nodiscard]] UINT height() const noexcept {
        return description_.ModeDesc.Height;
    }

    [[nodiscard]] DXGI_FORMAT format() const noexcept {
        return description_.ModeDesc.Format;
    }

    [[nodiscard]] std::uint64_t updates() const noexcept {
        return updates_;
    }

    [[nodiscard]] const DesktopPointer& pointer() const noexcept {
        return pointer_;
    }

    // Some virtual-display drivers expose cursor-only desktop duplication
    // updates only while an application is repainting (for example during a
    // drag). Polling the Windows cursor keeps an independently captured VDD
    // display responsive during ordinary moves and when its cursor shape
    // changes to text, hand, or resize.
    void refresh_system_cursor() {
        if (!use_system_cursor_fallback_) {
            return;
        }

        CURSORINFO cursor_info{};
        cursor_info.cbSize = sizeof(cursor_info);
        if (!GetCursorInfo(&cursor_info) ||
            (cursor_info.flags & CURSOR_SHOWING) == 0U ||
            cursor_info.ptScreenPos.x < desktop_bounds_.left ||
            cursor_info.ptScreenPos.x >= desktop_bounds_.right ||
            cursor_info.ptScreenPos.y < desktop_bounds_.top ||
            cursor_info.ptScreenPos.y >= desktop_bounds_.bottom) {
            pointer_.visible = false;
            return;
        }

        // On this capture path GetCursorInfo's location already aligns with
        // the cursor image top-left used by Windows' virtual display. Applying
        // ICONINFO's hotspot again shifts hand/resize cursors left or up.
        if (cursor_info.hCursor == nullptr) {
            pointer_.visible = false;
            return;
        }
        if (cursor_info.hCursor != system_cursor_ || !system_shape_available_) {
            if (!capture_system_cursor_shape(cursor_info.hCursor, system_pointer_)) {
                pointer_.visible = false;
                return;
            }
            system_cursor_ = cursor_info.hCursor;
            system_shape_available_ = true;
            if (!system_cursor_fallback_logged_) {
                std::cout << "Cursor source: Windows system cursor fallback active\n";
                system_cursor_fallback_logged_ = true;
            }
        }
        if (native_drag_cursor_is_visible()) {
            // While moving or resizing a window, Parsec VDD paints a correctly
            // positioned cursor into the desktop frame. Keep the fallback until
            // that native cursor path is confirmed, then hide it to avoid both
            // a blank flash and a second cursor throughout the operation.
            pointer_.visible = false;
            return;
        }
        pointer_ = system_pointer_;
        pointer_.visible = true;
        pointer_.x = cursor_info.ptScreenPos.x - desktop_bounds_.left;
        pointer_.y = cursor_info.ptScreenPos.y - desktop_bounds_.top;
    }

private:
    [[nodiscard]] static bool mouse_button_is_down() noexcept {
        constexpr int buttons[]{VK_LBUTTON, VK_RBUTTON, VK_MBUTTON, VK_XBUTTON1, VK_XBUTTON2};
        return std::any_of(std::begin(buttons), std::end(buttons), [](int button) {
            return (GetAsyncKeyState(button) & 0x8000) != 0;
        });
    }

    [[nodiscard]] static bool window_move_or_resize_is_active() noexcept {
        GUITHREADINFO information{};
        information.cbSize = sizeof(information);
        return GetGUIThreadInfo(0U, &information) != FALSE &&
            (information.flags & GUI_INMOVESIZE) != 0U;
    }

    [[nodiscard]] bool native_drag_cursor_is_visible() noexcept {
        const bool button_down = mouse_button_is_down();
        const bool window_move_or_resize =
            button_down && window_move_or_resize_is_active();
        const auto now = Clock::now();
        if (button_down && !mouse_button_was_down_) {
            mouse_button_down_at_ = now;
            native_drag_cursor_confirmed_ = false;
        }
        if (!button_down || !window_move_or_resize) {
            native_drag_cursor_confirmed_ = false;
        }
        mouse_button_was_down_ = button_down;
        // Desktop/Explorer selection rectangles and file drag-and-drop also
        // hold a mouse button, but the VDD does not paint a replacement cursor
        // for them. Only hand off during Windows' modal move/resize loop, and
        // only after a post-press DXGI update confirms the native cursor path.
        if (window_move_or_resize && !native_drag_cursor_confirmed_ &&
            now - mouse_button_down_at_ >= std::chrono::milliseconds(50) &&
            dxgi_pointer_updated_this_frame_) {
            native_drag_cursor_confirmed_ = true;
        }
        return window_move_or_resize && native_drag_cursor_confirmed_;
    }

    [[nodiscard]] static bool is_stock_cursor(HCURSOR cursor, LPCSTR resource) noexcept {
        const HCURSOR stock = LoadCursorA(nullptr, resource);
        return stock != nullptr && cursor == stock;
    }

    [[nodiscard]] static bool is_text_cursor(
        HCURSOR cursor,
        const ICONINFO& icon_info) noexcept {
        // This system's standard 48px I-beam reports this stable hotspot pair;
        // unlike resource IDs it is independent of ANSI/Unicode header mode.
        return is_stock_cursor(cursor, IDC_IBEAM) ||
            (icon_info.xHotspot == 14U && icon_info.yHotspot == 15U);
    }

    [[nodiscard]] static float system_cursor_scale_adjustment(
        HCURSOR cursor,
        const ICONINFO& icon_info) noexcept {
        if (is_text_cursor(cursor, icon_info)) {
            return 25.0F / 35.0F;
        }
        if (is_stock_cursor(cursor, IDC_SIZEWE) ||
            is_stock_cursor(cursor, IDC_SIZENS)) {
            return 33.0F / 34.0F;
        }
        if (is_stock_cursor(cursor, IDC_ARROW) ||
            (icon_info.xHotspot == 0U && icon_info.yHotspot == 0U)) {
            return 21.0F / 25.0F;
        }
        return 1.0F;
    }

    [[nodiscard]] static std::size_t draw_text_cursor_fallback(
        std::vector<std::uint8_t>& pixels,
        UINT width,
        UINT height,
        UINT hotspot_x,
        UINT hotspot_y) {
        // The desktop cursor is a black I-beam with a white outline. Drawing it in
        // a single colour made the I-beam look wrong next to the primary display
        // (white instead of black) and invisible on matching backgrounds, so build
        // the two-tone shape here.
        const auto set_pixel = [&pixels, width, height](
                                   int x, int y, std::uint8_t value) {
            if (x < 0 || y < 0 || x >= static_cast<int>(width) || y >= static_cast<int>(height)) {
                return;
            }
            auto* pixel = pixels.data() +
                (static_cast<std::size_t>(y) * width + static_cast<UINT>(x)) * 4U;
            pixel[0] = value;
            pixel[1] = value;
            pixel[2] = value;
            pixel[3] = 0xffU;
        };
        const int center_x = static_cast<int>(hotspot_x);
        const int top = std::max(1, static_cast<int>(hotspot_y) - 14);
        const int bottom = std::min(static_cast<int>(height) - 1, top + 31);
        const int source_left = std::max(0, center_x - 9);
        const int source_right = std::min(static_cast<int>(width) - 1, center_x + 9);
        const int source_top = top - 1;
        const int source_bottom = std::min(static_cast<int>(height) - 1, bottom + 1);
        const int source_height = source_bottom - source_top + 1;
        std::vector<bool> shape(
            static_cast<std::size_t>(width) * height, false);

        // Encode the measured Windows cursor as an exact logical 14x25 mask.
        // Coordinates use the user's convention: (1,1) is the lower-left
        // pixel. Rows 3..23 contain only columns 7 and 8. Rows 2 and 24
        // are complete bars; rows 1 and 25 omit their two center pixels.
        for (int y = source_top; y <= source_bottom; ++y) {
            const int logical_y_from_top =
                ((y - source_top) * 25) / source_height;
            const int logical_y = 25 - logical_y_from_top;
            for (int x = source_left; x <= source_right; ++x) {
                // The 19-source-pixel mask is downsampled to a 14-pixel
                // on-screen cursor. A two-source-pixel stem collapsed to one
                // output pixel on the reference tablet, so supersample the
                // logical columns 7 and 8 with three centered source pixels.
                const bool center_stem =
                    x >= center_x - 1 && x <= center_x + 1;
                const bool side_cutout = logical_y >= 3 && logical_y <= 23 &&
                    !center_stem;
                const bool outer_center_cutout =
                    (logical_y == 1 || logical_y == 25) &&
                    center_stem;
                if (!side_cutout && !outer_center_cutout) {
                    shape[static_cast<std::size_t>(y) * width + static_cast<UINT>(x)] = true;
                }
            }
        }
        // White outline first, then the black I-beam on top: the outline keeps the
        // cursor visible on dark desktops, the black body matches Windows.
        for (int y = 0; y < static_cast<int>(height); ++y) {
            for (int x = 0; x < static_cast<int>(width); ++x) {
                if (shape[static_cast<std::size_t>(y) * width + static_cast<UINT>(x)]) {
                    continue;
                }
                bool touches_shape = false;
                for (int dy = -1; dy <= 1 && !touches_shape; ++dy) {
                    for (int dx = -1; dx <= 1 && !touches_shape; ++dx) {
                        const int nx = x + dx;
                        const int ny = y + dy;
                        if (nx < 0 || ny < 0 || nx >= static_cast<int>(width) ||
                            ny >= static_cast<int>(height)) {
                            continue;
                        }
                        touches_shape =
                            shape[static_cast<std::size_t>(ny) * width + static_cast<UINT>(nx)];
                    }
                }
                if (touches_shape) {
                    set_pixel(x, y, 0xffU);
                }
            }
        }
        std::size_t opaque_pixels = 0U;
        for (int y = 0; y < static_cast<int>(height); ++y) {
            for (int x = 0; x < static_cast<int>(width); ++x) {
                if (shape[static_cast<std::size_t>(y) * width + static_cast<UINT>(x)]) {
                    set_pixel(x, y, 0x00U);
                    ++opaque_pixels;
                }
            }
        }
        return opaque_pixels;
    }

    // One-pixel white ring around the opaque cursor pixels so a black I-beam stays
    // readable on dark desktops.
    static void add_cursor_outline(
        std::vector<std::uint8_t>& pixels, UINT width, UINT height) {
        const std::vector<std::uint8_t> source = pixels;
        for (UINT y = 0U; y < height; ++y) {
            for (UINT x = 0U; x < width; ++x) {
                const std::size_t offset =
                    (static_cast<std::size_t>(y) * width + x) * 4U;
                if (source[offset + 3U] != 0U) {
                    continue;
                }
                bool neighbour = false;
                for (int dy = -1; dy <= 1 && !neighbour; ++dy) {
                    for (int dx = -1; dx <= 1 && !neighbour; ++dx) {
                        const int nx = static_cast<int>(x) + dx;
                        const int ny = static_cast<int>(y) + dy;
                        if (nx < 0 || ny < 0 || nx >= static_cast<int>(width) ||
                            ny >= static_cast<int>(height)) {
                            continue;
                        }
                        neighbour =
                            source[(static_cast<std::size_t>(ny) * width +
                                    static_cast<UINT>(nx)) * 4U + 3U] != 0U;
                    }
                }
                if (neighbour) {
                    pixels[offset] = 0xffU;
                    pixels[offset + 1U] = 0xffU;
                    pixels[offset + 2U] = 0xffU;
                    pixels[offset + 3U] = 0xffU;
                }
            }
        }
    }

    [[nodiscard]] static bool capture_system_cursor_shape(
        HCURSOR cursor,
        DesktopPointer& captured_pointer) {
        ICONINFO icon_info{};
        if (!GetIconInfo(cursor, &icon_info)) {
            return false;
        }
        const auto release_icon_bitmaps = [&icon_info] {
            if (icon_info.hbmColor != nullptr) {
                DeleteObject(icon_info.hbmColor);
            }
            if (icon_info.hbmMask != nullptr) {
                DeleteObject(icon_info.hbmMask);
            }
        };

        const HBITMAP source_bitmap = icon_info.hbmColor != nullptr
            ? icon_info.hbmColor
            : icon_info.hbmMask;
        BITMAP bitmap{};
        if (source_bitmap == nullptr ||
            GetObjectW(source_bitmap, sizeof(bitmap), &bitmap) != sizeof(bitmap)) {
            release_icon_bitmaps();
            return false;
        }
        const LONG source_height = bitmap.bmHeight < 0 ? -bitmap.bmHeight : bitmap.bmHeight;
        const UINT width = static_cast<UINT>(bitmap.bmWidth);
        const UINT height = icon_info.hbmColor != nullptr
            ? static_cast<UINT>(source_height)
            : static_cast<UINT>(source_height / 2L);
        if (width == 0U || height == 0U || width > 256U || height > 256U) {
            release_icon_bitmaps();
            return false;
        }

        HDC screen_dc = GetDC(nullptr);
        HDC draw_dc = screen_dc != nullptr ? CreateCompatibleDC(screen_dc) : nullptr;
        HDC mask_dc = screen_dc != nullptr && icon_info.hbmColor == nullptr &&
                icon_info.hbmMask != nullptr
            ? CreateCompatibleDC(screen_dc)
            : nullptr;
        BITMAPINFO bitmap_info{};
        bitmap_info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bitmap_info.bmiHeader.biWidth = static_cast<LONG>(width);
        bitmap_info.bmiHeader.biHeight = -static_cast<LONG>(height);
        bitmap_info.bmiHeader.biPlanes = 1;
        bitmap_info.bmiHeader.biBitCount = 32;
        bitmap_info.bmiHeader.biCompression = BI_RGB;
        void* dib_pixels = nullptr;
        HBITMAP dib = draw_dc != nullptr
            ? CreateDIBSection(draw_dc, &bitmap_info, DIB_RGB_COLORS, &dib_pixels, nullptr, 0U)
            : nullptr;
        HGDIOBJ old_draw_bitmap = dib != nullptr ? SelectObject(draw_dc, dib) : nullptr;
        HGDIOBJ old_mask_bitmap = mask_dc != nullptr
            ? SelectObject(mask_dc, icon_info.hbmMask)
            : nullptr;
        const std::size_t pixel_bytes = static_cast<std::size_t>(width) * height * 4U;
        const bool can_draw = dib != nullptr && dib_pixels != nullptr && old_draw_bitmap != nullptr;
        bool rendered = false;
        std::vector<std::uint8_t> black_background_pixels;
        if (can_draw) {
            std::memset(dib_pixels, 0, pixel_bytes);
            rendered = DrawIconEx(
                draw_dc, 0, 0, cursor, width, height, 0U, nullptr, DI_NORMAL) != FALSE;
            if (rendered) {
                const auto* source = static_cast<const std::uint8_t*>(dib_pixels);
                black_background_pixels.assign(source, source + pixel_bytes);
                std::memset(dib_pixels, 0xff, pixel_bytes);
                rendered = DrawIconEx(
                    draw_dc, 0, 0, cursor, width, height, 0U, nullptr, DI_NORMAL) != FALSE;
            }
        }

        if (rendered) {
            const auto* white_background_pixels = static_cast<const std::uint8_t*>(dib_pixels);
            std::vector<std::uint8_t> pixels(
                static_cast<std::size_t>(width) * height * 4U, 0U);
            std::size_t opaque_pixels = 0U;
            for (UINT y = 0U; y < height; ++y) {
                for (UINT x = 0U; x < width; ++x) {
                    const auto offset = (static_cast<std::size_t>(y) * width + x) * 4U;
                    const auto* black = black_background_pixels.data() + offset;
                    const auto* white = white_background_pixels + offset;
                    if (mask_dc != nullptr) {
                        // Monochrome cursors store an AND bitmap above an XOR
                        // bitmap. DrawIconEx does not reliably preserve these
                        // pixels in a 32-bit DIB, so decode them directly.
                        const COLORREF and_pixel = GetPixel(mask_dc, x, y);
                        const COLORREF xor_pixel = GetPixel(mask_dc, x, y + height);
                        if (and_pixel != CLR_INVALID && xor_pixel != CLR_INVALID) {
                            const bool and_mask = and_pixel != RGB(0, 0, 0);
                            const bool xor_mask = xor_pixel != RGB(0, 0, 0);
                            if (and_mask && !xor_mask) {
                                continue;
                            }
                            // Windows draws the I-beam as an inverting (XOR) cursor:
                            // black on light backgrounds, white on dark ones. Drawing
                            // the XOR pixels white made the tablet show a white I-beam
                            // next to the primary's black one, so draw them black and
                            // rely on the white outline added below for contrast.
                            const std::uint8_t value = 0x00U;
                            pixels[offset] = value;
                            pixels[offset + 1U] = value;
                            pixels[offset + 2U] = value;
                            pixels[offset + 3U] = 0xffU;
                            ++opaque_pixels;
                            continue;
                        }
                    }
                    const int blue_delta = std::abs(static_cast<int>(white[0]) - black[0]);
                    const int green_delta = std::abs(static_cast<int>(white[1]) - black[1]);
                    const int red_delta = std::abs(static_cast<int>(white[2]) - black[2]);
                    const UINT alpha = 255U - static_cast<UINT>(std::min(
                        255,
                        std::max({blue_delta, green_delta, red_delta})));
                    if (alpha == 0U) {
                        continue;
                    }
                    ++opaque_pixels;
                    for (UINT channel = 0U; channel < 3U; ++channel) {
                        const UINT straight = (static_cast<UINT>(black[channel]) * 255U + alpha / 2U) /
                            alpha;
                        pixels[offset + channel] = static_cast<std::uint8_t>(std::min(255U, straight));
                    }
                    pixels[offset + 3U] = static_cast<std::uint8_t>(alpha);
                }
            }
            if (opaque_pixels == 0U && is_text_cursor(cursor, icon_info)) {
                opaque_pixels = draw_text_cursor_fallback(
                    pixels, width, height, icon_info.xHotspot, icon_info.yHotspot);
            } else if (is_text_cursor(cursor, icon_info)) {
                add_cursor_outline(pixels, width, height);
            }
            captured_pointer.width = width;
            captured_pointer.height = height;
            captured_pointer.pitch = width * 4U;
            captured_pointer.hotspot_x = icon_info.xHotspot;
            captured_pointer.hotspot_y = icon_info.yHotspot;
            captured_pointer.type = DXGI_OUTDUPL_POINTER_SHAPE_TYPE_COLOR;
            captured_pointer.system_cursor_shape = true;
            captured_pointer.render_scale_adjustment =
                system_cursor_scale_adjustment(cursor, icon_info);
            // The first 14/13 correction crossed two raster boundaries and
            // measured 15 pixels. Applying 14/15 to that measured result gives
            // a near-neutral 196/195 source-width adjustment for 14 pixels.
            captured_pointer.render_width_adjustment =
                is_text_cursor(cursor, icon_info) ? (196.0F / 195.0F) : 1.0F;
            captured_pointer.bytes = std::move(pixels);
            ++captured_pointer.shape_version;
            std::cout << "System cursor captured: size=" << width << 'x' << height
                      << " | hotspot=" << icon_info.xHotspot << ',' << icon_info.yHotspot
                      << " | monochrome=" << (icon_info.hbmColor == nullptr ? "yes" : "no")
                      << " | opaquePixels=" << opaque_pixels << '\n';
        }

        if (old_mask_bitmap != nullptr) {
            SelectObject(mask_dc, old_mask_bitmap);
        }
        if (old_draw_bitmap != nullptr) {
            SelectObject(draw_dc, old_draw_bitmap);
        }
        if (dib != nullptr) {
            DeleteObject(dib);
        }
        if (mask_dc != nullptr) {
            DeleteDC(mask_dc);
        }
        if (draw_dc != nullptr) {
            DeleteDC(draw_dc);
        }
        if (screen_dc != nullptr) {
            ReleaseDC(nullptr, screen_dc);
        }
        release_icon_bitmaps();
        return rendered;
    }

    void update_pointer(const DXGI_OUTDUPL_FRAME_INFO& frame_info) {
        if (frame_info.LastMouseUpdateTime.QuadPart != 0) {
            dxgi_pointer_.visible = frame_info.PointerPosition.Visible != FALSE;
            dxgi_pointer_.system_cursor_shape = false;
            dxgi_pointer_.x = frame_info.PointerPosition.Position.x - desktop_bounds_.left;
            dxgi_pointer_.y = frame_info.PointerPosition.Position.y - desktop_bounds_.top;
            dxgi_pointer_updated_this_frame_ = true;
        }
        if (frame_info.PointerShapeBufferSize == 0U) {
            if (dxgi_pointer_updated_this_frame_) {
                pointer_ = dxgi_pointer_;
            }
            return;
        }
        std::vector<std::uint8_t> bytes(frame_info.PointerShapeBufferSize);
        DXGI_OUTDUPL_POINTER_SHAPE_INFO shape{};
        UINT written = 0U;
        check_hresult(
            duplication_->GetFramePointerShape(
                static_cast<UINT>(bytes.size()), bytes.data(), &written, &shape),
            "IDXGIOutputDuplication::GetFramePointerShape");
        bytes.resize(written);
        dxgi_pointer_.width = shape.Width;
        dxgi_pointer_.height = shape.Height;
        dxgi_pointer_.pitch = shape.Pitch;
        dxgi_pointer_.hotspot_x = shape.HotSpot.x;
        dxgi_pointer_.hotspot_y = shape.HotSpot.y;
        dxgi_pointer_.type = static_cast<DXGI_OUTDUPL_POINTER_SHAPE_TYPE>(shape.Type);
        dxgi_pointer_.system_cursor_shape = false;
        dxgi_pointer_.bytes = std::move(bytes);
        ++dxgi_pointer_.shape_version;
        pointer_ = dxgi_pointer_;
    }

    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> context_;
    ComPtr<IDXGIOutputDuplication> duplication_;
    ComPtr<ID3D11Texture2D> latest_;
    DXGI_OUTDUPL_DESC description_{};
    RECT desktop_bounds_{};
    DesktopPointer pointer_;
    DesktopPointer dxgi_pointer_;
    DesktopPointer system_pointer_;
    bool use_system_cursor_fallback_{false};
    HCURSOR system_cursor_{};
    bool system_shape_available_{false};
    bool system_cursor_fallback_logged_{false};
    bool dxgi_pointer_updated_this_frame_{false};
    bool mouse_button_was_down_{false};
    bool native_drag_cursor_confirmed_{false};
    Clock::time_point mouse_button_down_at_{};
    std::uint64_t updates_{0};
};

class CursorCompositor {
public:
    CursorCompositor(
        ID3D11Device* device,
        ID3D11DeviceContext* context,
        UINT width,
        UINT height,
        DXGI_FORMAT format,
        float dpi_scale)
        : device_(device),
          context_(context),
          width_(width),
          height_(height),
          dpi_scale_(dpi_scale) {
        if (format != DXGI_FORMAT_B8G8R8A8_UNORM &&
            format != DXGI_FORMAT_B8G8R8A8_UNORM_SRGB) {
            throw std::runtime_error("cursor compositor requires BGRA desktop frames");
        }

        D3D11_TEXTURE2D_DESC output_description{};
        output_description.Width = width_;
        output_description.Height = height_;
        output_description.MipLevels = 1;
        output_description.ArraySize = 1;
        output_description.Format = format;
        output_description.SampleDesc.Count = 1;
        output_description.Usage = D3D11_USAGE_DEFAULT;
        output_description.BindFlags = D3D11_BIND_RENDER_TARGET;
        check_hresult(
            device_->CreateTexture2D(&output_description, nullptr, &composited_),
            "ID3D11Device::CreateTexture2D(cursor compositor)");
        check_hresult(
            device_->CreateRenderTargetView(composited_.Get(), nullptr, &render_target_),
            "ID3D11Device::CreateRenderTargetView(cursor compositor)");

        D3D11_TEXTURE2D_DESC pointer_description{};
        pointer_description.Width = kMaximumPointerDimension;
        pointer_description.Height = kMaximumPointerDimension;
        pointer_description.MipLevels = 1;
        pointer_description.ArraySize = 1;
        pointer_description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        pointer_description.SampleDesc.Count = 1;
        pointer_description.Usage = D3D11_USAGE_DEFAULT;
        pointer_description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        check_hresult(
            device_->CreateTexture2D(&pointer_description, nullptr, &pointer_texture_),
            "ID3D11Device::CreateTexture2D(cursor texture)");
        check_hresult(
            device_->CreateShaderResourceView(pointer_texture_.Get(), nullptr, &pointer_view_),
            "ID3D11Device::CreateShaderResourceView(cursor texture)");

        const ComPtr<ID3DBlob> vertex = compile_shader(kVertexShader, "main", "vs_5_0");
        const ComPtr<ID3DBlob> pixel = compile_shader(kPixelShader, "main", "ps_5_0");
        check_hresult(
            device_->CreateVertexShader(
                vertex->GetBufferPointer(), vertex->GetBufferSize(), nullptr, &vertex_shader_),
            "ID3D11Device::CreateVertexShader(cursor compositor)");
        check_hresult(
            device_->CreatePixelShader(
                pixel->GetBufferPointer(), pixel->GetBufferSize(), nullptr, &pixel_shader_),
            "ID3D11Device::CreatePixelShader(cursor compositor)");

        D3D11_BUFFER_DESC constant_description{};
        constant_description.ByteWidth = sizeof(CursorConstants);
        constant_description.Usage = D3D11_USAGE_DYNAMIC;
        constant_description.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        constant_description.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        check_hresult(
            device_->CreateBuffer(&constant_description, nullptr, &constant_buffer_),
            "ID3D11Device::CreateBuffer(cursor constants)");

        D3D11_SAMPLER_DESC sampler_description{};
        sampler_description.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
        sampler_description.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
        sampler_description.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
        sampler_description.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sampler_description.MaxLOD = D3D11_FLOAT32_MAX;
        check_hresult(
            device_->CreateSamplerState(&sampler_description, &sampler_),
            "ID3D11Device::CreateSamplerState(cursor compositor)");

        D3D11_BLEND_DESC blend_description{};
        blend_description.RenderTarget[0].BlendEnable = TRUE;
        blend_description.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
        blend_description.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
        blend_description.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
        blend_description.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
        blend_description.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
        blend_description.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        blend_description.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        check_hresult(
            device_->CreateBlendState(&blend_description, &blend_state_),
            "ID3D11Device::CreateBlendState(cursor compositor)");
        std::cout << "Cursor overlay DPI scale=" << dpi_scale_ << "x\n";
    }

    void compose(ID3D11Texture2D* desktop, const DesktopPointer& pointer) {
        context_->CopyResource(composited_.Get(), desktop);
        const UINT shape_height = rendered_height(pointer);
        ++compose_count_;
        const bool log_this_frame =
            (pointer.visible && cursor_visible_logs_ < 8U) ||
            (!pointer.visible && cursor_visible_logs_ == 0U && (compose_count_ % 240U) == 0U);
        if (log_this_frame) {
            cursor_geometry_logged_ = true;
            if (pointer.visible) {
                ++cursor_visible_logs_;
            }
            std::cout << "Cursor frame: visible=" << (pointer.visible ? "yes" : "no")
                      << " | shape=" << pointer.width << 'x' << shape_height
                      << " | shapeType=" << pointer.type
                      << " | bytes=" << pointer.bytes.size()
                      << " | systemShape=" << (pointer.system_cursor_shape ? "yes" : "no")
                      << " | renderAdjust=" << pointer.render_scale_adjustment
                      << " | pos=" << pointer.x << ',' << pointer.y
                      << " | hotspot=" << pointer.hotspot_x << ',' << pointer.hotspot_y
                      << " | dpiScale=" << dpi_scale_
                      << " | frame=" << width_ << 'x' << height_ << '\n';
        }
        if (!pointer.visible || !is_supported_shape(pointer) || pointer.bytes.empty() ||
            pointer.width == 0U || shape_height == 0U ||
            pointer.width > kMaximumPointerDimension ||
            shape_height > kMaximumPointerDimension) {
            return;
        }
        if (uploaded_shape_version_ != pointer.shape_version) {
            // The reusable 256x256 texture can otherwise retain opaque pixels
            // outside a smaller new cursor (for example the text I-beam), which
            // appears as a black square around that cursor on the tablet.
            context_->UpdateSubresource(
                pointer_texture_.Get(),
                0,
                nullptr,
                transparent_pointer_pixels_.data(),
                kMaximumPointerDimension * 4U,
                0U);
            D3D11_BOX destination{};
            destination.left = 0U;
            destination.top = 0U;
            destination.front = 0U;
            destination.right = pointer.width;
            destination.bottom = shape_height;
            destination.back = 1U;
            const std::uint8_t* pixels = pointer.bytes.data();
            UINT pitch = pointer.pitch;
            if (pointer.type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME) {
                convert_monochrome_shape(pointer, shape_height);
                pixels = monochrome_pixels_.data();
                pitch = pointer.width * 4U;
            } else if (pointer.type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MASKED_COLOR) {
                convert_masked_color_shape(pointer, shape_height);
                pixels = masked_color_pixels_.data();
                pitch = pointer.width * 4U;
            }
            context_->UpdateSubresource(
                pointer_texture_.Get(), 0, &destination, pixels, pitch, 0U);
            uploaded_shape_version_ = pointer.shape_version;
        }

        // A shape captured straight from desktop duplication is already sized for
        // this display. The Win32 fallback always hands back the raw 32 px system
        // cursor, so it must be scaled by the monitor DPI to match what Windows
        // draws on that display (200% display -> 64 px cursor).
        const float effective_cursor_scale =
            pointer.system_cursor_shape ? dpi_scale_ : 1.0F;
        const float base_cursor_width =
            static_cast<float>(pointer.width) * effective_cursor_scale;
        const float cursor_width = base_cursor_width;
        const float cursor_height = static_cast<float>(shape_height) * effective_cursor_scale;

        const float system_cursor_hotspot_adjustment = pointer.system_cursor_shape
            // The virtual desktop reports physical cursor coordinates while
            // WireView enlarges the cursor image for the tablet. One native
            // hotspot unit is the calibrated midpoint between no adjustment
            // (right/down) and full scaled adjustment (left/up).
            ? 1.0F
            : 0.0F;
        const float cursor_x = static_cast<float>(pointer.x) -
            static_cast<float>(pointer.hotspot_x) * system_cursor_hotspot_adjustment -
            (cursor_width - base_cursor_width) * 0.5F;
        const float cursor_y = static_cast<float>(pointer.y) -
            static_cast<float>(pointer.hotspot_y) * system_cursor_hotspot_adjustment;
        if (cursor_x >= static_cast<float>(width_) || cursor_y >= static_cast<float>(height_) ||
            cursor_x + cursor_width <= 0.0F || cursor_y + cursor_height <= 0.0F) {
            if (log_this_frame) {
                std::cout << "Cursor draw: culled at " << cursor_x << ',' << cursor_y
                          << " drawn=" << cursor_width << 'x' << cursor_height << '\n';
            }
            return;
        }
        if (log_this_frame) {
            std::cout << "Cursor draw: at=" << cursor_x << ',' << cursor_y
                      << " | drawn=" << cursor_width << 'x' << cursor_height
                      << " | effectiveScale=" << effective_cursor_scale << '\n';
        }

        D3D11_MAPPED_SUBRESOURCE mapped{};
        check_hresult(
            context_->Map(constant_buffer_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped),
            "ID3D11DeviceContext::Map(cursor constants)");
        *static_cast<CursorConstants*>(mapped.pData) = CursorConstants{
            .x = cursor_x,
            .y = cursor_y,
            .width = cursor_width,
            .height = cursor_height,
            .output_width = static_cast<float>(width_),
            .output_height = static_cast<float>(height_),
            .uv_width = static_cast<float>(pointer.width) /
                static_cast<float>(kMaximumPointerDimension),
            .uv_height = static_cast<float>(shape_height) /
                static_cast<float>(kMaximumPointerDimension),
        };
        context_->Unmap(constant_buffer_.Get(), 0);

        const D3D11_VIEWPORT viewport{
            .TopLeftX = 0.0F,
            .TopLeftY = 0.0F,
            .Width = static_cast<float>(width_),
            .Height = static_cast<float>(height_),
            .MinDepth = 0.0F,
            .MaxDepth = 1.0F,
        };
        const float blend_factor[4]{0.0F, 0.0F, 0.0F, 0.0F};
        ID3D11Buffer* constant_buffers[]{constant_buffer_.Get()};
        ID3D11ShaderResourceView* resources[]{pointer_view_.Get()};
        ID3D11SamplerState* samplers[]{sampler_.Get()};
        context_->OMSetRenderTargets(1, render_target_.GetAddressOf(), nullptr);
        context_->OMSetBlendState(blend_state_.Get(), blend_factor, 0xffffffffU);
        context_->RSSetViewports(1, &viewport);
        context_->IASetInputLayout(nullptr);
        context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
        context_->VSSetShader(vertex_shader_.Get(), nullptr, 0U);
        context_->VSSetConstantBuffers(0U, 1U, constant_buffers);
        context_->PSSetShader(pixel_shader_.Get(), nullptr, 0U);
        context_->PSSetShaderResources(0U, 1U, resources);
        context_->PSSetSamplers(0U, 1U, samplers);
        context_->Draw(4U, 0U);
        ID3D11ShaderResourceView* empty_resources[]{nullptr};
        context_->PSSetShaderResources(0U, 1U, empty_resources);
        context_->OMSetRenderTargets(0U, nullptr, nullptr);
    }

    [[nodiscard]] ID3D11Texture2D* texture() const noexcept {
        return composited_.Get();
    }

    // Keeps the cursor proportional when the user changes the Windows scaling of
    // the captured display while a session is running.
    void set_dpi_scale(float scale) noexcept {
        dpi_scale_ = scale > 0.0F ? scale : 1.0F;
    }

private:
    struct CursorConstants {
        float x;
        float y;
        float width;
        float height;
        float output_width;
        float output_height;
        // The cursor lives in the top-left corner of a 256x256 texture, so the
        // shader must sample only that sub-rectangle; sampling 0..1 scaled the
        // cursor down by (shape / 256) and made it look tiny on the tablet.
        float uv_width{1.0F};
        float uv_height{1.0F};
    };

    [[nodiscard]] static ComPtr<ID3DBlob> compile_shader(
        const char* source,
        const char* entry,
        const char* target) {
        ComPtr<ID3DBlob> bytecode;
        ComPtr<ID3DBlob> errors;
        const HRESULT result = D3DCompile(
            source,
            std::char_traits<char>::length(source),
            nullptr,
            nullptr,
            nullptr,
            entry,
            target,
            D3DCOMPILE_ENABLE_STRICTNESS,
            0U,
            &bytecode,
            &errors);
        if (FAILED(result)) {
            const std::string detail = errors
                ? std::string(
                    static_cast<const char*>(errors->GetBufferPointer()),
                    errors->GetBufferSize())
                : "";
            throw std::runtime_error("cursor shader compilation failed: " + detail);
        }
        return bytecode;
    }

    static constexpr UINT kMaximumPointerDimension = 256U;

    [[nodiscard]] static bool is_supported_shape(const DesktopPointer& pointer) noexcept {
        return pointer.type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_COLOR ||
               pointer.type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME ||
               pointer.type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MASKED_COLOR;
    }

    [[nodiscard]] static UINT rendered_height(const DesktopPointer& pointer) noexcept {
        return pointer.type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME
            ? pointer.height / 2U
            : pointer.height;
    }

    void convert_monochrome_shape(const DesktopPointer& pointer, UINT shape_height) {
        const std::size_t required_bytes =
            static_cast<std::size_t>(pointer.pitch) * pointer.height;
        if (pointer.pitch == 0U || pointer.bytes.size() < required_bytes) {
            throw std::runtime_error("monochrome cursor shape data is incomplete");
        }
        monochrome_pixels_.assign(
            static_cast<std::size_t>(pointer.width) * shape_height * 4U, 0U);
        for (UINT y = 0U; y < shape_height; ++y) {
            const auto* and_row = pointer.bytes.data() +
                static_cast<std::size_t>(y) * pointer.pitch;
            const auto* xor_row = pointer.bytes.data() +
                static_cast<std::size_t>(y + shape_height) * pointer.pitch;
            for (UINT x = 0U; x < pointer.width; ++x) {
                const std::uint8_t bit = static_cast<std::uint8_t>(0x80U >> (x % 8U));
                const bool and_mask = (and_row[x / 8U] & bit) != 0U;
                const bool xor_mask = (xor_row[x / 8U] & bit) != 0U;
                auto* pixel = monochrome_pixels_.data() +
                    (static_cast<std::size_t>(y) * pointer.width + x) * 4U;
                if (and_mask && !xor_mask) {
                    continue; // Transparent.
                }
                const std::uint8_t color = xor_mask ? 0xffU : 0x00U;
                pixel[0] = color;
                pixel[1] = color;
                pixel[2] = color;
                // AND=1/XOR=1 means invert. The overlay cannot XOR the desktop
                // texture yet, so use white; this keeps text cursors visible.
                pixel[3] = 0xffU;
            }
        }
    }

    void convert_masked_color_shape(const DesktopPointer& pointer, UINT shape_height) {
        const std::size_t required_bytes =
            static_cast<std::size_t>(pointer.pitch) * shape_height;
        if (pointer.pitch < pointer.width * 4U || pointer.bytes.size() < required_bytes) {
            throw std::runtime_error("masked-color cursor shape data is incomplete");
        }
        masked_color_pixels_.assign(
            static_cast<std::size_t>(pointer.width) * shape_height * 4U, 0U);
        for (UINT y = 0U; y < shape_height; ++y) {
            const auto* source = pointer.bytes.data() +
                static_cast<std::size_t>(y) * pointer.pitch;
            auto* destination = masked_color_pixels_.data() +
                static_cast<std::size_t>(y) * pointer.width * 4U;
            for (UINT x = 0U; x < pointer.width; ++x) {
                const auto* source_pixel = source + static_cast<std::size_t>(x) * 4U;
                auto* destination_pixel = destination + static_cast<std::size_t>(x) * 4U;
                const bool xor_mask = source_pixel[3] == 0xffU;
                const bool changes_pixel = source_pixel[0] != 0U ||
                    source_pixel[1] != 0U || source_pixel[2] != 0U;
                if (xor_mask && !changes_pixel) {
                    continue; // XOR with black leaves the desktop unchanged.
                }
                destination_pixel[0] = source_pixel[0];
                destination_pixel[1] = source_pixel[1];
                destination_pixel[2] = source_pixel[2];
                // Alpha here is an XOR mask rather than transparency. A true
                // XOR pass would sample the desktop; drawing nonzero XOR pixels
                // opaquely is a stable visual approximation, while zero XOR
                // pixels above remain transparent instead of forming a box.
                destination_pixel[3] = 0xffU;
            }
        }
    }

    static constexpr const char* kVertexShader = R"(
cbuffer CursorConstants : register(b0) {
    float2 origin;
    float2 extent;
    float2 outputSize;
    float2 uvScale;
};
struct VertexOutput { float4 position : SV_POSITION; float2 uv : TEXCOORD0; };
VertexOutput main(uint vertexId : SV_VertexID) {
    const float2 uv[4] = { float2(0, 0), float2(1, 0), float2(0, 1), float2(1, 1) };
    VertexOutput output;
    output.uv = uv[vertexId] * uvScale;
    float2 pixel = origin + uv[vertexId] * extent;
    output.position = float4(
        (pixel.x / outputSize.x) * 2.0 - 1.0,
        1.0 - (pixel.y / outputSize.y) * 2.0,
        0.0,
        1.0);
    return output;
}
)";
    static constexpr const char* kPixelShader = R"(
Texture2D cursorTexture : register(t0);
SamplerState pointSampler : register(s0);
float4 main(float4 position : SV_POSITION, float2 uv : TEXCOORD0) : SV_TARGET {
    return cursorTexture.Sample(pointSampler, uv);
}
)";

    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> context_;
    ComPtr<ID3D11Texture2D> composited_;
    ComPtr<ID3D11RenderTargetView> render_target_;
    ComPtr<ID3D11Texture2D> pointer_texture_;
    ComPtr<ID3D11ShaderResourceView> pointer_view_;
    ComPtr<ID3D11VertexShader> vertex_shader_;
    ComPtr<ID3D11PixelShader> pixel_shader_;
    ComPtr<ID3D11Buffer> constant_buffer_;
    ComPtr<ID3D11SamplerState> sampler_;
    ComPtr<ID3D11BlendState> blend_state_;
    UINT width_{};
    UINT height_{};
    float dpi_scale_{1.0F};
    bool cursor_geometry_logged_{false};
    unsigned compose_count_{0U};
    unsigned cursor_visible_logs_{0U};
    std::array<std::uint8_t,
               static_cast<std::size_t>(kMaximumPointerDimension) *
                   kMaximumPointerDimension * 4U> transparent_pointer_pixels_{};
    std::vector<std::uint8_t> monochrome_pixels_;
    std::vector<std::uint8_t> masked_color_pixels_;
    std::uint64_t uploaded_shape_version_{std::numeric_limits<std::uint64_t>::max()};
};

class GpuNv12Converter {
public:
    struct ConvertedFrame {
        ID3D11Texture2D* texture{};
        std::size_t slot{};
    };

    GpuNv12Converter(
        ID3D11Device* device,
        ID3D11DeviceContext* context,
        ID3D11Texture2D* input,
        UINT input_width,
        UINT input_height,
        UINT output_width,
        UINT output_height,
        DXGI_FORMAT input_format,
        UINT frame_rate)
        : device_(device),
          context_(context),
          output_width_(output_width),
          output_height_(output_height) {
        check_hresult(device->QueryInterface(IID_PPV_ARGS(&video_device_)),
                      "ID3D11Device::QueryInterface(ID3D11VideoDevice)");
        check_hresult(context->QueryInterface(IID_PPV_ARGS(&video_context_)),
                      "ID3D11DeviceContext::QueryInterface(ID3D11VideoContext)");

        D3D11_VIDEO_PROCESSOR_CONTENT_DESC content{};
        content.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
        content.InputFrameRate = {frame_rate, 1U};
        content.InputWidth = input_width;
        content.InputHeight = input_height;
        content.OutputFrameRate = {frame_rate, 1U};
        content.OutputWidth = output_width;
        content.OutputHeight = output_height;
        content.Usage = D3D11_VIDEO_USAGE_OPTIMAL_SPEED;
        check_hresult(
            video_device_->CreateVideoProcessorEnumerator(&content, &enumerator_),
            "ID3D11VideoDevice::CreateVideoProcessorEnumerator");

        UINT input_flags = 0;
        UINT output_flags = 0;
        check_hresult(
            enumerator_->CheckVideoProcessorFormat(input_format, &input_flags),
            "CheckVideoProcessorFormat(BGRA)");
        check_hresult(
            enumerator_->CheckVideoProcessorFormat(DXGI_FORMAT_NV12, &output_flags),
            "CheckVideoProcessorFormat(NV12)");
        if ((input_flags & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_INPUT) == 0U ||
            (output_flags & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_OUTPUT) == 0U) {
            throw std::runtime_error("GPU video processor does not support BGRA-to-NV12");
        }
        check_hresult(
            video_device_->CreateVideoProcessor(enumerator_.Get(), 0, &processor_),
            "ID3D11VideoDevice::CreateVideoProcessor");

        D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC input_view_description{};
        input_view_description.FourCC = 0;
        input_view_description.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
        input_view_description.Texture2D.MipSlice = 0;
        input_view_description.Texture2D.ArraySlice = 0;
        check_hresult(
            video_device_->CreateVideoProcessorInputView(
                input, enumerator_.Get(), &input_view_description, &input_view_),
            "ID3D11VideoDevice::CreateVideoProcessorInputView");

        const RECT input_rectangle{
            0, 0, static_cast<LONG>(input_width), static_cast<LONG>(input_height)};
        const RECT output_rectangle{
            0, 0, static_cast<LONG>(output_width), static_cast<LONG>(output_height)};
        video_context_->VideoProcessorSetStreamFrameFormat(
            processor_.Get(), 0, D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);
        video_context_->VideoProcessorSetStreamSourceRect(
            processor_.Get(), 0, TRUE, &input_rectangle);
        video_context_->VideoProcessorSetStreamDestRect(
            processor_.Get(), 0, TRUE, &output_rectangle);
        video_context_->VideoProcessorSetOutputTargetRect(
            processor_.Get(), TRUE, &output_rectangle);
        video_context_->VideoProcessorSetStreamAutoProcessingMode(
            processor_.Get(), 0, FALSE);

        D3D11_VIDEO_PROCESSOR_COLOR_SPACE input_color{};
        input_color.RGB_Range = 0;
        input_color.Nominal_Range = D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_0_255;
        video_context_->VideoProcessorSetStreamColorSpace(
            processor_.Get(), 0, &input_color);

        D3D11_VIDEO_PROCESSOR_COLOR_SPACE output_color{};
        output_color.YCbCr_Matrix = 1;
        output_color.Nominal_Range = D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_16_235;
        video_context_->VideoProcessorSetOutputColorSpace(
            processor_.Get(), &output_color);

        for (std::size_t index = 0; index < kOutputSlotCount; ++index) {
            D3D11_TEXTURE2D_DESC texture_description{};
            texture_description.Width = output_width_;
            texture_description.Height = output_height_;
            texture_description.MipLevels = 1;
            texture_description.ArraySize = 1;
            texture_description.Format = DXGI_FORMAT_NV12;
            texture_description.SampleDesc.Count = 1;
            texture_description.Usage = D3D11_USAGE_DEFAULT;
            texture_description.BindFlags = D3D11_BIND_RENDER_TARGET |
                                            D3D11_BIND_VIDEO_ENCODER;
            check_hresult(
                device_->CreateTexture2D(
                    &texture_description, nullptr, &output_slots_[index].texture),
                "ID3D11Device::CreateTexture2D(NV12 output pool)");

            D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC view_description{};
            view_description.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
            view_description.Texture2D.MipSlice = 0;
            check_hresult(
                video_device_->CreateVideoProcessorOutputView(
                    output_slots_[index].texture.Get(),
                    enumerator_.Get(),
                    &view_description,
                    &output_slots_[index].view),
                "ID3D11VideoDevice::CreateVideoProcessorOutputView(NV12 output pool)");
        }

        D3D11_TEXTURE2D_DESC diagnostic_description{};
        diagnostic_description.Width = output_width_;
        diagnostic_description.Height = output_height_;
        diagnostic_description.MipLevels = 1;
        diagnostic_description.ArraySize = 1;
        diagnostic_description.Format = DXGI_FORMAT_NV12;
        diagnostic_description.SampleDesc.Count = 1;
        diagnostic_description.Usage = D3D11_USAGE_STAGING;
        diagnostic_description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        check_hresult(
            device_->CreateTexture2D(
                &diagnostic_description, nullptr, &diagnostic_staging_texture_),
            "ID3D11Device::CreateTexture2D(NV12 diagnostic staging)");
    }

    [[nodiscard]] ConvertedFrame convert() {
        std::size_t slot_index = kOutputSlotCount;
        for (std::size_t index = 0; index < kOutputSlotCount; ++index) {
            if (!output_slots_[index].in_use) {
                slot_index = index;
                output_slots_[index].in_use = true;
                break;
            }
        }
        if (slot_index == kOutputSlotCount) {
            throw std::runtime_error("GPU NV12 output surface pool is exhausted");
        }

        D3D11_VIDEO_PROCESSOR_STREAM stream{};
        stream.Enable = TRUE;
        stream.OutputIndex = 0;
        stream.InputFrameOrField = 0;
        stream.pInputSurface = input_view_.Get();
        check_hresult(
            video_context_->VideoProcessorBlt(
                processor_.Get(), output_slots_[slot_index].view.Get(), 0, 1, &stream),
            "ID3D11VideoContext::VideoProcessorBlt(BGRA-to-NV12)");
        report_first_luma(output_slots_[slot_index].texture.Get());
        return {
            .texture = output_slots_[slot_index].texture.Get(),
            .slot = slot_index,
        };
    }

    void release_slot(std::size_t slot_index) {
        if (slot_index >= kOutputSlotCount || !output_slots_[slot_index].in_use) {
            throw std::runtime_error("GPU NV12 output surface pool release is invalid");
        }
        output_slots_[slot_index].in_use = false;
    }

    // Copies an NV12 surface back to system memory for the software encoder
    // fallback. NV12 is one resource: RowPitch * Height bytes of luma followed by
    // the interleaved chroma plane at the same pitch.
    [[nodiscard]] std::vector<std::uint8_t> read_nv12(ID3D11Texture2D* texture) {
        context_->CopyResource(diagnostic_staging_texture_.Get(), texture);
        D3D11_MAPPED_SUBRESOURCE mapped{};
        check_hresult(
            context_->Map(diagnostic_staging_texture_.Get(), 0, D3D11_MAP_READ, 0, &mapped),
            "ID3D11DeviceContext::Map(NV12 readback)");
        const std::size_t luma_size =
            static_cast<std::size_t>(output_width_) * static_cast<std::size_t>(output_height_);
        std::vector<std::uint8_t> bytes(luma_size + luma_size / 2U);
        const auto* source = static_cast<const std::uint8_t*>(mapped.pData);
        const std::size_t pitch = mapped.RowPitch;
        for (UINT row = 0U; row < output_height_; ++row) {
            std::memcpy(bytes.data() + static_cast<std::size_t>(row) * output_width_,
                        source + static_cast<std::size_t>(row) * pitch,
                        output_width_);
        }
        const auto* chroma = source + pitch * static_cast<std::size_t>(output_height_);
        for (UINT row = 0U; row < output_height_ / 2U; ++row) {
            std::memcpy(bytes.data() + luma_size + static_cast<std::size_t>(row) * output_width_,
                        chroma + static_cast<std::size_t>(row) * pitch,
                        output_width_);
        }
        context_->Unmap(diagnostic_staging_texture_.Get(), 0);
        return bytes;
    }

private:
    void report_first_luma(ID3D11Texture2D* texture) {
        if (diagnostic_luma_reported_) {
            return;
        }
        context_->CopyResource(diagnostic_staging_texture_.Get(), texture);
        D3D11_MAPPED_SUBRESOURCE mapped{};
        check_hresult(
            context_->Map(
                diagnostic_staging_texture_.Get(), 0, D3D11_MAP_READ, 0, &mapped),
            "ID3D11DeviceContext::Map(NV12 diagnostic staging)");
        const UINT x_step = (output_width_ / 64U) + 1U;
        const UINT y_step = (output_height_ / 64U) + 1U;
        std::uint64_t luma_sum = 0U;
        std::uint64_t samples = 0U;
        const auto* rows = static_cast<const std::uint8_t*>(mapped.pData);
        for (UINT y = 0U; y < output_height_; y += y_step) {
            const auto* row = rows + static_cast<std::size_t>(y) * mapped.RowPitch;
            for (UINT x = 0U; x < output_width_; x += x_step) {
                luma_sum += row[x];
                ++samples;
            }
        }
        context_->Unmap(diagnostic_staging_texture_.Get(), 0);
        diagnostic_luma_reported_ = true;
        std::cout << "Video diagnostic: first NV12 luma average="
                  << (samples == 0U ? 0U : luma_sum / samples)
                  << " (16 is video black, 235 is video white)\n";
    }

    struct OutputSlot {
        ComPtr<ID3D11Texture2D> texture;
        ComPtr<ID3D11VideoProcessorOutputView> view;
        bool in_use{false};
    };

    // Low-delay AVC with no B-frames needs only a small number of frames in
    // flight. Keeping this pool small is important on integrated GPUs, where
    // output surfaces consume shared system memory.
    static constexpr std::size_t kOutputSlotCount = 3U;

    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> context_;
    ComPtr<ID3D11VideoDevice> video_device_;
    ComPtr<ID3D11VideoContext> video_context_;
    ComPtr<ID3D11VideoProcessorEnumerator> enumerator_;
    ComPtr<ID3D11VideoProcessor> processor_;
    ComPtr<ID3D11VideoProcessorInputView> input_view_;
    ComPtr<ID3D11Texture2D> diagnostic_staging_texture_;
    std::array<OutputSlot, kOutputSlotCount> output_slots_{};
    UINT output_width_{};
    UINT output_height_{};
    bool diagnostic_luma_reported_{false};
};

[[nodiscard]] std::string activation_name(IMFActivate* activation) {
    wchar_t* name = nullptr;
    UINT32 length = 0;
    const HRESULT result = activation->GetAllocatedString(
        MFT_FRIENDLY_NAME_Attribute, &name, &length);
    if (FAILED(result) || name == nullptr) {
        return "<unnamed>";
    }
    const std::string converted = utf8(name);
    CoTaskMemFree(name);
    return converted;
}

[[nodiscard]] ComPtr<IMFActivate> find_hardware_encoder(const LUID& adapter_luid) {
    ComPtr<IMFAttributes> attributes;
    check_hresult(MFCreateAttributes(&attributes, 1), "MFCreateAttributes(MFT enumeration)");
    check_hresult(
        attributes->SetBlob(
            MFT_ENUM_ADAPTER_LUID,
            reinterpret_cast<const UINT8*>(&adapter_luid),
            sizeof(adapter_luid)),
        "IMFAttributes::SetBlob(MFT_ENUM_ADAPTER_LUID)");

    const MFT_REGISTER_TYPE_INFO input_type{MFMediaType_Video, MFVideoFormat_NV12};
    const MFT_REGISTER_TYPE_INFO output_type{MFMediaType_Video, MFVideoFormat_H264};
    IMFActivate** activations = nullptr;
    UINT32 count = 0;
    check_hresult(
        MFTEnum2(
            MFT_CATEGORY_VIDEO_ENCODER,
            MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER,
            &input_type,
            &output_type,
            attributes.Get(),
            &activations,
            &count),
        "MFTEnum2(H.264 hardware encoder for adapter)");
    if (count == 0 || activations == nullptr) {
        CoTaskMemFree(activations);
        throw std::runtime_error("no H.264 hardware encoder matched the desktop adapter");
    }

    ComPtr<IMFActivate> selected;
    selected.Attach(activations[0]);
    for (UINT32 index = 1; index < count; ++index) {
        activations[index]->Release();
    }
    CoTaskMemFree(activations);
    return selected;
}

// Fallback for machines whose hardware H.264 MFT cannot encode (for example the
// Intel Quick Sync MFT on some driver/OS combinations). The Microsoft software
// encoder accepts system-memory NV12, so the desktop frame is read back to the
// CPU before it is submitted.
[[nodiscard]] ComPtr<IMFActivate> find_software_encoder() {
    const MFT_REGISTER_TYPE_INFO input_type{MFMediaType_Video, MFVideoFormat_NV12};
    const MFT_REGISTER_TYPE_INFO output_type{MFMediaType_Video, MFVideoFormat_H264};
    IMFActivate** activations = nullptr;
    UINT32 count = 0;
    check_hresult(
        MFTEnum2(
            MFT_CATEGORY_VIDEO_ENCODER,
            // Omitting MFT_ENUM_FLAG_HARDWARE restricts the result to software
            // transforms; there is no MFT_ENUM_FLAG_SOFTWARE flag.
            MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_ASYNCMFT,
            &input_type,
            &output_type,
            nullptr,
            &activations,
            &count),
        "MFTEnum2(software H.264 encoder)");
    if (count == 0 || activations == nullptr) {
        CoTaskMemFree(activations);
        throw std::runtime_error("no software H.264 encoder is registered on this system");
    }
    ComPtr<IMFActivate> selected;
    selected.Attach(activations[0]);
    for (UINT32 index = 1; index < count; ++index) {
        activations[index]->Release();
    }
    CoTaskMemFree(activations);
    return selected;
}

void log_h264_media_type(const char* label, IMFMediaType* type) {
    UINT32 width = 0;
    UINT32 height = 0;
    UINT32 numerator = 0;
    UINT32 denominator = 0;
    const HRESULT size_result = MFGetAttributeSize(type, MF_MT_FRAME_SIZE, &width, &height);
    const HRESULT rate_result = MFGetAttributeRatio(
        type, MF_MT_FRAME_RATE, &numerator, &denominator);
    const UINT32 bit_rate = MFGetAttributeUINT32(type, MF_MT_AVG_BITRATE, 0U);
    const UINT32 profile = MFGetAttributeUINT32(type, MF_MT_MPEG2_PROFILE, 0U);
    std::cout << label << ": H.264"
              << " | size=" << (SUCCEEDED(size_result)
                  ? std::to_string(width) + "x" + std::to_string(height) : "unspecified")
              << " | frameRate=" << (SUCCEEDED(rate_result) && denominator != 0U
                  ? std::to_string(numerator) + "/" + std::to_string(denominator)
                  : "unspecified")
              << " | bitRate=" << bit_rate
              << " | profile=" << profile << '\n';
}

bool set_codec_value(
    ICodecAPI* codec_api,
    const GUID& property,
    VARIANT& value,
    const char* name) {
    if (codec_api->IsSupported(&property) != S_OK) {
        std::cout << "  " << name << "=unsupported\n";
        return false;
    }
    const HRESULT result = codec_api->SetValue(&property, &value);
    std::cout << "  " << name << '='
              << (SUCCEEDED(result) ? "enabled" : hresult_text(result)) << '\n';
    return SUCCEEDED(result);
}

bool set_codec_bool(
    ICodecAPI* codec_api,
    const GUID& property,
    bool enabled,
    const char* name) {
    VARIANT value{};
    value.vt = VT_BOOL;
    value.boolVal = enabled ? VARIANT_TRUE : VARIANT_FALSE;
    return set_codec_value(codec_api, property, value, name);
}

bool set_codec_uint32(
    ICodecAPI* codec_api,
    const GUID& property,
    UINT32 number,
    const char* name) {
    VARIANT value{};
    value.vt = VT_UI4;
    value.ulVal = number;
    return set_codec_value(codec_api, property, value, name);
}

[[nodiscard]] ComPtr<IMFMediaType> make_video_type(
    const GUID& subtype,
    UINT32 width,
    UINT32 height,
    UINT32 frame_rate) {
    ComPtr<IMFMediaType> type;
    check_hresult(MFCreateMediaType(&type), "MFCreateMediaType");
    check_hresult(type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video),
                  "SetGUID(MF_MT_MAJOR_TYPE)");
    check_hresult(type->SetGUID(MF_MT_SUBTYPE, subtype), "SetGUID(MF_MT_SUBTYPE)");
    check_hresult(MFSetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, width, height),
                  "MFSetAttributeSize(MF_MT_FRAME_SIZE)");
    check_hresult(MFSetAttributeRatio(type.Get(), MF_MT_FRAME_RATE, frame_rate, 1),
                  "MFSetAttributeRatio(MF_MT_FRAME_RATE)");
    check_hresult(MFSetAttributeRatio(type.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1),
                  "MFSetAttributeRatio(MF_MT_PIXEL_ASPECT_RATIO)");
    check_hresult(type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive),
                  "SetUINT32(MF_MT_INTERLACE_MODE)");
    return type;
}

// TEMPORARY debugging aid: writes a BGRA8 texture to a bottom-up 32-bit BMP.
[[nodiscard]] bool debug_frames_enabled() {
    char value[8]{};
    return GetEnvironmentVariableA("WIREVIEW_DUMP_FRAMES", value, sizeof(value)) > 0U;
}

void save_debug_bmp(
    ID3D11Device* device,
    ID3D11DeviceContext* context,
    ID3D11Texture2D* texture,
    const char* path) {
    D3D11_TEXTURE2D_DESC description{};
    texture->GetDesc(&description);
    description.Usage = D3D11_USAGE_STAGING;
    description.BindFlags = 0U;
    description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    description.MiscFlags = 0U;
    ComPtr<ID3D11Texture2D> staging;
    const HRESULT create_result = device->CreateTexture2D(&description, nullptr, &staging);
    if (FAILED(create_result)) {
        std::cout << "Debug BMP: CreateTexture2D failed " << hresult_text(create_result) << '\n';
        return;
    }
    context->CopyResource(staging.Get(), texture);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    const HRESULT map_result = context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(map_result)) {
        std::cout << "Debug BMP: Map failed " << hresult_text(map_result) << '\n';
        return;
    }
    const UINT width = description.Width;
    const UINT height = description.Height;
    const UINT row_bytes = width * 4U;
    const UINT image_bytes = row_bytes * height;
    FILE* file = nullptr;
    if (fopen_s(&file, path, "wb") == 0 && file != nullptr) {
        std::uint8_t header[54]{};
        const UINT file_size = image_bytes + 54U;
        const UINT pixel_offset = 54U;
        const UINT header_size = 40U;
        const std::uint16_t planes = 1U;
        const std::uint16_t bits_per_pixel = 32U;
        header[0] = 'B';
        header[1] = 'M';
        std::memcpy(header + 2, &file_size, 4);
        std::memcpy(header + 10, &pixel_offset, 4);
        std::memcpy(header + 14, &header_size, 4);
        std::memcpy(header + 18, &width, 4);
        std::memcpy(header + 22, &height, 4);
        std::memcpy(header + 26, &planes, 2);
        std::memcpy(header + 28, &bits_per_pixel, 2);
        std::memcpy(header + 34, &image_bytes, 4);
        (void)std::fwrite(header, 1, sizeof(header), file);
        for (UINT row = 0; row < height; ++row) {
            const auto* source = static_cast<const std::uint8_t*>(mapped.pData) +
                static_cast<std::size_t>(height - 1U - row) * mapped.RowPitch;
            (void)std::fwrite(source, 1, row_bytes, file);
        }
        std::fclose(file);
    } else {
        std::cout << "Debug BMP: fopen failed for " << path << '\n';
    }
    context->Unmap(staging.Get(), 0);
}

class HardwareAvcEncoder {
public:
    HardwareAvcEncoder(
        IMFActivate* activation,
        ID3D11Device* device,
        UINT32 width,
        UINT32 height,
        UINT32 frame_rate,
        bool hardware = true)
        : activation_(activation),
          hardware_(hardware),
          width_(width),
          height_(height),
          frame_rate_(frame_rate),
          bit_rate_(kBitRate * frame_rate / 60U) {
        check_hresult(
            activation_->ActivateObject(IID_PPV_ARGS(&transform_)),
            "IMFActivate::ActivateObject(H.264 encoder)");

        ComPtr<IMFAttributes> transform_attributes;
        check_hresult(transform_->GetAttributes(&transform_attributes),
                      "IMFTransform::GetAttributes");
        asynchronous_ = MFGetAttributeUINT32(
            transform_attributes.Get(), MF_TRANSFORM_ASYNC, FALSE) != FALSE;
        if (hardware_ && !asynchronous_) {
            throw std::runtime_error("selected hardware encoder is not asynchronous");
        }
        if (asynchronous_) {
            check_hresult(
                transform_attributes->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE),
                "SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK)");
        }

        if (hardware_) {
            UINT reset_token = 0;
            check_hresult(
                MFCreateDXGIDeviceManager(&reset_token, &device_manager_),
                "MFCreateDXGIDeviceManager");
            check_hresult(device_manager_->ResetDevice(device, reset_token),
                          "IMFDXGIDeviceManager::ResetDevice");
            if (MFGetAttributeUINT32(
                    transform_attributes.Get(), MF_SA_D3D11_AWARE, FALSE) != FALSE) {
                check_hresult(
                    transform_->ProcessMessage(
                        MFT_MESSAGE_SET_D3D_MANAGER,
                        reinterpret_cast<ULONG_PTR>(device_manager_.Get())),
                    "IMFTransform::ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER)");
                direct3d_aware_ = true;
            }
        }

        if (SUCCEEDED(transform_.As(&codec_api_))) {
            std::cout << "Encoder controls:\n";
            low_latency_enabled_ = set_codec_bool(
                codec_api_.Get(), CODECAPI_AVEncCommonLowLatency, true, "lowLatency");
            set_codec_bool(codec_api_.Get(), CODECAPI_AVEncCommonRealTime, true, "realTime");
            if (!set_codec_uint32(
                    codec_api_.Get(),
                    CODECAPI_AVEncCommonRateControlMode,
                    eAVEncCommonRateControlMode_LowDelayVBR,
                    "lowDelayVBR")) {
                set_codec_uint32(
                    codec_api_.Get(),
                    CODECAPI_AVEncCommonRateControlMode,
                    eAVEncCommonRateControlMode_CBR,
                    "CBR");
            }
            set_codec_uint32(
                codec_api_.Get(), CODECAPI_AVEncCommonMeanBitRate, bit_rate_, "meanBitRate");
            set_codec_uint32(
                codec_api_.Get(), CODECAPI_AVEncMPVGOPSize, frame_rate_, "gop");
            set_codec_uint32(
                codec_api_.Get(), CODECAPI_AVEncMPVDefaultBPictureCount, 0, "bFrames0");
            set_codec_uint32(
                codec_api_.Get(), CODECAPI_AVEncVideoMaxNumRefFrame, 1, "referenceFrames1");
        } else {
            codec_api_.Reset();
            std::cout << "Encoder controls: none (ICodecAPI is not available)\n";
        }

        // Both encoders must see the output type before the input type: the
        // Microsoft software encoder answers SetInputType with
        // MF_E_TRANSFORM_TYPE_NOT_SET otherwise.
        if (hardware_) {
            auto output_type = make_video_type(MFVideoFormat_H264, width, height, frame_rate_);
            check_hresult(output_type->SetUINT32(MF_MT_AVG_BITRATE, bit_rate_),
                          "SetUINT32(MF_MT_AVG_BITRATE)");
            check_hresult(
                output_type->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_High),
                "SetUINT32(MF_MT_MPEG2_PROFILE)");
            check_hresult(transform_->SetOutputType(0, output_type.Get(), 0),
                          "IMFTransform::SetOutputType(H.264)");
            log_h264_media_type("Encoder initial output type", output_type.Get());
        } else {
            // The software encoder needs a complete output type (bit rate and
            // profile included) and refuses some profiles, so try High, then
            // Main, then whatever the encoder publishes itself.
            const UINT32 profiles[2] = {
                static_cast<UINT32>(eAVEncH264VProfile_High),
                static_cast<UINT32>(eAVEncH264VProfile_Main),
            };
            HRESULT set_result = E_FAIL;
            for (const UINT32 profile : profiles) {
                auto candidate =
                    make_video_type(MFVideoFormat_H264, width, height, frame_rate_);
                (void)candidate->SetUINT32(MF_MT_AVG_BITRATE, bit_rate_);
                (void)candidate->SetUINT32(MF_MT_MPEG2_PROFILE, profile);
                set_result = transform_->SetOutputType(0, candidate.Get(), 0);
                if (SUCCEEDED(set_result)) {
                    log_h264_media_type("Encoder initial output type", candidate.Get());
                    break;
                }
            }
            if (FAILED(set_result)) {
                auto fallback = select_h264_output_type();
                if (fallback.Get() == nullptr) {
                    throw std::runtime_error(
                        "software H.264 encoder published no matching output type");
                }
                (void)fallback->SetUINT32(MF_MT_AVG_BITRATE, bit_rate_);
                check_hresult(transform_->SetOutputType(0, fallback.Get(), 0),
                              "IMFTransform::SetOutputType(H.264 software)");
                log_h264_media_type("Encoder initial output type", fallback.Get());
            }
        }

        auto input_type = make_video_type(MFVideoFormat_NV12, width, height, frame_rate_);
        check_hresult(input_type->SetUINT32(
                          MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_16_235),
                      "SetUINT32(MF_MT_VIDEO_NOMINAL_RANGE)");
        check_hresult(input_type->SetUINT32(MF_MT_YUV_MATRIX, MFVideoTransferMatrix_BT709),
                      "SetUINT32(MF_MT_YUV_MATRIX)");
        check_hresult(transform_->SetInputType(0, input_type.Get(), 0),
                      "IMFTransform::SetInputType(NV12)");
        check_hresult(transform_->GetOutputStreamInfo(0, &output_info_),
                      "IMFTransform::GetOutputStreamInfo");
        if (asynchronous_) {
            check_hresult(transform_.As(&events_),
                          "IMFTransform::QueryInterface(IMFMediaEventGenerator)");
        }
    }

    ~HardwareAvcEncoder() {
        if (transform_) {
            transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
        }
        if (activation_) {
            activation_->ShutdownObject();
        }
    }

    void start() {
        check_hresult(
            transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0),
            "MFT_MESSAGE_NOTIFY_BEGIN_STREAMING");
        check_hresult(
            transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0),
            "MFT_MESSAGE_NOTIFY_START_OF_STREAM");
        started_ = true;
    }

    void submit(ID3D11Texture2D* texture, LONGLONG timestamp, LONGLONG duration) {
        if (!hardware_) {
            throw std::logic_error("submit(texture) requires the hardware encoder path");
        }
        ComPtr<IMFMediaBuffer> buffer;
        check_hresult(
            MFCreateDXGISurfaceBuffer(
                __uuidof(ID3D11Texture2D), texture, 0, FALSE, &buffer),
            "MFCreateDXGISurfaceBuffer(NV12)");
        ComPtr<IMFSample> sample;
        check_hresult(MFCreateSample(&sample), "MFCreateSample(input)");
        check_hresult(sample->AddBuffer(buffer.Get()), "IMFSample::AddBuffer(input)");
        submit_sample(sample.Get(), timestamp, duration);
    }

    // Software encoder fallback: the Microsoft H.264 MFT is not D3D11 aware, so the
    // NV12 frame has to be read back to system memory and copied into an MF buffer.
    void submit_bytes(std::span<const std::uint8_t> frame, LONGLONG timestamp, LONGLONG duration) {
        ComPtr<IMFMediaBuffer> buffer;
        check_hresult(
            MFCreateMemoryBuffer(static_cast<DWORD>(frame.size()), &buffer),
            "MFCreateMemoryBuffer(input)");
        BYTE* target = nullptr;
        DWORD capacity = 0;
        check_hresult(buffer->Lock(&target, &capacity, nullptr),
                      "IMFMediaBuffer::Lock(input)");
        std::memcpy(target, frame.data(), frame.size());
        check_hresult(buffer->Unlock(), "IMFMediaBuffer::Unlock(input)");
        check_hresult(buffer->SetCurrentLength(static_cast<DWORD>(frame.size())),
                      "IMFMediaBuffer::SetCurrentLength(input)");
        ComPtr<IMFSample> sample;
        check_hresult(MFCreateSample(&sample), "MFCreateSample(input)");
        check_hresult(sample->AddBuffer(buffer.Get()), "IMFSample::AddBuffer(input)");
        submit_sample(sample.Get(), timestamp, duration);
    }

    void force_keyframe() {
        // A decoder that discarded any inter frame must not continue from its
        // old reference picture. Request an IDR on the encoder thread before
        // submitting the next source frame, then let the receiver resume only
        // after that clean point arrives.
        if (codec_api_.Get() == nullptr) {
            needs_keyframe_ = true;
            return;
        }
        if (!set_codec_bool(
                codec_api_.Get(), CODECAPI_AVEncVideoForceKeyFrame, true, "forceRecoveryKeyframe")) {
            throw std::runtime_error("hardware encoder rejected forced recovery keyframe");
        }
    }

    [[nodiscard]] MediaEventType next_event() {
        if (!asynchronous_) {
            // The Microsoft software encoder is a synchronous MFT with no event
            // queue. Drive it through the same state machine as the asynchronous
            // path: ask for a frame, then report the output ProcessInput produced.
            if (input_pending_) {
                return METransformHaveOutput;
            }
            if (draining_) {
                return METransformDrainComplete;
            }
            // A synchronous encoder buffers a few frames; report "nothing to do"
            // while it refuses more input instead of failing in ProcessInput with
            // MF_E_NOTACCEPTING.
            DWORD input_flags = 0;
            if (SUCCEEDED(transform_->GetInputStatus(0, &input_flags)) &&
                (input_flags & MFT_INPUT_STATUS_ACCEPT_DATA) == 0U) {
                return MEUnknown;
            }
            return METransformNeedInput;
        }
        ComPtr<IMFMediaEvent> event;
        const HRESULT result = events_->GetEvent(MF_EVENT_FLAG_NO_WAIT, &event);
        if (result == MF_E_NO_EVENTS_AVAILABLE) {
            return MEUnknown;
        }
        check_hresult(result, "IMFMediaEventGenerator::GetEvent");
        HRESULT event_status = S_OK;
        check_hresult(event->GetStatus(&event_status), "IMFMediaEvent::GetStatus");
        check_hresult(event_status, "hardware encoder event");
        MediaEventType type = MEUnknown;
        check_hresult(event->GetType(&type), "IMFMediaEvent::GetType");
        return type;
    }

    struct Output {
        LONGLONG timestamp{-1};
        std::vector<std::uint8_t> bytes;
        bool keyframe{false};
        bool annex_b{false};
    };

    // Returns an empty result when the encoder produced no frame for this
    // METransformHaveOutput notification. ProcessOutput must be called exactly
    // once per notification: an asynchronous encoder that provides its own
    // samples answers a second call, made without new input, with E_UNEXPECTED.
    [[nodiscard]] std::optional<Output> take_output() {
        dropped_input_ = false;
        ComPtr<IMFSample> allocated_sample;
        const bool transform_provides_sample =
            (output_info_.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES |
                                     MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)) != 0U;
        if (!transform_provides_sample) {
            check_hresult(MFCreateSample(&allocated_sample), "MFCreateSample(output)");
            const std::uint64_t native_size =
                static_cast<std::uint64_t>(width_) * static_cast<std::uint64_t>(height_) * 2U;
            const DWORD buffer_size = static_cast<DWORD>(std::min<std::uint64_t>(
                std::max<std::uint64_t>(output_info_.cbSize, native_size),
                std::numeric_limits<DWORD>::max()));
            ComPtr<IMFMediaBuffer> buffer;
            check_hresult(MFCreateMemoryBuffer(buffer_size, &buffer),
                          "MFCreateMemoryBuffer(output)");
            check_hresult(allocated_sample->AddBuffer(buffer.Get()),
                          "IMFSample::AddBuffer(output)");
        }

        MFT_OUTPUT_DATA_BUFFER output_data{};
        output_data.dwStreamID = 0;
        output_data.pSample = allocated_sample.Get();
        DWORD status = 0;
        const HRESULT result = transform_->ProcessOutput(0, 1, &output_data, &status);
        input_pending_ = false;
        if (output_data.pEvents != nullptr) {
            output_data.pEvents->Release();
            output_data.pEvents = nullptr;
        }
        if (result == MF_E_TRANSFORM_STREAM_CHANGE) {
            std::cout << "IMFTransform::ProcessOutput requested output-format change"
                      << " | status=" << hresult_text(static_cast<HRESULT>(status)) << '\n';
            if ((status & MFT_PROCESS_OUTPUT_STATUS_NEW_STREAMS) != 0U) {
                throw std::runtime_error(
                    "hardware encoder created a new output stream, which WireView does not support");
            }
            if (++stream_changes_ > kMaxStreamChanges) {
                throw std::runtime_error(
                    "hardware encoder repeated its output-format change after renegotiation");
            }
            renegotiate_output_type();
            reset_stream_after_renegotiation();
            dropped_input_ = true;
            return std::nullopt;
        }
        if (result == MF_E_TRANSFORM_NEED_MORE_INPUT) {
            return std::nullopt;
        }
        if (result == E_UNEXPECTED) {
            // The encoder lost the frame it was holding. Report "no output", ask
            // for a fresh IDR on the next submit, and keep the session alive; a
            // short run of these is normal right after a stream reset.
            if (++unexpected_output_failures_ > kMaxUnexpectedOutputFailures) {
                throw std::runtime_error(
                    "IMFTransform::ProcessOutput kept returning E_UNEXPECTED (0x8000FFFF)");
            }
            needs_keyframe_ = true;
            std::cout << "IMFTransform::ProcessOutput returned E_UNEXPECTED; "
                         "waiting for the next encoder event\n";
            return std::nullopt;
        }
        unexpected_output_failures_ = 0U;
        check_hresult(result, "IMFTransform::ProcessOutput");

        ComPtr<IMFSample> sample;
        if (output_data.pSample == allocated_sample.Get()) {
            sample = allocated_sample;
        } else {
            sample.Attach(output_data.pSample);
        }
        if (!sample) {
            throw std::runtime_error("hardware encoder returned no output sample");
        }

        Output output;
        if (FAILED(sample->GetSampleTime(&output.timestamp))) {
            output.timestamp = -1;
        }
        output.keyframe = MFGetAttributeUINT32(
            sample.Get(), MFSampleExtension_CleanPoint, FALSE) != FALSE;

        ComPtr<IMFMediaBuffer> contiguous;
        check_hresult(sample->ConvertToContiguousBuffer(&contiguous),
                      "IMFSample::ConvertToContiguousBuffer");
        BYTE* data = nullptr;
        DWORD capacity = 0;
        DWORD current_length = 0;
        check_hresult(contiguous->Lock(&data, &capacity, &current_length),
                      "IMFMediaBuffer::Lock(output)");
        output.annex_b = current_length >= 4U && data[0] == 0U && data[1] == 0U &&
                         ((data[2] == 1U) || (data[2] == 0U && data[3] == 1U));
        output.bytes.assign(data, data + current_length);
        check_hresult(contiguous->Unlock(), "IMFMediaBuffer::Unlock(output)");
        if (!asynchronous_ && draining_) {
            // Keep pulling while the drained software encoder still holds frames;
            // next_event() stops reporting output once ProcessOutput says there is
            // nothing left.
            input_pending_ = true;
        }
        return output;
    }

    void drain() {
        if (!started_ || draining_) {
            return;
        }
        check_hresult(
            transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0),
            "MFT_MESSAGE_NOTIFY_END_OF_STREAM");
        check_hresult(transform_->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0),
                      "MFT_MESSAGE_COMMAND_DRAIN");
        draining_ = true;
        if (!asynchronous_) {
            // Pull whatever the synchronous encoder still holds; next_event()
            // keeps reporting output until ProcessOutput drains the queue.
            input_pending_ = true;
        }
    }

    [[nodiscard]] bool direct3d_aware() const noexcept {
        return direct3d_aware_;
    }

    [[nodiscard]] bool low_latency_enabled() const noexcept {
        return low_latency_enabled_;
    }

    [[nodiscard]] bool software() const noexcept {
        return !hardware_;
    }

    [[nodiscard]] bool asynchronous() const noexcept {
        return asynchronous_;
    }

    // True when the last take_output() call discarded the frame it was encoding
    // because the encoder renegotiated its output type.
    [[nodiscard]] bool dropped_input() const noexcept {
        return dropped_input_;
    }

private:
    void submit_sample(IMFSample* sample, LONGLONG timestamp, LONGLONG duration) {
        check_hresult(sample->SetSampleTime(timestamp), "IMFSample::SetSampleTime(input)");
        check_hresult(sample->SetSampleDuration(duration),
                      "IMFSample::SetSampleDuration(input)");
        if ((!first_input_submitted_ || needs_keyframe_) && codec_api_.Get() != nullptr) {
            set_codec_bool(
                codec_api_.Get(), CODECAPI_AVEncVideoForceKeyFrame, true,
                needs_keyframe_ ? "forceRecoveryKeyframe" : "forceFirstKeyframe");
            needs_keyframe_ = false;
        }
        first_input_submitted_ = true;
        check_hresult(transform_->ProcessInput(0, sample, 0),
                      "IMFTransform::ProcessInput");
        if (!asynchronous_) {
            input_pending_ = true;
        }
    }

    // Picks the H.264 output type the transform publishes for the configured
    // geometry. The Microsoft software encoder rejects client-built types, and a
    // hardware encoder may publish several.
    [[nodiscard]] ComPtr<IMFMediaType> select_h264_output_type() {
        ComPtr<IMFMediaType> first_h264;
        for (DWORD index = 0U;; ++index) {
            ComPtr<IMFMediaType> candidate;
            const HRESULT result = transform_->GetOutputAvailableType(0, index, &candidate);
            if (result == MF_E_NO_MORE_TYPES) {
                break;
            }
            check_hresult(result, "IMFTransform::GetOutputAvailableType(H.264)");
            GUID subtype{};
            if (FAILED(candidate->GetGUID(MF_MT_SUBTYPE, &subtype)) ||
                subtype != MFVideoFormat_H264) {
                continue;
            }
            UINT32 candidate_width = 0U;
            UINT32 candidate_height = 0U;
            if (SUCCEEDED(MFGetAttributeSize(candidate.Get(), MF_MT_FRAME_SIZE,
                                            &candidate_width, &candidate_height)) &&
                candidate_width == width_ && candidate_height == height_) {
                return candidate;
            }
            if (first_h264.Get() == nullptr) {
                first_h264 = candidate;
            }
        }
        return first_h264;
    }
    void renegotiate_output_type() {
        // The encoder publishes its new type list here. Prefer an H.264 type whose
        // frame size matches the configured output instead of blindly taking
        // index 0, which may describe a different geometry.
        ComPtr<IMFMediaType> first_h264;
        ComPtr<IMFMediaType> matching_size;
        for (DWORD index = 0U;; ++index) {
            ComPtr<IMFMediaType> candidate;
            const HRESULT result = transform_->GetOutputAvailableType(0, index, &candidate);
            if (result == MF_E_NO_MORE_TYPES) {
                break;
            }
            check_hresult(result, "IMFTransform::GetOutputAvailableType(H.264 stream change)");
            GUID subtype{};
            if (FAILED(candidate->GetGUID(MF_MT_SUBTYPE, &subtype)) ||
                subtype != MFVideoFormat_H264) {
                continue;
            }
            if (first_h264.Get() == nullptr) {
                first_h264 = candidate;
            }
            UINT32 candidate_width = 0U;
            UINT32 candidate_height = 0U;
            if (SUCCEEDED(MFGetAttributeSize(candidate.Get(), MF_MT_FRAME_SIZE,
                                            &candidate_width, &candidate_height)) &&
                candidate_width == width_ && candidate_height == height_) {
                matching_size = candidate;
                break;
            }
        }
        ComPtr<IMFMediaType> output_type =
            matching_size.Get() != nullptr ? matching_size : first_h264;
        if (output_type.Get() == nullptr) {
            throw std::runtime_error(
                "hardware encoder published no H.264 output type after the stream change");
        }
        GUID subtype{};
        check_hresult(output_type->GetGUID(MF_MT_SUBTYPE, &subtype),
                      "IMFMediaType::GetGUID(MF_MT_SUBTYPE after stream change)");
        if (subtype != MFVideoFormat_H264) {
            throw std::runtime_error(
                "hardware encoder changed to a non-H.264 output type, which WireView does not support");
        }
        // Ask for the same bit rate, frame rate and profile the session was
        // started with; the enumerated type is passed back unmodified.
        check_hresult(output_type->SetUINT32(MF_MT_AVG_BITRATE, bit_rate_),
                      "SetUINT32(MF_MT_AVG_BITRATE after stream change)");
        check_hresult(transform_->SetOutputType(0, output_type.Get(), 0),
                      "IMFTransform::SetOutputType(H.264 stream change)");
        check_hresult(transform_->GetOutputStreamInfo(0, &output_info_),
                      "IMFTransform::GetOutputStreamInfo(after stream change)");
        std::cout << "Hardware encoder requested an H.264 output-format change; renegotiated output type"
                  << " | streamFlags=" << hresult_text(static_cast<HRESULT>(output_info_.dwFlags))
                  << " | bufferBytes=" << output_info_.cbSize
                  << " | alignment=" << output_info_.cbAlignment << '\n';
        log_h264_media_type("Encoder renegotiated output type", output_type.Get());
    }

    // A stream change invalidates whatever the encoder had queued. Ask for an IDR
    // on the next frame because the reference chain is gone. Do not flush the
    // transform here: a flushed asynchronous MFT can stop raising the
    // METransformNeedInput/METransformHaveOutput events this loop relies on.
    void reset_stream_after_renegotiation() {
        needs_keyframe_ = true;
    }

    ComPtr<IMFActivate> activation_;
    ComPtr<IMFTransform> transform_;
    ComPtr<ICodecAPI> codec_api_;
    ComPtr<IMFDXGIDeviceManager> device_manager_;
    ComPtr<IMFMediaEventGenerator> events_;
    MFT_OUTPUT_STREAM_INFO output_info_{};
    UINT32 width_{};
    UINT32 height_{};
    UINT32 frame_rate_{};
    UINT32 bit_rate_{};
    bool hardware_{true};
    bool asynchronous_{false};
    bool direct3d_aware_{false};
    bool low_latency_enabled_{false};
    bool first_input_submitted_{false};
    bool started_{false};
    bool draining_{false};
    bool needs_keyframe_{false};
    bool input_pending_{false};
    bool dropped_input_{false};
    unsigned stream_changes_{0U};
    unsigned unexpected_output_failures_{0U};
};

// TEMPORARY probe helper: dumps a BGRA pixel buffer to a bottom-up 32-bit BMP.
void write_rgba_bmp(
    const std::uint8_t* pixels, UINT width, UINT height, const char* path) {
    const UINT row_bytes = width * 4U;
    const UINT image_bytes = row_bytes * height;
    FILE* file = nullptr;
    if (fopen_s(&file, path, "wb") != 0 || file == nullptr) {
        return;
    }
    std::uint8_t header[54]{};
    const UINT file_size = image_bytes + 54U;
    const UINT pixel_offset = 54U;
    const UINT header_size = 40U;
    const std::uint16_t planes = 1U;
    const std::uint16_t bits_per_pixel = 32U;
    header[0] = 'B';
    header[1] = 'M';
    std::memcpy(header + 2, &file_size, 4);
    std::memcpy(header + 10, &pixel_offset, 4);
    std::memcpy(header + 14, &header_size, 4);
    std::memcpy(header + 18, &width, 4);
    std::memcpy(header + 22, &height, 4);
    std::memcpy(header + 26, &planes, 2);
    std::memcpy(header + 28, &bits_per_pixel, 2);
    std::memcpy(header + 34, &image_bytes, 4);
    (void)std::fwrite(header, 1, sizeof(header), file);
    for (UINT row = 0; row < height; ++row) {
        const auto* source = pixels +
            static_cast<std::size_t>(height - 1U - row) * row_bytes;
        (void)std::fwrite(source, 1, row_bytes, file);
    }
    std::fclose(file);
}

}  // namespace

int run_desktop_encode_session(
    std::chrono::seconds duration,
    const DesktopStreamCallbacks& callbacks,
    std::uint16_t requested_output_width,
    std::uint16_t requested_output_height,
    std::uint16_t frame_rate,
    std::string_view capture_display_device,
    bool prefer_software_encoder) {
    if (duration <= std::chrono::seconds::zero()) {
        throw std::invalid_argument("encode duration must be positive");
    }
    if (frame_rate != 60U && frame_rate != 120U) {
        throw std::invalid_argument("desktop encoder frame rate must be 60 or 120");
    }

    ComApartment apartment;
    MediaFoundationSession media_foundation;
    auto target = std::make_unique<CaptureTarget>(find_capture_target(capture_display_device));
    auto d3d = std::make_unique<D3DDevice>(create_video_device(target->adapter.Get()));
    auto frames = std::make_unique<DesktopFrameSource>(
        target->output.Get(),
        d3d->device.Get(),
        d3d->context.Get(),
        target->output_description.DesktopCoordinates,
        !target->primary);
    if (!frames->refresh(1'000)) {
        throw std::runtime_error("desktop duplication did not produce an initial frame");
    }
    if ((requested_output_width == 0U) != (requested_output_height == 0U)) {
        throw std::invalid_argument(
            "desktop encoder output dimensions must be both set or both zero");
    }
    const UINT output_width = requested_output_width == 0U
        ? frames->width()
        : requested_output_width;
    const UINT output_height = requested_output_height == 0U
        ? frames->height()
        : requested_output_height;
    if ((output_width % 2U) != 0U || (output_height % 2U) != 0U) {
        throw std::invalid_argument("desktop encoder output dimensions must be even for H.264 NV12");
    }
    auto cursor_compositor = std::make_unique<CursorCompositor>(
        d3d->device.Get(),
        d3d->context.Get(),
        frames->width(),
        frames->height(),
        frames->format(),
        monitor_cursor_scale(target->output_description.Monitor));
    auto converter = std::make_unique<GpuNv12Converter>(
        d3d->device.Get(),
        d3d->context.Get(),
        cursor_compositor->texture(),
        frames->width(),
        frames->height(),
        output_width,
        output_height,
        frames->format(),
        frame_rate);
    const UINT capture_width = frames->width();
    const UINT capture_height = frames->height();
    const DXGI_FORMAT capture_format = frames->format();
    const LUID capture_adapter_luid = target->adapter_description.AdapterLuid;

    // Prefer the hardware encoder bound to the desktop adapter. If it is missing
    // or refuses to run (some Intel Quick Sync driver/OS combinations return
    // E_UNEXPECTED from ProcessOutput), fall back to the Microsoft software H.264
    // encoder so the session still starts instead of failing outright.
    bool use_software_encoder = prefer_software_encoder;
    ComPtr<IMFActivate> activation;
    if (!use_software_encoder) {
        try {
            activation = find_hardware_encoder(target->adapter_description.AdapterLuid);
        } catch (const std::exception& error) {
            std::cout << "Hardware H.264 encoder unavailable: " << error.what() << '\n';
            use_software_encoder = true;
        }
    }
    if (use_software_encoder) {
        activation = find_software_encoder();
    }
    const RECT& desktop_bounds = target->output_description.DesktopCoordinates;
    std::cout << "Encoding " << utf8(target->output_description.DeviceName)
              << " at " << output_width << 'x' << output_height << '@'
              << frame_rate << " with " << activation_name(activation.Get()) << '\n'
              << "Capture target: rect=" << desktop_bounds.left << ',' << desktop_bounds.top
              << '-' << desktop_bounds.right << ',' << desktop_bounds.bottom
              << " | primary=" << (target->primary ? "yes" : "no") << '\n'
              << "Capture adapter: " << utf8(target->adapter_description.Description)
              << " | vendor=0x" << std::hex << std::uppercase << target->adapter_description.VendorId
              << " | device=0x" << target->adapter_description.DeviceId << std::dec
              << " | dedicatedMB=" << (target->adapter_description.DedicatedVideoMemory / (1024U * 1024U))
              << " | sharedMB=" << (target->adapter_description.SharedSystemMemory / (1024U * 1024U))
              << '\n'
              << "Cursor source: " << (target->primary
                  ? "DXGI desktop duplication"
                  : "DXGI with Windows system-cursor fallback") << '\n';
    std::unique_ptr<HardwareAvcEncoder> encoder;
    try {
        encoder = std::make_unique<HardwareAvcEncoder>(
            activation.Get(), d3d->device.Get(), output_width, output_height, frame_rate,
            !use_software_encoder);
    } catch (const std::exception& error) {
        if (use_software_encoder) {
            throw;
        }
        std::cout << "Hardware encoder setup failed (" << error.what()
                  << "); falling back to the Microsoft software H.264 encoder\n";
        activation = find_software_encoder();
        encoder = std::make_unique<HardwareAvcEncoder>(
            activation.Get(), d3d->device.Get(), output_width, output_height, frame_rate, false);
        use_software_encoder = true;
    }
    std::cout << "Encoder path: asynchronous="
              << (encoder->asynchronous() ? "yes" : "no")
              << " | d3d11Aware=" << (encoder->direct3d_aware() ? "yes" : "no")
              << " | lowLatencySet=" << (encoder->low_latency_enabled() ? "yes" : "no")
              << " | software=" << (use_software_encoder ? "yes" : "no") << '\n';

    if (output_width > std::numeric_limits<std::uint16_t>::max() ||
        output_height > std::numeric_limits<std::uint16_t>::max()) {
        throw std::runtime_error("desktop dimensions exceed the WireView protocol limit");
    }
    if (callbacks.on_started) {
        callbacks.on_started(
            static_cast<std::uint16_t>(output_width),
            static_cast<std::uint16_t>(output_height),
            frame_rate);
    }

    const LONGLONG frame_duration = kHundredNanosecondsPerSecond / frame_rate;
    struct SubmittedFrame {
        Clock::time_point submitted_at;
        std::size_t converter_slot;
        bool slot_held{true};
    };
    std::unordered_map<LONGLONG, SubmittedFrame> submitted_at;
    std::uint64_t submitted = 0;
    // TEMPORARY cursor-debugging state.
    unsigned dump_index = 0U;
    std::uint64_t encoded = 0;
    std::uint64_t keyframes = 0;
    std::uint64_t total_bytes = 0;
    std::uint64_t annex_b_outputs = 0;
    double total_latency_ms = 0.0;
    double maximum_latency_ms = 0.0;
    double first_frame_latency_ms = 0.0;

    const auto started = Clock::now();
    const auto stream_timestamp_base = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            started.time_since_epoch()).count());
    const auto deadline = started + duration;
    auto drain_deadline = deadline + std::chrono::seconds(5);
    auto last_event = started;
    bool draining = false;
    bool drained = false;
    std::uint64_t desktop_updates_before_reset = 0;
    encoder->start();

    while (!drained) {
        const MediaEventType event = encoder->next_event();
        if (event == MEUnknown) {
            // The encoder can go quiet (for example right after it renegotiated
            // its output type), so the deadline has to be honoured here as well;
            // otherwise a silent encoder would spin forever.
            if (!draining && Clock::now() >= deadline) {
                encoder->drain();
                draining = true;
                drain_deadline = Clock::now() + std::chrono::seconds(5);
                continue;
            }
            if (draining && Clock::now() > drain_deadline) {
                throw std::runtime_error("encoder drain timed out");
            }
            if (!draining && Clock::now() - last_event > std::chrono::seconds(5)) {
                throw std::runtime_error(
                    "hardware encoder stopped delivering events; no input or output for 5 seconds");
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }
        last_event = Clock::now();

        if (event == METransformNeedInput) {
            if (!draining && Clock::now() >= deadline) {
                encoder->drain();
                draining = true;
                drain_deadline = Clock::now() + std::chrono::seconds(5);
                continue;
            }
            if (draining) {
                continue;
            }

            const auto scheduled = started +
                std::chrono::nanoseconds(
                    (submitted * 1'000'000'000ULL) / frame_rate);
            std::this_thread::sleep_until(scheduled);
            try {
                (void)frames->refresh(0);
            } catch (const DesktopDuplicationLost& error) {
                std::cout << "Desktop capture paused for the Windows secure desktop; "
                             "keeping the Android connection alive: " << error.what() << '\n';
                // A secure-desktop switch can invalidate more than the
                // IDXGIOutputDuplication object. Some drivers let a newly
                // created duplication return one frame and then immediately
                // invalidate it again when it still belongs to the old D3D11
                // device. Tear down every device-bound stage, including the
                // Media Foundation encoder, and rebuild the complete GPU
                // pipeline on a fresh D3D11 device.
                desktop_updates_before_reset += frames->updates();
                submitted_at.clear();
                encoder.reset();
                converter.reset();
                cursor_compositor.reset();
                frames.reset();
                d3d.reset();
                target.reset();
                bool resumed = false;
                while (Clock::now() < deadline && !resumed) {
                    try {
                        auto refreshed_target = std::make_unique<CaptureTarget>(
                            find_capture_target(capture_display_device));
                        if (refreshed_target->adapter_description.AdapterLuid.LowPart !=
                                capture_adapter_luid.LowPart ||
                            refreshed_target->adapter_description.AdapterLuid.HighPart !=
                                capture_adapter_luid.HighPart ||
                            refreshed_target->output_description.DesktopCoordinates.right -
                                    refreshed_target->output_description.DesktopCoordinates.left !=
                                static_cast<LONG>(capture_width) ||
                            refreshed_target->output_description.DesktopCoordinates.bottom -
                                    refreshed_target->output_description.DesktopCoordinates.top !=
                                static_cast<LONG>(capture_height)) {
                            throw std::runtime_error(
                                "capture display changed while Windows secure desktop was active");
                        }
                        auto refreshed_d3d = std::make_unique<D3DDevice>(
                            create_video_device(refreshed_target->adapter.Get()));
                        auto replacement = std::make_unique<DesktopFrameSource>(
                            refreshed_target->output.Get(),
                            refreshed_d3d->device.Get(),
                            refreshed_d3d->context.Get(),
                            refreshed_target->output_description.DesktopCoordinates,
                            !refreshed_target->primary);
                        if (replacement->format() != capture_format) {
                            throw std::runtime_error(
                                "capture display format changed while Windows secure desktop was active");
                        }
                        if (!replacement->refresh(250U)) {
                            std::this_thread::sleep_for(std::chrono::milliseconds(100));
                            continue;
                        }
                        auto replacement_cursor = std::make_unique<CursorCompositor>(
                            refreshed_d3d->device.Get(),
                            refreshed_d3d->context.Get(),
                            replacement->width(),
                            replacement->height(),
                            replacement->format(),
                            monitor_cursor_scale(refreshed_target->output_description.Monitor));
                        auto replacement_converter = std::make_unique<GpuNv12Converter>(
                            refreshed_d3d->device.Get(),
                            refreshed_d3d->context.Get(),
                            replacement_cursor->texture(),
                            replacement->width(),
                            replacement->height(),
                            output_width,
                            output_height,
                            replacement->format(),
                            frame_rate);
                        ComPtr<IMFActivate> replacement_activation = use_software_encoder
                            ? find_software_encoder()
                            : find_hardware_encoder(
                                  refreshed_target->adapter_description.AdapterLuid);
                        auto replacement_encoder = std::make_unique<HardwareAvcEncoder>(
                            replacement_activation.Get(), refreshed_d3d->device.Get(),
                            output_width, output_height, frame_rate, !use_software_encoder);
                        replacement_encoder->start();

                        target = std::move(refreshed_target);
                        d3d = std::move(refreshed_d3d);
                        frames = std::move(replacement);
                        cursor_compositor = std::move(replacement_cursor);
                        converter = std::move(replacement_converter);
                        activation = std::move(replacement_activation);
                        encoder = std::move(replacement_encoder);
                        const auto elapsed = Clock::now() - started;
                        const auto elapsed_ticks = std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count();
                        submitted = std::max(submitted, static_cast<std::uint64_t>(
                            (elapsed_ticks * static_cast<std::int64_t>(frame_rate)) / 1'000'000'000LL));
                        // Re-send stream setup so Android discards the old
                        // H.264 reference chain and creates a clean decoder.
                        // The new encoder forces its first submitted frame to
                        // be an IDR.
                        if (callbacks.on_started) {
                            callbacks.on_started(
                                static_cast<std::uint16_t>(output_width),
                                static_cast<std::uint16_t>(output_height),
                                frame_rate);
                        }
                        resumed = true;
                        std::cout << "Desktop capture resumed after Windows secure desktop closed; "
                                     "rebuilt D3D11 capture, conversion, encoder, and Android decoder pipelines\n";
                    } catch (const DesktopDuplicationLost&) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(150));
                    }
                }
                if (!resumed) {
                    drained = true;
                    continue;
                }
                // The METransformNeedInput event that brought us here came
                // from the encoder that was just destroyed. Do not use that
                // stale event to submit a frame to the replacement MFT;
                // wait until the replacement encoder reports its own
                // METransformNeedInput event.
                continue;
            }
            frames->refresh_system_cursor();
            if ((submitted % 60U) == 0U && target) {
                cursor_compositor->set_dpi_scale(
                    monitor_cursor_scale(target->output_description.Monitor));
            }
            cursor_compositor->compose(frames->texture(), frames->pointer());
            // TEMPORARY: dump a few composited frames while the pointer is visible.
            // Debug aid: set WIREVIEW_DUMP_FRAMES=1 to save a few frames as BMP.
            if (frames->pointer().visible && dump_index < 4U &&
                submitted >= 30U * (dump_index + 1U) && debug_frames_enabled()) {
                char local_app_data[MAX_PATH]{};
                if (GetEnvironmentVariableA("LOCALAPPDATA", local_app_data, MAX_PATH) > 0U) {
                    const std::string base = std::string(local_app_data) +
                        "\\WireView\\logs\\frame" + std::to_string(dump_index);
                    save_debug_bmp(d3d->device.Get(), d3d->context.Get(),
                                   frames->texture(), (base + "-raw.bmp").c_str());
                    save_debug_bmp(d3d->device.Get(), d3d->context.Get(),
                                   cursor_compositor->texture(),
                                   (base + "-composed.bmp").c_str());
                    std::cout << "Cursor debug frame " << dump_index << " written"
                              << " | pos=" << frames->pointer().x << ',' << frames->pointer().y
                              << " | renderAdjust="
                              << frames->pointer().render_scale_adjustment << '\n';
                }
                ++dump_index;
            }
            const GpuNv12Converter::ConvertedFrame nv12 = converter->convert();
            const LONGLONG timestamp = static_cast<LONGLONG>(submitted) * frame_duration;
            if (callbacks.consume_keyframe_request && callbacks.consume_keyframe_request()) {
                std::cout << "Android requested H.264 recovery keyframe; forcing next frame\n";
                encoder->force_keyframe();
            }
            if (use_software_encoder) {
                // The software MFT is not D3D11 aware: read the NV12 frame back and
                // hand it over as system memory. The GPU surface is free as soon as
                // the copy exists, and the software encoder buffers several frames
                // internally, so release it now instead of waiting for the output:
                // otherwise the three-slot pool starves instantly.
                const std::vector<std::uint8_t> nv12_bytes =
                    converter->read_nv12(nv12.texture);
                converter->release_slot(nv12.slot);
                submitted_at.emplace(
                    timestamp,
                    SubmittedFrame{
                        .submitted_at = Clock::now(),
                        .converter_slot = nv12.slot,
                        .slot_held = false,
                    });
                encoder->submit_bytes(nv12_bytes, timestamp, frame_duration);
            } else {
                submitted_at.emplace(
                    timestamp,
                    SubmittedFrame{
                        .submitted_at = Clock::now(),
                        .converter_slot = nv12.slot,
                    });
                encoder->submit(nv12.texture, timestamp, frame_duration);
            }
            ++submitted;
            continue;
        }

        if (event == METransformHaveOutput) {
            const std::optional<HardwareAvcEncoder::Output> output = encoder->take_output();
            if (!output) {
                // A renegotiation dropped the frame the encoder was holding. Release
                // its GPU slot so the NV12 pool cannot starve, and keep the session
                // running: the event loop submits the next frame as soon as the
                // encoder asks for one. A plain "no output yet" is not a drop.
                if (encoder->dropped_input() && !submitted_at.empty()) {
                    auto oldest = submitted_at.begin();
                    for (auto candidate = submitted_at.begin();
                         candidate != submitted_at.end();
                         ++candidate) {
                        if (candidate->first < oldest->first) {
                            oldest = candidate;
                        }
                    }
                    if (oldest->second.slot_held) {
                        converter->release_slot(oldest->second.converter_slot);
                        oldest->second.slot_held = false;
                    }
                    submitted_at.erase(oldest);
                }
                continue;
            }
            ++encoded;
            total_bytes += output->bytes.size();
            keyframes += output->keyframe ? 1U : 0U;
            annex_b_outputs += output->annex_b ? 1U : 0U;
            const auto input = submitted_at.find(output->timestamp);
            if (input != submitted_at.end()) {
                const double latency_ms = std::chrono::duration<double, std::milli>(
                    Clock::now() - input->second.submitted_at).count();
                total_latency_ms += latency_ms;
                maximum_latency_ms = std::max(maximum_latency_ms, latency_ms);
                if (encoded == 1U) {
                    first_frame_latency_ms = latency_ms;
                }
                if (input->second.slot_held) {
                    converter->release_slot(input->second.converter_slot);
                    input->second.slot_held = false;
                }
                submitted_at.erase(input);
            }
            if (callbacks.on_frame) {
                const std::uint64_t timestamp_micros = output->timestamp >= 0
                    ? stream_timestamp_base + static_cast<std::uint64_t>(output->timestamp / 10)
                    : stream_timestamp_base;
                callbacks.on_frame(
                    output->bytes, output->keyframe, timestamp_micros);
            }
            continue;
        }

        if (event == METransformDrainComplete) {
            drained = true;
            continue;
        }

        if (event == MEError || event == MENonFatalError) {
            throw std::runtime_error("hardware encoder reported an error event");
        }
    }

    const double elapsed_seconds = std::chrono::duration<double>(Clock::now() - started).count();
    const double output_rate = elapsed_seconds > 0.0
        ? static_cast<double>(encoded) / elapsed_seconds
        : 0.0;
    const double actual_mbps = elapsed_seconds > 0.0
        ? static_cast<double>(total_bytes) * 8.0 / elapsed_seconds / 1'000'000.0
        : 0.0;
    const double desktop_update_rate = elapsed_seconds > 0.0
        ? static_cast<double>(desktop_updates_before_reset + frames->updates()) / elapsed_seconds
        : 0.0;
    const double average_latency_ms = encoded > 0U
        ? total_latency_ms / static_cast<double>(encoded)
        : 0.0;

    std::cout << std::fixed << std::setprecision(2)
              << "Encode result: submitted=" << submitted
              << " | encoded=" << encoded
              << " | desktopUpdates=" << (desktop_updates_before_reset + frames->updates())
              << " | desktopUpdateRate=" << desktop_update_rate << " fps"
              << " | keyframes=" << keyframes
              << " | outputRate=" << output_rate << " fps"
              << " | bitrate=" << actual_mbps << " Mbps\n"
              << "Latency: first=" << first_frame_latency_ms
              << " ms | average=" << average_latency_ms
              << " ms | maximum=" << maximum_latency_ms << " ms\n"
              << "Bitstream framing: AnnexB=" << annex_b_outputs << '/' << encoded << '\n';
    return encoded > 0U && (encoder->low_latency_enabled() || encoder->software()) ? 0 : 2;
}

int run_desktop_encode_benchmark(
    std::chrono::seconds duration,
    bool prefer_software_encoder,
    std::string_view capture_display_device) {
    return run_desktop_encode_session(
        duration, {}, 0U, 0U, 60U, capture_display_device, prefer_software_encoder);
}

int run_cursor_shape_probe(const char* output_path) {
    ComApartment apartment;
    if (output_path == nullptr || *output_path == '\0') {
        throw std::invalid_argument("cursor shape probe needs an output path");
    }
    struct Probe {
        const char* name;
        LPCSTR resource;
    };
    const Probe probes[2] = {
        {"arrow", IDC_ARROW},
        {"ibeam", IDC_IBEAM},
    };
    for (const Probe& probe : probes) {
        const HCURSOR cursor = LoadCursorA(nullptr, probe.resource);
        std::cout << probe.name << ": ";
        if (cursor == nullptr) {
            std::cout << "LoadCursor failed\n";
            continue;
        }
        DesktopPointer pointer;
        if (!DesktopFrameSource::capture_cursor_shape_for_probe(cursor, pointer)) {
            std::cout << "capture failed\n";
            continue;
        }
        std::size_t opaque = 0U;
        std::size_t white = 0U;
        std::size_t black = 0U;
        for (std::size_t offset = 0; offset + 3U < pointer.bytes.size(); offset += 4U) {
            if (pointer.bytes[offset + 3U] == 0U) {
                continue;
            }
            ++opaque;
            if (pointer.bytes[offset] > 200U) {
                ++white;
            } else if (pointer.bytes[offset] < 60U) {
                ++black;
            }
        }
        std::cout << pointer.width << 'x' << pointer.height
                  << " | opaque=" << opaque
                  << " | white=" << white
                  << " | black=" << black
                  << " | hotspot=" << pointer.hotspot_x << ',' << pointer.hotspot_y
                  << '\n';
        const std::string path = std::string(output_path) + '-' + probe.name + ".bmp";
        write_rgba_bmp(pointer.bytes.data(), pointer.width, pointer.height, path.c_str());
        std::cout << "  written " << path << '\n';
    }
    return 0;
}

int run_desktop_display_list() {
    ComApartment apartment;
    const auto targets = enumerate_capture_targets();
    std::cout << "Attached WireView capture displays: " << targets.size() << '\n';
    for (const CaptureTarget& target : targets) {
        const RECT& bounds = target.output_description.DesktopCoordinates;
        UINT effective_x = 0U;
        UINT effective_y = 0U;
        const HRESULT effective_result = GetDpiForMonitor(
            target.output_description.Monitor, MDT_EFFECTIVE_DPI, &effective_x, &effective_y);
        UINT raw_x = 0U;
        UINT raw_y = 0U;
        const HRESULT raw_result = GetDpiForMonitor(
            target.output_description.Monitor, MDT_RAW_DPI, &raw_x, &raw_y);
        std::cout << "  " << utf8(target.output_description.DeviceName)
                  << " | " << utf8(target.adapter_description.Description)
                  << " | " << (target.primary ? "primary" : "secondary")
                  << " | rect=" << bounds.left << ',' << bounds.top << '-'
                  << bounds.right << ',' << bounds.bottom
                  << " | size=" << (bounds.right - bounds.left) << 'x'
                  << (bounds.bottom - bounds.top)
                  << " | dpi=" << (SUCCEEDED(effective_result) ? effective_x : 0U)
                  << " (" << (SUCCEEDED(raw_result) ? raw_x : 0U) << " raw)"
                  << " | dpiScale="
                  << monitor_cursor_scale(target.output_description.Monitor)
                  << '\n';
    }
    return 0;
}

DesktopCaptureArea resolve_desktop_capture_area(std::string_view capture_display_device) {
    ComApartment apartment;
    const CaptureTarget target = find_capture_target(capture_display_device);
    const RECT& bounds = target.output_description.DesktopCoordinates;
    if (bounds.right <= bounds.left || bounds.bottom <= bounds.top) {
        throw std::runtime_error("requested capture display has invalid desktop bounds");
    }
    return DesktopCaptureArea{
        .left = bounds.left,
        .top = bounds.top,
        .width = static_cast<std::uint32_t>(bounds.right - bounds.left),
        .height = static_cast<std::uint32_t>(bounds.bottom - bounds.top),
    };
}

}  // namespace wireview::host
