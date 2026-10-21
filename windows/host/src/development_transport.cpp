#include "development_transport.hpp"

#include "annex_b_stream.hpp"
#include "input_injector.hpp"
#include "reliable_byte_stream.hpp"
#include "usb_accessory_discovery.hpp"
#include "winusb_byte_stream.hpp"
#include "windows_video.hpp"

#include "wireview/protocol.hpp"

#include <WinSock2.h>
#include <WS2tcpip.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <mutex>
#include <optional>
#include <sstream>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace wireview::host {
namespace {

class WinsockSession {
public:
    WinsockSession() {
        WSADATA data{};
        const int error = WSAStartup(MAKEWORD(2, 2), &data);
        if (error != 0) {
            throw std::runtime_error("WSAStartup failed: " + std::to_string(error));
        }
    }

    ~WinsockSession() {
        WSACleanup();
    }

    WinsockSession(const WinsockSession&) = delete;
    WinsockSession& operator=(const WinsockSession&) = delete;
};

class SocketHandle {
public:
    explicit SocketHandle(SOCKET value = INVALID_SOCKET) noexcept : value_(value) {
    }

    ~SocketHandle() {
        reset();
    }

    SocketHandle(SocketHandle&& other) noexcept : value_(other.release()) {
    }

    SocketHandle& operator=(SocketHandle&& other) noexcept {
        if (this != &other) {
            reset(other.release());
        }
        return *this;
    }

    SocketHandle(const SocketHandle&) = delete;
    SocketHandle& operator=(const SocketHandle&) = delete;

    [[nodiscard]] SOCKET get() const noexcept {
        return value_;
    }

    [[nodiscard]] bool valid() const noexcept {
        return value_ != INVALID_SOCKET;
    }

private:
    [[nodiscard]] SOCKET release() noexcept {
        const auto value = value_;
        value_ = INVALID_SOCKET;
        return value;
    }

    void reset(SOCKET value = INVALID_SOCKET) noexcept {
        if (valid()) {
            closesocket(value_);
        }
        value_ = value;
    }

    SOCKET value_{INVALID_SOCKET};
};

class DevelopmentStatusPublisher {
public:
    explicit DevelopmentStatusPublisher(std::string_view path)
        : path_(path.empty() ? std::filesystem::path{} : std::filesystem::path(path)) {
    }

    void configure(
        std::string_view display,
        bool inject_input,
        std::uint16_t requested_width,
        std::uint16_t requested_height,
        std::uint16_t requested_frame_rate,
        std::uint64_t reconnect_count,
        std::string_view transport) {
        std::lock_guard lock(mutex_);
        display_ = display.empty() ? "primary display" : std::string(display);
        input_mode_ = inject_input ? "enabled" : "log-only";
        requested_width_ = requested_width;
        requested_height_ = requested_height;
        requested_frame_rate_ = requested_frame_rate;
        reconnect_count_ = reconnect_count;
        transport_ = transport;
        state_ = "starting";
        message_ = "Windows Host is starting";
        publish_locked();
    }

    void waiting_for_client() {
        update_state("waiting_client", "Waiting for the Android receiver");
    }

    void waiting_for_usb_accessory() {
        update_state("waiting_usb", "Waiting for one WireView AOA accessory");
    }

    void connected(std::string_view client) {
        std::lock_guard lock(mutex_);
        client_ = client;
        state_ = "connected";
        message_ = "Android receiver connected";
        publish_locked();
    }

    void device_profile(
        std::uint16_t width, std::uint16_t height, double refresh_hz, double maximum_refresh_hz,
        std::string_view manufacturer, bool tablet) {
        std::lock_guard lock(mutex_);
        android_width_ = width;
        android_height_ = height;
        android_refresh_hz_ = refresh_hz;
        android_max_refresh_hz_ = maximum_refresh_hz;
        android_manufacturer_ = manufacturer;
        android_device_type_ = tablet ? "tablet" : "phone";
        publish_locked();
    }

    void streaming(std::uint16_t width, std::uint16_t height, std::uint16_t frame_rate) {
        std::lock_guard lock(mutex_);
        actual_width_ = width;
        actual_height_ = height;
        actual_frame_rate_ = frame_rate;
        delivered_frames_ = 0U;
        delivered_bytes_ = 0U;
        output_frame_rate_ = 0.0;
        bitrate_mbps_ = 0.0;
        stream_started_at_ = std::chrono::steady_clock::now();
        state_ = "streaming";
        message_ = "Desktop stream is active";
        publish_locked();
    }

    void delivered_frame(std::size_t bytes) {
        std::lock_guard lock(mutex_);
        ++delivered_frames_;
        delivered_bytes_ += bytes;
        const std::uint64_t publish_interval = actual_frame_rate_ == 0U
            ? 60U
            : actual_frame_rate_;
        if (delivered_frames_ % publish_interval != 0U) {
            return;
        }
        const double elapsed_seconds = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - stream_started_at_).count();
        if (elapsed_seconds <= 0.0) {
            return;
        }
        output_frame_rate_ = static_cast<double>(delivered_frames_) / elapsed_seconds;
        bitrate_mbps_ = static_cast<double>(delivered_bytes_) * 8.0 /
            elapsed_seconds / 1'000'000.0;
        publish_locked();
    }

    void first_frame(std::uint32_t startup_millis) {
        std::lock_guard lock(mutex_);
        first_frame_millis_ = startup_millis;
        publish_locked();
    }

    void surface_sample(double latency_millis) {
        std::lock_guard lock(mutex_);
        surface_samples_.push_back(latency_millis);
        auto sorted = surface_samples_;
        std::sort(sorted.begin(), sorted.end());
        const auto percentile = [&sorted](double fraction) {
            const std::size_t index = static_cast<std::size_t>(std::ceil(
                fraction * static_cast<double>(sorted.size() - 1U)));
            return sorted[index];
        };
        surface_p50_millis_ = percentile(0.50);
        surface_p95_millis_ = percentile(0.95);
        publish_locked();
    }

    void android_capabilities(
        std::string_view decoder_name,
        bool hardware_decoder,
        bool software_decoder,
        bool low_latency,
        bool supports_1080p60,
        std::uint16_t width,
        std::uint16_t height,
        double refresh_hz,
        double maximum_refresh_hz) {
        std::lock_guard lock(mutex_);
        decoder_name_ = single_line(decoder_name);
        decoder_hardware_ = hardware_decoder;
        decoder_software_ = software_decoder;
        decoder_low_latency_ = low_latency;
        decoder_1080p60_ = supports_1080p60;
        android_width_ = width;
        android_height_ = height;
        android_refresh_hz_ = refresh_hz;
        android_max_refresh_hz_ = maximum_refresh_hz;
        publish_locked();
    }

