#include "wireview/input.hpp"
#include "wireview/protocol.hpp"

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using wireview::protocol::DecodeError;
using wireview::protocol::PacketFlag;
using wireview::protocol::PacketHeader;
using wireview::protocol::PacketStreamParser;
using wireview::protocol::PacketType;
using wireview::protocol::StreamStatus;

int failures = 0;

void check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        ++failures;
    }
}

std::vector<std::uint8_t> bytes_of(const std::string& value) {
    return {value.begin(), value.end()};
}

std::uint32_t crc32_reference(std::span<const std::uint8_t> bytes) {
    std::uint32_t crc = 0xffffffffU;
    for (const auto byte : bytes) {
        crc ^= byte;
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc & 1U) != 0U ? (crc >> 1U) ^ 0xedb88320U : crc >> 1U;
        }
    }
    return ~crc;
}

void round_trip_test() {
    const auto payload = bytes_of("wireview-hello");
    const PacketHeader header{
        .type = PacketType::hello,
        .flags = PacketFlag::acknowledgement_required,
        .sequence = 42,
        .timestamp_micros = 123456789,
    };

    const auto encoded = wireview::protocol::encode_packet(header, payload);
    const auto decoded = wireview::protocol::decode_packet(encoded);
    check(decoded.ok(), "round trip should decode");
    if (!decoded.ok()) {
        return;
    }
    check(decoded.packet->header.type == PacketType::hello, "packet type should survive");
    check(decoded.packet->header.sequence == 42, "sequence should survive");
    check(decoded.packet->header.timestamp_micros == 123456789, "timestamp should survive");
    check(decoded.packet->payload == payload, "payload should survive");
}

void checksum_test() {
    const auto expected = 0xcbf43926U;
    const auto input = bytes_of("123456789");
    check(wireview::protocol::crc32(input) == expected, "CRC32 should match the standard vector");
    check(crc32_reference(input) == expected, "reference CRC should match the standard vector");

    auto packet = wireview::protocol::encode_packet(
        PacketHeader{.type = PacketType::telemetry}, input);
    packet.back() ^= 0x01U;
    const auto decoded = wireview::protocol::decode_packet(packet);
    check(decoded.error == DecodeError::payload_checksum_mismatch, "payload corruption must be detected");

    packet = wireview::protocol::encode_packet(
        PacketHeader{.type = PacketType::telemetry}, input);
    packet[24] ^= 0x01U;
    const auto header_corrupted = wireview::protocol::decode_packet(packet);
    check(
        header_corrupted.error == DecodeError::header_checksum_mismatch,
        "header corruption must be detected");
}

void stream_fragmentation_test() {
    const auto first = wireview::protocol::encode_packet(
        PacketHeader{.type = PacketType::ping, .sequence = 7}, bytes_of("a"));
    const auto second = wireview::protocol::encode_packet(
        PacketHeader{.type = PacketType::pong, .sequence = 8}, bytes_of("bc"));

    std::vector<std::uint8_t> stream;
    stream.insert(stream.end(), first.begin(), first.end());
    stream.insert(stream.end(), second.begin(), second.end());

    PacketStreamParser parser;
    std::vector<std::uint64_t> sequences;
    for (const auto byte : stream) {
        check(parser.push(std::span<const std::uint8_t>(&byte, 1)), "single byte push should fit");
        while (true) {
            auto result = parser.next();
            if (result.status == StreamStatus::need_more_data) {
                break;
            }
            check(result.status == StreamStatus::packet_ready, "stream parser should not fail");
            if (result.packet) {
                sequences.push_back(result.packet->header.sequence);
            }
        }
    }

    check(sequences == std::vector<std::uint64_t>({7, 8}), "stream packet order should survive fragmentation");
    check(parser.buffered_bytes() == 0U, "stream parser should consume complete packets");
}

void validation_test() {
    const std::vector<std::uint8_t> short_packet(12U, 0U);
    check(
        wireview::protocol::decode_packet(short_packet).error == DecodeError::truncated_header,
        "short header must be rejected");

    auto packet = wireview::protocol::encode_packet(
        PacketHeader{.type = PacketType::hello}, {});
    packet[0] = 'X';
    check(
        wireview::protocol::decode_packet(packet).error == DecodeError::invalid_magic,
        "invalid magic must be rejected");

    bool threw = false;
    try {
        const std::vector<std::uint8_t> oversized(
            static_cast<std::size_t>(wireview::protocol::kMaxPayloadSize) + 1U, 0U);
        static_cast<void>(wireview::protocol::encode_packet(
            PacketHeader{.type = PacketType::video_chunk}, oversized));
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    check(threw, "oversized payload must be rejected before allocation of a packet");
}

void input_batch_test() {
    using wireview::protocol::InputAction;
    using wireview::protocol::InputBatch;
    using wireview::protocol::InputDecodeError;
    using wireview::protocol::InputRecord;
    using wireview::protocol::InputTool;

    const InputBatch batch{
        .viewport_width = 2560,
        .viewport_height = 1600,
        .records = {
            InputRecord{
                .action = InputAction::down,
                .tool = InputTool::stylus,
                .pointer_id = 7,
                .normalized_x = 12'345,
                .normalized_y = 54'321,
                .pressure = 40'000,
                .tilt_x_hundredths = -1'250,
                .tilt_y_hundredths = 875,
                .buttons = 2,
                .time_offset_micros = 123,
            },
        },
    };
    const auto encoded = wireview::protocol::encode_input_batch(batch);
    const auto decoded = wireview::protocol::decode_input_batch(encoded);
    check(decoded.ok(), "input batch should round trip");
    if (decoded.ok()) {
        const auto& record = decoded.batch->records.front();
        check(decoded.batch->viewport_width == 2560, "input viewport width should survive");
        check(record.action == InputAction::down, "input action should survive");
        check(record.tool == InputTool::stylus, "input tool should survive");
        check(record.normalized_x == 12'345, "input X should survive");
        check(record.pressure == 40'000, "input pressure should survive");
        check(record.tilt_x_hundredths == -1'250, "signed input tilt should survive");
    }

    auto invalid = encoded;
    invalid[1] = 19;
    check(
        wireview::protocol::decode_input_batch(invalid).error ==
            InputDecodeError::invalid_record_size,
        "unknown input record size must be rejected");
    invalid = encoded;
    invalid.pop_back();
    check(
        wireview::protocol::decode_input_batch(invalid).error ==
            InputDecodeError::size_mismatch,
        "truncated input record must be rejected");
}

}  // namespace

int main() {
    round_trip_test();
    checksum_test();
    stream_fragmentation_test();
    validation_test();
    input_batch_test();

    if (failures != 0) {
        std::cerr << failures << " protocol test(s) failed\n";
        return 1;
    }
    std::cout << "All WireView protocol tests passed\n";
    return 0;
}
