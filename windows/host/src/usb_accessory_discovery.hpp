#pragma once

#include <chrono>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace wireview::host {

struct AndroidWinUsbInterface {
    std::wstring name;
    std::wstring instance_id;
    std::wstring path;
};

enum class AccessorySelectionState {
    waiting,
    ready,
    ambiguous,
};

struct AccessorySelection {
    AccessorySelectionState state{AccessorySelectionState::waiting};
    std::optional<AndroidWinUsbInterface> device;
};

[[nodiscard]] bool is_aoa_accessory_instance(std::wstring_view instance_id);

[[nodiscard]] std::vector<AndroidWinUsbInterface> enumerate_android_winusb_interfaces();

[[nodiscard]] AccessorySelection select_unique_aoa_accessory(
    std::span<const AndroidWinUsbInterface> interfaces);

using AndroidWinUsbEnumerator =
    std::function<std::vector<AndroidWinUsbInterface>()>;

[[nodiscard]] std::optional<AndroidWinUsbInterface> wait_for_unique_aoa_accessory(
    std::chrono::milliseconds timeout,
    std::chrono::milliseconds poll_interval = std::chrono::milliseconds(250),
    AndroidWinUsbEnumerator enumerate = enumerate_android_winusb_interfaces);

}  // namespace wireview::host