    void decoder_health(
        std::uint64_t received_frames,
        std::uint64_t queued_frames,
        std::uint64_t dropped_frames,
        std::uint32_t decoder_errors,
        bool decoder_active,
        bool first_frame_rendered) {
        std::lock_guard lock(mutex_);
        decoder_health_seen_ = true;
        decoder_active_ = decoder_active;
        decoder_first_frame_ = first_frame_rendered;
        decoder_received_frames_ = received_frames;
        decoder_queued_frames_ = queued_frames;
        decoder_dropped_frames_ = dropped_frames;
        decoder_errors_ = decoder_errors;
        publish_locked();
    }

    void interrupted(std::string_view message) {
        std::lock_guard lock(mutex_);
        ++reconnect_count_;
        state_ = "recovering";
        message_ = single_line(message);
        publish_locked();
    }

    void completed() {
        update_state("completed", "Desktop stream completed");
    }

private:
    static std::string single_line(std::string_view value) {
        std::string result(value);
        std::replace(result.begin(), result.end(), '\r', ' ');
        std::replace(result.begin(), result.end(), '\n', ' ');
        return result;
    }

    void update_state(std::string_view state, std::string_view message) {
        std::lock_guard lock(mutex_);
        state_ = state;
        message_ = single_line(message);
        publish_locked();
    }

    void publish_locked() const {
        if (path_.empty()) {
            return;
        }
        std::error_code error;
        if (!path_.parent_path().empty()) {
            std::filesystem::create_directories(path_.parent_path(), error);
            if (error) {
                return;
            }
        }
        auto temporary = path_;
        temporary += L".host.tmp";
        {
            std::ofstream output(temporary, std::ios::trunc);
            if (!output) {
                return;
            }
            output << "version=1\n"
                   << "state=" << state_ << '\n'
                   << "message=" << single_line(message_) << '\n'
                   << "display=" << single_line(display_) << '\n'
                   << "input_mode=" << input_mode_ << '\n'
                   << "requested_width=" << requested_width_ << '\n'
                   << "requested_height=" << requested_height_ << '\n'
                   << "requested_fps=" << requested_frame_rate_ << '\n'
                   << "transport=" << single_line(transport_) << '\n'
                   << "actual_width=" << actual_width_ << '\n'
                   << "actual_height=" << actual_height_ << '\n'
                   << "actual_fps=" << actual_frame_rate_ << '\n'
                   << "client=" << single_line(client_) << '\n'
                   << "reconnects=" << reconnect_count_ << '\n'
                   << "first_frame_ms=" << first_frame_millis_ << '\n'
                   << "surface_samples=" << surface_samples_.size() << '\n'
                   << std::fixed << std::setprecision(2)
                   << "surface_p50_ms=" << surface_p50_millis_ << '\n'
                   << "surface_p95_ms=" << surface_p95_millis_ << '\n'
                   << "delivered_frames=" << delivered_frames_ << '\n'
                   << "output_fps=" << output_frame_rate_ << '\n'
                   << "bitrate_mbps=" << bitrate_mbps_ << '\n'
                   << "decoder_name=" << single_line(decoder_name_) << '\n'
                   << "decoder_hardware=" << (decoder_hardware_ ? 1 : 0) << '\n'
                   << "decoder_software=" << (decoder_software_ ? 1 : 0) << '\n'
                   << "decoder_low_latency=" << (decoder_low_latency_ ? 1 : 0) << '\n'
                   << "decoder_1080p60=" << (decoder_1080p60_ ? 1 : 0) << '\n'
                   << "android_width=" << android_width_ << '\n'
                   << "android_height=" << android_height_ << '\n'
                   << "android_refresh_hz=" << android_refresh_hz_ << '\n'
                   << "android_max_refresh_hz=" << android_max_refresh_hz_ << '\n'
                   << "android_manufacturer=" << single_line(android_manufacturer_) << '\n'
                   << "android_device_type=" << android_device_type_ << '\n'
                   << "decoder_health_seen=" << (decoder_health_seen_ ? 1 : 0) << '\n'
                   << "decoder_active=" << (decoder_active_ ? 1 : 0) << '\n'
                   << "decoder_first_frame=" << (decoder_first_frame_ ? 1 : 0) << '\n'
                   << "decoder_received_frames=" << decoder_received_frames_ << '\n'
                   << "decoder_queued_frames=" << decoder_queued_frames_ << '\n'
                   << "decoder_dropped_frames=" << decoder_dropped_frames_ << '\n'
                   << "decoder_errors=" << decoder_errors_ << '\n';
        }
        if (MoveFileExW(
                temporary.c_str(),
                path_.c_str(),
                MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == FALSE) {
            std::filesystem::remove(temporary, error);
        }
    }

    std::filesystem::path path_;
    mutable std::mutex mutex_;
    std::string state_;
    std::string message_;
    std::string display_;
    std::string input_mode_;
    std::string client_;
    std::string transport_{"adb"};
    std::string decoder_name_;
    std::string android_manufacturer_;
    std::string android_device_type_{"tablet"};
    std::uint16_t requested_width_{};
    std::uint16_t requested_height_{};
    std::uint16_t requested_frame_rate_{60U};
    std::uint16_t actual_width_{};
    std::uint16_t actual_height_{};
    std::uint16_t actual_frame_rate_{};
    std::uint64_t reconnect_count_{};
    std::uint32_t first_frame_millis_{};
    std::uint64_t delivered_frames_{};
    std::uint64_t delivered_bytes_{};
    std::uint64_t decoder_received_frames_{};
    std::uint64_t decoder_queued_frames_{};
    std::uint64_t decoder_dropped_frames_{};
    std::uint32_t decoder_errors_{};
    std::uint16_t android_width_{};
    std::uint16_t android_height_{};
    bool decoder_hardware_{};
    bool decoder_software_{};
    bool decoder_low_latency_{};
    bool decoder_1080p60_{};
    bool decoder_health_seen_{};
    bool decoder_active_{};
    bool decoder_first_frame_{};
    std::vector<double> surface_samples_;
    double surface_p50_millis_{};
    double surface_p95_millis_{};
    double output_frame_rate_{};
    double bitrate_mbps_{};
    double android_refresh_hz_{};
    double android_max_refresh_hz_{};
    std::chrono::steady_clock::time_point stream_started_at_{};
};

struct AndroidDeviceProfile {
    std::string client_name;
    std::string manufacturer;
    std::uint16_t width{};
    std::uint16_t height{};
    double active_refresh_hz{};
    double maximum_refresh_hz{};
    bool available{false};
    bool tablet{true};
};

[[nodiscard]] std::uint64_t monotonic_micros();

[[nodiscard]] std::uint16_t read_u16_le(std::span<const std::uint8_t> bytes, std::size_t offset) {
    return static_cast<std::uint16_t>(bytes[offset]) |
        (static_cast<std::uint16_t>(bytes[offset + 1U]) << 8U);
}

[[nodiscard]] AndroidDeviceProfile parse_hello_ack(
    std::span<const std::uint8_t> payload) {
    constexpr std::array<std::uint8_t, 4> kProfileMagic{'W', 'V', 'P', 'C'};
    AndroidDeviceProfile profile;
    if (payload.size() < 18U ||
        !std::equal(kProfileMagic.begin(), kProfileMagic.end(), payload.begin()) ||
        (payload[4U] != 1U && payload[4U] != 2U)) {
        profile.client_name.assign(payload.begin(), payload.end());
        return profile;
    }
    const std::size_t name_length = read_u16_le(payload, 14U);
    const std::size_t manufacturer_length = payload[4U] >= 2U
        ? read_u16_le(payload, 16U) : 0U;
    if (18U + name_length + manufacturer_length != payload.size()) {
        profile.client_name.assign(payload.begin(), payload.end());
        return profile;
    }
    profile.client_name.assign(
        reinterpret_cast<const char*>(payload.data() + 18U), name_length);
    profile.manufacturer.assign(
        reinterpret_cast<const char*>(payload.data() + 18U + name_length),
        manufacturer_length);
    profile.width = read_u16_le(payload, 6U);
    profile.height = read_u16_le(payload, 8U);
    profile.active_refresh_hz = static_cast<double>(read_u16_le(payload, 10U)) / 100.0;
    profile.maximum_refresh_hz = static_cast<double>(read_u16_le(payload, 12U)) / 100.0;
    profile.available = profile.width != 0U && profile.height != 0U;
    profile.tablet = payload[4U] == 1U || (payload[5U] & 1U) != 0U;
    return profile;
}

void write_device_preview(
    ReliableByteStream& stream,
    std::uint16_t width,
    std::uint16_t height,
    std::uint16_t frame_rate) {
    std::array<std::uint8_t, 10> payload{};
    payload[0] = 1U;  // Preview layout schema version.
    const auto write_u16 = [&payload](std::size_t offset, std::uint16_t value) {
        payload[offset] = static_cast<std::uint8_t>(value & 0xffU);
        payload[offset + 1U] = static_cast<std::uint8_t>(value >> 8U);
    };
    write_u16(4U, width);
    write_u16(6U, height);
    write_u16(8U, frame_rate);
    const wireview::protocol::PacketHeader header{
        .type = wireview::protocol::PacketType::device_preview,
        .sequence = 2U,
        .timestamp_micros = monotonic_micros(),
    };
    write_all(stream, wireview::protocol::encode_packet(header, payload));
}

[[nodiscard]] std::uint64_t monotonic_micros() {
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(now).count());
}

