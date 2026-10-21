#pragma once

#include "reliable_byte_stream.hpp"

#include <cstdint>
#include <memory>
#include <span>
#include <string_view>

namespace wireview::host {

struct UsbPipeCandidate {
    std::uint8_t id{};
    bool is_bulk{};
};

struct BulkPipePair {
    std::uint8_t input{};
    std::uint8_t output{};
};

[[nodiscard]] BulkPipePair select_unique_bulk_pipes(
    std::span<const UsbPipeCandidate> pipes);

/**
 * ReliableByteStream adapter for one already-enumerated WinUSB interface.
 *
 * The device path must identify the primary Android Open Accessory interface.
 * Reads and writes use independent overlapped operations and may run on
 * different threads. WVR1 remains responsible for framing and integrity.
 */
class WinUsbByteStream final : public ReliableByteStream {
public:
    [[nodiscard]] static std::unique_ptr<WinUsbByteStream> open(
        std::wstring_view device_path);

    ~WinUsbByteStream() override;

    WinUsbByteStream(const WinUsbByteStream&) = delete;
    WinUsbByteStream& operator=(const WinUsbByteStream&) = delete;

    std::size_t read(std::span<std::uint8_t> destination) override;
    std::size_t write(std::span<const std::uint8_t> source) override;
    bool wait_readable(std::chrono::milliseconds timeout) override;

private:
    struct Impl;

    explicit WinUsbByteStream(std::unique_ptr<Impl> implementation) noexcept;

    std::unique_ptr<Impl> implementation_;
};

}  // namespace wireview::host
