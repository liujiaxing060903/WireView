#include "development_transport.hpp"

// WinSock2 must precede any header that pulls in <Windows.h>.
#include <WinSock2.h>
#include <WS2tcpip.h>

#include "annex_b_stream.hpp"
#include "bitrate_governor.hpp"
#include "input_injector.hpp"
#include "reliable_byte_stream.hpp"
#include "socket_stream.hpp"
#include "tablet_commands.hpp"
#include "usb_accessory_discovery.hpp"
#include "wifi_crypto.hpp"
#include "wifi_link.hpp"
#include "winusb_byte_stream.hpp"
#include "windows_video.hpp"

#include "wireview/protocol.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
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

// R3 adaptive bitrate bounds for the WiFi transport (framerate first):
// the ceiling is the measured 2026-11-06 WiFi budget (<= 60 Mbps); the
// floor keeps H.264 at 1080p legible. USB keeps the fixed 24 Mbps start.
constexpr std::uint32_t kAdaptiveMinBitrateBps = 6'000'000U;
constexpr std::uint32_t kAdaptiveMaxBitrateBps = 60'000'000U;
constexpr std::uint32_t kAdaptiveInitialBitrateBps = 24'000'000U;

namespace {

class DevelopmentStatusPublisher {
public:
    explicit DevelopmentStatusPublisher(std::string_view path)
        : path_(path.empty() ? std::filesystem::path{} : std::filesystem::path(path)) {
        // The status file is rewritten from a dedicated thread: the encoder
        // thread used to pay a synchronous WRITE_THROUGH flush once per second
        // (audit item P4), turning disk stalls into dropped beats.
        if (!path_.empty()) {
            publisher_ = std::thread([this] { publish_worker(); });
        }
    }

