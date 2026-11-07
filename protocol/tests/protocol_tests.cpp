#include "wireview/auth_messages.hpp"
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

void auth_message_test() {
    using namespace wireview::protocol;

    AuthChallenge challenge{};
    challenge.mode = AuthMode::pairing;
    challenge.nonce_pc.fill(0xa5U);
    challenge.pc_uuid = {0x01U, 0x02U, 0x03U, 0x04U, 0x05U, 0x06U, 0x07U, 0x08U,
                         0x09U, 0x0aU, 0x0bU, 0x0cU, 0x0dU, 0x0eU, 0x0fU, 0x10U};
    challenge.host_name = "DESKTOP-ABC123";
    const auto challenge_bytes = encode_auth_challenge(challenge);
    const auto parsed_challenge = parse_auth_challenge(challenge_bytes);
    check(parsed_challenge.has_value(), "auth challenge should round trip");
    if (parsed_challenge) {
        check(parsed_challenge->mode == AuthMode::pairing, "challenge mode should survive");
        check(parsed_challenge->nonce_pc == challenge.nonce_pc, "challenge nonce should survive");
        check(parsed_challenge->pc_uuid == challenge.pc_uuid, "challenge pc uuid should survive");
        check(parsed_challenge->host_name == challenge.host_name, "challenge name should survive");
    }
    check(!parse_auth_challenge({}).has_value(), "empty challenge must be rejected");
    check(!parse_auth_challenge(challenge_bytes).has_value() ||
              parse_auth_challenge(challenge_bytes)->host_name == challenge.host_name,
          "challenge parse is deterministic");

    AuthResponse response{};
    response.status = AuthStatus::ok;
    response.nonce_tablet.fill(0x5aU);
    response.android_id = "0123456789abcdef";
    response.device_name = "Xiaomi Pad 4";
    response.mac.fill(0x77U);
    const auto response_bytes = encode_auth_response(response);
    const auto parsed_response = parse_auth_response(response_bytes);
    check(parsed_response.has_value(), "auth response should round trip");
    if (parsed_response) {
        check(parsed_response->status == AuthStatus::ok, "response status should survive");
        check(parsed_response->android_id == response.android_id, "response android id should survive");
        check(parsed_response->device_name == response.device_name, "response name should survive");
        check(parsed_response->nonce_tablet == response.nonce_tablet, "response nonce should survive");
        check(parsed_response->mac == response.mac, "response mac should survive");
    }
    check(!parse_auth_response({}).has_value(), "empty response must be rejected");

    AuthResultPayload result{};
    result.result = AuthResult::pairing_complete;
    result.message = "DESKTOP-ABC123";
    result.credential.fill(0x33U);
    const auto result_bytes = encode_auth_result(result);
    const auto parsed_result = parse_auth_result(result_bytes);
    check(parsed_result.has_value(), "auth result with credential should round trip");
    if (parsed_result) {
        check(parsed_result->result == AuthResult::pairing_complete, "result kind should survive");
        check(parsed_result->message == result.message, "result message should survive");
        check(parsed_result->credential == result.credential, "result credential should survive");
    }

    AuthResultPayload rejection{};
    rejection.result = AuthResult::rejected;
    rejection.message = "device is not paired";
    const auto rejection_bytes = encode_auth_result(rejection);
    const auto parsed_rejection = parse_auth_result(rejection_bytes);
    check(parsed_rejection.has_value(), "auth rejection should round trip");
    if (parsed_rejection) {
        // A rejection carries no credential bytes: the payload ends after the
        // message, and parsing must not silently accept trailing garbage.
        check(parsed_rejection->credential.front() == 0U, "rejection must not carry a credential");
        const auto trailing = rejection_bytes;
        AuthResultPayload trailing_result{};
        trailing_result.result = AuthResult::accepted;
        trailing_result.message = "x";
        auto trailing_bytes = encode_auth_result(trailing_result);
        trailing_bytes.push_back(0U);
        check(!parse_auth_result(trailing_bytes).has_value(),
              "trailing bytes after an accepted result must be rejected");
    }

    // Oversized strings must be rejected before allocation.
    AuthChallenge oversized{};
    oversized.host_name.assign(kMaxAuthHostName + 1U, 'x');
    const auto oversized_bytes = encode_auth_challenge(oversized);
    check(!parse_auth_challenge(oversized_bytes).has_value(),
          "oversized host name must be rejected");
}

void auth_constants_test() {
    using namespace wireview::protocol;
    check(kAuthNonceSize == 16U, "nonce size is part of the wire contract");
    check(kAuthKeySize == kAuthMacSize, "key and MAC sizes must match (HMAC-SHA256)");
    check(kAuthPbkdf2Iterations >= 10'000U, "PIN derivation must stay costed");
}

}  // namespace

int main() {
    round_trip_test();
    checksum_test();
    stream_fragmentation_test();
    validation_test();
    input_batch_test();
    auth_message_test();
    auth_constants_test();

    if (failures != 0) {
        std::cerr << failures << " protocol test(s) failed\n";
        return 1;
    }
    std::cout << "All WireView protocol tests passed\n";
    return 0;
}
