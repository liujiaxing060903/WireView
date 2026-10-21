#include "parsec_vdd.hpp"

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
    if (!completed && GetLastError() == ERROR_IO_PENDING) {
        const DWORD wait = WaitForSingleObject(event, 5'000U);
        completed = wait == WAIT_OBJECT_0 &&
            GetOverlappedResult(device, &overlapped, &bytes, FALSE) != FALSE;
    }
    CloseHandle(event);
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

[[nodiscard]] std::wstring target_description(const DISPLAYCONFIG_PATH_INFO& path) {
    DISPLAYCONFIG_TARGET_DEVICE_NAME target{};
    target.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
    target.header.size = sizeof(target);
    target.header.adapterId = path.targetInfo.adapterId;
    target.header.id = path.targetInfo.id;
    if (DisplayConfigGetDeviceInfo(&target.header) != ERROR_SUCCESS) {
        return {};
    }
    return std::wstring(target.monitorFriendlyDeviceName) + L" " + target.monitorDevicePath;
}

[[nodiscard]] CloneTopology inspect_clone_topology() {
    UINT32 path_count = 0U;
    UINT32 mode_count = 0U;
    LONG result = GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &path_count, &mode_count);
    if (result != ERROR_SUCCESS) {
        return {.diagnostic = "GetDisplayConfigBufferSizes failed: " + std::to_string(result)};
    }
    std::vector<DISPLAYCONFIG_PATH_INFO> paths(path_count);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(mode_count);
    result = QueryDisplayConfig(
        QDC_ONLY_ACTIVE_PATHS, &path_count, paths.data(), &mode_count, modes.data(), nullptr);
    if (result != ERROR_SUCCESS) {
        return {.diagnostic = "QueryDisplayConfig failed: " + std::to_string(result)};
    }
    paths.resize(path_count);

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

void ParsecVddSession::start() {
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
        if (existing_parsec) {
            throw std::runtime_error(
                "an attached Parsec VDD display already exists; select and reuse it instead");
        }
        displays_before_start_.clear();
        displays_before_start_.reserve(displays_before_start.size());
        for (const AttachedDisplay& display : displays_before_start) {
            displays_before_start_.push_back(display.identity);
        }
        const DWORD index = checked_vdd_ioctl(device, kVddIoctlAdd);
        if (index == kInvalidVddResult || index > 15U) {
            throw std::runtime_error("Parsec VDD returned an invalid virtual display index");
        }
        created_index = static_cast<int>(index);
        auto pending_impl = std::make_unique<Impl>();
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
            (void)vdd_ioctl(device, kVddIoctlRemove, &big_endian_index, sizeof(big_endian_index), &ignored);
            (void)vdd_ioctl(device, kVddIoctlUpdate, nullptr, 0U, &ignored);
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
            (void)vdd_ioctl(device, kVddIoctlRemove, &big_endian_index, sizeof(big_endian_index), &ignored);
            (void)vdd_ioctl(device, kVddIoctlUpdate, nullptr, 0U, &ignored);
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
        if (known_parsec != new_displays.end()) {
            return (*known_parsec)->name;
        }
        // Some hybrid-GPU Windows 11 systems create a real new attached output
        // but omit Parsec's marker from EnumDisplayDevices. There is no
        // ambiguity when exactly one display appeared after our own ADD call.
        if (new_displays.size() == 1U) {
            return new_displays.front()->name;
        }
        if (!extend_requested) {
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
