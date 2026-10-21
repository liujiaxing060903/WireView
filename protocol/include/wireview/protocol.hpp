#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace wireview::protocol {

inline constexpr std::array<std::uint8_t, 4> kMagic{'W', 'V', 'R', '1'};
inline constexpr std::uint8_t kProtocolMajor = 1;
inline constexpr std::uint8_t kProtocolMinor = 0;
inline constexpr std::uint16_t kHeaderSize = 40;
inline constexpr std::uint32_t kMaxPayloadSize = 16U * 1024U * 1024U;
inline constexpr std::size_t kMaxBufferedBytes =
    (static_cast<std::size_t>(kMaxPayloadSize) + kHeaderSize) * 2U;

enum class PacketType : std::uint8_t {
    hello = 1,
    hello_ack = 2,
    start_stream = 3,
    stop_stream = 4,
    video_chunk = 5,
    keyframe_request = 6,
    input_batch = 7,
    telemetry = 8,
    ping = 9,
    pong = 10,
    error = 11,
    device_preview = 12,
};

enum class PacketFlag : std::uint8_t {
    none = 0,
    keyframe = 1U << 0U,
    final_chunk = 1U << 1U,
    acknowledgement_required = 1U << 2U,
};

constexpr PacketFlag operator|(PacketFlag left, PacketFlag right) noexcept {
    return static_cast<PacketFlag>(
        static_cast<std::uint8_t>(left) | static_cast<std::uint8_t>(right));
}

constexpr bool has_flag(PacketFlag value, PacketFlag flag) noexcept {
    return (static_cast<std::uint8_t>(value) & static_cast<std::uint8_t>(flag)) != 0U;
}

struct PacketHeader {
    std::uint8_t version_major{kProtocolMajor};
    std::uint8_t version_minor{kProtocolMinor};
    PacketType type{PacketType::error};
    PacketFlag flags{PacketFlag::none};
    std::uint64_t sequence{0};
    std::uint64_t timestamp_micros{0};
};

struct DecodedPacket {
    PacketHeader header;
    std::vector<std::uint8_t> payload;
};

enum class DecodeError {
    none,
    truncated_header,
    invalid_magic,
    unsupported_version,
    invalid_header_size,
    reserved_field_nonzero,
    payload_too_large,
    size_mismatch,
    header_checksum_mismatch,
    payload_checksum_mismatch,
    buffer_limit_exceeded,
};

struct DecodeResult {
    DecodeError error{DecodeError::none};
    std::optional<DecodedPacket> packet;

    [[nodiscard]] bool ok() const noexcept {
        return error == DecodeError::none && packet.has_value();
    }
};

enum class StreamStatus {
    need_more_data,
    packet_ready,
    error,
};

struct StreamResult {
    StreamStatus status{StreamStatus::need_more_data};
    DecodeError error{DecodeError::none};
    std::optional<DecodedPacket> packet;
};

[[nodiscard]] std::uint32_t crc32(std::span<const std::uint8_t> bytes) noexcept;

[[nodiscard]] std::vector<std::uint8_t> encode_packet(
    const PacketHeader& header,
    std::span<const std::uint8_t> payload);

[[nodiscard]] DecodeResult decode_packet(std::span<const std::uint8_t> bytes);

class PacketStreamParser {
public:
    [[nodiscard]] bool push(std::span<const std::uint8_t> bytes);
    [[nodiscard]] StreamResult next();
    [[nodiscard]] std::size_t buffered_bytes() const noexcept;
    void reset() noexcept;

private:
    std::vector<std::uint8_t> buffer_;
    bool buffer_limit_exceeded_{false};
};

}  // namespace wireview::protocol