    ~DevelopmentStatusPublisher() {
        {
            std::lock_guard lock(mutex_);
            publish_stopped_ = true;
        }
        publish_cv_.notify_all();
        if (publisher_.joinable()) {
            publisher_.join();
        }
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
        // Each (re)connected session is a fresh measurement window: drop the
        // samples and the publish throttle of the previous session instead of
        // mixing percentiles across reconnects.
        surface_samples_.clear();
        last_surface_publish_ = std::chrono::steady_clock::time_point{};
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
        // Sliding window: this publisher lives across the whole reconnecting
        // session while surface telemetry arrives per rendered frame (up to
        // 60+ per second), so an unbounded vector would grow for days.
        if (surface_samples_.size() >= kMaxSurfaceSamples) {
            surface_samples_.erase(surface_samples_.begin());
        }
        surface_samples_.push_back(latency_millis);
        // Recomputing percentiles costs a full copy + sort and publishing
        // costs a status-file write; once per second is enough for readers.
        const auto now = std::chrono::steady_clock::now();
        if (now - last_surface_publish_ <= std::chrono::seconds(1)) {
            return;
        }
        last_surface_publish_ = now;
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

    void publish_locked() {
        if (path_.empty()) {
            return;
        }
        // Serialize the report under the state mutex and hand it to the
        // publisher thread; the file I/O itself never runs on the caller
        // (encoder) thread.
        std::ostringstream report;
        report << "version=1\n"
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
        pending_publish_ = report.str();
        publish_dirty_ = true;
        publish_cv_.notify_one();
    }

    void publish_worker() {
        std::unique_lock lock(mutex_);
        while (true) {
            publish_cv_.wait(lock, [this] {
                return publish_dirty_ || publish_stopped_;
            });
            const bool dirty = publish_dirty_;
            publish_dirty_ = false;
            if (publish_stopped_) {
                if (dirty && !pending_publish_.empty()) {
                    write_status_file(pending_publish_);
                }
                return;
            }
            std::string content = std::move(pending_publish_);
            lock.unlock();
            write_status_file(content);
            lock.lock();
        }
    }

    void write_status_file(const std::string& content) const {
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
            std::ofstream output(temporary, std::ios::trunc | std::ios::binary);
            if (!output) {
                return;
            }
            output << content;
            output.flush();
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
    static constexpr std::size_t kMaxSurfaceSamples = 512U;
    std::vector<double> surface_samples_;
    std::chrono::steady_clock::time_point last_surface_publish_{};
    double surface_p50_millis_{};
    double surface_p95_millis_{};
    double output_frame_rate_{};
    double bitrate_mbps_{};
    double android_refresh_hz_{};
    double android_max_refresh_hz_{};
    std::chrono::steady_clock::time_point stream_started_at_{};
    std::thread publisher_;
    std::condition_variable publish_cv_;
    bool publish_dirty_{false};
    bool publish_stopped_{false};
    std::string pending_publish_;
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

// R3 adaptive bitrate: shared between the receive thread (which diffs the
// decoder health telemetry) and the encoder thread (which consumes the
// target). The parent keeps this object alive for both threads' lifetimes.
struct AdaptiveBitrateState {
    BitrateGovernor governor;
    std::atomic_bool active{false};
    std::atomic_uint32_t target{0};
    std::uint64_t base_received = 0;
    std::uint64_t base_dropped = 0;
    std::uint32_t base_errors = 0;
    bool base_valid = false;
    std::mutex base_mutex;

    explicit AdaptiveBitrateState(std::uint32_t initial_bps)
        : governor(kAdaptiveMinBitrateBps, kAdaptiveMaxBitrateBps, initial_bps) {
    }

    void on_health(
        std::uint64_t received_frames,
        std::uint64_t queued_frames,
        std::uint64_t dropped_frames,
        std::uint32_t decoder_errors) {
        if (!active.load(std::memory_order_relaxed)) {
            return;
        }
        std::uint32_t command = 0;
        {
            std::lock_guard lock(base_mutex);
            if (base_valid) {
                const std::uint32_t delivered_delta = static_cast<std::uint32_t>(
                    received_frames >= base_received ? received_frames - base_received : 0U);
                const std::uint32_t dropped_delta = static_cast<std::uint32_t>(
                    dropped_frames >= base_dropped ? dropped_frames - base_dropped : 0U);
                const std::uint32_t errors_delta =
                    decoder_errors >= base_errors ? decoder_errors - base_errors : 0U;
                command = governor.on_sample(
                    delivered_delta, dropped_delta,
                    static_cast<std::uint32_t>(queued_frames), errors_delta);
            }
            base_received = received_frames;
            base_dropped = dropped_frames;
            base_errors = decoder_errors;
            base_valid = true;
        }
        if (command != 0U) {
            target.store(command, std::memory_order_relaxed);
            // The governor is otherwise silent; without this line field
            // debugging cannot tell whether it ever adjusted the bitrate.
            std::cout << "adaptive bitrate -> " << (command / 1'000'000.0)
                      << " Mbps\n";
        }
    }

    [[nodiscard]] std::uint32_t consume() {
        return active.load(std::memory_order_relaxed) ? target.exchange(0U) : 0U;
    }
};

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
    DevelopmentStatusPublisher* status,
    const std::function<void(
        wireview::protocol::PacketType, std::vector<std::uint8_t>&&)>& send_reply,
    const std::wstring& vdd_device_name,
    AdaptiveBitrateState* adaptive) {
    wireview::protocol::PacketStreamParser parser;
    std::vector<wireview::host::TabletWindowEntry> window_entries;
    std::uint32_t window_list_token = 0U;
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
                    if (adaptive != nullptr) {
                        adaptive->on_health(
                            received_frames, queued_frames, dropped_frames, decoder_errors);
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
            } else if (parsed.packet->header.type ==
                       wireview::protocol::PacketType::control_command) {
            // Tablet sidebar control commands (type 13). Handled inline on the
            // receive thread; replies go through send_reply, which shares the
            // send-side write mutex with the frame sender thread.
            const auto payload = std::span(parsed.packet->payload);
            wireview::host::TabletCommand command{};
            if (!wireview::host::decode_control_command(payload, command)) {
                std::cerr << "Rejected WireView control command payload\n";
                continue;
            }
            std::cout << "Tablet control command: "
                      << static_cast<int>(payload[0]) << '\n';
            switch (command) {
                case wireview::host::TabletCommand::escape:
                    if (!wireview::host::inject_escape_key()) {
                        std::cerr << "Failed to inject the ESC key\n";
                    }
                    break;
                case wireview::host::TabletCommand::rotate_orientation:
                    wireview::host::request_control_center_rotation();
                    break;
                case wireview::host::TabletCommand::request_window_list: {
                    window_list_token += 1U;
                    window_entries = wireview::host::enumerate_streaming_windows(
                        vdd_device_name);
                    std::cout << "Tablet window list: " << window_entries.size()
                              << " windows | token " << window_list_token << '\n';
                    send_reply(
                        wireview::protocol::PacketType::window_list,
                        wireview::host::encode_window_list(
                            window_list_token, window_entries));
                    break;
                }
                case wireview::host::TabletCommand::ime_text: {
                    std::wstring text;
                    if (!wireview::host::decode_ime_text(payload, text)) {
                        std::cerr << "Rejected WireView IME text payload\n";
                        break;
                    }
                    if (!wireview::host::inject_unicode_text(text)) {
                        std::cerr << "Failed to inject the IME text ("
                                  << text.size() << " chars)\n";
                    } else {
                        std::cout << "Tablet IME text injected: " << text.size()
                                  << " chars\n";
                    }
                    break;
                }
                case wireview::host::TabletCommand::ime_key: {
                    UINT virtual_key = 0U;
                    if (!wireview::host::decode_ime_key(payload, virtual_key)) {
                        std::cerr << "Rejected WireView IME key payload\n";
                        break;
                    }
                    if (!wireview::host::inject_virtual_key(virtual_key)) {
                        std::cerr << "Failed to inject the IME key "
                                  << virtual_key << '\n';
                    } else {
                        std::cout << "Tablet IME key injected: " << virtual_key << '\n';
                    }
                    break;
                }
                case wireview::host::TabletCommand::move_window: {
                    std::uint32_t token = 0U;
                    std::uint32_t index = 0U;
                    std::uint8_t target_area = 0U;
                    if (!wireview::host::decode_window_move(
                            payload, token, index, target_area)) {
                        std::cerr << "Rejected WireView window move payload\n";
                        break;
                    }
                    if (token != window_list_token || index >= window_entries.size()) {
                        std::cerr << "Rejected stale WireView window move request\n";
                        break;
                    }
                    const bool moved = wireview::host::move_window_to_area(
                        window_entries[index].window, target_area, vdd_device_name);
                    std::cout << "Tablet window move: index=" << index
                              << " area=" << static_cast<int>(target_area)
                              << " -> " << (moved ? "moved" : "failed") << '\n';
                    break;
                }
                default:
                    std::cerr << "Ignored unknown WireView control command "
                              << static_cast<int>(payload[0]) << '\n';
                    break;
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
    DevelopmentStatusPublisher* status,
    bool adaptive_bitrate) {
    std::uint64_t sequence = 2U;
    std::atomic_bool keyframe_requested{false};
    // R3 adaptive bitrate (WiFi only): the receive thread diffs the decoder
    // health telemetry once per second; the encoder thread consumes the
    // governor's target before each frame. Lives on this frame: the input
    // thread is joined before this function returns.
    AdaptiveBitrateState adaptive_state(
        kAdaptiveInitialBitrateBps * frame_rate / 60U);
    adaptive_state.active.store(adaptive_bitrate, std::memory_order_relaxed);
    constexpr std::uint64_t kClockSyncPingSequenceBase = 0x5756520000000000ULL;
    std::uint64_t clock_sync_sequence = kClockSyncPingSequenceBase;
    // Windows headers define a max macro, so use min() here to schedule the
    // first in-stream calibration immediately without invoking that macro.
    auto next_clock_sync = (std::chrono::steady_clock::time_point::min)();

    // Dedicated send thread (audit item P3): the encoder thread used to block
    // in write_all on every frame, so an adb/USB stall translated directly
    // into missed beats. Frames now flow through a bounded queue; overflow
    // drops the oldest frame and the Android pipeline resyncs (drain-to-
    // latest + watchdog). All stream writes share write_mutex because the
    // receive thread answers window-list requests on the same socket.
    std::mutex write_mutex;
    std::mutex queue_mutex;
    std::condition_variable queue_cv;
    std::deque<std::vector<std::uint8_t>> outbound;
    bool sender_running{true};
    std::exception_ptr sender_error;
    std::uint64_t dropped_frames{0};
    std::uint64_t slow_writes{0};
    constexpr std::size_t kMaxQueuedFrames = 4;

    // Resolve the capture target and build the input injector BEFORE any
    // thread is created: resolve_desktop_capture_area throws when the named
    // display just vanished, and an exception escaping while sender_thread
    // is joinable would unwind it and std::terminate the whole host (audit
    // item P1). The sender lambda below only touches the queue, the two
    // mutexes and the stop flag, so it has no dependency on these locals.
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
    // input_running must also predate the callbacks below: on_frame checks
    // it so the encode loop stops once the input thread is gone (audit P3).
    std::atomic_bool input_running{true};
    std::exception_ptr input_error;
    InputReceiveStats input_stats;

    std::thread sender_thread([&] {
        auto last_slow_log = std::chrono::steady_clock::now() - std::chrono::seconds(10);
        for (;;) {
            std::vector<std::uint8_t> bytes;
            {
                std::unique_lock lock(queue_mutex);
                queue_cv.wait(lock, [&] {
                    return !outbound.empty() || !sender_running;
                });
                if (outbound.empty()) {
                    return;
                }
                bytes = std::move(outbound.front());
                outbound.pop_front();
            }
            const auto write_started = std::chrono::steady_clock::now();
            try {
                std::lock_guard lock(write_mutex);
                write_all(stream, bytes);
            } catch (...) {
                std::lock_guard lock(queue_mutex);
                sender_error = std::current_exception();
                sender_running = false;
                queue_cv.notify_all();
                return;
            }
            const auto write_elapsed = std::chrono::steady_clock::now() - write_started;
            if (write_elapsed > std::chrono::milliseconds(5)) {
                ++slow_writes;
                const auto now = std::chrono::steady_clock::now();
                if (now - last_slow_log > std::chrono::seconds(5)) {
                    last_slow_log = now;
                    std::cerr << "WireView frame send stalled for "
                              << std::chrono::duration<double, std::milli>(
                                  write_elapsed)
                                  .count()
                              << " ms\n";
                }
            }
        }
    });

    // Enqueue an encoded packet on the wire. Called from the encoder thread
    // (clock-sync pings, stream config, video frames).
    auto enqueue_outbound = [&](std::vector<std::uint8_t>&& bytes) {
        std::lock_guard lock(queue_mutex);
        if (!sender_running) {
            if (sender_error) {
                std::rethrow_exception(sender_error);
            }
            throw std::runtime_error("wireview send thread stopped");
        }
        if (outbound.size() >= kMaxQueuedFrames) {
            // Control packets (start_stream config, clock-sync pings) must
            // never be evicted by video: losing start_stream leaves the
            // tablet decoding nothing for the rest of the session (there is
            // no config re-send in the protocol). Drop the oldest queued
            // video chunk instead (the packet type byte sits at offset 6,
            // after the 4-byte magic and 2 version bytes) and only fall back
            // to the queue head when no frame is queued at all.
            const auto victim = std::find_if(
                outbound.begin(), outbound.end(),
                [](const std::vector<std::uint8_t>& queued) {
                    return queued.size() > 6U &&
                        queued[6] == static_cast<std::uint8_t>(
                                         wireview::protocol::PacketType::video_chunk);
                });
            outbound.erase(victim != outbound.end() ? victim : outbound.begin());
            ++dropped_frames;
        }
        outbound.push_back(std::move(bytes));
        queue_cv.notify_one();
    };

    const auto send_clock_sync_ping = [&] {
        const std::uint64_t sent_at_host = monotonic_micros();
        enqueue_outbound(
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
            enqueue_outbound(
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
            // The input thread also carries keyframe requests, telemetry and
            // input injection; once it is gone the session cannot recover,
            // so fail fast instead of streaming blind until the duration
            // expires. Throwing here ends the session through the existing
            // catch path and feeds the reconnect loop (audit item P3).
            if (!input_running.load()) {
                throw std::runtime_error("input thread is gone; ending the stream session");
            }
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
            enqueue_outbound(
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
        .consume_bitrate_command = [&] {
            return adaptive_state.consume();
        },
    };

    auto stop_sender = [&] {
        {
            std::lock_guard lock(queue_mutex);
            sender_running = false;
        }
        queue_cv.notify_all();
        if (sender_thread.joinable()) {
            sender_thread.join();
        }
    };

    // The VDD device name classifies window-list entries (primary vs. the
    // WireView display vs. other monitors).
    std::wstring vdd_device_name_wide;
    if (!capture_display_device.empty()) {
        const int wide_length = MultiByteToWideChar(
            CP_UTF8, 0, capture_display_device.data(),
            static_cast<int>(capture_display_device.size()), nullptr, 0);
        if (wide_length > 0) {
            vdd_device_name_wide.resize(static_cast<std::size_t>(wide_length));
            MultiByteToWideChar(
                CP_UTF8, 0, capture_display_device.data(),
                static_cast<int>(capture_display_device.size()),
                vdd_device_name_wide.data(), wide_length);
        }
    }
    // PC -> Android replies (window list) share the send-side write mutex
    // with the frame sender thread.
    std::atomic_uint64_t reply_sequence{0x5756530000000000ULL};
    const auto send_reply = [&](wireview::protocol::PacketType type,
                                std::vector<std::uint8_t>&& payload) {
        std::lock_guard lock(write_mutex);
        write_all(
            stream,
            wireview::protocol::encode_packet(
                wireview::protocol::PacketHeader{
                    .type = type,
                    .sequence = reply_sequence.fetch_add(1U),
                    .timestamp_micros = monotonic_micros(),
                },
                payload));
    };
    std::thread input_thread([&] {
        try {
            receive_input_packets(
                stream,
                input_running,
                keyframe_requested,
                injector,
                inject_input,
                input_stats,
                status,
                send_reply,
                vdd_device_name_wide,
                &adaptive_state);
        } catch (...) {
            input_error = std::current_exception();
            input_running.store(false);
        }
    });

    try {
        for (std::uint64_t index = 0U; index < 8U; ++index) {
            send_clock_sync_ping();
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        next_clock_sync = std::chrono::steady_clock::now() + std::chrono::seconds(5);

        int encoder_result = 1;
        encoder_result = run_desktop_encode_session(
            duration,
            callbacks,
            output_width,
            output_height,
            frame_rate,
            capture_display_device);
    stop_sender();
    input_running.store(false);
    input_thread.join();
        if (encoder_result != 0) {
            throw std::runtime_error(
                "desktop encoder session failed: " + std::to_string(encoder_result));
        }
    } catch (...) {
        // The input thread is owned by this scope: tear it down on every
        // escape path before the exception continues.
        stop_sender();
        input_running.store(false);
        if (input_thread.joinable()) {
            input_thread.join();
        }
        throw;
    }
    // stop_stream is sent best-effort even when the input thread recorded a
    // failure: the link may still be alive (e.g. a parser error killed only
    // the receiver), and a clean stop beats a tablet-side disconnect. A dead
    // link merely logs here; the original input exception is rethrown below
    // so the reconnect loop still sees the failure (audit claim 3).
    try {
        std::lock_guard lock(write_mutex);
        write_all(
            stream,
            wireview::protocol::encode_packet(
                wireview::protocol::PacketHeader{
                    .type = wireview::protocol::PacketType::stop_stream,
                    .sequence = sequence,
                    .timestamp_micros = monotonic_micros(),
                },
                {}));
    } catch (const std::exception& error) {
        std::cerr << "WireView stop_stream notification failed: " << error.what()
                  << '\n';
    }
    if (input_error) {
        std::rethrow_exception(input_error);
    }
    std::cout << "Send queue: drops=" << dropped_frames
              << " | slowWrites=" << slow_writes << '\n';
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
    DevelopmentStatusPublisher* status,
    bool adaptive_bitrate = false) {
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
            status,
            adaptive_bitrate);
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

int run_wifi_desktop_stream_session(
    std::string_view remote_ip,
    std::uint16_t remote_port,
    std::chrono::milliseconds connect_timeout,
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
    // The pairing credential and PC identity arrive through the environment
    // (injected by the control center): not visible on any command line.
    const auto environment_hex = [](const char* name) -> std::string {
        std::string value(129U, '\0');
        const DWORD length = GetEnvironmentVariableA(
            name, value.data(), static_cast<DWORD>(value.size()));
        if (length == 0U || length >= value.size()) {
            return {};
        }
        value.resize(length);
        return value;
    };
    const std::string key_hex = environment_hex("WIREVIEW_WIFI_KEY");
    const std::string uuid_hex = environment_hex("WIREVIEW_WIFI_UUID");
    std::string host_name(256U, '\0');
    const DWORD name_length = GetEnvironmentVariableA(
        "WIREVIEW_WIFI_NAME", host_name.data(), static_cast<DWORD>(host_name.size()));
    host_name.resize(name_length == 0U || name_length >= host_name.size()
        ? 0U : name_length);
    if (key_hex.empty() || uuid_hex.empty()) {
        throw std::runtime_error(
            "the WiFi session is missing WIREVIEW_WIFI_KEY / WIREVIEW_WIFI_UUID; "
            "pair the tablet in the WireView control center first");
    }
    std::vector<std::uint8_t> key;
    std::vector<std::uint8_t> pc_uuid;
    try {
        key = from_hex(key_hex);
        pc_uuid = from_hex(uuid_hex);
    } catch (const std::runtime_error& error) {
        throw std::runtime_error(std::string("invalid WiFi credential environment: ") +
            error.what());
    }
    if (key.size() != wireview::protocol::kAuthKeySize ||
        pc_uuid.size() != wireview::protocol::kAuthUuidSize) {
        throw std::runtime_error("the WiFi credential environment has the wrong size");
    }

    DevelopmentStatusPublisher status(status_file);
    status.configure(
        capture_display_device,
        inject_input,
        output_width,
        output_height,
        frame_rate,
        reconnect_count,
        "wifi");

    WinsockSession winsock;
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(remote_port);
    if (inet_pton(AF_INET, std::string(remote_ip).c_str(), &address.sin_addr) != 1) {
        throw std::runtime_error("the WiFi endpoint IP is not a valid IPv4 address");
    }
    std::cout << "WireView WiFi session dialing " << remote_ip << ':' << remote_port << '\n';
    auto socket = connect_tcp_with_timeout(address, connect_timeout);
    std::cout << "Connected; authenticating the WiFi link\n";
    SocketByteStream stream(socket.get());

    WifiLinkIdentity identity{pc_uuid, host_name.empty() ? "WireView PC" : host_name};
    const WifiPeerInfo peer = perform_wifi_handshake(
        stream, identity, wireview::protocol::AuthMode::authenticate, key, {},
        false, nullptr);
    std::cout << "WiFi authentication verified with " << peer.device_name
              << " (android_id " << peer.android_id << ")\n";
    if (status_file.empty() == false) {
        status.connected(peer.device_name);
    }
    return run_connected_stream_impl(
        stream,
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
        &status,
        // Adaptive bitrate is a WiFi-transport feature: the single-shot
        // entry must behave like the reconnect entry (which passes true
        // below), otherwise manual --transport wifi runs silently lose the
        // governor.
        true);
}

int run_reconnecting_desktop_stream_session(
    const SessionEndpoint& endpoint,
    std::chrono::milliseconds accept_timeout,
    std::chrono::seconds total_duration,
    bool inject_input,
    std::uint16_t output_width,
    std::uint16_t output_height,
    std::uint16_t frame_rate,
    std::string_view capture_display_device,
    std::string_view status_file) {
    if (total_duration <= std::chrono::seconds::zero()) {
        throw std::invalid_argument("reconnecting session duration must be positive");
    }
    if (frame_rate != 60U && frame_rate != 120U) {
        throw std::invalid_argument("desktop stream frame rate must be 60 or 120");
    }
    const char* transport_name =
        endpoint.kind == SessionEndpoint::Kind::wifi_dial ? "wifi" : "adb";
    DevelopmentStatusPublisher status(status_file);
    status.configure(
        capture_display_device,
        inject_input,
        output_width,
        output_height,
        frame_rate,
        0U,
        transport_name);

    WinsockSession winsock;
    const auto deadline = std::chrono::steady_clock::now() + total_duration;
    std::uint64_t interrupted_sessions = 0;
    std::uint64_t completed_sessions = 0;
    while (std::chrono::steady_clock::now() < deadline) {
        const auto remaining = std::chrono::duration_cast<std::chrono::seconds>(
            deadline - std::chrono::steady_clock::now());
        if (remaining <= std::chrono::seconds::zero()) {
            break;
        }
        // A named target is a hard product requirement; re-verify before
        // every session so reconnect never silently falls back to primary.
        if (!capture_display_device.empty()) {
            (void)resolve_desktop_capture_area(capture_display_device);
        }
        try {
            int result;
            if (endpoint.kind == SessionEndpoint::Kind::wifi_dial) {
                sockaddr_in address{};
                address.sin_family = AF_INET;
                address.sin_port = htons(endpoint.remote_port);
                if (inet_pton(
                        AF_INET, endpoint.remote_ip.c_str(), &address.sin_addr) != 1) {
                    throw std::runtime_error("the WiFi endpoint IP is not valid IPv4");
                }
                std::cout << "WireView WiFi session dialing " << endpoint.remote_ip
                          << ':' << endpoint.remote_port << '\n';
                auto socket = connect_tcp_with_timeout(address, accept_timeout);
                SocketByteStream stream(socket.get());
                WifiLinkIdentity identity{endpoint.pc_uuid, endpoint.pc_name};
                const WifiPeerInfo peer = perform_wifi_handshake(
                    stream,
                    identity,
                    wireview::protocol::AuthMode::authenticate,
                    endpoint.wifi_key,
                    {},
                    false,
                    nullptr);
                std::cout << "WiFi authentication verified with " << peer.device_name << '\n';
                status.connected(peer.device_name);
                result = run_connected_stream_impl(
                    stream,
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
                    &status,
                    true);
            } else {
                result = run_server_impl(
                    endpoint.port,
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
            }
            ++completed_sessions;
            if (result == 0) {
                break;
            }
            // result != 0: the endpoint timed out waiting for the tablet. Keep
            // the loop going so the session survives a slow tablet restart.
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
        } catch (const std::exception& error) {
            if (!capture_display_device.empty()) {
                (void)resolve_desktop_capture_area(capture_display_device);
            }
            ++interrupted_sessions;
            status.interrupted(error.what());
            std::cerr << "WireView session interrupted: " << error.what()
                      << ". Waiting for the tablet to reconnect...\n";
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
        }
    }
    std::cout << "WireView reconnecting session completed; interrupted="
              << interrupted_sessions << " | completed=" << completed_sessions << '\n';
    status.completed();
    return 0;
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
