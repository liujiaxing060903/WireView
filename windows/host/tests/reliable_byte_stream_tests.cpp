#include "reliable_byte_stream.hpp"

#include "wireview/protocol.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

class MemoryStream final : public wireview::host::ReliableByteStream {
public:
    explicit MemoryStream(
        std::vector<std::uint8_t> incoming = {},
        std::size_t read_chunk = 3,
        std::size_t write_chunk = 5)
        : incoming_(std::move(incoming)),
          read_chunk_(read_chunk),
          write_chunk_(write_chunk) {
    }

    std::size_t read(std::span<std::uint8_t> destination) override {
        const std::size_t remaining = incoming_.size() - read_offset_;
        const std::size_t count = std::min({remaining, destination.size(), read_chunk_});
        std::copy_n(incoming_.begin() + static_cast<std::ptrdiff_t>(read_offset_), count, destination.begin());
        read_offset_ += count;
        return count;
    }

    std::size_t write(std::span<const std::uint8_t> source) override {
        const std::size_t count = std::min(source.size(), write_chunk_);
        written_.insert(written_.end(), source.begin(), source.begin() + static_cast<std::ptrdiff_t>(count));
        return count;
    }

    bool wait_readable(std::chrono::milliseconds) override {
        return read_offset_ < incoming_.size();
    }

    [[nodiscard]] const std::vector<std::uint8_t>& written() const noexcept {
        return written_;
    }

private:
    std::vector<std::uint8_t> incoming_;
    std::vector<std::uint8_t> written_;
    std::size_t read_offset_{};
    std::size_t read_chunk_;
    std::size_t write_chunk_;
};

class TimeoutStream final : public wireview::host::ReliableByteStream {
public:
    std::size_t read(std::span<std::uint8_t>) override {
        read_called = true;
        return 0;
    }

    std::size_t write(std::span<const std::uint8_t>) override {
        return 0;
    }

    bool wait_readable(std::chrono::milliseconds timeout) override {
        observed_timeout = timeout;
        return false;
    }

    bool read_called{};
    std::chrono::milliseconds observed_timeout{};
};

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void test_short_writes() {
    std::vector<std::uint8_t> bytes(257);
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        bytes[index] = static_cast<std::uint8_t>(index);
    }
    MemoryStream stream({}, 3, 5);
    wireview::host::write_all(stream, bytes);
    require(stream.written() == bytes, "write_all lost bytes across short writes");
}

void test_fragmented_packet() {
    const std::string text = "fragmented-byte-stream";
    const std::vector<std::uint8_t> payload(text.begin(), text.end());
    const auto encoded = wireview::protocol::encode_packet(
        wireview::protocol::PacketHeader{
            .type = wireview::protocol::PacketType::hello,
            .sequence = 42,
            .timestamp_micros = 123,
        },
        payload);
    MemoryStream stream(encoded, 1, 5);
    const auto packet = wireview::host::read_packet(stream);
    require(packet.header.type == wireview::protocol::PacketType::hello, "packet type mismatch");
    require(packet.header.sequence == 42, "packet sequence mismatch");
    require(packet.payload == payload, "fragmented packet payload mismatch");
}

void test_truncated_packet() {
    const auto encoded = wireview::protocol::encode_packet(
        wireview::protocol::PacketHeader{
            .type = wireview::protocol::PacketType::hello,
            .sequence = 1,
        },
        {});
    std::vector<std::uint8_t> truncated(encoded.begin(), encoded.end() - 1);
    MemoryStream stream(std::move(truncated), 7, 5);
    bool rejected = false;
    try {
        (void)wireview::host::read_packet(stream);
    } catch (const std::runtime_error&) {
        rejected = true;
    }
    require(rejected, "truncated stream packet was accepted");
}

void test_corrupted_packet() {
    const std::vector<std::uint8_t> payload{1, 2, 3};
    auto encoded = wireview::protocol::encode_packet(
        wireview::protocol::PacketHeader{
            .type = wireview::protocol::PacketType::telemetry,
            .sequence = 7,
        },
        payload);
    encoded.back() ^= 0xffU;
    MemoryStream stream(std::move(encoded), 11, 5);
    bool rejected = false;
    try {
        (void)wireview::host::read_packet(stream);
    } catch (const std::runtime_error&) {
        rejected = true;
    }
    require(rejected, "corrupted stream packet was accepted");
}

void test_packet_read_timeout() {
    TimeoutStream stream;
    bool timed_out = false;
    try {
        (void)wireview::host::read_packet(stream, std::chrono::milliseconds(17));
    } catch (const std::runtime_error&) {
        timed_out = true;
    }
    require(timed_out, "packet read timeout was not reported");
    require(!stream.read_called, "timed-out packet read consumed the stream");
    require(
        stream.observed_timeout == std::chrono::milliseconds(17),
        "packet read used the wrong timeout");
}

}  // namespace

int main() {
    try {
        test_short_writes();
        test_fragmented_packet();
        test_truncated_packet();
        test_corrupted_packet();
        test_packet_read_timeout();
        std::cout << "All WireView reliable byte stream tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Reliable byte stream test failed: " << error.what() << '\n';
        return 1;
    }
}
