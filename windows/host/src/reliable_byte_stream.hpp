#pragma once

#include "wireview/protocol.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>

namespace wireview::host {

/**
 * Ordered, reliable, bidirectional bytes for one WireView session.
 *
 * WVR1 owns packet framing. Implementations may use TCP, WinUSB bulk pipes, or
 * an in-memory test stream, and may be read and written from different threads.
 */
class ReliableByteStream {
public:
    virtual ~ReliableByteStream() = default;

    virtual std::size_t read(std::span<std::uint8_t> destination) = 0;
    virtual std::size_t write(std::span<const std::uint8_t> source) = 0;
    virtual bool wait_readable(std::chrono::milliseconds timeout) = 0;
};

void write_all(ReliableByteStream& stream, std::span<const std::uint8_t> bytes);

[[nodiscard]] wireview::protocol::DecodedPacket read_packet(ReliableByteStream& stream);

[[nodiscard]] wireview::protocol::DecodedPacket read_packet(
    ReliableByteStream& stream,
    std::chrono::milliseconds read_timeout);

}  // namespace wireview::host
