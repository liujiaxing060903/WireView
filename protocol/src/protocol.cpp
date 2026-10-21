#include "wireview/protocol.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace wireview::protocol {
namespace {

constexpr std::size_t kPayloadSizeOffset = 12;
constexpr std::size_t kPayloadCrcOffset = 32;
constexpr std::size_t kHeaderCrcOffset = 36;

void write_u16(std::vector<std::uint8_t>& output, std::size_t offset, std::uint16_t value) {
    output[offset] = static_cast<std::uint8_t>(value & 0xffU);
    output[offset + 1U] = static_cast<std::uint8_t>((value >> 8U) & 0xffU);
}

void write_u32(std::vector<std::uint8_t>& output, std::size_t offset, std::uint32_t value) {
    for (std::size_t index = 0; index < 4U; ++index) {
        output[offset + index] =
            static_cast<std::uint8_t>((value >> (index * 8U)) & 0xffU);
    }
}

void write_u64(std::vector<std::uint8_t>& output, std::size_t offset, std::uint64_t value) {
    for (std::size_t index = 0; index < 8U; ++index) {
        output[offset + index] =
            static_cast<std::uint8_t>((value >> (index * 8U)) & 0xffU);
    }
}

[[nodiscard]] std::uint16_t read_u16(std::span<const std::uint8_t> input, std::size_t offset) {
    return static_cast<std::uint16_t>(input[offset]) |
           static_cast<std::uint16_t>(static_cast<std::uint16_t>(input[offset + 1U]) << 8U);
}

[[nodiscard]] std::uint32_t read_u32(std::span<const std::uint8_t> input, std::size_t offset) {
    std::uint32_t value = 0;
    for (std::size_t index = 0; index < 4U; ++index) {
        value |= static_cast<std::uint32_t>(input[offset + index]) << (index * 8U);
    }
    return value;
}

[[nodiscard]] std::uint64_t read_u64(std::span<const std::uint8_t> input, std::size_t offset) {
    std::uint64_t value = 0;
    for (std::size_t index = 0; index < 8U; ++index) {
        value |= static_cast<std::uint64_t>(input[offset + index]) << (index * 8U);
    }
    return value;
}

[[nodiscard]] DecodeError validate_header(std::span<const std::uint8_t> bytes) {
    if (bytes.size() < kHeaderSize) {
        return DecodeError::truncated_header;
    }
    if (!std::equal(kMagic.begin(), kMagic.end(), bytes.begin())) {
        return DecodeError::invalid_magic;
    }
    if (bytes[4] != kProtocolMajor) {
        return DecodeError::unsupported_version;
    }
    if (read_u16(bytes, 8) != kHeaderSize) {
        return DecodeError::invalid_header_size;
    }
    if (read_u16(bytes, 10) != 0U) {
        return DecodeError::reserved_field_nonzero;
    }
    if (read_u32(bytes, kPayloadSizeOffset) > kMaxPayloadSize) {
        return DecodeError::payload_too_large;
    }
    const auto expected_crc = read_u32(bytes, kHeaderCrcOffset);
    const auto actual_crc = crc32(bytes.first(kHeaderCrcOffset));
    if (expected_crc != actual_crc) {
        return DecodeError::header_checksum_mismatch;
    }
    return DecodeError::none;
}

}  // namespace

std::uint32_t crc32(std::span<const std::uint8_t> bytes) noexcept {
    std::uint32_t crc = 0xffffffffU;
    for (const auto byte : bytes) {
        crc ^= byte;
        for (std::uint32_t bit = 0; bit < 8U; ++bit) {
            const std::uint32_t mask = 0U - (crc & 1U);
            crc = (crc >> 1U) ^ (0xedb88320U & mask);
        }
    }
    return ~crc;
}

