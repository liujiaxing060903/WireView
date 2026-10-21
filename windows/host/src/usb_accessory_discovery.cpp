#include "usb_accessory_discovery.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <setupapi.h>

#include <algorithm>
#include <cstdint>
#include <cwctype>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <vector>

namespace wireview::host {
namespace {

constexpr GUID kAndroidWinUsbInterfaceGuid{
    0xf72fe0d4,
    0xcbcb,
    0x407d,
    {0x88, 0x14, 0x9e, 0xd6, 0x73, 0xd0, 0xdd, 0x6b},
};

class DeviceInfoSet final {
public:
    explicit DeviceInfoSet(HDEVINFO value) noexcept : value_(value) {
    }

    ~DeviceInfoSet() {
        if (value_ != INVALID_HANDLE_VALUE) {
            SetupDiDestroyDeviceInfoList(value_);
        }
    }

    DeviceInfoSet(const DeviceInfoSet&) = delete;
    DeviceInfoSet& operator=(const DeviceInfoSet&) = delete;

    [[nodiscard]] HDEVINFO get() const noexcept {
        return value_;
    }

private:
    HDEVINFO value_{INVALID_HANDLE_VALUE};
};

[[noreturn]] void throw_windows_error(std::string_view operation) {
    throw std::runtime_error(
        std::string(operation) + " failed: Windows error " +
        std::to_string(GetLastError()));
}

std::wstring device_property(
    HDEVINFO devices,
    SP_DEVINFO_DATA& device,
    DWORD property) {
    DWORD required = 0;
    DWORD type = 0;
    if (SetupDiGetDeviceRegistryPropertyW(
            devices,
            &device,
            property,
            &type,
            nullptr,
            0,
            &required) != FALSE) {
        return {};
    }
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || required == 0) {
        return {};
    }

    std::vector<std::uint8_t> storage(required);
    if (SetupDiGetDeviceRegistryPropertyW(
            devices,
            &device,
            property,
            &type,
            storage.data(),
            required,
            nullptr) == FALSE) {
        return {};
    }
    return std::wstring(reinterpret_cast<const wchar_t*>(storage.data()));
}

std::wstring device_instance_id(HDEVINFO devices, SP_DEVINFO_DATA& device) {
    DWORD required = 0;
    SetupDiGetDeviceInstanceIdW(devices, &device, nullptr, 0, &required);
    if (required == 0) {
        return {};
    }
    std::vector<wchar_t> storage(required);
    if (SetupDiGetDeviceInstanceIdW(
            devices,
            &device,
            storage.data(),
            static_cast<DWORD>(storage.size()),
            nullptr) == FALSE) {
        return {};
    }
    return std::wstring(storage.data());
}

}  // namespace

bool is_aoa_accessory_instance(std::wstring_view instance_id) {
    std::wstring normalized(instance_id);
    std::transform(
        normalized.begin(),
        normalized.end(),
        normalized.begin(),
        [](wchar_t value) { return static_cast<wchar_t>(std::towupper(value)); });
    return normalized.find(L"VID_18D1&PID_2D00") != std::wstring::npos ||
        normalized.find(L"VID_18D1&PID_2D01") != std::wstring::npos;
}

std::vector<AndroidWinUsbInterface> enumerate_android_winusb_interfaces() {
    DeviceInfoSet devices(SetupDiGetClassDevsW(
        &kAndroidWinUsbInterfaceGuid,
        nullptr,
        nullptr,
        DIGCF_DEVICEINTERFACE | DIGCF_PRESENT));
    if (devices.get() == INVALID_HANDLE_VALUE) {
        throw_windows_error("SetupDiGetClassDevsW(Android WinUSB)");
    }

    std::vector<AndroidWinUsbInterface> result;
    for (DWORD index = 0;; ++index) {
        SP_DEVICE_INTERFACE_DATA interface_data{};
        interface_data.cbSize = sizeof(interface_data);
        if (SetupDiEnumDeviceInterfaces(
                devices.get(),
                nullptr,
                &kAndroidWinUsbInterfaceGuid,
                index,
                &interface_data) == FALSE) {
            if (GetLastError() == ERROR_NO_MORE_ITEMS) {
                break;
            }
            throw_windows_error("SetupDiEnumDeviceInterfaces(Android WinUSB)");
        }

        DWORD required = 0;
        SetupDiGetDeviceInterfaceDetailW(
            devices.get(),
            &interface_data,
            nullptr,
            0,
            &required,
            nullptr);
        if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || required == 0) {
            throw_windows_error("SetupDiGetDeviceInterfaceDetailW(size)");
        }

        std::vector<std::uint8_t> detail_storage(required);
        auto* detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(
            detail_storage.data());
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        SP_DEVINFO_DATA device{};
        device.cbSize = sizeof(device);
        if (SetupDiGetDeviceInterfaceDetailW(
                devices.get(),
                &interface_data,
                detail,
                required,
                nullptr,
                &device) == FALSE) {
            throw_windows_error("SetupDiGetDeviceInterfaceDetailW(data)");
        }

        std::wstring name = device_property(devices.get(), device, SPDRP_FRIENDLYNAME);
        if (name.empty()) {
            name = device_property(devices.get(), device, SPDRP_DEVICEDESC);
        }
        result.push_back(AndroidWinUsbInterface{
            .name = std::move(name),
            .instance_id = device_instance_id(devices.get(), device),
            .path = detail->DevicePath,
        });
    }
    return result;
}

AccessorySelection select_unique_aoa_accessory(
    std::span<const AndroidWinUsbInterface> interfaces) {
    std::optional<AndroidWinUsbInterface> selected;
    for (const auto& candidate : interfaces) {
        if (!is_aoa_accessory_instance(candidate.instance_id)) {
            continue;
        }
        if (selected) {
            return AccessorySelection{
                .state = AccessorySelectionState::ambiguous,
                .device = std::nullopt,
            };
        }
        selected = candidate;
    }
    if (!selected) {
        return {};
    }
    return AccessorySelection{
        .state = AccessorySelectionState::ready,
        .device = std::move(selected),
    };
}

std::optional<AndroidWinUsbInterface> wait_for_unique_aoa_accessory(
    std::chrono::milliseconds timeout,
    std::chrono::milliseconds poll_interval,
    AndroidWinUsbEnumerator enumerate) {
    if (timeout < std::chrono::milliseconds::zero()) {
        throw std::invalid_argument("AOA accessory wait timeout must not be negative");
    }
    if (poll_interval <= std::chrono::milliseconds::zero()) {
        throw std::invalid_argument("AOA accessory poll interval must be positive");
    }
    if (!enumerate) {
        throw std::invalid_argument("AOA accessory enumerator must be callable");
    }

    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (true) {
        auto selection = select_unique_aoa_accessory(enumerate());
        if (selection.state == AccessorySelectionState::ready) {
            return std::move(selection.device);
        }
        if (selection.state == AccessorySelectionState::ambiguous) {
            throw std::runtime_error(
                "more than one AOA accessory is present; select a device explicitly");
        }
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            return std::nullopt;
        }
        std::this_thread::sleep_for(std::min(
            poll_interval,
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now)));
    }
}

}  // namespace wireview::host
