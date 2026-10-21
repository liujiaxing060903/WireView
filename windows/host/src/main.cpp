#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include "development_transport.hpp"
#include "idd_frame_relay_pipe.hpp"
#include "parsec_vdd.hpp"
#include "windows_video.hpp"

#include "wireview/protocol.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <exception>
#include <iostream>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

constexpr std::uint16_t kLoopbackTestPort = 39872;

std::pair<std::uint16_t, std::uint16_t> parse_video_size(std::string_view text) {
    const std::size_t separator = text.find('x');
    if (separator == std::string_view::npos || separator == 0U ||
        separator + 1U >= text.size()) {
        throw std::invalid_argument("video size must use WIDTHxHEIGHT, for example 1920x1200");
    }
    const unsigned long width = std::stoul(std::string(text.substr(0U, separator)));
    const unsigned long height = std::stoul(std::string(text.substr(separator + 1U)));
    if (width == 0UL || height == 0UL ||
        width > std::numeric_limits<std::uint16_t>::max() ||
        height > std::numeric_limits<std::uint16_t>::max()) {
        throw std::invalid_argument("video size is outside the WireView protocol range");
    }
    return {
        static_cast<std::uint16_t>(width),
        static_cast<std::uint16_t>(height)};
}

std::uint64_t monotonic_micros() {
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(now).count());
}

int run_in_memory_demo() {
    using namespace wireview::protocol;

    const std::string hello_text = "WireView host phase-0";
    const std::vector<std::uint8_t> payload(hello_text.begin(), hello_text.end());
    const PacketHeader header{
        .type = PacketType::hello,
        .flags = PacketFlag::acknowledgement_required,
        .sequence = 1,
        .timestamp_micros = monotonic_micros(),
    };
    const auto encoded = encode_packet(header, payload);

    PacketStreamParser parser;
    constexpr std::size_t chunk_size = 7;
    for (std::size_t offset = 0; offset < encoded.size(); offset += chunk_size) {
        const auto size = std::min(chunk_size, encoded.size() - offset);
        if (!parser.push(std::span(encoded).subspan(offset, size))) {
            std::cerr << "WireView protocol buffer limit exceeded\n";
            return 1;
        }
    }

    const auto result = parser.next();
    if (result.status != StreamStatus::packet_ready || !result.packet) {
        std::cerr << "WireView host failed to decode the phase-0 handshake\n";
        return 1;
    }

    const std::string decoded_text(
        result.packet->payload.begin(), result.packet->payload.end());
    std::cout << "WireView phase-0 host is running\n"
              << "protocol=" << static_cast<int>(result.packet->header.version_major)
              << '.' << static_cast<int>(result.packet->header.version_minor) << '\n'
              << "sequence=" << result.packet->header.sequence << '\n'
              << "payload=" << decoded_text << '\n';
    return 0;
}

