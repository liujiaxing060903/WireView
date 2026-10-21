#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include "parsec_vdd_custom_modes.hpp"

#include <stdexcept>
#include <string>

namespace wireview::host {
namespace {

[[noreturn]] void fail(const char* operation) {
    throw std::runtime_error(std::string(operation) + " failed: " +
        std::to_string(GetLastError()));
}

}  // namespace

void apply_parsec_vdd_custom_modes(
    const std::array<std::optional<ParsecVddCustomMode>, 5U>& modes) {
    HKEY root{};
    const LONG opened = RegCreateKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Parsec\\vdd", 0U,
        nullptr, REG_OPTION_NON_VOLATILE, KEY_READ | KEY_WRITE, nullptr, &root, nullptr);
    if (opened != ERROR_SUCCESS) {
        SetLastError(static_cast<DWORD>(opened));
        fail("RegCreateKeyExW(HKLM\\SOFTWARE\\Parsec\\vdd)");
    }
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
        const auto write = [slot](const wchar_t* value, std::uint32_t data) {
            const LONG result = RegSetValueExW(slot, value, 0U, REG_DWORD,
                reinterpret_cast<const BYTE*>(&data), sizeof(data));
            if (result != ERROR_SUCCESS) {
                SetLastError(static_cast<DWORD>(result));
                fail("RegSetValueExW(Parsec VDD custom mode)");
            }
        };
        write(L"width", modes[index]->width);
        write(L"height", modes[index]->height);
        write(L"hz", modes[index]->refresh_hz);
        RegCloseKey(slot);
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
}

}  // namespace wireview::host
