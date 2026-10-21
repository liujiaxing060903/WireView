#include "windows_video.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <codecapi.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <strmif.h>
#include <wrl/client.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace wireview::host {
namespace {

using Microsoft::WRL::ComPtr;

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

[[nodiscard]] std::string utf8(const wchar_t* value) {
    if (value == nullptr || *value == L'\0') {
        return {};
    }
    const int length = WideCharToMultiByte(
        CP_UTF8,
        0,
        value,
        -1,
        nullptr,
        0,
        nullptr,
        nullptr);
    if (length <= 1) {
        return {};
    }
    std::string result(static_cast<std::size_t>(length), '\0');
    const int converted = WideCharToMultiByte(
        CP_UTF8,
        0,
        value,
        -1,
        result.data(),
        length,
        nullptr,
        nullptr);
    if (converted != length) {
        throw std::runtime_error("WideCharToMultiByte failed");
    }
    result.pop_back();
    return result;
}

[[nodiscard]] std::string guid_text(const GUID& value) {
    wchar_t buffer[64]{};
    StringFromGUID2(value, buffer, static_cast<int>(std::size(buffer)));
    return utf8(buffer);
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

[[nodiscard]] std::vector<CaptureTarget> enumerate_capture_targets(bool print_details) {
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
        if (print_details) {
            std::cout << "Adapter " << adapter_index << ": "
                      << utf8(adapter_description.Description)
                      << " | dedicatedVRAM="
                      << (adapter_description.DedicatedVideoMemory / (1024U * 1024U))
                      << " MiB | software="
                      << ((adapter_description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0U ? "yes" : "no")
                      << '\n';
        }

        for (UINT output_index = 0;; ++output_index) {
            ComPtr<IDXGIOutput> output;
            const HRESULT output_result = adapter->EnumOutputs(output_index, &output);
            if (output_result == DXGI_ERROR_NOT_FOUND) {
                break;
            }
            check_hresult(output_result, "IDXGIAdapter1::EnumOutputs");

            DXGI_OUTPUT_DESC output_description{};
            check_hresult(output->GetDesc(&output_description), "IDXGIOutput::GetDesc");
            ComPtr<IDXGIOutput1> output1;
            check_hresult(output.As(&output1), "IDXGIOutput::QueryInterface(IDXGIOutput1)");

            const RECT& bounds = output_description.DesktopCoordinates;
            const bool primary = contains_desktop_origin(bounds);
            if (print_details) {
                std::cout << "  Output " << output_index << ": "
                          << utf8(output_description.DeviceName)
                          << " | attached=" << (output_description.AttachedToDesktop ? "yes" : "no")
                          << " | primary=" << (primary ? "yes" : "no")
                          << " | rect=" << bounds.left << ',' << bounds.top << '-'
                          << bounds.right << ',' << bounds.bottom
                          << " | rotation=" << static_cast<unsigned>(output_description.Rotation)
                          << '\n';
            }

            if (output_description.AttachedToDesktop) {
                targets.push_back(CaptureTarget{
                    .adapter = adapter,
                    .output = output1,
                    .adapter_description = adapter_description,
                    .output_description = output_description,
                    .primary = primary,
                });
            }
        }
    }
    return targets;
}

[[nodiscard]] std::string activate_string(IMFActivate* activation, REFGUID key) {
    wchar_t* value = nullptr;
    UINT32 length = 0;
    const HRESULT result = activation->GetAllocatedString(key, &value, &length);
    if (FAILED(result) || value == nullptr) {
        return {};
    }
    const std::string converted = utf8(value);
    CoTaskMemFree(value);
    return converted;
}

[[nodiscard]] const char* support_text(ICodecAPI* codec_api, const GUID& property) {
    if (codec_api == nullptr) {
        return "no ICodecAPI";
    }
    return codec_api->IsSupported(&property) == S_OK ? "yes" : "no";
}

void enumerate_hardware_avc_encoders() {
    const MFT_REGISTER_TYPE_INFO input_type{
        MFMediaType_Video,
        MFVideoFormat_NV12,
    };
    const MFT_REGISTER_TYPE_INFO output_type{
        MFMediaType_Video,
        MFVideoFormat_H264,
    };

    IMFActivate** activations = nullptr;
    UINT32 count = 0;
    check_hresult(
        MFTEnumEx(
            MFT_CATEGORY_VIDEO_ENCODER,
            MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER,
            &input_type,
            &output_type,
            &activations,
            &count),
        "MFTEnumEx(H.264 hardware encoders)");

    std::cout << "Hardware H.264 encoder MFTs: " << count << '\n';
    for (UINT32 index = 0; index < count; ++index) {
        IMFActivate* activation = activations[index];
        const std::string name = activate_string(activation, MFT_FRIENDLY_NAME_Attribute);
        const std::string hardware_url =
            activate_string(activation, MFT_ENUM_HARDWARE_URL_Attribute);
        GUID class_id{};
        const HRESULT class_result = activation->GetGUID(MFT_TRANSFORM_CLSID_Attribute, &class_id);

        std::cout << "  Encoder " << index << ": "
                  << (name.empty() ? "<unnamed>" : name);
        if (SUCCEEDED(class_result)) {
            std::cout << " | clsid=" << guid_text(class_id);
        }
        if (!hardware_url.empty()) {
            std::cout << " | hardwareUrl=" << hardware_url;
        }
        std::cout << '\n';

        ComPtr<IMFTransform> transform;
        const HRESULT activate_result = activation->ActivateObject(
            __uuidof(IMFTransform),
            reinterpret_cast<void**>(transform.GetAddressOf()));
        if (FAILED(activate_result)) {
            std::cout << "    activation=" << hresult_text(activate_result) << '\n';
        } else {
            ComPtr<ICodecAPI> codec_api;
            transform.As(&codec_api);
            std::cout << "    lowLatency="
                      << support_text(codec_api.Get(), CODECAPI_AVEncCommonLowLatency)
                      << " | rateControl="
                      << support_text(codec_api.Get(), CODECAPI_AVEncCommonRateControlMode)
                      << " | meanBitrate="
                      << support_text(codec_api.Get(), CODECAPI_AVEncCommonMeanBitRate)
                      << " | gopSize="
                      << support_text(codec_api.Get(), CODECAPI_AVEncMPVGOPSize)
                      << " | forceKeyframe="
                      << support_text(codec_api.Get(), CODECAPI_AVEncVideoForceKeyFrame)
                      << '\n';
            activation->ShutdownObject();
        }
        activation->Release();
    }
    CoTaskMemFree(activations);
}

[[nodiscard]] ComPtr<ID3D11Device> create_capture_device(IDXGIAdapter1* adapter) {
    constexpr UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT |
                           D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
    constexpr D3D_FEATURE_LEVEL requested_levels[]{
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
    };

    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    D3D_FEATURE_LEVEL selected_level{};
    HRESULT result = D3D11CreateDevice(
        adapter,
        D3D_DRIVER_TYPE_UNKNOWN,
        nullptr,
        flags,
        requested_levels,
        static_cast<UINT>(std::size(requested_levels)),
        D3D11_SDK_VERSION,
        &device,
        &selected_level,
        &context);
    if (result == E_INVALIDARG) {
        result = D3D11CreateDevice(
            adapter,
            D3D_DRIVER_TYPE_UNKNOWN,
            nullptr,
            flags,
            &requested_levels[1],
            1,
            D3D11_SDK_VERSION,
            &device,
            &selected_level,
            &context);
    }
    check_hresult(result, "D3D11CreateDevice(capture adapter)");
    std::cout << "D3D feature level: 0x" << std::hex
              << static_cast<unsigned>(selected_level) << std::dec << '\n';
    return device;
}

}  // namespace

int run_windows_video_probe() {
    ComApartment apartment;
    MediaFoundationSession media_foundation;

    const auto targets = enumerate_capture_targets(true);
    std::cout << "Attached desktop outputs: " << targets.size() << '\n';
    enumerate_hardware_avc_encoders();
    return targets.empty() ? 2 : 0;
}

int run_desktop_capture_benchmark(std::chrono::seconds duration) {
    if (duration <= std::chrono::seconds::zero()) {
        throw std::invalid_argument("capture duration must be positive");
    }

    ComApartment apartment;
    auto targets = enumerate_capture_targets(false);
    if (targets.empty()) {
        throw std::runtime_error("no attached DXGI desktop output was found");
    }
    const auto primary = std::find_if(
        targets.begin(),
        targets.end(),
        [](const CaptureTarget& target) { return target.primary; });
    CaptureTarget& target = primary != targets.end() ? *primary : targets.front();

    std::cout << "Capturing " << utf8(target.output_description.DeviceName)
              << " on " << utf8(target.adapter_description.Description)
              << " for " << duration.count() << " seconds\n";

    ComPtr<ID3D11Device> device = create_capture_device(target.adapter.Get());
    ComPtr<IDXGIOutputDuplication> duplication;
    check_hresult(
        target.output->DuplicateOutput(device.Get(), &duplication),
        "IDXGIOutput1::DuplicateOutput");

    DXGI_OUTDUPL_DESC duplication_description{};
    duplication->GetDesc(&duplication_description);
    std::cout << "Duplication mode: "
              << duplication_description.ModeDesc.Width << 'x'
              << duplication_description.ModeDesc.Height
              << " | format="
              << static_cast<unsigned>(duplication_description.ModeDesc.Format)
              << " | rotation="
              << static_cast<unsigned>(duplication_description.Rotation)
              << '\n';

    LARGE_INTEGER performance_frequency{};
    QueryPerformanceFrequency(&performance_frequency);
    std::int64_t previous_present_time = 0;
    double interval_sum_ms = 0.0;
    double interval_max_ms = 0.0;
    std::uint64_t interval_count = 0;
    std::uint64_t acquired_frames = 0;
    std::uint64_t present_frames = 0;
    std::uint64_t pointer_updates = 0;
    std::uint64_t dropped_before_acquire = 0;
    std::uint64_t timeouts = 0;
    D3D11_TEXTURE2D_DESC first_texture_description{};

    const auto started = std::chrono::steady_clock::now();
    const auto deadline = started + duration;
    while (std::chrono::steady_clock::now() < deadline) {
        DXGI_OUTDUPL_FRAME_INFO frame_info{};
        ComPtr<IDXGIResource> resource;
        const HRESULT acquire_result = duplication->AcquireNextFrame(
            250,
            &frame_info,
            &resource);
        if (acquire_result == DXGI_ERROR_WAIT_TIMEOUT) {
            ++timeouts;
            continue;
        }
        check_hresult(acquire_result, "IDXGIOutputDuplication::AcquireNextFrame");

        ++acquired_frames;
        if (frame_info.AccumulatedFrames > 1U) {
            dropped_before_acquire += frame_info.AccumulatedFrames - 1U;
        }
        if (frame_info.LastPresentTime.QuadPart != 0) {
            ++present_frames;
            if (previous_present_time != 0 && performance_frequency.QuadPart > 0) {
                const double interval_ms =
                    static_cast<double>(frame_info.LastPresentTime.QuadPart - previous_present_time) *
                    1000.0 / static_cast<double>(performance_frequency.QuadPart);
                interval_sum_ms += interval_ms;
                interval_max_ms = std::max(interval_max_ms, interval_ms);
                ++interval_count;
            }
            previous_present_time = frame_info.LastPresentTime.QuadPart;
        } else {
            ++pointer_updates;
        }

        if (acquired_frames == 1U) {
            ComPtr<ID3D11Texture2D> texture;
            const HRESULT texture_result = resource.As(&texture);
            if (SUCCEEDED(texture_result)) {
                texture->GetDesc(&first_texture_description);
            }
        }

        check_hresult(
            duplication->ReleaseFrame(),
            "IDXGIOutputDuplication::ReleaseFrame");
    }

    const double elapsed_seconds = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - started).count();
    const double capture_rate = elapsed_seconds > 0.0
        ? static_cast<double>(present_frames) / elapsed_seconds
        : 0.0;
    const double average_interval = interval_count > 0U
        ? interval_sum_ms / static_cast<double>(interval_count)
        : 0.0;

    std::cout << std::fixed << std::setprecision(2)
              << "Capture result: acquired=" << acquired_frames
              << " | presented=" << present_frames
              << " | pointerOnly=" << pointer_updates
              << " | droppedBeforeAcquire=" << dropped_before_acquire
              << " | timeouts=" << timeouts
              << " | presentRate=" << capture_rate << " fps"
              << " | avgInterval=" << average_interval << " ms"
              << " | maxInterval=" << interval_max_ms << " ms\n";
    if (acquired_frames > 0U) {
        std::cout << "First GPU texture: "
                  << first_texture_description.Width << 'x'
                  << first_texture_description.Height
                  << " | format=" << static_cast<unsigned>(first_texture_description.Format)
                  << " | bindFlags=0x" << std::hex
                  << first_texture_description.BindFlags << std::dec << '\n';
    }
    return acquired_frames > 0U ? 0 : 2;
}

}  // namespace wireview::host