std::vector<std::uint8_t> encode_packet(
    const PacketHeader& header,
    std::span<const std::uint8_t> payload) {
    if (payload.size() > kMaxPayloadSize ||
        payload.size() > std::numeric_limits<std::uint32_t>::max()) {
        throw std::invalid_argument("WireView packet payload is too large");
    }

    std::vector<std::uint8_t> output(kHeaderSize + payload.size(), 0U);
    std::copy(kMagic.begin(), kMagic.end(), output.begin());
    output[4] = header.version_major;
    output[5] = header.version_minor;
    output[6] = static_cast<std::uint8_t>(header.type);
    output[7] = static_cast<std::uint8_t>(header.flags);
    write_u16(output, 8, kHeaderSize);
    write_u16(output, 10, 0U);
    write_u32(output, kPayloadSizeOffset, static_cast<std::uint32_t>(payload.size()));
    write_u64(output, 16, header.sequence);
    write_u64(output, 24, header.timestamp_micros);
    write_u32(output, kPayloadCrcOffset, crc32(payload));
    write_u32(output, kHeaderCrcOffset, crc32(std::span(output).first(kHeaderCrcOffset)));
    std::copy(payload.begin(), payload.end(), output.begin() + kHeaderSize);
    return output;
}

DecodeResult decode_packet(std::span<const std::uint8_t> bytes) {
    const auto header_error = validate_header(bytes);
    if (header_error != DecodeError::none) {
        return {.error = header_error, .packet = std::nullopt};
    }

    const auto payload_size = read_u32(bytes, kPayloadSizeOffset);
    const auto total_size = static_cast<std::size_t>(kHeaderSize) + payload_size;
    if (bytes.size() != total_size) {
        return {.error = DecodeError::size_mismatch, .packet = std::nullopt};
    }

    const auto payload = bytes.subspan(kHeaderSize, payload_size);
    if (read_u32(bytes, kPayloadCrcOffset) != crc32(payload)) {
        return {.error = DecodeError::payload_checksum_mismatch, .packet = std::nullopt};
    }

    DecodedPacket packet;
    packet.header.version_major = bytes[4];
    packet.header.version_minor = bytes[5];
    packet.header.type = static_cast<PacketType>(bytes[6]);
    packet.header.flags = static_cast<PacketFlag>(bytes[7]);
    packet.header.sequence = read_u64(bytes, 16);
    packet.header.timestamp_micros = read_u64(bytes, 24);
    packet.payload.assign(payload.begin(), payload.end());
    return {.error = DecodeError::none, .packet = std::move(packet)};
}

bool PacketStreamParser::push(std::span<const std::uint8_t> bytes) {
    if (bytes.size() > kMaxBufferedBytes - std::min(buffer_.size(), kMaxBufferedBytes)) {
        buffer_.clear();
        buffer_limit_exceeded_ = true;
        return false;
    }
    buffer_.insert(buffer_.end(), bytes.begin(), bytes.end());
    return true;
}

StreamResult PacketStreamParser::next() {
    if (buffer_limit_exceeded_) {
        buffer_limit_exceeded_ = false;
        return {
            .status = StreamStatus::error,
            .error = DecodeError::buffer_limit_exceeded,
            .packet = std::nullopt,
        };
    }
    if (buffer_.size() < kHeaderSize) {
        return {};
    }

    const auto header = std::span<const std::uint8_t>(buffer_).first(kHeaderSize);
    const auto header_error = validate_header(header);
    if (header_error != DecodeError::none) {
        buffer_.clear();
        return {
            .status = StreamStatus::error,
            .error = header_error,
            .packet = std::nullopt,
        };
    }

    const auto payload_size = read_u32(header, kPayloadSizeOffset);
    const auto packet_size = static_cast<std::size_t>(kHeaderSize) + payload_size;
    if (buffer_.size() < packet_size) {
        return {};
    }

    auto decoded = decode_packet(std::span<const std::uint8_t>(buffer_).first(packet_size));
    if (!decoded.ok()) {
        buffer_.clear();
        return {
            .status = StreamStatus::error,
            .error = decoded.error,
            .packet = std::nullopt,
        };
    }

    buffer_.erase(buffer_.begin(), buffer_.begin() + static_cast<std::ptrdiff_t>(packet_size));
    return {
        .status = StreamStatus::packet_ready,
        .error = DecodeError::none,
        .packet = std::move(decoded.packet),
    };
}

std::size_t PacketStreamParser::buffered_bytes() const noexcept {
    return buffer_.size();
}

void PacketStreamParser::reset() noexcept {
    buffer_.clear();
    buffer_limit_exceeded_ = false;
}

}  // namespace wireview::protocol

