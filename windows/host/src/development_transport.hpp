#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>

namespace wireview::host {

inline constexpr std::uint16_t kDevelopmentPort = 39871;

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

}  // namespace wireview::host
