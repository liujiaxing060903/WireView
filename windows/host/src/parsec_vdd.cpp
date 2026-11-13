#include "parsec_vdd.hpp"
#include "display_layout_lock.hpp"
#include "overlapped_completion.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <SetupAPI.h>

#include <array>
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <cwctype>
#include <mutex>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace wireview::host {
namespace {

// The external controller protocol is based on the MIT-licensed
// nomi-san/parsec-vdd public single-header library. WireView deliberately
// talks only to an already installed, externally supplied signed driver.
// https://github.com/nomi-san/parsec-vdd
constexpr GUID kParsecVddAdapterGuid{
    0x00b41627, 0x04c4, 0x429e, {0xa2, 0x6e, 0x02, 0x65, 0xcf, 0x50, 0xc8, 0xfa}};
constexpr DWORD kVddIoctlAdd = 0x0022e004U;
constexpr DWORD kVddIoctlRemove = 0x0022a008U;
constexpr DWORD kVddIoctlUpdate = 0x0022a00cU;
constexpr DWORD kVddIoctlVersion = 0x0022e010U;
constexpr DWORD kInvalidVddResult = 0xFFFFFFFFU;

[[nodiscard]] std::string win32_error(const char* operation) {
    return std::string(operation) + " failed with Win32 error " +
        std::to_string(GetLastError());
}

[[nodiscard]] HANDLE open_parsec_vdd_handle() {
    HDEVINFO devices = SetupDiGetClassDevsW(
        &kParsecVddAdapterGuid, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (devices == INVALID_HANDLE_VALUE) {
        return INVALID_HANDLE_VALUE;
    }
    HANDLE result = INVALID_HANDLE_VALUE;
    for (DWORD index = 0U;; ++index) {
        SP_DEVICE_INTERFACE_DATA interface_data{};
        interface_data.cbSize = sizeof(interface_data);
        if (!SetupDiEnumDeviceInterfaces(
                devices, nullptr, &kParsecVddAdapterGuid, index, &interface_data)) {
            if (GetLastError() == ERROR_NO_MORE_ITEMS) {
                break;
            }
            continue;
        }
        DWORD detail_size = 0U;
        (void)SetupDiGetDeviceInterfaceDetailW(
            devices, &interface_data, nullptr, 0U, &detail_size, nullptr);
        if (detail_size < sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W)) {
            continue;
        }
        std::vector<std::byte> storage(detail_size);
        auto* detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(storage.data());
        detail->cbSize = sizeof(*detail);
        if (!SetupDiGetDeviceInterfaceDetailW(
                devices, &interface_data, detail, detail_size, nullptr, nullptr)) {
            continue;
        }
        result = CreateFileW(
            detail->DevicePath,
            GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_NO_BUFFERING | FILE_FLAG_OVERLAPPED |
                FILE_FLAG_WRITE_THROUGH,
            nullptr);
        if (result != INVALID_HANDLE_VALUE && result != nullptr) {
            break;
        }
    }
    SetupDiDestroyDeviceInfoList(devices);
    return result;
}

[[nodiscard]] bool vdd_ioctl(
    HANDLE device,
    DWORD code,
    const void* data,
    std::size_t size,
    DWORD* result) {
    if (device == nullptr || device == INVALID_HANDLE_VALUE || result == nullptr || size > 32U) {
        return false;
    }
    std::array<std::byte, 32U> input{};
    if (data != nullptr && size != 0U) {
        std::memcpy(input.data(), data, size);
    }
    OVERLAPPED overlapped{};
    HANDLE event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (event == nullptr) {
        return false;
    }
    overlapped.hEvent = event;
    DWORD bytes = 0U;
    const BOOL submitted = DeviceIoControl(
        device,
        code,
        input.data(),
        static_cast<DWORD>(input.size()),
        result,
        sizeof(*result),
        &bytes,
        &overlapped);
    bool completed = submitted != FALSE;
    DWORD error = completed ? ERROR_SUCCESS : GetLastError();
    if (!completed && error == ERROR_IO_PENDING) {
        completed = finish_overlapped_io(device, overlapped, bytes, 5'000U);
        error = completed ? ERROR_SUCCESS : GetLastError();
    }
    CloseHandle(event);
    SetLastError(error);
    return completed;
}

[[nodiscard]] DWORD checked_vdd_ioctl(HANDLE device, DWORD code, const void* data = nullptr, std::size_t size = 0U) {
    DWORD result = kInvalidVddResult;
    if (!vdd_ioctl(device, code, data, size, &result)) {
        throw std::runtime_error(win32_error("DeviceIoControl(Parsec VDD)"));
    }
    return result;
}

[[nodiscard]] std::string utf8(std::wstring_view value) {
    if (value.empty()) {
        return {};
    }
    const int length = WideCharToMultiByte(
        CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (length <= 0) {
        return {};
    }
    std::string result(static_cast<std::size_t>(length), '\0');
    if (WideCharToMultiByte(
            CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), length,
            nullptr, nullptr) != length) {
        return {};
    }
    return result;
}

[[nodiscard]] bool contains_parsec_marker(std::wstring_view value) {
    std::wstring lower(value);
    std::transform(lower.begin(), lower.end(), lower.begin(), [](wchar_t character) {
        return static_cast<wchar_t>(std::towlower(character));
    });
    return lower.find(L"parsec") != std::wstring::npos ||
        lower.find(L"psccdd0") != std::wstring::npos;
}

[[nodiscard]] bool display_mentions_parsec(const DISPLAY_DEVICEW& display) {
    return contains_parsec_marker(display.DeviceString) ||
        contains_parsec_marker(display.DeviceID) ||
        contains_parsec_marker(display.DeviceKey);
}

[[nodiscard]] bool is_parsec_display(const DISPLAY_DEVICEW& display) {
    if (display_mentions_parsec(display)) {
        return true;
    }
    // A physical primary display can expose a Parsec-related monitor child on
    // some NVIDIA laptop stacks. It is still the existing desktop source, not
    // a newly attached virtual display, so never classify it as Parsec via a
    // child-only marker.
    if ((display.StateFlags & DISPLAY_DEVICE_PRIMARY_DEVICE) != 0U) {
        return false;
    }
    // Hybrid-GPU laptops can expose the virtual adapter at the top level and
    // leave the Parsec identity only on its monitor child.
    for (DWORD index = 0U;; ++index) {
        DISPLAY_DEVICEW monitor{};
        monitor.cb = sizeof(monitor);
        if (EnumDisplayDevicesW(display.DeviceName, index, &monitor, 0U) == FALSE) {
            break;
        }
        if (display_mentions_parsec(monitor)) {
            return true;
        }
    }
    return false;
}

struct AttachedDisplay {
    std::string name;
    std::string identity;
    std::string diagnostic;
    bool parsec{false};
};

[[nodiscard]] std::vector<AttachedDisplay> attached_display_devices() {
    std::vector<AttachedDisplay> result;
    for (DWORD index = 0U;; ++index) {
        DISPLAY_DEVICEW display{};
        display.cb = sizeof(display);
        if (!EnumDisplayDevicesW(nullptr, index, &display, 0U)) {
            break;
        }
        if ((display.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP) == 0U) {
            continue;
        }
        AttachedDisplay snapshot;
        snapshot.name = utf8(display.DeviceName);
        const std::string description = utf8(display.DeviceString);
        const std::string identifier = utf8(display.DeviceID);
        const std::string key = utf8(display.DeviceKey);
        snapshot.identity = snapshot.name + "\x1f" + description + "\x1f" + identifier +
            "\x1f" + key;
        snapshot.parsec = is_parsec_display(display);
        snapshot.diagnostic = snapshot.name + " {description=" + description +
            ", id=" + identifier + ", parsec=" + (snapshot.parsec ? "yes" : "no") + "}";
        result.push_back(std::move(snapshot));
    }
    return result;
}

[[nodiscard]] bool was_present(
    const AttachedDisplay& display, const std::vector<std::string>& before) {
    return std::find(before.begin(), before.end(), display.identity) != before.end();
}

[[nodiscard]] std::string display_summary(const std::vector<AttachedDisplay>& displays) {
    std::string result;
    for (const AttachedDisplay& display : displays) {
        if (!result.empty()) {
            result += "; ";
        }
        result += display.diagnostic;
    }
    return result.empty() ? "<none>" : result;
}

// Windows represents "Duplicate these displays" as two active paths that
// share one source. EnumDisplayDevices can then expose only the existing
// DISPLAY1 even though Parsec has accepted its ADD request. Inspect the lower
// level display configuration as well, so that this case is not mistaken for
// a driver failure.
struct CloneTopology {
    bool parsec_clone{};
    bool safe_to_extend{};
    std::string diagnostic;
};

[[nodiscard]] bool same_source(
    const DISPLAYCONFIG_PATH_INFO& left, const DISPLAYCONFIG_PATH_INFO& right) {
    return left.sourceInfo.adapterId.HighPart == right.sourceInfo.adapterId.HighPart &&
        left.sourceInfo.adapterId.LowPart == right.sourceInfo.adapterId.LowPart &&
        left.sourceInfo.id == right.sourceInfo.id;
}

[[nodiscard]] DISPLAYCONFIG_TARGET_DEVICE_NAME display_target_name(const DISPLAYCONFIG_PATH_INFO& path) {
    DISPLAYCONFIG_TARGET_DEVICE_NAME target{};
    target.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
    target.header.size = sizeof(target);
    target.header.adapterId = path.targetInfo.adapterId;
    target.header.id = path.targetInfo.id;
    if (DisplayConfigGetDeviceInfo(&target.header) != ERROR_SUCCESS) {
        return {};
    }
    return target;
}

[[nodiscard]] std::wstring target_description(const DISPLAYCONFIG_PATH_INFO& path) {
    const auto target = display_target_name(path);
    return std::wstring(target.monitorFriendlyDeviceName) + L" " + target.monitorDevicePath;
}

[[nodiscard]] std::wstring display_source_name(const DISPLAYCONFIG_PATH_INFO& path) {
    DISPLAYCONFIG_SOURCE_DEVICE_NAME source{};
    source.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
    source.header.size = sizeof(source);
    source.header.adapterId = path.sourceInfo.adapterId;
    source.header.id = path.sourceInfo.id;
    if (DisplayConfigGetDeviceInfo(&source.header) != ERROR_SUCCESS) {
        return {};
    }
    return source.viewGdiDeviceName;
}

[[nodiscard]] LONG read_active_display_config(UINT32 flags,
    std::vector<DISPLAYCONFIG_PATH_INFO>& paths, std::vector<DISPLAYCONFIG_MODE_INFO>& modes) {
    UINT32 path_count = 0U;
    UINT32 mode_count = 0U;
    const LONG size_result = GetDisplayConfigBufferSizes(flags, &path_count, &mode_count);
    if (size_result != ERROR_SUCCESS) {
        return size_result;
    }
    paths.resize(path_count);
    modes.resize(mode_count);
    const LONG result = QueryDisplayConfig(
        flags, &path_count, paths.data(), &mode_count, modes.data(), nullptr);
    if (result == ERROR_SUCCESS) {
        paths.resize(path_count);
        modes.resize(mode_count);
    }
    return result;
}

[[nodiscard]] bool is_internal_panel(const DISPLAYCONFIG_PATH_INFO& path) {
    const auto technology = path.targetInfo.outputTechnology;
    return technology == DISPLAYCONFIG_OUTPUT_TECHNOLOGY_INTERNAL ||
        technology == DISPLAYCONFIG_OUTPUT_TECHNOLOGY_DISPLAYPORT_EMBEDDED ||
        technology == DISPLAYCONFIG_OUTPUT_TECHNOLOGY_LVDS ||
        technology == DISPLAYCONFIG_OUTPUT_TECHNOLOGY_UDI_EMBEDDED;
}

[[nodiscard]] DISPLAYCONFIG_SOURCE_MODE* source_mode(const DISPLAYCONFIG_PATH_INFO& path,
    std::vector<DISPLAYCONFIG_MODE_INFO>& modes) {
    const UINT32 index = (path.flags & DISPLAYCONFIG_PATH_SUPPORT_VIRTUAL_MODE) != 0U
        ? static_cast<UINT32>(path.sourceInfo.sourceModeInfoIdx) : path.sourceInfo.modeInfoIdx;
    return index < modes.size() && modes[index].infoType == DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE
        ? &modes[index].sourceMode : nullptr;
}

[[nodiscard]] bool same_position(const DISPLAYCONFIG_SOURCE_MODE& left, const DISPLAYCONFIG_SOURCE_MODE& right) {
    return left.position.x == right.position.x && left.position.y == right.position.y;
}

[[nodiscard]] LONG apply_virtual_display_config(std::vector<DISPLAYCONFIG_PATH_INFO>& paths,
    std::vector<DISPLAYCONFIG_MODE_INFO>& modes, UINT32 apply_flags = 0U) {
    const auto path_count = static_cast<UINT32>(paths.size());
    const auto mode_count = static_cast<UINT32>(modes.size());
    constexpr UINT32 flags = SDC_USE_SUPPLIED_DISPLAY_CONFIG | SDC_VIRTUAL_MODE_AWARE;
    const LONG validation = SetDisplayConfig(path_count, paths.data(), mode_count, modes.data(), flags | SDC_VALIDATE);
    if (validation != ERROR_SUCCESS) { return validation; }
    return SetDisplayConfig(path_count, paths.data(), mode_count, modes.data(), flags | SDC_APPLY | apply_flags);
}

struct ExistingVddDesktop {
    std::wstring monitor_path;
    DISPLAYCONFIG_SOURCE_MODE mode;
    bool shared_with_panel{};
};

[[nodiscard]] std::optional<ExistingVddDesktop> existing_vdd_desktop() {
    std::vector<DISPLAYCONFIG_PATH_INFO> paths;
    std::vector<DISPLAYCONFIG_MODE_INFO> modes;
    if (read_active_display_config(QDC_ONLY_ACTIVE_PATHS | QDC_VIRTUAL_MODE_AWARE,
            paths, modes) != ERROR_SUCCESS || paths.size() != 2U) {
        return std::nullopt;
    }
    const auto& panel = is_internal_panel(paths.front()) ? paths.front() : paths.back();
    const auto& other = &panel == &paths.front() ? paths.back() : paths.front();
    const auto target = display_target_name(other);
    const auto* primary = source_mode(panel, modes);
    const auto* existing = source_mode(other, modes);
    if (!is_internal_panel(panel) || !contains_parsec_marker(target.monitorDevicePath) ||
        primary == nullptr || existing == nullptr) {
        return std::nullopt;
    }
    return ExistingVddDesktop{target.monitorDevicePath, *existing, same_position(*primary, *existing)};
}

struct ActiveMonitor {
    std::wstring path;
    WindowsDisplaySource source;
    bool parsec{};
};

[[nodiscard]] std::vector<ActiveMonitor> active_monitors(UINT32 flags) {
    std::vector<DISPLAYCONFIG_PATH_INFO> paths;
    std::vector<DISPLAYCONFIG_MODE_INFO> modes;
    if (read_active_display_config(QDC_ONLY_ACTIVE_PATHS | flags, paths, modes) != ERROR_SUCCESS) {
        return {};
    }
    std::vector<ActiveMonitor> result;
    for (const auto& path : paths) {
        const auto target = display_target_name(path);
        if (target.monitorDevicePath[0] == L'\0') { continue; }
        const bool shared = std::any_of(paths.begin(), paths.end(), [&path](const auto& other) {
            return &path != &other && same_source(path, other);
        });
        result.push_back({target.monitorDevicePath, {display_source_name(path), shared, paths.size() == 1U},
            contains_parsec_marker(target.monitorFriendlyDeviceName) ||
            contains_parsec_marker(target.monitorDevicePath)});
    }
    return result;
}

[[nodiscard]] CloneTopology inspect_clone_topology() {
    std::vector<DISPLAYCONFIG_PATH_INFO> paths;
    std::vector<DISPLAYCONFIG_MODE_INFO> modes;
    const LONG result = read_active_display_config(QDC_ONLY_ACTIVE_PATHS, paths, modes);
    if (result != ERROR_SUCCESS) {
        return {.diagnostic = "Active display configuration query failed: " + std::to_string(result)};
    }

    bool has_clone = false;
    bool parsec_in_clone = false;
    std::string details;
    for (std::size_t index = 0U; index < paths.size(); ++index) {
        const auto& path = paths[index];
        const std::wstring target = target_description(path);
        const bool target_is_parsec = contains_parsec_marker(target);
        for (std::size_t other = index + 1U; other < paths.size(); ++other) {
            if (!same_source(path, paths[other])) {
                continue;
            }
            has_clone = true;
            const std::wstring other_target = target_description(paths[other]);
            if (target_is_parsec || contains_parsec_marker(other_target)) {
                parsec_in_clone = true;
            }
        }
        if (!details.empty()) {
            details += "; ";
        }
        details += "source=" + std::to_string(path.sourceInfo.adapterId.HighPart) + ":" +
            std::to_string(path.sourceInfo.adapterId.LowPart) + "/" +
            std::to_string(path.sourceInfo.id) + ", target=" +
            std::to_string(path.targetInfo.id) + ", name=" + utf8(target);
    }

    // On some NVIDIA laptop stacks the Parsec identity is visible only as a
    // monitor child of DISPLAY1 while the clone is active. It is safe evidence
    // here because this inspection runs only after this session's ADD call.
    bool any_parsec_monitor = false;
    for (DWORD index = 0U;; ++index) {
        DISPLAY_DEVICEW display{};
        display.cb = sizeof(display);
        if (!EnumDisplayDevicesW(nullptr, index, &display, 0U)) {
            break;
        }
        if (display_mentions_parsec(display)) {
            any_parsec_monitor = true;
        }
        for (DWORD child = 0U;; ++child) {
            DISPLAY_DEVICEW monitor{};
            monitor.cb = sizeof(monitor);
            if (!EnumDisplayDevicesW(display.DeviceName, child, &monitor, 0U)) {
                break;
            }
            if (display_mentions_parsec(monitor)) {
                any_parsec_monitor = true;
            }
        }
    }
    if (has_clone && any_parsec_monitor) {
        parsec_in_clone = true;
    }
    return {
        .parsec_clone = has_clone && parsec_in_clone,
        // Do not change an owner's multi-monitor layout. The automatic repair
        // is only for the unambiguous primary + newly-created VDD pair.
        .safe_to_extend = has_clone && parsec_in_clone && paths.size() == 2U,
        .diagnostic = "active paths=" + std::to_string(paths.size()) +
            ", clone=" + (has_clone ? "yes" : "no") +
            ", parsec-clone=" + (parsec_in_clone ? "yes" : "no") + "; " + details};
}

[[nodiscard]] std::string request_windows_extend() {
    wchar_t system_directory[MAX_PATH]{};
    const UINT length = GetSystemDirectoryW(system_directory, MAX_PATH);
    if (length == 0U || length >= MAX_PATH) {
        return "could not locate Windows System32 for DisplaySwitch.exe";
    }
    const std::wstring executable = std::wstring(system_directory) + L"\\DisplaySwitch.exe";
    std::wstring command_line = L"\"" + executable + L"\" /extend";
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(
            executable.c_str(), command_line.data(), nullptr, nullptr, FALSE,
            CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process)) {
        return win32_error("CreateProcess(DisplaySwitch.exe /extend)");
    }
    CloseHandle(process.hThread);
    const DWORD wait = WaitForSingleObject(process.hProcess, 15'000U);
    DWORD exit_code = STILL_ACTIVE;
    if (wait == WAIT_OBJECT_0) {
        (void)GetExitCodeProcess(process.hProcess, &exit_code);
    }
    CloseHandle(process.hProcess);
    if (wait != WAIT_OBJECT_0 || exit_code != 0U) {
        return "DisplaySwitch.exe /extend did not complete successfully (wait=" +
            std::to_string(wait) + ", exit=" + std::to_string(exit_code) + ")";
    }
    return {};
}

}  // namespace

class ParsecVddSession::Impl {
public:
    std::thread worker;
    mutable std::mutex mutex;
    std::condition_variable wake;
    std::optional<ExistingVddDesktop> previous_desktop;
};

ParsecVddProbe probe_parsec_vdd() {
    HANDLE device = open_parsec_vdd_handle();
    if (device == INVALID_HANDLE_VALUE || device == nullptr) {
        return {.available = false, .detail = "Parsec VDD is not installed or is inaccessible"};
    }
    try {
        const DWORD version = checked_vdd_ioctl(device, kVddIoctlVersion);
        CloseHandle(device);
        if (version == kInvalidVddResult) {
            return {.available = false, .detail = "Parsec VDD did not return a valid version"};
        }
        return {.available = true, .version = version, .detail = "Parsec VDD is ready"};
    } catch (const std::exception& error) {
        CloseHandle(device);
        return {.available = false, .detail = error.what()};
    }
}

std::vector<std::string> enumerate_active_parsec_vdd_displays() {
    std::vector<std::string> result;
    for (const AttachedDisplay& display : attached_display_devices()) {
        if (display.parsec && !display.name.empty()) {
            result.push_back(display.name);
        }
    }
    return result;
}

bool has_single_internal_display() {
    std::vector<DISPLAYCONFIG_PATH_INFO> paths;
    std::vector<DISPLAYCONFIG_MODE_INFO> modes;
    return read_active_display_config(QDC_ONLY_ACTIVE_PATHS, paths, modes) == ERROR_SUCCESS &&
        paths.size() == 1U && is_internal_panel(paths.front());
}

std::wstring monitor_path_for_display(std::wstring_view device_name) {
    std::wstring result;
    for (const auto& monitor : active_monitors(QDC_VIRTUAL_MODE_AWARE)) {
        if (monitor.source.device_name != device_name) { continue; }
        if (!result.empty()) { return {}; }
        result = monitor.path;
    }
    return result;
}

WindowsDisplaySource desktop_source_for_monitor(std::wstring_view monitor_path) {
    // The non-virtual-aware query resolves the shared desktop's actual GDI
    // source, including targets that have no separately attached DXGI output.
    for (const auto& monitor : active_monitors(0U)) {
        if (monitor.path == monitor_path) { return monitor.source; }
    }
    return {};
}

bool clone_internal_display_to_owned_vdd(const std::wstring& owned_device) {
    std::vector<DISPLAYCONFIG_PATH_INFO> paths;
    std::vector<DISPLAYCONFIG_MODE_INFO> modes;
    if (read_active_display_config(QDC_ONLY_ACTIVE_PATHS | QDC_VIRTUAL_MODE_AWARE,
            paths, modes) != ERROR_SUCCESS || paths.size() != 2U) {
        return false;
    }
    const auto& panel = is_internal_panel(paths.front()) ? paths.front() : paths.back();
    auto& owned = &panel == &paths.front() ? paths.back() : paths.front();
    if (!is_internal_panel(panel) || is_internal_panel(owned) ||
        (panel.flags & owned.flags & DISPLAYCONFIG_PATH_SUPPORT_VIRTUAL_MODE) == 0U) {
        return false;
    }
    if (owned_device != display_source_name(owned)) {
        return false;
    }
    const UINT32 panel_mode = panel.sourceInfo.sourceModeInfoIdx;
    const UINT32 owned_mode = owned.sourceInfo.sourceModeInfoIdx;
    if (panel_mode >= modes.size() || owned_mode >= modes.size() || panel_mode == owned_mode ||
        modes[panel_mode].infoType != DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE ||
        modes[owned_mode].infoType != DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE) {
        return false;
    }
    const auto& primary = modes[panel_mode].sourceMode;
    auto& secondary = modes[owned_mode].sourceMode;
    if (primary.position.x != 0 || primary.position.y != 0 ||
        primary.width != secondary.width || primary.height != secondary.height) {
        return false;
    }

    // Virtual clone paths share a desktop position. Keep the panel's mode and
    // the current target timings, and let Windows rebuild only the VDD's desktop image.
    secondary = primary;
    owned.targetInfo.desktopModeInfoIdx = DISPLAYCONFIG_PATH_DESKTOP_IMAGE_IDX_INVALID;
    // Persist this exact pair so Windows restores the shared desktop when the
    // panel comes back after a lid close. Removing our VDD ends the pairing.
    return apply_virtual_display_config(paths, modes, SDC_SAVE_TO_DATABASE) == ERROR_SUCCESS;
}

std::optional<long> restore_lid_display_size(std::wstring_view monitor_path, LidDisplaySize& open_size) {
    // Other device windows can be repairing a different VDD at the same time.
    // Keep the snapshot and its application together so their modes are preserved.
    const LidDisplayLayoutLock layout_lock;
    if (!layout_lock.acquired()) { return std::nullopt; }

    std::vector<DISPLAYCONFIG_PATH_INFO> paths;
    std::vector<DISPLAYCONFIG_MODE_INFO> modes;
    if (read_active_display_config(QDC_ONLY_ACTIVE_PATHS | QDC_VIRTUAL_MODE_AWARE,
            paths, modes) != ERROR_SUCCESS) {
        return std::nullopt;
    }

    const auto owned = std::find_if(paths.begin(), paths.end(), [&](const auto& path) {
        return (path.flags & DISPLAYCONFIG_PATH_SUPPORT_VIRTUAL_MODE) != 0U &&
            monitor_path == display_target_name(path).monitorDevicePath;
    });
    if (owned == paths.end()) { return std::nullopt; }

    auto* current = source_mode(*owned, modes);
    if (current == nullptr) { return std::nullopt; }
    const auto shares_desktop = [&](const auto& path) {
        const auto* source = source_mode(path, modes);
        return same_source(*owned, path) || (source != nullptr && same_position(*source, *current));
    };
    std::vector<std::wstring> topology;
    for (const auto& path : paths) { topology.emplace_back(display_target_name(path).monitorDevicePath); }
    std::sort(topology.begin(), topology.end());
    const bool changed = !open_size.monitor_paths.empty() && topology != open_size.monitor_paths;
    const auto saved = std::exchange(open_size, LidDisplaySize{current->width, current->height,
        std::move(topology), static_cast<std::uint32_t>(owned->targetInfo.rotation)});
    if (!changed || saved.width == 0U ||
        (saved.width == current->width && saved.height == current->height &&
            saved.rotation == static_cast<std::uint32_t>(owned->targetInfo.rotation)) ||
        std::any_of(paths.begin(), paths.end(), [&](const auto& path) {
            return &path != &*owned && shares_desktop(path);
        })) {
        return std::nullopt;
    }

    const LONG old_right = current->position.x + static_cast<LONG>(current->width);
    const LONG old_bottom = current->position.y + static_cast<LONG>(current->height);
    current->width = saved.width;
    current->height = saved.height;
    owned->targetInfo.rotation = static_cast<DISPLAYCONFIG_ROTATION>(saved.rotation);
    owned->targetInfo.targetModeInfoIdx = DISPLAYCONFIG_PATH_TARGET_MODE_IDX_INVALID;
    owned->targetInfo.desktopModeInfoIdx = DISPLAYCONFIG_PATH_DESKTOP_IMAGE_IDX_INVALID;
    // Windows can place a newly added screen beside the temporary wrong size.
    // Keep that new screen adjacent after restoring the existing screen.
    for (auto& path : paths) {
        if (&path == &*owned || std::find(saved.monitor_paths.begin(), saved.monitor_paths.end(),
                display_target_name(path).monitorDevicePath) != saved.monitor_paths.end()) { continue; }
        auto* added = source_mode(path, modes);
        if (added == nullptr || added == current ||
            std::any_of(paths.begin(), paths.end(), [&](const auto& other) {
                const auto* mode = source_mode(other, modes);
                return &other != &path && mode != nullptr && same_position(*mode, *added);
            })) { continue; }
        if (added->position.x == old_right) {
            added->position.x = current->position.x + static_cast<LONG>(current->width);
        }
        if (added->position.y == old_bottom) {
            added->position.y = current->position.y + static_cast<LONG>(current->height);
        }
        path.targetInfo.desktopModeInfoIdx = DISPLAYCONFIG_PATH_DESKTOP_IMAGE_IDX_INVALID;
    }
    const LONG result = apply_virtual_display_config(paths, modes);
    if (result == ERROR_SUCCESS) {
        open_size.width = saved.width;
        open_size.height = saved.height;
        open_size.rotation = saved.rotation;
    }
    return result;
}

std::optional<long> ParsecVddSession::restore_previous_vdd_desktop(std::wstring_view added_display) const {
    if (impl_ == nullptr || !impl_->previous_desktop) { return std::nullopt; }
    const LidDisplayLayoutLock layout_lock(1'000U);
    if (!layout_lock.acquired()) { return ERROR_BUSY; }
    const auto& previous = *impl_->previous_desktop;
    std::vector<DISPLAYCONFIG_PATH_INFO> paths;
    std::vector<DISPLAYCONFIG_MODE_INFO> modes;
    if (read_active_display_config(QDC_ONLY_ACTIVE_PATHS | QDC_VIRTUAL_MODE_AWARE,
            paths, modes) != ERROR_SUCCESS || paths.size() != 3U) {
        return std::nullopt;
    }
    const auto panel = std::find_if(paths.begin(), paths.end(), is_internal_panel);
    const auto existing = std::find_if(paths.begin(), paths.end(), [&](const auto& path) {
        return previous.monitor_path == display_target_name(path).monitorDevicePath;
    });
    if (panel == paths.end() || existing == paths.end() ||
        (existing->flags & DISPLAYCONFIG_PATH_SUPPORT_VIRTUAL_MODE) == 0U) { return std::nullopt; }
    const auto added = std::find_if(paths.begin(), paths.end(), [&](const auto& path) {
        return &path != &*panel && &path != &*existing && added_display == display_source_name(path);
    });
    if (added == paths.end()) { return std::nullopt; }
    const auto* panel_mode = source_mode(*panel, modes);
    auto* current = source_mode(*existing, modes);
    if (current == nullptr || panel_mode == nullptr || current == panel_mode ||
        current == source_mode(*added, modes) ||
        same_position(*current, *panel_mode) == previous.shared_with_panel) { return std::nullopt; }

    const auto& restored = previous.shared_with_panel ? *panel_mode : previous.mode;
    if (current->width != restored.width || current->height != restored.height) {
        existing->targetInfo.targetModeInfoIdx = DISPLAYCONFIG_PATH_TARGET_MODE_IDX_INVALID;
    }
    *current = restored;
    existing->targetInfo.desktopModeInfoIdx = DISPLAYCONFIG_PATH_DESKTOP_IMAGE_IDX_INVALID;
    return apply_virtual_display_config(paths, modes, SDC_SAVE_TO_DATABASE);
}

void remove_parsec_vdd_display(int display_index) {
    if (display_index < 0 || display_index > 15) {
        throw std::invalid_argument("Parsec VDD display index must be between 0 and 15");
    }
    HANDLE device = open_parsec_vdd_handle();
    if (device == INVALID_HANDLE_VALUE || device == nullptr) {
        throw std::runtime_error("Parsec VDD is not installed or is inaccessible");
    }
    try {
        const std::uint16_t index = static_cast<std::uint16_t>(display_index);
        const std::uint16_t big_endian_index = static_cast<std::uint16_t>(
            ((index & 0x00FFU) << 8U) | ((index & 0xFF00U) >> 8U));
        (void)checked_vdd_ioctl(
            device, kVddIoctlRemove, &big_endian_index, sizeof(big_endian_index));
        (void)checked_vdd_ioctl(device, kVddIoctlUpdate);
        CloseHandle(device);
    } catch (...) {
        CloseHandle(device);
        throw;
    }
}

ParsecVddSession::~ParsecVddSession() {
    stop();
}

void ParsecVddSession::start(bool allow_additional) {
    if (running()) {
        throw std::runtime_error("Parsec VDD session is already running");
    }
    const ParsecVddProbe probe = probe_parsec_vdd();
    if (!probe.available) {
        throw std::runtime_error(probe.detail);
    }
    HANDLE device = open_parsec_vdd_handle();
    if (device == INVALID_HANDLE_VALUE || device == nullptr) {
        throw std::runtime_error("Parsec VDD became unavailable while opening its control interface");
    }
    int created_index = -1;
    try {
        const std::vector<AttachedDisplay> displays_before_start = attached_display_devices();
        const bool existing_parsec = std::any_of(
            displays_before_start.begin(), displays_before_start.end(),
            [](const AttachedDisplay& display) { return display.parsec; });
        if (existing_parsec && !allow_additional) {
            throw std::runtime_error(
                "an attached Parsec VDD display already exists; select and reuse it instead");
        }
        displays_before_start_.clear();
        displays_before_start_.reserve(displays_before_start.size());
        for (const AttachedDisplay& display : displays_before_start) {
            displays_before_start_.push_back(display.identity);
        }
        monitor_paths_before_start_.clear();
        for (const auto& monitor : active_monitors(QDC_VIRTUAL_MODE_AWARE)) {
            monitor_paths_before_start_.push_back(monitor.path);
        }
        auto previous_desktop = allow_additional ? existing_vdd_desktop() : std::nullopt;
        const DWORD index = checked_vdd_ioctl(device, kVddIoctlAdd);
        if (index == kInvalidVddResult || index > 15U) {
            throw std::runtime_error("Parsec VDD returned an invalid virtual display index");
        }
        created_index = static_cast<int>(index);
        auto pending_impl = std::make_unique<Impl>();
        pending_impl->previous_desktop = std::move(previous_desktop);
        device_handle_ = device;
        display_index_ = created_index;
        stop_requested_.store(false);
        worker_failed_.store(false);
        last_error_.clear();
        creation_diagnostic_.clear();
        // Publish the synchronization object before the worker can observe
        // the device handle.
        impl_ = pending_impl.get();
        pending_impl->worker = std::thread(&ParsecVddSession::keep_alive_loop, this);
        impl_ = pending_impl.release();
    } catch (...) {
        if (created_index >= 0) {
            const std::uint16_t index = static_cast<std::uint16_t>(created_index);
            const std::uint16_t big_endian_index = static_cast<std::uint16_t>(
                ((index & 0x00FFU) << 8U) | ((index & 0xFF00U) >> 8U));
            DWORD ignored = 0U;
            if (!vdd_ioctl(device, kVddIoctlRemove, &big_endian_index, sizeof(big_endian_index), &ignored)) {
                last_error_ = win32_error("DeviceIoControl(Parsec VDD owned-display removal)");
            }
            if (!vdd_ioctl(device, kVddIoctlUpdate, nullptr, 0U, &ignored) && last_error_.empty()) {
                last_error_ = win32_error("DeviceIoControl(Parsec VDD update after removal)");
            }
        }
        CloseHandle(device);
        device_handle_ = nullptr;
        display_index_ = -1;
        impl_ = nullptr;
        throw;
    }
}

void ParsecVddSession::stop() noexcept {
    if (impl_ != nullptr) {
        {
            std::lock_guard lock(impl_->mutex);
            stop_requested_.store(true);
        }
        impl_->wake.notify_all();
        if (impl_->worker.joinable()) {
            impl_->worker.join();
        }
        delete impl_;
        impl_ = nullptr;
    }
    const HANDLE device = static_cast<HANDLE>(device_handle_);
    if (device != nullptr && device != INVALID_HANDLE_VALUE) {
        if (display_index_ >= 0) {
            const std::uint16_t index = static_cast<std::uint16_t>(display_index_);
            const std::uint16_t big_endian_index = static_cast<std::uint16_t>(
                ((index & 0x00FFU) << 8U) | ((index & 0xFF00U) >> 8U));
            DWORD ignored = 0U;
            if (!vdd_ioctl(device, kVddIoctlRemove, &big_endian_index, sizeof(big_endian_index), &ignored)) {
                last_error_ = win32_error("DeviceIoControl(Parsec VDD owned-display removal)");
            }
            if (!vdd_ioctl(device, kVddIoctlUpdate, nullptr, 0U, &ignored) && last_error_.empty()) {
                last_error_ = win32_error("DeviceIoControl(Parsec VDD update after removal)");
            }
        }
        CloseHandle(device);
    }
    device_handle_ = nullptr;
    display_index_ = -1;
    stop_requested_.store(false);
}

bool ParsecVddSession::running() const noexcept {
    return device_handle_ != nullptr && display_index_ >= 0 && !worker_failed_.load();
}

int ParsecVddSession::display_index() const noexcept {
    return display_index_;
}

std::string ParsecVddSession::last_error() const {
    if (impl_ == nullptr) {
        return last_error_;
    }
    std::lock_guard lock(impl_->mutex);
    return last_error_;
}

std::string ParsecVddSession::creation_diagnostic() const {
    if (impl_ == nullptr) {
        return creation_diagnostic_;
    }
    std::lock_guard lock(impl_->mutex);
    return creation_diagnostic_;
}

std::string ParsecVddSession::wait_for_display(std::chrono::milliseconds timeout) const {
    if (!running()) {
        throw std::runtime_error("Parsec VDD session is not running");
    }
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    const auto fallback_extend_at =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(1'500);
    std::vector<AttachedDisplay> displays_after_start;
    bool extend_requested = false;
    do {
        if (!running()) { throw std::runtime_error(last_error()); }
        displays_after_start = attached_display_devices();
        std::vector<const AttachedDisplay*> new_displays;
        for (const AttachedDisplay& display : displays_after_start) {
            if (!was_present(display, displays_before_start_)) {
                new_displays.push_back(&display);
            }
        }
        const auto known_parsec = std::find_if(
            new_displays.begin(), new_displays.end(),
            [](const AttachedDisplay* display) { return display->parsec; });
        const auto monitors = active_monitors(QDC_VIRTUAL_MODE_AWARE);
        std::vector<std::string> new_monitor_sources;
        std::vector<std::string> new_parsec_sources;
        if (!monitor_paths_before_start_.empty() && !monitors.empty()) {
            for (const auto& monitor : monitors) {
                if (std::find(monitor_paths_before_start_.begin(),
                        monitor_paths_before_start_.end(), monitor.path) != monitor_paths_before_start_.end()) {
                    continue;
                }
                const std::string name = utf8(monitor.source.device_name);
                if (std::any_of(new_displays.begin(), new_displays.end(),
                        [&name](const auto* display) { return display->name == name; })) {
                    new_monitor_sources.push_back(name);
                    if (monitor.parsec) { new_parsec_sources.push_back(name); }
                }
            }
        }
        if (new_parsec_sources.size() == 1U) {
            return new_parsec_sources.front();
        }
        if (new_monitor_sources.size() == 1U) {
            return new_monitor_sources.front();
        }
        if (monitor_paths_before_start_.empty() && known_parsec != new_displays.end()) {
            return (*known_parsec)->name;
        }
        // Some hybrid-GPU Windows 11 systems create a real new attached output
        // but omit Parsec's marker from EnumDisplayDevices. There is no
        // ambiguity when exactly one display appeared after our own ADD call.
        if (monitor_paths_before_start_.empty() && new_displays.size() == 1U) {
            return new_displays.front()->name;
        }
        if (!extend_requested && (monitor_paths_before_start_.size() <= 1U ||
                std::chrono::steady_clock::now() >= fallback_extend_at)) {
            const CloneTopology topology = inspect_clone_topology();
            if (topology.parsec_clone) {
                extend_requested = true;
                if (!topology.safe_to_extend) {
                    creation_diagnostic_ =
                        "Detected a Parsec VDD clone topology but did not alter Windows "
                        "because more than two active display paths exist; " + topology.diagnostic;
                } else {
                    const std::string extend_error = request_windows_extend();
                    creation_diagnostic_ = extend_error.empty()
                        ? "Detected the newly created Parsec VDD cloned with the primary display; "
                          "requested Windows DisplaySwitch.exe /extend; " + topology.diagnostic
                        : "Detected the newly created Parsec VDD clone topology, but the request "
                          "to Windows DisplaySwitch.exe /extend failed: " + extend_error + "; " +
                            topology.diagnostic;
                }
            } else if (displays_before_start_.size() == 1U &&
                       std::chrono::steady_clock::now() >= fallback_extend_at) {
                // Some GPU/driver combinations hide the new VDD completely
                // while Windows still has the topology in duplicate/internal
                // mode: it is absent from both EnumDisplayDevices and the
                // active DisplayConfig paths, so there is no Parsec marker to
                // identify. The user requested a new secondary display and
                // had only one attached output beforehand, making /extend an
                // unambiguous and safe repair. Continue polling afterwards;
                // Windows should then publish the VDD as a distinct DISPLAY.
                extend_requested = true;
                const std::string extend_error = request_windows_extend();
                creation_diagnostic_ = extend_error.empty()
                    ? "No new attached display was visible after Parsec VDD ADD with one "
                      "pre-existing display; requested Windows DisplaySwitch.exe /extend as "
                      "a hidden-clone topology repair; " + topology.diagnostic
                    : "No new attached display was visible after Parsec VDD ADD with one "
                      "pre-existing display, but Windows DisplaySwitch.exe /extend failed: " +
                        extend_error + "; " + topology.diagnostic;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    } while (std::chrono::steady_clock::now() < deadline);
    throw std::runtime_error(
        "Parsec VDD created a display but WireView did not find a uniquely new attached "
        "Windows display before the timeout; driver index " + std::to_string(display_index_) +
        "; attached displays after request: " + display_summary(displays_after_start) +
        (creation_diagnostic_.empty() ? "" : "; topology diagnostic: " + creation_diagnostic_));
}

void ParsecVddSession::keep_alive_loop() {
    const HANDLE device = static_cast<HANDLE>(device_handle_);
    for (;;) {
        DWORD ignored = 0U;
        if (!vdd_ioctl(device, kVddIoctlUpdate, nullptr, 0U, &ignored)) {
            std::lock_guard lock(impl_->mutex);
            worker_failed_.store(true);
            last_error_ = win32_error("DeviceIoControl(Parsec VDD keep-alive)");
            return;
        }
        std::unique_lock lock(impl_->mutex);
        if (impl_->wake.wait_for(lock, std::chrono::milliseconds(100), [this] {
                return stop_requested_.load();
            })) {
            return;
        }
    }
}

}  // namespace wireview::host
