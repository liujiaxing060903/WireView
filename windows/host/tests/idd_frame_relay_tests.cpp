#include "idd_frame_relay_codec.hpp"

#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string_view>

namespace {

void expect(bool condition, std::string_view message) {
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

template <typename Function>
void expect_invalid_argument(Function&& function, std::string_view message) {
    try {
        function();
    } catch (const std::invalid_argument&) {
        return;
    }
    throw std::runtime_error(std::string(message));
}

void test_stream_description_round_trip() {
    const wireview::idd_relay::StreamDescription original{
        .adapter_luid_low = 0x12345678U,
        .adapter_luid_high = -42,
        .dxgi_format = 87U,
        .width = 2560U,
        .height = 1600U,
        .frame_rate = 60U,
        .slot_count = wireview::idd_relay::kSlotCount,
    };
    const auto encoded = wireview::idd_relay::encode_message(
        wireview::idd_relay::MessageType::stream_description, original);
    const auto message = wireview::idd_relay::parse_message(encoded);
    const auto decoded = wireview::idd_relay::parse_payload<wireview::idd_relay::StreamDescription>(
        message, wireview::idd_relay::MessageType::stream_description);
    expect(decoded.adapter_luid_low == original.adapter_luid_low, "LUID low part changed");
    expect(decoded.adapter_luid_high == original.adapter_luid_high, "LUID high part changed");
    expect(decoded.dxgi_format == original.dxgi_format, "format changed");
    expect(decoded.width == original.width && decoded.height == original.height,
        "dimensions changed");
    expect(decoded.frame_rate == original.frame_rate, "frame rate changed");
    expect(decoded.slot_count == wireview::idd_relay::kSlotCount, "slot count changed");
}

void test_frame_messages_are_strictly_typed() {
    const wireview::idd_relay::FrameReady original{
        .slot = 2U,
        .sequence = 99U,
        .capture_timestamp_micros = 123456789U,
    };
    const auto encoded = wireview::idd_relay::encode_message(
        wireview::idd_relay::MessageType::frame_ready, original);
    const auto message = wireview::idd_relay::parse_message(encoded);
    const auto decoded = wireview::idd_relay::parse_payload<wireview::idd_relay::FrameReady>(
        message, wireview::idd_relay::MessageType::frame_ready);
    expect(decoded.slot == original.slot && decoded.sequence == original.sequence,
        "frame identity changed");
    expect_invalid_argument(
        [&] {
            (void)wireview::idd_relay::parse_payload<wireview::idd_relay::FrameReleased>(
                message, wireview::idd_relay::MessageType::frame_released);
        },
        "type mismatch was accepted");
}

void test_shared_texture_name_round_trip() {
    const auto encoded = wireview::idd_relay::encode_shared_texture_name(
        1U, L"Local\\WireViewIdd-test-texture");
    const auto message = wireview::idd_relay::parse_message(encoded);
    const auto decoded = wireview::idd_relay::parse_shared_texture_name(message);
    expect(decoded.slot == 1U, "shared texture name slot changed");
    expect(decoded.name == L"Local\\WireViewIdd-test-texture", "shared texture name changed");
}

void test_malformed_messages_are_rejected() {
    std::vector<std::byte> too_short(sizeof(wireview::idd_relay::MessageHeader) - 1U);
    expect_invalid_argument(
        [&] { (void)wireview::idd_relay::parse_message(too_short); },
        "short message was accepted");

    const wireview::idd_relay::FrameReleased payload{.slot = 1U, .sequence = 5U};
    auto encoded = wireview::idd_relay::encode_message(
        wireview::idd_relay::MessageType::frame_released, payload);
    encoded[0] = std::byte{0};
    expect_invalid_argument(
        [&] { (void)wireview::idd_relay::parse_message(encoded); },
        "bad magic was accepted");

    encoded = wireview::idd_relay::encode_message(
        wireview::idd_relay::MessageType::frame_released, payload);
    encoded.pop_back();
    expect_invalid_argument(
        [&] { (void)wireview::idd_relay::parse_message(encoded); },
        "truncated payload was accepted");
}

}  // namespace

int main() {
    try {
        test_stream_description_round_trip();
        test_frame_messages_are_strictly_typed();
        test_shared_texture_name_round_trip();
        test_malformed_messages_are_rejected();
        std::cout << "WireView Idd frame relay protocol tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "WireView Idd frame relay protocol test failed: " << error.what() << '\n';
        return 1;
    }
}
