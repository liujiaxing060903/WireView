#include "winusb_byte_stream.hpp"

#include <array>
#include <iostream>
#include <stdexcept>

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void expect_rejected(std::span<const wireview::host::UsbPipeCandidate> pipes) {
    bool rejected = false;
    try {
        (void)wireview::host::select_unique_bulk_pipes(pipes);
    } catch (const std::runtime_error&) {
        rejected = true;
    }
    require(rejected, "invalid WinUSB endpoint layout was accepted");
}

void test_selects_one_bulk_pipe_per_direction() {
    const std::array pipes{
        wireview::host::UsbPipeCandidate{.id = 0x82, .is_bulk = false},
        wireview::host::UsbPipeCandidate{.id = 0x01, .is_bulk = true},
        wireview::host::UsbPipeCandidate{.id = 0x81, .is_bulk = true},
    };
    const auto selected = wireview::host::select_unique_bulk_pipes(pipes);
    require(selected.input == 0x81, "wrong WinUSB bulk IN pipe selected");
    require(selected.output == 0x01, "wrong WinUSB bulk OUT pipe selected");
}

void test_rejects_missing_or_ambiguous_bulk_pipes() {
    const std::array missing_output{
        wireview::host::UsbPipeCandidate{.id = 0x81, .is_bulk = true},
    };
    expect_rejected(missing_output);

    const std::array duplicate_input{
        wireview::host::UsbPipeCandidate{.id = 0x81, .is_bulk = true},
        wireview::host::UsbPipeCandidate{.id = 0x82, .is_bulk = true},
        wireview::host::UsbPipeCandidate{.id = 0x01, .is_bulk = true},
    };
    expect_rejected(duplicate_input);

    const std::array no_bulk{
        wireview::host::UsbPipeCandidate{.id = 0x81, .is_bulk = false},
        wireview::host::UsbPipeCandidate{.id = 0x01, .is_bulk = false},
    };
    expect_rejected(no_bulk);
}

}  // namespace

int main() {
    try {
        test_selects_one_bulk_pipe_per_direction();
        test_rejects_missing_or_ambiguous_bulk_pipes();
        std::cout << "All WireView WinUSB byte stream tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "WinUSB byte stream test failed: " << error.what() << '\n';
        return 1;
    }
}
