#include "reliable_byte_stream.hpp"

#include <algorithm>
#include <array>
#include <stdexcept>
#include <string>

namespace wireview::host {

void write_all(ReliableByteStream& stream, std::span<const std::uint8_t> bytes) {
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const std::size_t batch = std::min<std::size_t>(
            bytes.size() - offset,
            64U * 1024U);
        const std::size_t written = stream.write(bytes.subspan(offset, batch));
        if (written == 0 || written > batch) {
            throw std::runtime_error("byte stream closed or returned an invalid short write");
        }
        offset += written;
    }
}

namespace {

wireview::protocol::DecodedPacket read_packet_impl(
    ReliableByteStream& stream,
    const std::chrono::milliseconds* read_timeout) {
    wireview::protocol::PacketStreamParser parser;
    std::array<std::uint8_t, 16U * 1024U> receive_buffer{};

    while (true) {
        auto parsed = parser.next();
        if (parsed.status == wireview::protocol::StreamStatus::packet_ready && parsed.packet) {
            return std::move(*parsed.packet);
        }
        if (parsed.status == wireview::protocol::StreamStatus::error) {
            throw std::runtime_error(
                "WireView packet parser failed: " +
                std::to_string(static_cast<int>(parsed.error)));
        }

        if (read_timeout != nullptr && !stream.wait_readable(*read_timeout)) {
            throw std::runtime_error("timed out waiting for WireView packet bytes");
        }
        const std::size_t received = stream.read(receive_buffer);
        if (received == 0) {
            throw std::runtime_error("peer closed before a complete packet arrived");
        }
        if (received > receive_buffer.size()) {
            throw std::runtime_error("byte stream returned more bytes than requested");
        }
        if (!parser.push(std::span(receive_buffer).first(received))) {
            throw std::runtime_error("WireView receive buffer limit exceeded");
        }
    }
}

}  // namespace

wireview::protocol::DecodedPacket read_packet(ReliableByteStream& stream) {
    return read_packet_impl(stream, nullptr);
}

wireview::protocol::DecodedPacket read_packet(
    ReliableByteStream& stream,
    std::chrono::milliseconds read_timeout) {
    if (read_timeout < std::chrono::milliseconds::zero()) {
        throw std::invalid_argument("WireView packet read timeout must not be negative");
    }
    return read_packet_impl(stream, &read_timeout);
}

}  // namespace wireview::host