int run_loopback_test() {
    int server_result = 1;
    std::exception_ptr server_error;
    std::thread server([&server_result, &server_error] {
        try {
            server_result = wireview::host::run_development_server(
                kLoopbackTestPort,
                std::chrono::seconds(5));
        } catch (...) {
            server_error = std::current_exception();
        }
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    const int client_result = wireview::host::run_development_client(
        kLoopbackTestPort,
        "WireView local loopback");
    server.join();

    if (server_error) {
        std::rethrow_exception(server_error);
    }
    if (server_result != 0 || client_result != 0) {
        return 1;
    }
    std::cout << "WireView TCP loopback handshake passed\n";
    return 0;
}

}  // namespace

int main(int argument_count, char** arguments) {
    try {
        (void)SetProcessDpiAwarenessContext(
            DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        if (argument_count == 2 && std::string_view(arguments[1]) == "--listen") {
            return wireview::host::run_development_server(
                wireview::host::kDevelopmentPort,
                std::chrono::minutes(5));
        }
        if (argument_count == 2 && std::string_view(arguments[1]) == "--loopback") {
            return run_loopback_test();
        }
        if (argument_count == 2 && std::string_view(arguments[1]) == "--listen-idd-relay") {
            return wireview::host::run_idd_frame_relay_listener();
        }
        if (argument_count == 2 && std::string_view(arguments[1]) == "--probe-video") {
            return wireview::host::run_windows_video_probe();
        }
        if (argument_count == 2 && std::string_view(arguments[1]) == "--list-displays") {
            return wireview::host::run_desktop_display_list();
        }
        if (argument_count == 2 && std::string_view(arguments[1]) == "--probe-parsec-vdd") {
            const auto probe = wireview::host::probe_parsec_vdd();
            std::cout << "Parsec VDD: " << probe.detail;
            if (probe.available) {
                std::cout << " | version=" << probe.version;
            }
            std::cout << '\n';
            return 0;
        }
        if (argument_count == 3 &&
            std::string_view(arguments[1]) == "--remove-parsec-vdd-index") {
            const int display_index = std::stoi(arguments[2]);
            wireview::host::remove_parsec_vdd_display(display_index);
            std::cout << "Removed Parsec VDD driver index " << display_index << '\n';
            return 0;
        }
        if (argument_count == 3 && std::string_view(arguments[1]) == "--capture-benchmark") {
            const int seconds = std::stoi(arguments[2]);
            if (seconds < 1 || seconds > 60) {
                throw std::invalid_argument("capture duration must be between 1 and 60 seconds");
            }
            return wireview::host::run_desktop_capture_benchmark(
                std::chrono::seconds(seconds));
        }
        if (argument_count == 3 && std::string_view(arguments[1]) == "--encode-benchmark") {
            const int seconds = std::stoi(arguments[2]);
            if (seconds < 1 || seconds > 60) {
                throw std::invalid_argument("encode duration must be between 1 and 60 seconds");
            }
            return wireview::host::run_desktop_encode_benchmark(
                std::chrono::seconds(seconds));
        }
        if (argument_count >= 3 && std::string_view(arguments[1]) == "--device-preview") {
            const int seconds = std::stoi(arguments[2]);
            if (seconds < 1 || seconds > 1'800) {
                throw std::invalid_argument(
                    "device preview duration must be between 1 and 1800 seconds");
            }
            std::uint16_t width = 0U;
            std::uint16_t height = 0U;
            std::uint16_t frame_rate = 60U;
            std::string_view status_file;
            for (int index = 3; index < argument_count; ++index) {
                const std::string_view option(arguments[index]);
                if (option == "--size" && index + 1 < argument_count && width == 0U) {
                    const auto dimensions = parse_video_size(arguments[++index]);
                    width = dimensions.first;
                    height = dimensions.second;
                    continue;
                }
                if (option == "--frame-rate" && index + 1 < argument_count &&
                    frame_rate == 60U) {
                    const unsigned long parsed = std::stoul(arguments[++index]);
                    if (parsed != 60UL && parsed != 120UL) {
                        throw std::invalid_argument("device preview frame rate must be 60 or 120");
                    }
                    frame_rate = static_cast<std::uint16_t>(parsed);
                    continue;
                }
                if (option == "--status-file" && index + 1 < argument_count && status_file.empty()) {
                    status_file = arguments[++index];
                    continue;
                }
                throw std::invalid_argument(
                    "--device-preview options are --size WIDTHxHEIGHT, --frame-rate 60|120, and --status-file PATH");
            }
            return wireview::host::run_device_preview_server(
                wireview::host::kDevelopmentPort,
                std::chrono::minutes(5),
                std::chrono::seconds(seconds),
                width,
                height,
                frame_rate,
                status_file);
        }
        if (argument_count == 4 && std::string_view(arguments[1]) == "--probe-device-profile" &&
            std::string_view(arguments[2]) == "--status-file") {
            return wireview::host::run_device_preview_server(
                wireview::host::kDevelopmentPort, std::chrono::minutes(2), std::chrono::seconds(2),
                0U, 0U, 60U, arguments[3], false);
        }
        if (argument_count >= 3 && std::string_view(arguments[1]) == "--stream-desktop") {
            bool input_log_only = false;
            std::uint16_t output_width = 0U;
            std::uint16_t output_height = 0U;
            std::uint16_t frame_rate = 60U;
            std::string capture_display_device;
            std::string status_file;
            std::uint64_t reconnect_count = 0U;
            bool create_parsec_vdd = false;
            bool use_winusb = false;
            bool transport_set = false;
            for (int index = 3; index < argument_count; ++index) {
                const std::string_view option(arguments[index]);
                if (option == "--input-log-only" && !input_log_only) {
                    input_log_only = true;
                    continue;
                }
                if (option == "--size" && index + 1 < argument_count &&
                    output_width == 0U) {
                    const auto dimensions = parse_video_size(arguments[++index]);
                    output_width = dimensions.first;
                    output_height = dimensions.second;
                    continue;
                }
                if (option == "--frame-rate" && index + 1 < argument_count &&
                    frame_rate == 60U) {
                    const unsigned long parsed = std::stoul(arguments[++index]);
                    if (parsed != 60UL && parsed != 120UL) {
                        throw std::invalid_argument("desktop stream frame rate must be 60 or 120");
                    }
                    frame_rate = static_cast<std::uint16_t>(parsed);
                    continue;
                }
                if (option == "--display" && index + 1 < argument_count &&
                    capture_display_device.empty()) {
                    capture_display_device = arguments[++index];
                    if (capture_display_device.empty()) {
                        throw std::invalid_argument("desktop capture display name cannot be empty");
                    }
                    continue;
                }
                if (option == "--parsec-vdd" && !create_parsec_vdd) {
                    create_parsec_vdd = true;
                    continue;
                }
                if (option == "--status-file" && index + 1 < argument_count &&
                    status_file.empty()) {
                    status_file = arguments[++index];
                    if (status_file.empty()) {
                        throw std::invalid_argument("desktop status file cannot be empty");
                    }
                    continue;
                }
                if (option == "--reconnect-count" && index + 1 < argument_count &&
                    reconnect_count == 0U) {
                    reconnect_count = std::stoull(arguments[++index]);
                    continue;
                }
                if (option == "--transport" && index + 1 < argument_count &&
                    !transport_set) {
                    const std::string_view transport(arguments[++index]);
                    if (transport == "adb") {
                        use_winusb = false;
                    } else if (transport == "winusb") {
                        use_winusb = true;
                    } else {
                        throw std::invalid_argument(
                            "desktop stream transport must be adb or winusb");
                    }
                    transport_set = true;
                    continue;
                }
                throw std::invalid_argument(
                    "--stream-desktop options are --input-log-only, --size WIDTHxHEIGHT, "
                    "--frame-rate 60|120, --display \\\\.\\DISPLAYn, --parsec-vdd, "
                    "--status-file PATH, --reconnect-count N, and --transport adb|winusb");
            }
            const int seconds = std::stoi(arguments[2]);
            if (seconds < 0) {
                throw std::invalid_argument(
                    "desktop stream duration must be zero (until stopped) or a positive number of seconds");
            }
            std::optional<wireview::host::ParsecVddSession> parsec_vdd;
            if (create_parsec_vdd) {
                if (!capture_display_device.empty()) {
                    throw std::invalid_argument(
                        "--parsec-vdd selects its new display automatically; do not also pass --display");
                }
                parsec_vdd.emplace();
                parsec_vdd->start();
                capture_display_device = parsec_vdd->wait_for_display(std::chrono::seconds(30));
                std::cout << "WireView created Parsec VDD display " << capture_display_device
                          << " (driver index " << parsec_vdd->display_index() << ")\n";
            }
            if (use_winusb) {
                return wireview::host::run_winusb_desktop_stream_session(
                    std::chrono::minutes(5),
                    std::chrono::seconds(seconds),
                    !input_log_only,
                    output_width,
                    output_height,
                    frame_rate,
                    capture_display_device,
                    status_file,
                    reconnect_count);
            }
            return wireview::host::run_development_desktop_stream_server(
                wireview::host::kDevelopmentPort,
                std::chrono::minutes(5),
                std::chrono::seconds(seconds),
                !input_log_only,
                output_width,
                output_height,
                frame_rate,
                capture_display_device,
                status_file,
                reconnect_count);
        }
        if (argument_count >= 3 && std::string_view(arguments[1]) == "--reconnect-desktop") {
            const int seconds = std::stoi(arguments[2]);
            if (seconds < 10 || seconds > 600) {
                throw std::invalid_argument(
                    "reconnect test duration must be between 10 and 600 seconds");
            }
            std::uint16_t output_width = 0U;
            std::uint16_t output_height = 0U;
            std::uint16_t frame_rate = 60U;
            std::string capture_display_device;
            std::string status_file;
            for (int index = 3; index < argument_count; ++index) {
                const std::string_view option(arguments[index]);
                if (option == "--size" && index + 1 < argument_count &&
                    output_width == 0U) {
                    const auto dimensions = parse_video_size(arguments[++index]);
                    output_width = dimensions.first;
                    output_height = dimensions.second;
                    continue;
                }
                if (option == "--frame-rate" && index + 1 < argument_count &&
                    frame_rate == 60U) {
                    const unsigned long parsed = std::stoul(arguments[++index]);
                    if (parsed != 60UL && parsed != 120UL) {
                        throw std::invalid_argument(
                            "reconnect desktop frame rate must be 60 or 120");
                    }
                    frame_rate = static_cast<std::uint16_t>(parsed);
                    continue;
                }
                if (option == "--display" && index + 1 < argument_count &&
                    capture_display_device.empty()) {
                    capture_display_device = arguments[++index];
                    if (capture_display_device.empty()) {
                        throw std::invalid_argument(
                            "reconnect capture display name cannot be empty");
                    }
                    continue;
                }
                if (option == "--status-file" && index + 1 < argument_count &&
                    status_file.empty()) {
                    status_file = arguments[++index];
                    if (status_file.empty()) {
                        throw std::invalid_argument("reconnect status file cannot be empty");
                    }
                    continue;
                }
                throw std::invalid_argument(
                    "--reconnect-desktop options are --size WIDTHxHEIGHT, "
                    "--frame-rate 60|120, --display \\\\.\\DISPLAYn, and --status-file PATH");
            }
            return wireview::host::run_development_reconnecting_desktop_stream_server(
                wireview::host::kDevelopmentPort,
                std::chrono::seconds(5),
                std::chrono::seconds(seconds),
                false,
                output_width,
                output_height,
                frame_rate,
                capture_display_device,
                status_file);
        }
        if (argument_count == 3 && std::string_view(arguments[1]) == "--stream") {
            return wireview::host::run_development_stream_server(
                wireview::host::kDevelopmentPort,
                std::chrono::minutes(5),
                arguments[2],
                320U,
                240U,
                25U);
        }
        if (argument_count != 1) {
            std::cerr << "Usage: wireview_host [--listen|--loopback|--listen-idd-relay|--probe-video|--list-displays|--probe-parsec-vdd|--remove-parsec-vdd-index <index>|"
                         "--capture-benchmark <seconds>|--encode-benchmark <seconds>|"
                         "--stream-desktop <seconds> [--input-log-only] [--size WIDTHxHEIGHT] [--frame-rate 60|120] [--display \\\\.\\DISPLAYn|--parsec-vdd] [--status-file PATH] [--reconnect-count N] [--transport adb|winusb]|"
                         "--reconnect-desktop <seconds> [--size WIDTHxHEIGHT] [--frame-rate 60|120] [--display \\\\.\\DISPLAYn] [--status-file PATH]|"
                         "--stream <annex-b.h264>]\n";
            return 64;
        }
        return run_in_memory_demo();
    } catch (const std::exception& error) {
        std::cerr << "WireView host error: " << error.what() << '\n';
        return 1;
    }
}
