#include "usb_accessory_discovery.hpp"

#include <array>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {

using wireview::host::AccessorySelectionState;
using wireview::host::AndroidWinUsbInterface;

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

AndroidWinUsbInterface device(std::wstring instance_id, std::wstring path) {
    return AndroidWinUsbInterface{
        .name = L"test",
        .instance_id = std::move(instance_id),
        .path = std::move(path),
    };
}

void test_filters_bootstrap_and_selects_one_accessory() {
    const std::array devices{
        device(L"USB\\VID_12D1&PID_107D&MI_02", L"bootstrap"),
        device(L"usb\\vid_18d1&pid_2d01", L"accessory"),
    };
    const auto selection = wireview::host::select_unique_aoa_accessory(devices);
    require(selection.state == AccessorySelectionState::ready, "AOA accessory was not selected");
    require(selection.device.has_value(), "ready selection has no device");
    require(selection.device->path == L"accessory", "wrong AOA accessory was selected");
}

void test_waiting_and_ambiguous_states() {
    const std::array bootstrap_only{
        device(L"USB\\VID_12D1&PID_107D&MI_02", L"bootstrap"),
    };
    require(
        wireview::host::select_unique_aoa_accessory(bootstrap_only).state ==
            AccessorySelectionState::waiting,
        "bootstrap interface was treated as an accessory");

    const std::array ambiguous{
        device(L"USB\\VID_18D1&PID_2D00", L"one"),
        device(L"USB\\VID_18D1&PID_2D01", L"two"),
    };
    require(
        wireview::host::select_unique_aoa_accessory(ambiguous).state ==
            AccessorySelectionState::ambiguous,
        "multiple AOA accessories were not rejected");
}

void test_wait_state_machine() {
    std::size_t calls = 0;
    const auto selected = wireview::host::wait_for_unique_aoa_accessory(
        std::chrono::milliseconds(10),
        std::chrono::milliseconds(1),
        [&calls] {
            ++calls;
            if (calls == 1U) {
                return std::vector<AndroidWinUsbInterface>{};
            }
            return std::vector<AndroidWinUsbInterface>{
                device(L"USB\\VID_18D1&PID_2D00", L"ready"),
            };
        });
    require(selected.has_value(), "wait state machine did not observe the accessory");
    require(selected->path == L"ready", "wait state machine selected the wrong path");
    require(calls == 2U, "wait state machine performed unexpected polls");

    const auto timed_out = wireview::host::wait_for_unique_aoa_accessory(
        std::chrono::milliseconds::zero(),
        std::chrono::milliseconds(1),
        [] { return std::vector<AndroidWinUsbInterface>{}; });
    require(!timed_out.has_value(), "zero-timeout accessory wait did not time out");
}

}  // namespace

int main() {
    try {
        test_filters_bootstrap_and_selects_one_accessory();
        test_waiting_and_ambiguous_states();
        test_wait_state_machine();
        std::cout << "All WireView USB accessory discovery tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "USB accessory discovery test failed: " << error.what() << '\n';
        return 1;
    }
}
