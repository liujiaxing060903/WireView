#include "socket_stream.hpp"

#include <algorithm>
#include <stdexcept>
#include <string>

namespace wireview::host {

WinsockSession::WinsockSession() {
    WSADATA data{};
    const int error = WSAStartup(MAKEWORD(2, 2), &data);
    if (error != 0) {
        throw std::runtime_error("WSAStartup failed: " + std::to_string(error));
    }
}

WinsockSession::~WinsockSession() {
    WSACleanup();
}

SocketHandle::SocketHandle(SOCKET value) noexcept : value_(value) {
}

SocketHandle::~SocketHandle() {
    reset();
}

SocketHandle::SocketHandle(SocketHandle&& other) noexcept : value_(other.release()) {
}

SocketHandle& SocketHandle::operator=(SocketHandle&& other) noexcept {
    if (this != &other) {
        reset(other.release());
    }
    return *this;
}

SOCKET SocketHandle::get() const noexcept {
    return value_;
}

bool SocketHandle::valid() const noexcept {
    return value_ != INVALID_SOCKET;
}

SOCKET SocketHandle::release() noexcept {
    const auto value = value_;
    value_ = INVALID_SOCKET;
    return value;
}

void SocketHandle::reset(SOCKET value) noexcept {
    if (valid()) {
        closesocket(value_);
    }
    value_ = value;
}

void SocketByteStream::close() {
    if (socket_ != INVALID_SOCKET) {
        // shutdown wakes threads blocked in send/recv on other sockets is
        // not guaranteed, but it is the documented way to interrupt this
        // socket's own pending operations before closesocket.
        (void)shutdown(socket_, SD_BOTH);
        closesocket(socket_);
        socket_ = INVALID_SOCKET;
    }
}

SocketByteStream::SocketByteStream(SOCKET socket) noexcept : socket_(socket) {
}

std::size_t SocketByteStream::read(std::span<std::uint8_t> destination) {
    const int batch = static_cast<int>(
        std::min<std::size_t>(destination.size(), 64U * 1024U));
    const int received = recv(
        socket_,
        reinterpret_cast<char*>(destination.data()),
        batch,
        0);
    if (received == SOCKET_ERROR) {
        throw_socket_error("recv");
    }
    return static_cast<std::size_t>(received);
}

std::size_t SocketByteStream::write(std::span<const std::uint8_t> source) {
    const int batch = static_cast<int>(
        std::min<std::size_t>(source.size(), 64U * 1024U));
    const int written = send(
        socket_,
        reinterpret_cast<const char*>(source.data()),
        batch,
        0);
    if (written == SOCKET_ERROR) {
        throw_socket_error("send");
    }
    return static_cast<std::size_t>(written);
}

bool SocketByteStream::wait_readable(std::chrono::milliseconds timeout) {
    fd_set readable{};
    FD_ZERO(&readable);
    FD_SET(socket_, &readable);
    timeval native_timeout{};
    native_timeout.tv_sec = static_cast<long>(timeout.count() / 1'000);
    native_timeout.tv_usec = static_cast<long>((timeout.count() % 1'000) * 1'000);
    const int ready = select(0, &readable, nullptr, nullptr, &native_timeout);
    if (ready == SOCKET_ERROR) {
        throw_socket_error("select(byte stream)");
    }
    return ready != 0;
}

void throw_socket_error(const char* operation) {
    throw std::runtime_error(
        std::string(operation) + " failed: " + std::to_string(WSAGetLastError()));
}

SocketHandle create_tcp_socket() {
    SocketHandle socket(::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
    if (!socket.valid()) {
        throw_socket_error("socket");
    }
    return socket;
}

void allow_listener_rebind(SOCKET socket) {
    // A USB disconnect or a Ctrl+C can leave the previous TCP connection in
    // Windows' closing state briefly. The development host is loopback-only,
    // so permit its recovery loop to reclaim this fixed local port immediately.
    const int enabled = 1;
    if (setsockopt(
            socket,
            SOL_SOCKET,
            SO_REUSEADDR,
            reinterpret_cast<const char*>(&enabled),
            sizeof(enabled)) == SOCKET_ERROR) {
        throw_socket_error("setsockopt(SO_REUSEADDR)");
    }
}

void set_low_latency_socket_options(SOCKET socket) {
    const int enabled = 1;
    if (setsockopt(
            socket,
            IPPROTO_TCP,
            TCP_NODELAY,
            reinterpret_cast<const char*>(&enabled),
            sizeof(enabled)) == SOCKET_ERROR) {
        throw_socket_error("setsockopt(TCP_NODELAY)");
    }

    constexpr DWORD timeout_millis = 10'000;
    if (setsockopt(
            socket,
            SOL_SOCKET,
            SO_RCVTIMEO,
            reinterpret_cast<const char*>(&timeout_millis),
            sizeof(timeout_millis)) == SOCKET_ERROR) {
        throw_socket_error("setsockopt(SO_RCVTIMEO)");
    }
    // Bound blocking sends as well: without this, a stalled adb/USB pipe
    // would freeze the sender thread (and session shutdown) forever.
    if (setsockopt(
            socket,
            SOL_SOCKET,
            SO_SNDTIMEO,
            reinterpret_cast<const char*>(&timeout_millis),
            sizeof(timeout_millis)) == SOCKET_ERROR) {
        throw_socket_error("setsockopt(SO_SNDTIMEO)");
    }
}

sockaddr_in loopback_address(std::uint16_t port) {
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    return address;
}

SocketHandle connect_tcp_with_timeout(
    const sockaddr_in& address,
    std::chrono::milliseconds timeout) {
    SocketHandle socket = create_tcp_socket();
    // Non-blocking connect: a blocking connect to an unreachable host is
    // governed by SYN retransmission (seconds to tens of seconds), which
    // would ruin the arbiter's retry cadence.
    u_long non_blocking = 1U;
    if (ioctlsocket(socket.get(), FIONBIO, &non_blocking) == SOCKET_ERROR) {
        throw_socket_error("ioctlsocket(FIONBIO)");
    }
    const int connect_result = connect(
        socket.get(),
        reinterpret_cast<const sockaddr*>(&const_cast<sockaddr_in&>(address)),
        sizeof(address));
    if (connect_result == SOCKET_ERROR) {
        const int error = WSAGetLastError();
        if (error != WSAEWOULDBLOCK) {
            throw std::runtime_error(
                std::string("connect failed: ") + std::to_string(error));
        }
        fd_set writable{};
        FD_ZERO(&writable);
        FD_SET(socket.get(), &writable);
        timeval native_timeout{};
        native_timeout.tv_sec = static_cast<long>(timeout.count() / 1'000);
        native_timeout.tv_usec = static_cast<long>((timeout.count() % 1'000) * 1'000);
        const int ready = select(0, nullptr, &writable, nullptr, &native_timeout);
        if (ready == 0) {
            throw std::runtime_error("connect timed out");
        }
        if (ready == SOCKET_ERROR) {
            throw_socket_error("select(connect)");
        }
        int so_error = 0;
        int so_error_size = static_cast<int>(sizeof(so_error));
        if (getsockopt(
                socket.get(),
                SOL_SOCKET,
                SO_ERROR,
                reinterpret_cast<char*>(&so_error),
                &so_error_size) == SOCKET_ERROR ||
            so_error != 0) {
            throw std::runtime_error(
                std::string("connect failed: ") + std::to_string(so_error));
        }
    }
    u_long blocking = 0U;
    if (ioctlsocket(socket.get(), FIONBIO, &blocking) == SOCKET_ERROR) {
        throw_socket_error("ioctlsocket(blocking)");
    }
    set_low_latency_socket_options(socket.get());
    return socket;
}

}  // namespace wireview::host