[[noreturn]] void throw_socket_error(const char* operation) {
    throw std::runtime_error(
        std::string(operation) + " failed: " + std::to_string(WSAGetLastError()));
}

class SocketByteStream final : public ReliableByteStream {
public:
    explicit SocketByteStream(SOCKET socket) noexcept : socket_(socket) {
    }

    std::size_t read(std::span<std::uint8_t> destination) override {
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

    std::size_t write(std::span<const std::uint8_t> source) override {
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

    bool wait_readable(std::chrono::milliseconds timeout) override {
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

private:
    SOCKET socket_{INVALID_SOCKET};
};

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
}

sockaddr_in loopback_address(std::uint16_t port) {
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    return address;
}

std::vector<std::uint8_t> read_binary_file(const std::string& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("cannot open H.264 test stream: " + path);
    }
    return {
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>(),
    };
}

void send_test_stream(
    ReliableByteStream& stream,
    const std::string& path,
    std::uint16_t width,
    std::uint16_t height,
    std::uint16_t frame_rate) {
    const auto bitstream = read_binary_file(path);
    const auto access_units = split_annex_b_access_units(bitstream);
    const auto config = make_avc_stream_config(width, height, frame_rate, 1U);

    std::uint64_t sequence = 2U;
    write_all(
        stream,
        wireview::protocol::encode_packet(
            wireview::protocol::PacketHeader{
                .type = wireview::protocol::PacketType::start_stream,
                .sequence = sequence++,
                .timestamp_micros = monotonic_micros(),
            },
            config));

    std::cout << "Sending " << access_units.size() << " H.264 access units at "
              << width << 'x' << height << '@' << frame_rate << " FPS\n";
    const auto start = std::chrono::steady_clock::now();
    const auto frame_duration = std::chrono::microseconds(1'000'000 / frame_rate);

    for (std::size_t index = 0; index < access_units.size(); ++index) {
        std::this_thread::sleep_until(start + frame_duration * index);
        auto flags = wireview::protocol::PacketFlag::final_chunk;
        if (access_units[index].keyframe) {
            flags = flags | wireview::protocol::PacketFlag::keyframe;
        }
        write_all(
            stream,
            wireview::protocol::encode_packet(
                wireview::protocol::PacketHeader{
                    .type = wireview::protocol::PacketType::video_chunk,
                    .flags = flags,
                    .sequence = sequence++,
                    .timestamp_micros = monotonic_micros(),
                },
                access_units[index].bytes));
    }

    write_all(
        stream,
        wireview::protocol::encode_packet(
            wireview::protocol::PacketHeader{
                .type = wireview::protocol::PacketType::stop_stream,
                .sequence = sequence,
                .timestamp_micros = monotonic_micros(),
            },
            {}));
    std::cout << "Synthetic H.264 stream completed\n";
}

struct InputReceiveStats {
    struct ClockSyncSample {
        double round_trip_millis;
        double android_minus_host_micros;
        std::uint64_t midpoint_host_micros;
    };
    struct FrameTiming {
        std::uint64_t sequence;
        std::uint64_t host_timestamp_micros;
        std::uint64_t arrival_micros;
        std::uint64_t surface_release_micros;
    };

