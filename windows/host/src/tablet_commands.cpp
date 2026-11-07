#include "tablet_commands.hpp"

#include <algorithm>
#include <cstring>
#include <cwchar>
#include <utility>

#include <dwmapi.h>
#include <shellapi.h>

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
    if (payload.size() != 10U) {
        return false;
    }
    token = read_u32_le(payload, 1U);
    index = read_u32_le(payload, 5U);
    target_area = payload[9U];
    return target_area <= kWindowAreaVdd + 1U;
}

std::vector<std::uint8_t> encode_window_list(
    std::uint32_t token,
    const std::vector<TabletWindowEntry>& entries) {
    std::vector<std::uint8_t> payload;
    payload.reserve(6U + entries.size() * 16U);
    write_u32_le(payload, token);
    payload.push_back(static_cast<std::uint8_t>(entries.size() & 0xffU));
    payload.push_back(static_cast<std::uint8_t>((entries.size() >> 8U) & 0xffU));
    for (const auto& entry : entries) {
        const std::uint64_t handle = reinterpret_cast<std::uint64_t>(entry.window);
        for (int shift = 0; shift < 64; shift += 8) {
            payload.push_back(
                static_cast<std::uint8_t>((handle >> shift) & 0xffU));
        }
        payload.push_back(entry.area);
        const std::string title = narrow_utf8(entry.title);
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

bool launch_touch_keyboard() {
    // The touch keyboard is the natural choice for the tablet: it follows the
    // focused window, so with focus on the WireView display it appears there.
    // A desktop without a touch digitizer may refuse TabTip; osk.exe is the
    // always-available fallback.
    wchar_t common_files[MAX_PATH]{};
    if (GetEnvironmentVariableW(L"CommonProgramFiles", common_files, MAX_PATH) != 0U) {
        const std::wstring tab_tip =
            std::wstring(common_files) + L"\\microsoft shared\\ink\\TabTip.exe";
        if (GetFileAttributesW(tab_tip.c_str()) != INVALID_FILE_ATTRIBUTES) {
            const HINSTANCE result = ShellExecuteW(
                nullptr, L"open", tab_tip.c_str(), nullptr, nullptr, SW_SHOW);
            if (reinterpret_cast<INT_PTR>(result) > 32) {
                return true;
            }
        }
    }
    wchar_t windows_directory[MAX_PATH]{};
    if (GetWindowsDirectoryW(windows_directory, MAX_PATH) == 0U) {
        return false;
    }
    const std::wstring osk = std::wstring(windows_directory) + L"\\System32\\osk.exe";
    const HINSTANCE result = ShellExecuteW(
        nullptr, L"open", osk.c_str(), nullptr, nullptr, SW_SHOW);
    return reinterpret_cast<INT_PTR>(result) > 32;
}

void request_control_center_rotation() {
    const HWND control = FindWindowW(L"WireViewControlWindow", nullptr);
    if (control != nullptr) {
        PostMessageW(
            control, kControlCenterRotateMessage, kControlCenterRotateRequest, 0);
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
