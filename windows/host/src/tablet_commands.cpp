#include "tablet_commands.hpp"

#include <algorithm>
#include <cstring>
#include <cwchar>
#include <iostream>
#include <utility>

#include <dwmapi.h>

namespace wireview::host {
namespace {

[[nodiscard]] std::uint32_t read_u32_le(
    std::span<const std::uint8_t> bytes, std::size_t offset) {
    return static_cast<std::uint32_t>(bytes[offset]) |
        (static_cast<std::uint32_t>(bytes[offset + 1U]) << 8U) |
        (static_cast<std::uint32_t>(bytes[offset + 2U]) << 16U) |
        (static_cast<std::uint32_t>(bytes[offset + 3U]) << 24U);
}

void write_u32_le(std::vector<std::uint8_t>& bytes, std::uint32_t value) {
    bytes.push_back(static_cast<std::uint8_t>(value & 0xffU));
    bytes.push_back(static_cast<std::uint8_t>((value >> 8U) & 0xffU));
    bytes.push_back(static_cast<std::uint8_t>((value >> 16U) & 0xffU));
    bytes.push_back(static_cast<std::uint8_t>((value >> 24U) & 0xffU));
}

std::string narrow_utf8(const std::wstring& text) {
    if (text.empty()) {
        return {};
    }
    const int size = WideCharToMultiByte(
        CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
        nullptr, 0, nullptr, nullptr);
    std::string result(static_cast<std::size_t>(size > 0 ? size : 0), '\0');
    if (size > 0) {
        WideCharToMultiByte(
            CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
            result.data(), size, nullptr, nullptr);
    }
    return result;
}

std::wstring widen_utf8(const std::string& text) {
    if (text.empty()) {
        return {};
    }
    const int size = MultiByteToWideChar(
        CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring result(static_cast<std::size_t>(size > 0 ? size : 0), L'\0');
    if (size > 0) {
        MultiByteToWideChar(
            CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
            result.data(), size);
    }
    return result;
}

// Resolve the monitor rect for a target area: primary monitor or the VDD.
bool resolve_area_monitor(
    std::uint8_t area, const std::wstring& vdd_device_name, RECT& rect) {
    if (area == kWindowAreaPrimary) {
        const HMONITOR monitor = MonitorFromPoint(
            POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
        MONITORINFO info{};
        info.cbSize = sizeof(info);
        if (monitor == nullptr || GetMonitorInfoW(monitor, &info) == FALSE) {
            return false;
        }
        rect = info.rcMonitor;
        return true;
    }
    if (area != kWindowAreaVdd) {
        return false;
    }
    struct Match {
        const std::wstring& vdd;
        HMONITOR monitor{nullptr};
    } match{vdd_device_name, nullptr};
    EnumDisplayMonitors(
        nullptr, nullptr,
        [](HMONITOR monitor, HDC, LPRECT, LPARAM lparam) -> BOOL {
            auto* context = reinterpret_cast<Match*>(lparam);
            MONITORINFOEXW info{};
            info.cbSize = sizeof(info);
            if (GetMonitorInfoW(monitor, &info) != FALSE &&
                _wcsicmp(info.szDevice, context->vdd.c_str()) == 0) {
                context->monitor = monitor;
                return FALSE;
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&match));
    if (match.monitor == nullptr) {
        return false;
    }
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    if (GetMonitorInfoW(match.monitor, &info) == FALSE) {
        return false;
    }
    rect = info.rcMonitor;
    return true;
}

}  // namespace

bool decode_control_command(
    std::span<const std::uint8_t> payload, TabletCommand& command) {
    if (payload.empty()) {
        return false;
    }
    command = static_cast<TabletCommand>(payload[0]);
    return true;
}

bool decode_window_move(
    std::span<const std::uint8_t> payload,
    std::uint32_t& token,
    std::uint32_t& index,
    std::uint8_t& target_area) {
    // Defense in depth: the dispatcher already routes by payload[0], but the
    // sibling decoders self-check their command byte, so mirror that here.
    if (payload.size() != 10U ||
        payload[0] != static_cast<std::uint8_t>(TabletCommand::move_window)) {
        return false;
    }
    token = read_u32_le(payload, 1U);
    index = read_u32_le(payload, 5U);
    target_area = payload[9U];
    return target_area <= kWindowAreaVdd;
}

bool decode_ime_text(std::span<const std::uint8_t> payload, std::wstring& text) {
    if (payload.size() < 3U ||
        payload[0] != static_cast<std::uint8_t>(TabletCommand::ime_text)) {
        return false;
    }
    const std::size_t length = static_cast<std::size_t>(payload[1]) |
        (static_cast<std::size_t>(payload[2]) << 8U);
    if (3U + length != payload.size() || length == 0U) {
        return false;
    }
    text = widen_utf8(std::string(
        reinterpret_cast<const char*>(payload.data() + 3U), length));
    return !text.empty();
}

bool decode_ime_key(std::span<const std::uint8_t> payload, UINT& virtual_key) {
    if (payload.size() != 2U ||
        payload[0] != static_cast<std::uint8_t>(TabletCommand::ime_key)) {
        return false;
    }
    // Whitelist editing keys only; printable text always rides ime_text.
    switch (payload[1U]) {
        case VK_BACK:
        case VK_TAB:
        case VK_RETURN:
        case VK_DELETE:
        case VK_LEFT:
        case VK_UP:
        case VK_RIGHT:
        case VK_DOWN:
            virtual_key = payload[1U];
            return true;
        default:
            return false;
    }
}

std::vector<std::uint8_t> encode_window_list(
    std::uint32_t token,
    const std::vector<TabletWindowEntry>& entries) {
    // The count field is u16: a silently wrapped count would make the
    // Android side read fewer entries than encoded and treat the remainder
    // as unparsed trailing bytes, so cap the list itself (same policy as
    // the title truncation below).
    const std::size_t count = entries.size() > 0xffffU ? 0xffffU : entries.size();
    std::vector<std::uint8_t> payload;
    payload.reserve(6U + count * 16U);
    write_u32_le(payload, token);
    payload.push_back(static_cast<std::uint8_t>(count & 0xffU));
    payload.push_back(static_cast<std::uint8_t>((count >> 8U) & 0xffU));
    for (std::size_t entry_index = 0U; entry_index < count; ++entry_index) {
        const auto& entry = entries[entry_index];
        const std::uint64_t handle = reinterpret_cast<std::uint64_t>(entry.window);
        for (int shift = 0; shift < 64; shift += 8) {
            payload.push_back(
                static_cast<std::uint8_t>((handle >> shift) & 0xffU));
        }
        payload.push_back(entry.area);
        std::string title = narrow_utf8(entry.title);
        // The length field is u16: a silently wrapped length would make the
        // Android side read beyond the entry and corrupt the whole stream,
        // so truncate oversized titles before encoding.
        if (title.size() > 0xffffU) {
            title.resize(0xffffU);
        }
        payload.push_back(static_cast<std::uint8_t>(title.size() & 0xffU));
        payload.push_back(static_cast<std::uint8_t>((title.size() >> 8U) & 0xffU));
        payload.insert(payload.end(), title.begin(), title.end());
    }
    return payload;
}

bool inject_escape_key() {
    INPUT inputs[2]{};
    inputs[0].type = INPUT_KEYBOARD;
    inputs[0].ki.wVk = VK_ESCAPE;
    inputs[1].type = INPUT_KEYBOARD;
    inputs[1].ki.wVk = VK_ESCAPE;
    inputs[1].ki.dwFlags = KEYEVENTF_KEYUP;
    return SendInput(2U, inputs, sizeof(INPUT)) == 2U;
}

bool inject_unicode_text(const std::wstring& text) {
    if (text.empty()) {
        return true;
    }
    // VK_PACKET + KEYEVENTF_UNICODE injects characters as if typed, without
    // running them through the desktop IME. Surrogate pairs are sent as two
    // consecutive units, which Windows joins. Flush in chunks so one giant
    // paste cannot overflow SendInput's internal queue.
    constexpr std::size_t kChunkInputs = 512U;
    std::vector<INPUT> inputs;
    inputs.reserve(kChunkInputs);
    const auto flush = [&inputs]() {
        while (!inputs.empty()) {
            const std::size_t batch = std::min(inputs.size(), kChunkInputs);
            if (SendInput(static_cast<UINT>(batch), inputs.data(), sizeof(INPUT)) !=
                static_cast<UINT>(batch)) {
                return false;
            }
            inputs.erase(inputs.begin(), inputs.begin() + static_cast<std::ptrdiff_t>(batch));
        }
        return true;
    };
    for (const wchar_t character : text) {
        // KEYEVENTF_UNICODE requires wVk == 0; the character rides wScan.
        INPUT down{};
        down.type = INPUT_KEYBOARD;
        down.ki.wVk = 0U;
        down.ki.wScan = static_cast<WORD>(character);
        down.ki.dwFlags = KEYEVENTF_UNICODE;
        INPUT up = down;
        up.ki.dwFlags = KEYEVENTF_UNICODE | KEYEVENTF_KEYUP;
        inputs.push_back(down);
        inputs.push_back(up);
        if (inputs.size() >= kChunkInputs && !flush()) {
            return false;
        }
    }
    return flush();
}

bool inject_virtual_key(UINT virtual_key) {
    INPUT inputs[2]{};
    inputs[0].type = INPUT_KEYBOARD;
    inputs[0].ki.wVk = static_cast<WORD>(virtual_key);
    inputs[1].type = INPUT_KEYBOARD;
    inputs[1].ki.wVk = static_cast<WORD>(virtual_key);
    inputs[1].ki.dwFlags = KEYEVENTF_KEYUP;
    return SendInput(2U, inputs, sizeof(INPUT)) == 2U;
}

void request_control_center_rotation() {
    const HWND control = FindWindowW(L"WireViewControlWindow", nullptr);
    if (control == nullptr) {
        std::cerr << "Rotation request dropped: control center window not found\n";
        return;
    }
    if (PostMessageW(
            control, kControlCenterRotateMessage, kControlCenterRotateRequest, 0) == FALSE) {
        std::cerr << "Rotation request dropped: PostMessage failed with "
                  << GetLastError() << '\n';
    } else {
        std::cout << "Rotation request posted to the control center\n";
    }
}

std::vector<TabletWindowEntry> enumerate_streaming_windows(
    const std::wstring& vdd_device_name) {
    struct Context {
        const std::wstring& vdd;
        std::vector<TabletWindowEntry>* entries;
    } context{vdd_device_name, nullptr};

    std::vector<TabletWindowEntry> entries;
    context.entries = &entries;
    EnumWindows(
        [](HWND window, LPARAM lparam) -> BOOL {
            auto* context = reinterpret_cast<Context*>(lparam);
            if (IsWindowVisible(window) == FALSE || IsIconic(window) != FALSE) {
                return TRUE;
            }
            DWORD process_id = 0U;
            GetWindowThreadProcessId(window, &process_id);
            if (process_id == GetCurrentProcessId()) {
                return TRUE;
            }
            if ((GetWindowLongW(window, GWL_EXSTYLE) & WS_EX_TOOLWINDOW) != 0U) {
                return TRUE;
            }
            const int title_length = GetWindowTextLengthW(window);
            if (title_length <= 0) {
                return TRUE;
            }
            // Skip cloaked windows (invisible UWP hosts keep living in the
            // background with a regular window handle).
            BOOL cloaked = FALSE;
            if (DwmGetWindowAttribute(
                    window, DWMWA_CLOAKED, &cloaked, sizeof(cloaked)) == S_OK &&
                cloaked != FALSE) {
                return TRUE;
            }
            wchar_t class_name[64]{};
            if (GetClassNameW(window, class_name, 64) != 0) {
                static const wchar_t* const skipped_classes[] = {
                    L"Shell_TrayWnd",
                    L"Shell_SecondaryTrayWnd",
                    L"Progman",
                    L"WorkerW",
                    L"Windows.UI.Core.CoreWindow",
                    L"XamlExplorerHostIslandWindow",
                };
                for (const wchar_t* skipped : skipped_classes) {
                    if (wcscmp(class_name, skipped) == 0) {
                        return TRUE;
                    }
                }
            }
            std::wstring title(static_cast<std::size_t>(title_length), L'\0');
            const int copied = GetWindowTextW(
                window, title.data(), title_length + 1);
            if (copied <= 0) {
                return TRUE;
            }
            title.resize(static_cast<std::size_t>(copied));

            TabletWindowEntry entry;
            entry.window = window;
            entry.title = std::move(title);
            const HMONITOR monitor = MonitorFromWindow(
                window, MONITOR_DEFAULTTONEAREST);
            MONITORINFOEXW info{};
            info.cbSize = sizeof(info);
            if (GetMonitorInfoW(monitor, &info) != FALSE) {
                if (_wcsicmp(info.szDevice, context->vdd.c_str()) == 0) {
                    entry.area = kWindowAreaVdd;
                } else if ((info.dwFlags & MONITORINFOF_PRIMARY) != 0U) {
                    entry.area = kWindowAreaPrimary;
                } else {
                    entry.area = kWindowAreaOther;
                }
            }
            context->entries->push_back(std::move(entry));
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&context));
    return entries;
}

bool move_window_to_area(
    HWND window,
    std::uint8_t target_area,
    const std::wstring& vdd_device_name) {
    if (window == nullptr || IsWindow(window) == FALSE) {
        return false;
    }
    // The tablet's list is a snapshot: if the window already sits on the
    // requested monitor, the user's intent is "switch sides", so flip to the
    // other main monitor instead of re-centering it where it already is.
    const HMONITOR current_monitor = MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST);
    MONITORINFOEXW current_info{};
    current_info.cbSize = sizeof(current_info);
    if (GetMonitorInfoW(current_monitor, &current_info) != FALSE) {
        std::uint8_t current_area = kWindowAreaOther;
        if (_wcsicmp(current_info.szDevice, vdd_device_name.c_str()) == 0) {
            current_area = kWindowAreaVdd;
        } else if ((current_info.dwFlags & MONITORINFOF_PRIMARY) != 0U) {
            current_area = kWindowAreaPrimary;
        }
        if (current_area == target_area && current_area != kWindowAreaOther) {
            target_area = current_area == kWindowAreaPrimary ? kWindowAreaVdd : kWindowAreaPrimary;
        }
    }
    RECT target{};
    if (!resolve_area_monitor(target_area, vdd_device_name, target)) {
        return false;
    }
    const bool was_zoomed = IsZoomed(window) != FALSE;
    if (was_zoomed) {
        ShowWindow(window, SW_RESTORE);
    }
    RECT current{};
    if (GetWindowRect(window, &current) == FALSE) {
        return false;
    }
    const LONG width = std::min<LONG>(
        current.right - current.left, target.right - target.left);
    const LONG height = std::min<LONG>(
        current.bottom - current.top, target.bottom - target.top);
    const LONG x = target.left + ((target.right - target.left) - width) / 2;
    const LONG y = target.top + ((target.bottom - target.top) - height) / 2;
    if (SetWindowPos(
            window, nullptr, x, y, 0, 0,
            SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE) == FALSE) {
        return false;
    }
    if (was_zoomed) {
        ShowWindow(window, SW_MAXIMIZE);
    }
    return true;
}

}  // namespace wireview::host