    std::uint64_t packets{0};
    std::uint64_t records{0};
    std::uint64_t rejected_packets{0};
    std::uint64_t injection_failures{0};
    std::uint64_t first_frame_reports{0};
    std::vector<double> android_surface_latency_millis;
    std::vector<ClockSyncSample> clock_sync_samples;
    std::optional<ClockSyncSample> best_clock_sync;
    std::vector<FrameTiming> frame_timings;
};

[[nodiscard]] std::uint64_t read_little_endian_u64(
    std::span<const std::uint8_t> bytes,
    std::size_t offset) noexcept {
    std::uint64_t value = 0;
    for (std::size_t index = 0; index < 8U; ++index) {
        value |= static_cast<std::uint64_t>(bytes[offset + index]) << (index * 8U);
    }
    return value;
}

[[nodiscard]] std::uint32_t read_little_endian_u32(
    std::span<const std::uint8_t> bytes,
    std::size_t offset) noexcept {
    std::uint32_t value = 0;
    for (std::size_t index = 0; index < 4U; ++index) {
        value |= static_cast<std::uint32_t>(bytes[offset + index]) << (index * 8U);
    }
    return value;
}

[[nodiscard]] std::uint16_t read_little_endian_u16(
    std::span<const std::uint8_t> bytes,
    std::size_t offset) noexcept {
    return static_cast<std::uint16_t>(
        static_cast<std::uint16_t>(bytes[offset]) |
        (static_cast<std::uint16_t>(bytes[offset + 1U]) << 8U));
}

void receive_input_packets(
    ReliableByteStream& stream,
    std::atomic_bool& running,
    std::atomic_bool& keyframe_requested,
    WindowsInputInjector& injector,
    bool inject_input,
    InputReceiveStats& stats,
    DevelopmentStatusPublisher* status) {
    wireview::protocol::PacketStreamParser parser;
    std::array<std::uint8_t, 4U * 1024U> buffer{};
    while (running.load()) {
        if (!stream.wait_readable(std::chrono::milliseconds(200))) {
            continue;
        }

        const std::size_t received = stream.read(buffer);
        if (received == 0) {
            running.store(false);
            return;
        }
        if (!parser.push(std::span(buffer).first(received))) {
            throw std::runtime_error("WireView input receive buffer limit exceeded");
        }

        while (true) {
            auto parsed = parser.next();
            if (parsed.status == wireview::protocol::StreamStatus::need_more_data) {
                break;
            }
            if (parsed.status == wireview::protocol::StreamStatus::error || !parsed.packet) {
                throw std::runtime_error("WireView input packet parser failed");
            }
            if (parsed.packet->header.type == wireview::protocol::PacketType::keyframe_request) {
                keyframe_requested.store(true);
                std::cout << "Android requested H.264 recovery keyframe\n";
            } else if (parsed.packet->header.type == wireview::protocol::PacketType::input_batch) {
                ++stats.packets;
                const auto decoded = wireview::protocol::decode_input_batch(
                    parsed.packet->payload);
                if (!decoded.ok()) {
                    ++stats.rejected_packets;
                    std::cerr << "Rejected WireView input batch: "
                              << static_cast<int>(decoded.error) << '\n';
                    continue;
                }
                stats.records += decoded.batch->records.size();
                if (inject_input && !injector.inject_batch(*decoded.batch)) {
                    ++stats.injection_failures;
                }
            } else if (parsed.packet->header.type == wireview::protocol::PacketType::telemetry) {
                const auto payload = std::span(parsed.packet->payload);
                if (payload.size() == 16U && payload[0] == 1U && payload[1] == 1U) {
                    ++stats.first_frame_reports;
                    const std::uint32_t startup_millis =
                        read_little_endian_u32(payload, 12U);
                    std::cout << "Android first-frame telemetry: pts="
                              << read_little_endian_u64(payload, 4U)
                              << " | startupMs="
                              << startup_millis << '\n';
                    if (status != nullptr) {
                        status->first_frame(startup_millis);
                    }
                } else if (payload.size() == 36U && payload[0] == 1U && payload[1] == 2U) {
                    const std::uint64_t sequence = read_little_endian_u64(payload, 4U);
                    const std::uint64_t host_timestamp = read_little_endian_u64(payload, 12U);
                    const std::uint64_t arrival = read_little_endian_u64(payload, 20U);
                    const std::uint64_t release = read_little_endian_u64(payload, 28U);
                    if (release >= arrival) {
                        const double latency_millis =
                            static_cast<double>(release - arrival) / 1'000.0;
                        stats.android_surface_latency_millis.push_back(latency_millis);
                        if (status != nullptr) {
                            status->surface_sample(latency_millis);
                        }
                        std::cout << "Android sampled arrival-to-Surface: sequence="
                                  << sequence << " | " << latency_millis << " ms\n";
                        stats.frame_timings.push_back(InputReceiveStats::FrameTiming{
                            .sequence = sequence,
                            .host_timestamp_micros = host_timestamp,
                            .arrival_micros = arrival,
                            .surface_release_micros = release,
                        });
                    }
                } else if (payload.size() >= 16U && payload[0] == 1U && payload[1] == 3U) {
                    const std::uint16_t decoder_name_size =
                        read_little_endian_u16(payload, 12U);
                    if (decoder_name_size != payload.size() - 16U) {
                        std::cerr << "Rejected WireView Android capability payload\n";
                        continue;
                    }
                    const std::uint8_t flags = payload[2];
                    const std::string decoder_name(
                        reinterpret_cast<const char*>(payload.data() + 16U),
                        decoder_name_size);
                    const std::uint16_t android_width =
                        read_little_endian_u16(payload, 4U);
                    const std::uint16_t android_height =
                        read_little_endian_u16(payload, 6U);
                    const double refresh_hz = static_cast<double>(
                        read_little_endian_u16(payload, 8U)) / 100.0;
                    const double maximum_refresh_hz = static_cast<double>(
                        read_little_endian_u16(payload, 10U)) / 100.0;
                    std::cout << "Android capabilities: decoder=" << decoder_name
                              << " | hardware=" << ((flags & 1U) != 0U ? "yes" : "no")
                              << " | lowLatencyFlag=" << ((flags & 4U) != 0U ? "yes" : "no")
                              << " | display=" << android_width << 'x' << android_height
                              << '@' << refresh_hz << " Hz"
                              << " | maxRefresh=" << maximum_refresh_hz << " Hz\n";
                    if (status != nullptr) {
                        status->android_capabilities(
                            decoder_name,
                            (flags & 1U) != 0U,
                            (flags & 2U) != 0U,
                            (flags & 4U) != 0U,
                            (flags & 8U) != 0U,
                            android_width,
                            android_height,
                            refresh_hz,
                            maximum_refresh_hz);
                    }
                } else if (payload.size() == 32U && payload[0] == 1U && payload[1] == 4U) {
                    const std::uint16_t flags = read_little_endian_u16(payload, 2U);
                    const std::uint64_t received_frames =
                        read_little_endian_u64(payload, 4U);
                    const std::uint64_t queued_frames =
                        read_little_endian_u64(payload, 12U);
                    const std::uint64_t dropped_frames =
                        read_little_endian_u64(payload, 20U);
                    const std::uint32_t decoder_errors =
                        read_little_endian_u32(payload, 28U);
                    std::cout << "Android decoder health: received=" << received_frames
                              << " | queued=" << queued_frames
                              << " | inputDrops=" << dropped_frames
                              << " | errors=" << decoder_errors
                              << " | active=" << ((flags & 1U) != 0U ? "yes" : "no")
                              << " | firstFrame=" << ((flags & 2U) != 0U ? "yes" : "no")
                              << '\n';
                    if (status != nullptr) {
                        status->decoder_health(
                            received_frames,
                            queued_frames,
                            dropped_frames,
                            decoder_errors,
                            (flags & 1U) != 0U,
                            (flags & 2U) != 0U);
                    }
                } else {
                    std::cerr << "Rejected WireView telemetry payload\n";
                }
            } else if (parsed.packet->header.type == wireview::protocol::PacketType::pong) {
                const auto payload = std::span(parsed.packet->payload);
                const std::uint64_t received_at_host = monotonic_micros();
                if (payload.size() != 16U) {
                    std::cerr << "Rejected WireView pong payload\n";
                    continue;
                }
                const std::uint64_t sent_at_host = read_little_endian_u64(payload, 0U);
                const std::uint64_t received_at_android = read_little_endian_u64(payload, 8U);
                const std::uint64_t sent_at_android = parsed.packet->header.timestamp_micros;
                if (received_at_host < sent_at_host || sent_at_android < received_at_android) {
                    std::cerr << "Rejected WireView pong timing\n";
                    continue;
                }
                const double round_trip_micros =
                    static_cast<double>(received_at_host - sent_at_host) -
                    static_cast<double>(sent_at_android - received_at_android);
                if (round_trip_micros < 0.0) {
                    std::cerr << "Rejected WireView negative ping round trip\n";
                    continue;
                }
                const InputReceiveStats::ClockSyncSample sample{
                    .round_trip_millis = round_trip_micros / 1'000.0,
                    .android_minus_host_micros =
                        ((static_cast<double>(received_at_android) -
                          static_cast<double>(sent_at_host)) +
                         (static_cast<double>(sent_at_android) -
                          static_cast<double>(received_at_host))) /
                        2.0,
                    .midpoint_host_micros = sent_at_host +
                        ((received_at_host - sent_at_host) / 2U),
                };
                stats.clock_sync_samples.push_back(sample);
                if (!stats.best_clock_sync ||
                    sample.round_trip_millis < stats.best_clock_sync->round_trip_millis) {
                    stats.best_clock_sync = sample;
                }
            }
        }
    }
}

void send_desktop_stream(
    ReliableByteStream& stream,
    std::chrono::seconds duration,
    bool inject_input,
    std::uint16_t output_width,
    std::uint16_t output_height,
    std::uint16_t frame_rate,
    std::string_view capture_display_device,
    DevelopmentStatusPublisher* status) {
    std::uint64_t sequence = 2U;
    std::atomic_bool keyframe_requested{false};
    constexpr std::uint64_t kClockSyncPingSequenceBase = 0x5756520000000000ULL;
    std::uint64_t clock_sync_sequence = kClockSyncPingSequenceBase;
    // Windows headers define a max macro, so use min() here to schedule the
    // first in-stream calibration immediately without invoking that macro.
    auto next_clock_sync = (std::chrono::steady_clock::time_point::min)();
    const auto send_clock_sync_ping = [&] {
        const std::uint64_t sent_at_host = monotonic_micros();
        write_all(
            stream,
            wireview::protocol::encode_packet(
                wireview::protocol::PacketHeader{
                    .type = wireview::protocol::PacketType::ping,
                    .sequence = clock_sync_sequence++,
                    .timestamp_micros = sent_at_host,
                },
                {}));
    };
    bool stream_started = false;
    const DesktopStreamCallbacks callbacks{
        .on_started = [&](std::uint16_t width,
                          std::uint16_t height,
                          std::uint16_t frame_rate) {
            const auto config = make_avc_stream_config(width, height, frame_rate, 1U);
            write_all(
                stream,
                wireview::protocol::encode_packet(
                    wireview::protocol::PacketHeader{
                        .type = wireview::protocol::PacketType::start_stream,
                        .sequence = sequence++,
                        .timestamp_micros = monotonic_micros(),
                    },
                    config));
            stream_started = true;
            if (status != nullptr) {
                status->streaming(width, height, frame_rate);
            }
            std::cout << "Streaming the real desktop at " << width << 'x' << height
                      << '@' << frame_rate << " FPS for " << duration.count()
                      << " seconds\n";
        },
        .on_frame = [&](std::span<const std::uint8_t> bytes,
                        bool keyframe,
                        std::uint64_t timestamp_micros) {
            if (!stream_started) {
                throw std::runtime_error(
                    "desktop encoder produced a frame before stream setup");
            }
            const auto now = std::chrono::steady_clock::now();
            if (now >= next_clock_sync) {
                send_clock_sync_ping();
                next_clock_sync = now + std::chrono::seconds(5);
            }
            auto flags = wireview::protocol::PacketFlag::final_chunk;
            if (keyframe) {
                flags = flags | wireview::protocol::PacketFlag::keyframe;
            }
            write_all(
                stream,
                wireview::protocol::encode_packet(
                    wireview::protocol::PacketHeader{
                        .type = wireview::protocol::PacketType::video_chunk,
                        .flags = flags,
                        .sequence = sequence++,
                        .timestamp_micros = timestamp_micros,
                    },
                    bytes));
            if (status != nullptr) {
                status->delivered_frame(bytes.size());
            }
        },
        .consume_keyframe_request = [&] {
            return keyframe_requested.exchange(false);
        },
    };

    std::cout << "Capture target: "
              << (capture_display_device.empty()
                      ? std::string("primary display")
                      : std::string(capture_display_device))
              << '\n';
    const DesktopCaptureArea input_target = resolve_desktop_capture_area(capture_display_device);
    std::cout << "Input target: rect=" << input_target.left << ',' << input_target.top
              << '+' << input_target.width << 'x' << input_target.height << '\n';
    std::cout << "Input mode: "
              << (inject_input ? "Windows injection enabled" : "log-only") << '\n';
    WindowsInputInjector injector(input_target);
    std::atomic_bool input_running{true};
    std::exception_ptr input_error;
    InputReceiveStats input_stats;
    std::thread input_thread([&] {
        try {
            receive_input_packets(
                stream, input_running, keyframe_requested, injector, inject_input, input_stats, status);
        } catch (...) {
            input_error = std::current_exception();
            input_running.store(false);
        }
    });

    for (std::uint64_t index = 0U; index < 8U; ++index) {
        send_clock_sync_ping();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    next_clock_sync = std::chrono::steady_clock::now() + std::chrono::seconds(5);

    int encoder_result = 1;
    try {
        encoder_result = run_desktop_encode_session(
            duration,
            callbacks,
            output_width,
            output_height,
            frame_rate,
            capture_display_device);
    } catch (...) {
        input_running.store(false);
        input_thread.join();
        throw;
    }
    input_running.store(false);
    input_thread.join();
    if (input_error) {
        std::rethrow_exception(input_error);
    }
    if (encoder_result != 0) {
        throw std::runtime_error(
            "desktop encoder session failed: " + std::to_string(encoder_result));
    }
    write_all(
        stream,
        wireview::protocol::encode_packet(
            wireview::protocol::PacketHeader{
                .type = wireview::protocol::PacketType::stop_stream,
                .sequence = sequence,
                .timestamp_micros = monotonic_micros(),
            },
            {}));
    std::cout << "Real desktop stream completed\n";
    if (status != nullptr) {
        status->completed();
    }
    std::cout << "Android input: packets=" << input_stats.packets
              << " | records=" << input_stats.records
              << " | rejected=" << input_stats.rejected_packets;
    if (inject_input) {
        const auto& metrics = injector.metrics();
        const std::uint64_t average_micros = metrics.attempts == 0U
            ? 0U
            : metrics.total_micros / metrics.attempts;
        std::cout << " | injected=" << injector.injected_records()
                  << " | injectionFailures=" << input_stats.injection_failures;
        std::cout << "\nInput injection timing: attempts=" << metrics.attempts
                  << " | successful=" << metrics.successful
                  << " | finger=" << metrics.finger_records
                  << " | pen=" << metrics.pen_records
                  << " | mouse=" << metrics.mouse_records
                  << " | average=" << average_micros << " us"
                  << " | maximum=" << metrics.maximum_micros << " us";
    } else {
        std::cout << " | mode=log-only (no Windows input injected)";
    }
    std::cout << '\n';
    std::cout << "Android first-frame telemetry reports: "
              << input_stats.first_frame_reports << '\n';
    if (!input_stats.android_surface_latency_millis.empty()) {
        auto samples = input_stats.android_surface_latency_millis;
        std::sort(samples.begin(), samples.end());
        const auto percentile = [&samples](double fraction) {
            const std::size_t index = static_cast<std::size_t>(std::ceil(
                fraction * static_cast<double>(samples.size() - 1U)));
            return samples[index];
        };
        std::cout << "Android arrival-to-Surface latency: samples=" << samples.size()
                  << " | P50=" << percentile(0.50)
                  << " ms | P95=" << percentile(0.95) << " ms\n";
    }
    if (input_stats.best_clock_sync) {
        const auto& sync = *input_stats.best_clock_sync;
        std::cout << "Clock sync: samples=" << input_stats.clock_sync_samples.size()
                  << " | bestRoundTrip=" << sync.round_trip_millis
                  << " ms | androidMinusHost=" << sync.android_minus_host_micros / 1'000.0
                  << " ms\n";
    }
    if (!input_stats.frame_timings.empty() && !input_stats.clock_sync_samples.empty()) {
        std::vector<double> samples;
        samples.reserve(input_stats.frame_timings.size());
        for (const auto& frame : input_stats.frame_timings) {
            const InputReceiveStats::ClockSyncSample* selected = nullptr;
            double selected_distance_micros = 0.0;
            for (const auto& sample : input_stats.clock_sync_samples) {
                const double distance_micros = std::abs(
                    static_cast<double>(sample.midpoint_host_micros) -
                    static_cast<double>(frame.host_timestamp_micros));
                const bool in_local_window = distance_micros <= 10'000'000.0;
                if (selected == nullptr ||
                    (in_local_window && selected_distance_micros > 10'000'000.0) ||
                    (in_local_window && selected_distance_micros <= 10'000'000.0 &&
                     sample.round_trip_millis < selected->round_trip_millis) ||
                    (!in_local_window && selected_distance_micros > 10'000'000.0 &&
                     distance_micros < selected_distance_micros)) {
                    selected = &sample;
                    selected_distance_micros = distance_micros;
                }
            }
            const double end_to_end_millis =
                (static_cast<double>(frame.surface_release_micros) -
                 static_cast<double>(frame.host_timestamp_micros) -
                 selected->android_minus_host_micros) /
                1'000.0;
            if (end_to_end_millis >= 0.0) {
                samples.push_back(end_to_end_millis);
            }
        }
        if (samples.empty()) {
            std::cout << "End-to-end frame latency: no non-negative samples\n";
        } else {
            std::sort(samples.begin(), samples.end());
            const auto percentile = [&samples](double fraction) {
                const std::size_t index = static_cast<std::size_t>(std::ceil(
                    fraction * static_cast<double>(samples.size() - 1U)));
                return samples[index];
            };
            std::cout << "End-to-end frame latency: samples=" << samples.size()
                      << " | P50=" << percentile(0.50)
                      << " ms | P95=" << percentile(0.95) << " ms\n";
        }
    }
}

}  // namespace

namespace {

int run_connected_stream_impl(
    ReliableByteStream& stream,
    const std::string* annex_b_path,
    std::uint16_t width,
    std::uint16_t height,
    std::uint16_t frame_rate,
    std::chrono::seconds desktop_stream_duration,
    bool inject_input,
    std::uint16_t output_width,
    std::uint16_t output_height,
    std::uint16_t desktop_frame_rate,
    std::string_view capture_display_device,
    DevelopmentStatusPublisher* status) {
    const std::string hello_text = "WireView Windows phase-0";
    const std::vector<std::uint8_t> hello_payload(hello_text.begin(), hello_text.end());
    const wireview::protocol::PacketHeader hello_header{
        .type = wireview::protocol::PacketType::hello,
        .flags = wireview::protocol::PacketFlag::acknowledgement_required,
        .sequence = 1,
        .timestamp_micros = monotonic_micros(),
    };
    write_all(stream, wireview::protocol::encode_packet(hello_header, hello_payload));

    const auto acknowledgement = read_packet(stream, std::chrono::seconds(10));
    if (acknowledgement.header.type != wireview::protocol::PacketType::hello_ack) {
        throw std::runtime_error("client did not return HELLO_ACK");
    }
    const AndroidDeviceProfile client_profile = parse_hello_ack(acknowledgement.payload);
    const std::string& client_name = client_profile.client_name;
    std::cout << "WireView handshake complete with: " << client_name << '\n';
    if (client_profile.available) {
        std::cout << "Android device profile: " << client_profile.width << 'x'
                  << client_profile.height << " @ " << client_profile.active_refresh_hz
                  << " Hz (max " << client_profile.maximum_refresh_hz << " Hz)\n";
    }
    if (status != nullptr) {
        status->connected(client_name);
        if (client_profile.available) {
            status->device_profile(
                client_profile.width, client_profile.height, client_profile.active_refresh_hz,
                client_profile.maximum_refresh_hz, client_profile.manufacturer,
                client_profile.tablet);
        }
    }
    if (annex_b_path != nullptr) {
        send_test_stream(stream, *annex_b_path, width, height, frame_rate);
    } else if (desktop_stream_duration > std::chrono::seconds::zero()) {
        send_desktop_stream(
            stream,
            desktop_stream_duration,
            inject_input,
            output_width,
            output_height,
            desktop_frame_rate,
            capture_display_device,
            status);
    }
    return 0;
}

int run_server_impl(
    std::uint16_t port,
    std::chrono::milliseconds accept_timeout,
    const std::string* annex_b_path,
    std::uint16_t width,
    std::uint16_t height,
    std::uint16_t frame_rate,
    std::chrono::seconds desktop_stream_duration,
    bool inject_input,
    std::uint16_t output_width,
    std::uint16_t output_height,
    std::uint16_t desktop_frame_rate,
    std::string_view capture_display_device,
    DevelopmentStatusPublisher* status) {
    WinsockSession winsock;
    auto listener = create_tcp_socket();
    allow_listener_rebind(listener.get());

    const auto address = loopback_address(port);
    if (bind(
            listener.get(),
            reinterpret_cast<const sockaddr*>(&address),
            sizeof(address)) == SOCKET_ERROR) {
        throw_socket_error("bind");
    }
    if (listen(listener.get(), 1) == SOCKET_ERROR) {
        throw_socket_error("listen");
    }

    std::cout << "WireView development host listening on 127.0.0.1:" << port << '\n';
    std::cout << "Android setup: adb reverse tcp:" << port << " tcp:" << port << '\n';
    if (status != nullptr) {
        status->waiting_for_client();
    }

    fd_set readable{};
    FD_ZERO(&readable);
    FD_SET(listener.get(), &readable);
    timeval timeout{};
    timeout.tv_sec = static_cast<long>(accept_timeout.count() / 1'000);
    timeout.tv_usec = static_cast<long>((accept_timeout.count() % 1'000) * 1'000);
    const int ready = select(0, &readable, nullptr, nullptr, &timeout);
    if (ready == SOCKET_ERROR) {
        throw_socket_error("select");
    }
    if (ready == 0) {
        std::cerr << "No WireView client connected before the timeout\n";
        return 2;
    }

    SocketHandle client(accept(listener.get(), nullptr, nullptr));
    if (!client.valid()) {
        throw_socket_error("accept");
    }
    set_low_latency_socket_options(client.get());
    SocketByteStream stream(client.get());
    return run_connected_stream_impl(
        stream,
        annex_b_path,
        width,
        height,
        frame_rate,
        desktop_stream_duration,
        inject_input,
        output_width,
        output_height,
        desktop_frame_rate,
        capture_display_device,
        status);
}

}  // namespace

int run_development_server(
    std::uint16_t port,
    std::chrono::milliseconds accept_timeout) {
    return run_server_impl(
        port,
        accept_timeout,
        nullptr,
        0U,
        0U,
        0U,
        std::chrono::seconds::zero(),
        true,
        0U,
        0U,
        60U,
        {},
        nullptr);
}

int run_development_stream_server(
    std::uint16_t port,
    std::chrono::milliseconds accept_timeout,
    const std::string& annex_b_path,
    std::uint16_t width,
    std::uint16_t height,
    std::uint16_t frame_rate) {
    if (width == 0U || height == 0U || frame_rate == 0U) {
        throw std::invalid_argument("stream dimensions and frame rate must be non-zero");
    }
    return run_server_impl(
        port,
        accept_timeout,
        &annex_b_path,
        width,
        height,
        frame_rate,
        std::chrono::seconds::zero(),
        true,
        0U,
        0U,
        60U,
        {},
        nullptr);
}

int run_device_preview_server(
    std::uint16_t port,
    std::chrono::milliseconds accept_timeout,
    std::chrono::seconds hold_duration,
    std::uint16_t width,
    std::uint16_t height,
    std::uint16_t frame_rate,
    std::string_view status_file,
    bool draw_preview) {
    DevelopmentStatusPublisher status(status_file);
    if (!status_file.empty()) {
        status.configure("device preview", false, width, height, frame_rate, 0U, "adb");
        status.waiting_for_client();
    }
    if ((width == 0U) != (height == 0U) || frame_rate == 0U) {
        throw std::invalid_argument("device preview size must be WIDTHxHEIGHT or native auto");
    }
    WinsockSession winsock;
    auto listener = create_tcp_socket();
    allow_listener_rebind(listener.get());
    const auto address = loopback_address(port);
    if (bind(listener.get(), reinterpret_cast<const sockaddr*>(&address), sizeof(address)) ==
        SOCKET_ERROR) {
        throw_socket_error("bind");
    }
    if (listen(listener.get(), 1) == SOCKET_ERROR) {
        throw_socket_error("listen");
    }
    std::cout << "WireView device preview listening on 127.0.0.1:" << port << '\n';
    fd_set readable{};
    FD_ZERO(&readable);
    FD_SET(listener.get(), &readable);
    timeval timeout{};
    timeout.tv_sec = static_cast<long>(accept_timeout.count() / 1'000);
    timeout.tv_usec = static_cast<long>((accept_timeout.count() % 1'000) * 1'000);
    const int ready = select(0, &readable, nullptr, nullptr, &timeout);
    if (ready == SOCKET_ERROR) {
        throw_socket_error("select");
    }
    if (ready == 0) {
        std::cerr << "No WireView client connected before the preview timeout\n";
        return 2;
    }
    SocketHandle client(accept(listener.get(), nullptr, nullptr));
    if (!client.valid()) {
        throw_socket_error("accept");
    }
    set_low_latency_socket_options(client.get());
    SocketByteStream stream(client.get());
    const std::string hello_text = "WireView device preview";
    const std::vector<std::uint8_t> hello_payload(hello_text.begin(), hello_text.end());
    write_all(stream, wireview::protocol::encode_packet(
        wireview::protocol::PacketHeader{
            .type = wireview::protocol::PacketType::hello,
            .flags = wireview::protocol::PacketFlag::acknowledgement_required,
            .sequence = 1U,
            .timestamp_micros = monotonic_micros(),
        }, hello_payload));
    const auto acknowledgement = read_packet(stream, std::chrono::seconds(10));
    if (acknowledgement.header.type != wireview::protocol::PacketType::hello_ack) {
        throw std::runtime_error("client did not return HELLO_ACK for device preview");
    }
    const AndroidDeviceProfile profile = parse_hello_ack(acknowledgement.payload);
    std::cout << "WireView preview connected with: " << profile.client_name << '\n';
    if (profile.available) {
        std::cout << "Android device profile: " << profile.width << 'x' << profile.height
                  << " @ " << profile.active_refresh_hz << " Hz (max "
                  << profile.maximum_refresh_hz << " Hz)\n";
        status.device_profile(
            profile.width, profile.height, profile.active_refresh_hz, profile.maximum_refresh_hz,
            profile.manufacturer, profile.tablet);
    }
    status.connected(profile.client_name);
    if (draw_preview) {
        write_device_preview(stream, width, height, frame_rate);
    }
    std::cout << "Device preview active: "
              << (width == 0U ? std::string("native auto")
                               : std::to_string(width) + "x" + std::to_string(height))
              << " @ " << frame_rate << " FPS\n";
    if (hold_duration > std::chrono::seconds::zero()) {
        std::this_thread::sleep_for(hold_duration);
    }
    return 0;
}

int run_development_desktop_stream_server(
    std::uint16_t port,
    std::chrono::milliseconds accept_timeout,
    std::chrono::seconds stream_duration,
    bool inject_input,
    std::uint16_t output_width,
    std::uint16_t output_height,
    std::uint16_t frame_rate,
    std::string_view capture_display_device,
    std::string_view status_file,
    std::uint64_t reconnect_count) {
    if (stream_duration <= std::chrono::seconds::zero()) {
        throw std::invalid_argument("desktop stream duration must be positive");
    }
    if (frame_rate != 60U && frame_rate != 120U) {
        throw std::invalid_argument("desktop stream frame rate must be 60 or 120");
    }
    DevelopmentStatusPublisher status(status_file);
    status.configure(
        capture_display_device,
        inject_input,
        output_width,
        output_height,
        frame_rate,
        reconnect_count,
        "adb");
    return run_server_impl(
        port,
        accept_timeout,
        nullptr,
        0U,
        0U,
        0U,
        stream_duration,
        inject_input,
        output_width,
        output_height,
        frame_rate,
        capture_display_device,
        &status);
}

int run_winusb_desktop_stream_session(
    std::chrono::milliseconds accessory_wait_timeout,
    std::chrono::seconds stream_duration,
    bool inject_input,
    std::uint16_t output_width,
    std::uint16_t output_height,
    std::uint16_t frame_rate,
    std::string_view capture_display_device,
    std::string_view status_file,
    std::uint64_t reconnect_count) {
    if (accessory_wait_timeout < std::chrono::milliseconds::zero()) {
        throw std::invalid_argument("WinUSB accessory wait timeout must not be negative");
    }
    if (stream_duration <= std::chrono::seconds::zero()) {
        throw std::invalid_argument("WinUSB desktop stream duration must be positive");
    }
    if (frame_rate != 60U && frame_rate != 120U) {
        throw std::invalid_argument("WinUSB desktop stream frame rate must be 60 or 120");
    }

    DevelopmentStatusPublisher status(status_file);
    status.configure(
        capture_display_device,
        inject_input,
        output_width,
        output_height,
        frame_rate,
        reconnect_count,
        "winusb");
    status.waiting_for_usb_accessory();
    std::cout
        << "Waiting for exactly one already-enumerated WireView AOA accessory. "
           "This command does not switch Android USB mode or modify drivers.\n";
    auto accessory = wait_for_unique_aoa_accessory(accessory_wait_timeout);
    if (!accessory) {
        std::cerr << "No WireView AOA accessory appeared before the timeout\n";
        return 2;
    }
    std::wcout << L"Opening WireView AOA accessory:\n"
               << L"  name: " << accessory->name << L'\n'
               << L"  instance: " << accessory->instance_id << L'\n';
    auto stream = WinUsbByteStream::open(accessory->path);
    return run_connected_stream_impl(
        *stream,
        nullptr,
        0U,
        0U,
        0U,
        stream_duration,
        inject_input,
        output_width,
        output_height,
        frame_rate,
        capture_display_device,
        &status);
}

int run_development_reconnecting_desktop_stream_server(
    std::uint16_t port,
    std::chrono::milliseconds accept_timeout,
    std::chrono::seconds total_duration,
    bool inject_input,
    std::uint16_t output_width,
    std::uint16_t output_height,
    std::uint16_t frame_rate,
    std::string_view capture_display_device,
    std::string_view status_file) {
    if (total_duration <= std::chrono::seconds::zero()) {
        throw std::invalid_argument("reconnecting desktop stream duration must be positive");
    }
    if (frame_rate != 60U && frame_rate != 120U) {
        throw std::invalid_argument("reconnecting desktop stream frame rate must be 60 or 120");
    }

    DevelopmentStatusPublisher status(status_file);
    status.configure(
        capture_display_device,
        inject_input,
        output_width,
        output_height,
        frame_rate,
        0U,
        "adb");
    const auto deadline = std::chrono::steady_clock::now() + total_duration;
    std::uint64_t interrupted_sessions = 0;
    while (std::chrono::steady_clock::now() < deadline) {
        const auto remaining = std::chrono::duration_cast<std::chrono::seconds>(
            deadline - std::chrono::steady_clock::now());
        if (remaining <= std::chrono::seconds::zero()) {
            break;
        }
        // A named target is a hard product requirement. Re-resolve it before
        // every session so reconnect can never silently fall back to primary.
        if (!capture_display_device.empty()) {
            (void)resolve_desktop_capture_area(capture_display_device);
        }
        try {
            const int result = run_server_impl(
                port,
                accept_timeout,
                nullptr,
                0U,
                0U,
                0U,
                remaining,
                inject_input,
                output_width,
                output_height,
                frame_rate,
                capture_display_device,
                &status);
            if (result == 0) {
                break;
            }
        } catch (const std::exception& error) {
            // If the selected display disappeared during the interrupted
            // session, propagate the target error instead of retrying primary.
            if (!capture_display_device.empty()) {
                (void)resolve_desktop_capture_area(capture_display_device);
            }
            ++interrupted_sessions;
            status.interrupted(error.what());
            std::cerr << "WireView desktop session interrupted: " << error.what()
                      << ". Waiting for Android to reconnect...\n";
        }
    }
    std::cout << "WireView reconnecting desktop server completed; interrupted sessions: "
              << interrupted_sessions << '\n';
    return 0;
}

int run_development_client(std::uint16_t port, std::string_view client_name) {
    WinsockSession winsock;
    auto socket = create_tcp_socket();
    const auto address = loopback_address(port);
    if (connect(
            socket.get(),
            reinterpret_cast<const sockaddr*>(&address),
            sizeof(address)) == SOCKET_ERROR) {
        throw_socket_error("connect");
    }
    set_low_latency_socket_options(socket.get());
    SocketByteStream stream(socket.get());

    const auto hello = read_packet(stream);
    if (hello.header.type != wireview::protocol::PacketType::hello) {
        throw std::runtime_error("server did not start with HELLO");
    }

    const std::vector<std::uint8_t> payload(client_name.begin(), client_name.end());
    const wireview::protocol::PacketHeader acknowledgement{
        .type = wireview::protocol::PacketType::hello_ack,
        .sequence = hello.header.sequence,
        .timestamp_micros = monotonic_micros(),
    };
    write_all(stream, wireview::protocol::encode_packet(acknowledgement, payload));
    return 0;
}

}  // namespace wireview::host
