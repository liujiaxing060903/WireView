#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include "parsec_vdd_custom_modes.hpp"
#include "display_layout_lock.hpp"

#include <algorithm>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace wireview::host {
namespace {

[[noreturn]] void fail(const char* operation) {
    throw std::runtime_error(std::string(operation) + " failed: " +
        std::to_string(GetLastError()));
}

HKEY open_custom_modes_for_write() {
    HKEY root{};
    const LONG opened = RegCreateKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Parsec\\vdd", 0U,
        nullptr, REG_OPTION_NON_VOLATILE, KEY_READ | KEY_WRITE, nullptr, &root, nullptr);
    if (opened != ERROR_SUCCESS) {
        SetLastError(static_cast<DWORD>(opened));
        fail("RegCreateKeyExW(HKLM\\SOFTWARE\\Parsec\\vdd)");
    }
    return root;
}

LONG write_custom_mode(HKEY slot, const ParsecVddCustomMode& mode) {
    const std::array values{std::pair{L"width", mode.width},
        std::pair{L"height", mode.height}, std::pair{L"hz", mode.refresh_hz}};
    for (const auto& [name, data] : values) {
        const LONG written = RegSetValueExW(slot, name, 0U, REG_DWORD,
            reinterpret_cast<const BYTE*>(&data), sizeof(data));
        if (written != ERROR_SUCCESS) { return written; }
    }
    return ERROR_SUCCESS;
}

class CustomModeLock {
public:
    CustomModeLock() : handle_(CreateMutexW(nullptr, FALSE, L"Local\\WireView.VddModeUpdate")) {
        if (handle_ == nullptr) { fail("CreateMutexW(VDD modes)"); }
        const DWORD result = WaitForSingleObject(handle_, 30'000U);
        if (result != WAIT_OBJECT_0 && result != WAIT_ABANDONED) {
            CloseHandle(handle_);
            throw std::runtime_error("another display mode update did not finish");
        }
    }
    ~CustomModeLock() { ReleaseMutex(handle_); CloseHandle(handle_); }
    CustomModeLock(const CustomModeLock&) = delete;
    CustomModeLock& operator=(const CustomModeLock&) = delete;
private:
    HANDLE handle_{};
};

void back_up_custom_slot(HKEY root, const std::wstring& name,
    const std::optional<ParsecVddCustomMode>& previous) {
    HKEY original{};
    const LONG opened = RegOpenKeyExW(root, name.c_str(), 0U, KEY_READ, &original);
    if (opened == ERROR_FILE_NOT_FOUND) { return; }
    if (opened != ERROR_SUCCESS) {
        SetLastError(static_cast<DWORD>(opened));
        fail("read previous Parsec VDD mode for backup");
    }
    RegCloseKey(original);
    if (!previous) { throw std::runtime_error("previous custom mode is unreadable; kept unchanged"); }
    FILETIME now{};
    GetSystemTimeAsFileTime(&now);
    const auto stamp = (static_cast<std::uint64_t>(now.dwHighDateTime) << 32U) | now.dwLowDateTime;
    const std::wstring path = L"SOFTWARE\\WireView\\ParsecVddModeBackups\\" +
        name + L"-" + std::to_wstring(stamp);
    HKEY backup{};
    LONG result = RegCreateKeyExW(HKEY_LOCAL_MACHINE, path.c_str(), 0U, nullptr,
        REG_OPTION_NON_VOLATILE, KEY_WRITE, nullptr, &backup, nullptr);
    if (result == ERROR_SUCCESS) {
        result = write_custom_mode(backup, *previous);
        RegCloseKey(backup);
    }
    if (result != ERROR_SUCCESS) {
        SetLastError(static_cast<DWORD>(result));
        fail("back up previous Parsec VDD mode");
    }
}

std::vector<ParsecVddCustomMode> active_custom_modes(
    const std::array<std::optional<ParsecVddCustomMode>, 5U>& modes) {
    std::vector<ParsecVddCustomMode> active;
    for (DWORD index = 0;; ++index) {
        DISPLAY_DEVICEW display{};
        display.cb = sizeof(display);
        if (!EnumDisplayDevicesW(nullptr, index, &display, 0U)) { break; }
        if ((display.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP) == 0U) { continue; }
        DEVMODEW current{};
        current.dmSize = sizeof(current);
        if (!EnumDisplaySettingsW(display.DeviceName, ENUM_CURRENT_SETTINGS, &current)) {
            throw std::runtime_error("cannot check an active display before updating its modes");
        }
        active.push_back({current.dmPelsWidth, current.dmPelsHeight, current.dmDisplayFrequency});
    }
    for (const auto& mode : modes) {
        if (!mode) { continue; }
        const HANDLE held = OpenEventW(SYNCHRONIZE, FALSE, parsec_vdd_mode_reservation_name(*mode).c_str());
        if (held != nullptr || GetLastError() == ERROR_ACCESS_DENIED) { active.push_back(*mode); }
        if (held != nullptr) { CloseHandle(held); }
    }
    return active;
}

}  // namespace

