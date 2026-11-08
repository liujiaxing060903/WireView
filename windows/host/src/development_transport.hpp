#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace wireview::host {

inline constexpr std::uint16_t kDevelopmentPort = 39871;

// One wire endpoint for a desktop-stream session. The ADB kind keeps the
// historical listener on the fixed loopback port; the WiFi kind dials the
// tablet and authenticates with the pairing credential before every session.
struct SessionEndpoint {
    enum class Kind { adb_listener, wifi_dial };
    Kind kind{Kind::adb_listener};
    std::uint16_t port{kDevelopmentPort};
    std::string remote_ip;
    std::uint16_t remote_port{};
    std::vector<std::uint8_t> wifi_key;
    std::vector<std::uint8_t> pc_uuid;
    std::string pc_name;
};

int run_development_server(
    std::uint16_t port,
    std::chrono::milliseconds accept_timeout);

int run_development_client(std::uint16_t port, std::string_view client_name);

int run_development_stream_server(
    std::uint16_t port,
    std::chrono::milliseconds accept_timeout,
    const std::string& annex_b_path,
    std::uint16_t width,
    std::uint16_t height,
    std::uint16_t frame_rate);

// Establishes only the preflight connection and asks Android to draw a centered
// aspect-ratio frame. No VDD, capture, encoder, or video stream is started.
int run_device_preview_server(
    std::uint16_t port,
    std::chrono::milliseconds accept_timeout,
    std::chrono::seconds hold_duration,
    std::uint16_t width = 0U,
    std::uint16_t height = 0U,
    std::uint16_t frame_rate = 60U,
    std::string_view status_file = {},
    bool draw_preview = true);

int run_development_desktop_stream_server(
    std::uint16_t port,
    std::chrono::milliseconds accept_timeout,
    std::chrono::seconds stream_duration,
    bool inject_input = true,
    std::uint16_t output_width = 0U,
    std::uint16_t output_height = 0U,
    std::uint16_t frame_rate = 60U,
    std::string_view capture_display_device = {},
    std::string_view status_file = {},
    std::uint64_t reconnect_count = 0U);

int run_winusb_desktop_stream_session(
    std::chrono::milliseconds accessory_wait_timeout,
    std::chrono::seconds stream_duration,
    bool inject_input = true,
    std::uint16_t output_width = 0U,
    std::uint16_t output_height = 0U,
    std::uint16_t frame_rate = 60U,
    std::string_view capture_display_device = {},
    std::string_view status_file = {},
    std::uint64_t reconnect_count = 0U);

// R3 WiFi session: the PC dials the tablet (ip:port), authenticates with the
// long-term pairing credential, then runs the same desktop stream. The
// credential is passed via the WIREVIEW_WIFI_KEY environment variable (hex).
int run_wifi_desktop_stream_session(
    std::string_view remote_ip,
    std::uint16_t remote_port,
    std::chrono::milliseconds connect_timeout,
    std::chrono::seconds stream_duration,
    bool inject_input = true,
    std::uint16_t output_width = 0U,
    std::uint16_t output_height = 0U,
    std::uint16_t frame_rate = 60U,
    std::string_view capture_display_device = {},
    std::string_view status_file = {},
    std::uint64_t reconnect_count = 0U);

int run_development_reconnecting_desktop_stream_server(
    std::uint16_t port,
    std::chrono::milliseconds accept_timeout,
    std::chrono::seconds total_duration,
    bool inject_input = false,
    std::uint16_t output_width = 0U,
    std::uint16_t output_height = 0U,
    std::uint16_t frame_rate = 60U,
    std::string_view capture_display_device = {},
    std::string_view status_file = {});

// Product session loop: keeps running until total_duration elapses, and on a
// channel failure re-establishes the endpoint (accept again for ADB, dial and
// re-authenticate for WiFi) so a USB hiccup or a WiFi blip costs seconds of
// frozen frames instead of a dead session. The encoder is rebuilt per
// reconnection; the Android side keeps its decoder alive for a repeated
// start_stream with the same configuration.
int run_reconnecting_desktop_stream_session(
    const SessionEndpoint& endpoint,
    std::chrono::milliseconds accept_timeout,
    std::chrono::seconds total_duration,
    bool inject_input,
    std::uint16_t output_width,
    std::uint16_t output_height,
    std::uint16_t frame_rate,
    std::string_view capture_display_device,
    std::string_view status_file);

}  // namespace wireview::host
