#pragma once

// Shared Winsock RAII helpers for the TCP transports. Extracted from
// development_transport.cpp so the WiFi session path (PC as client) and the
// ADB path (PC as listener) share one implementation. WinSock2.h must be
// included before any header that pulls in Windows.h.

#include <WinSock2.h>
#include <WS2tcpip.h>

#include "reliable_byte_stream.hpp"

#include <chrono>
#include <cstdint>
#include <span>
#include <string>

namespace wireview::host {

class WinsockSession {
public:
    WinsockSession();

    ~WinsockSession();

    WinsockSession(const WinsockSession&) = delete;
    WinsockSession& operator=(const WinsockSession&) = delete;
};

class SocketHandle {
public:
    explicit SocketHandle(SOCKET value = INVALID_SOCKET) noexcept;

    ~SocketHandle();

    SocketHandle(SocketHandle&& other) noexcept;

    SocketHandle& operator=(SocketHandle&& other) noexcept;

    SocketHandle(const SocketHandle&) = delete;
    SocketHandle& operator=(const SocketHandle&) = delete;

    [[nodiscard]] SOCKET get() const noexcept;

    [[nodiscard]] bool valid() const noexcept;

private:
    [[nodiscard]] SOCKET release() noexcept;

    void reset(SOCKET value = INVALID_SOCKET) noexcept;

    SOCKET value_{INVALID_SOCKET};
};

class SocketByteStream final : public ReliableByteStream {
public:
    explicit SocketByteStream(SOCKET socket) noexcept;

    std::size_t read(std::span<std::uint8_t> destination) override;

    std::size_t write(std::span<const std::uint8_t> source) override;

    bool wait_readable(std::chrono::milliseconds timeout) override;

    // Closes the socket unconditionally (close-wake for teardown). The
    // socket must not be used afterwards; shutdown(SD_BOTH) has already
    // been issued to wake threads blocked in send/recv.
    void close();

private:
    SOCKET socket_{INVALID_SOCKET};
};

[[noreturn]] void throw_socket_error(const char* operation);

SocketHandle create_tcp_socket();

void allow_listener_rebind(SOCKET socket);

void set_low_latency_socket_options(SOCKET socket);

sockaddr_in loopback_address(std::uint16_t port);

// Non-blocking connect bounded by the timeout; throws std::runtime_error on
// failure or timeout. Returns a connected socket with low-latency options.
SocketHandle connect_tcp_with_timeout(
    const sockaddr_in& address,
    std::chrono::milliseconds timeout);

}  // namespace wireview::host