void apply_parsec_vdd_custom_modes(
    const std::array<std::optional<ParsecVddCustomMode>, 5U>& modes) {
    const HKEY root = open_custom_modes_for_write();
    for (std::size_t index = 0U; index < modes.size(); ++index) {
        const std::wstring name = std::to_wstring(index);
        if (!modes[index]) {
            const LONG deleted = RegDeleteTreeW(root, name.c_str());
            if (deleted != ERROR_SUCCESS && deleted != ERROR_FILE_NOT_FOUND) {
                RegCloseKey(root);
                SetLastError(static_cast<DWORD>(deleted));
                fail("RegDeleteTreeW(Parsec VDD custom slot)");
            }
            continue;
        }
        HKEY slot{};
        const LONG created = RegCreateKeyExW(root, name.c_str(), 0U, nullptr,
            REG_OPTION_NON_VOLATILE, KEY_SET_VALUE, nullptr, &slot, nullptr);
        if (created != ERROR_SUCCESS) {
            RegCloseKey(root);
            SetLastError(static_cast<DWORD>(created));
            fail("RegCreateKeyExW(Parsec VDD custom slot)");
        }
        const LONG written = write_custom_mode(slot, *modes[index]);
        RegCloseKey(slot);
        if (written != ERROR_SUCCESS) {
            RegCloseKey(root);
            SetLastError(static_cast<DWORD>(written));
            fail("RegSetValueExW(Parsec VDD custom mode)");
        }
    }
    RegCloseKey(root);
}

void ensure_parsec_vdd_custom_mode(const ParsecVddCustomMode& mode) {
    const CustomModeLock update_lock;
    const auto modes = read_parsec_vdd_custom_modes().value_or(
        std::array<std::optional<ParsecVddCustomMode>, 5U>{});
    if (std::find(modes.begin(), modes.end(), mode) != modes.end()) { return; }
    const auto index = choose_parsec_vdd_custom_slot(modes, mode, active_custom_modes(modes));
    if (!index) { throw std::runtime_error("all custom modes are currently in use"); }
    const HKEY root = open_custom_modes_for_write();
    const std::wstring name = std::to_wstring(*index);
    try { back_up_custom_slot(root, name, modes[*index]); }
    catch (...) { RegCloseKey(root); throw; }
    HKEY slot{};
    DWORD disposition{};
    const LONG created = RegCreateKeyExW(root, name.c_str(), 0U, nullptr,
        REG_OPTION_NON_VOLATILE, KEY_SET_VALUE, nullptr, &slot, &disposition);
    if (created != ERROR_SUCCESS) {
        RegCloseKey(root);
        SetLastError(static_cast<DWORD>(created));
        fail("RegCreateKeyExW(Parsec VDD custom slot)");
    }
    const LONG written = write_custom_mode(slot, mode);
    if (written != ERROR_SUCCESS && modes[*index]) { (void)write_custom_mode(slot, *modes[*index]); }
    RegCloseKey(slot);
    if (written != ERROR_SUCCESS) {
        if (disposition == REG_CREATED_NEW_KEY) { RegDeleteKeyW(root, name.c_str()); }
        RegCloseKey(root);
        SetLastError(static_cast<DWORD>(written));
        fail("RegSetValueExW(Parsec VDD custom mode)");
    }
    RegCloseKey(root);
}

