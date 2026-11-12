#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <span>
#include <string_view>

namespace wireview::host {

struct DesktopStreamCallbacks {
    std::function<void(
        std::uint16_t width,
        std::uint16_t height,
        std::uint16_t frame_rate)> on_started;
    std::function<void(
        std::span<const std::uint8_t> bytes,
        bool keyframe,
        std::uint64_t timestamp_micros)> on_frame;
    // Called by the encoder thread before it submits an input frame. Returning
    // true asks for an IDR frame immediately, for example after the Android
    // decoder reports it had to discard a dependent H.264 frame.
    std::function<bool()> consume_keyframe_request;
};

struct DesktopCaptureArea {
    std::int32_t left{};
    std::int32_t top{};
    std::uint32_t width{};
    std::uint32_t height{};
};

int run_windows_video_probe();

// Prints every attached DXGI output and its stable Windows device name (for
// example \\.\DISPLAY2). A virtual display is selected by this name rather
// than by a fragile adapter/output index.
int run_desktop_display_list();

// Diagnostic: runs the Win32 cursor fallback for the stock arrow and I-beam
// cursors and writes the reconstructed bitmaps next to output_path.
int run_cursor_shape_probe(const char* output_path);

// Resolves the same target-selection rule used by the desktop encoder. An
// empty device name means the current primary display.
DesktopCaptureArea resolve_desktop_capture_area(std::string_view capture_display_device = {});

int run_desktop_capture_benchmark(std::chrono::seconds duration);

int run_desktop_encode_benchmark(
    std::chrono::seconds duration,
    bool prefer_software_encoder = false,
    std::string_view capture_display_device = {});

// Encodes the desktop for the given duration. The hardware H.264 encoder bound to
// the desktop adapter is preferred; when it is missing or refuses to run, the
// Microsoft software H.264 encoder is used instead (NV12 read back to the CPU)
// unless prefer_software_encoder already asked for that path.
int run_desktop_encode_session(
    std::chrono::seconds duration,
    const DesktopStreamCallbacks& callbacks,
    std::uint16_t output_width = 0U,
    std::uint16_t output_height = 0U,
    std::uint16_t frame_rate = 60U,
    std::string_view capture_display_device = {},
    bool prefer_software_encoder = false);

}  // namespace wireview::host