std::optional<std::array<std::optional<ParsecVddCustomMode>, 5U>> read_parsec_vdd_custom_modes() {
    HKEY root{};
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Parsec\\vdd", 0U, KEY_READ, &root) != ERROR_SUCCESS) {
        return std::nullopt;
    }
    std::array<std::optional<ParsecVddCustomMode>, 5U> modes;
    for (std::size_t index = 0U; index < modes.size(); ++index) {
        HKEY slot{};
        const std::wstring name = std::to_wstring(index);
        if (RegOpenKeyExW(root, name.c_str(), 0U, KEY_READ, &slot) != ERROR_SUCCESS) {
            continue;
        }
        const auto read_dword = [slot](const wchar_t* value) -> std::optional<std::uint32_t> {
            DWORD data{};
            DWORD type{};
            DWORD bytes = sizeof(data);
            return RegQueryValueExW(slot, value, nullptr, &type,
                       reinterpret_cast<BYTE*>(&data), &bytes) == ERROR_SUCCESS &&
                    type == REG_DWORD && bytes == sizeof(data)
                ? std::optional{static_cast<std::uint32_t>(data)}
                : std::nullopt;
        };
        const auto width = read_dword(L"width");
        const auto height = read_dword(L"height");
        const auto refresh = read_dword(L"hz");
        if (width && height && refresh && *width > 0U && *height > 0U && *refresh > 0U) {
            modes[index] = ParsecVddCustomMode{*width, *height, *refresh};
        }
        RegCloseKey(slot);
    }
    RegCloseKey(root);
    return modes;
}

void set_parsec_vdd_display_mode(
    const wchar_t* display_device,
    std::uint32_t width,
    std::uint32_t height,
    std::uint32_t refresh_hz) {
    if (display_device == nullptr || *display_device == L'\0') {
        throw std::invalid_argument("Parsec VDD display name is empty");
    }
    // A different device may restore its size by applying a full layout.
    // Serialize our mode change with that read/apply transaction, otherwise its
    // old snapshot can silently undo this newly created display's requested size.
    const LidDisplayLayoutLock layout_lock(5'000U);
    if (!layout_lock.acquired()) {
        throw std::runtime_error("another display layout update did not finish");
    }
    DEVMODEW mode{};
    mode.dmSize = sizeof(mode);
    mode.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_DISPLAYFREQUENCY;
    mode.dmPelsWidth = width;
    mode.dmPelsHeight = height;
    mode.dmDisplayFrequency = refresh_hz;
    const LONG result = ChangeDisplaySettingsExW(display_device, &mode, nullptr, CDS_UPDATEREGISTRY,
        nullptr);
    if (result != DISP_CHANGE_SUCCESSFUL) {
        throw std::runtime_error("ChangeDisplaySettingsExW rejected the selected Parsec VDD mode (" +
            std::to_string(result) + ")");
    }
    DEVMODEW actual{};
    actual.dmSize = sizeof(actual);
    if (!EnumDisplaySettingsW(display_device, ENUM_CURRENT_SETTINGS, &actual)) {
        fail("read back selected Parsec VDD desktop size");
    }
    if (actual.dmPelsWidth != width || actual.dmPelsHeight != height) {
        throw std::runtime_error("Parsec VDD desktop size differs from requested " +
            std::to_string(width) + "x" + std::to_string(height) + " (actual " +
            std::to_string(actual.dmPelsWidth) + "x" + std::to_string(actual.dmPelsHeight) + ")");
    }
}

}  // namespace wireview::host
