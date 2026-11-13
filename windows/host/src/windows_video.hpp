#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <span>
#include <string_view>

namespace wireview::host {

struct DesktopFrameTiming {
    bool fresh_capture{};
    std::uint64_t capture_begin_us{};
    std::uint64_t capture_end_us{};
    std::uint64_t submit_begin_us{};
    std::uint64_t submit_end_us{};
    std::uint64_t output_us{};
    std::uint64_t empty_polls{};
    std::uint64_t poll_wait_us{};
    std::uint64_t pace_wait_us{};
    std::uint64_t desktop_update_id{};
    std::uint64_t desktop_present_qpc{};
    std::uint64_t desktop_acquire_us{};
    std::uint32_t desktop_accumulated_frames{};
    // CPU timestamps at the end of each prepare step, in diagnostic sessions.
    std::uint64_t cursor_ready_us{};
    std::uint64_t compose_ready_us{};
    std::uint64_t cursor_diagnostics_ready_us{};
    std::uint64_t convert_ready_us{};
    std::uint32_t system_cursor_shape_update{};
};

struct DesktopCaptureArea {
    std::int32_t left{};
    std::int32_t top{};
    std::uint32_t width{};
    std::uint32_t height{};
};

inline bool follows_windows_display(std::string_view device) noexcept {
    return device.empty() || device.starts_with(R"(\\?\)");
}

struct DesktopStreamCallbacks {
    std::function<void(
        std::uint16_t width,
        std::uint16_t height,
        std::uint16_t frame_rate)> on_started;
    std::function<void(
        std::span<const std::uint8_t> bytes,
        bool keyframe,
        std::uint64_t timestamp_micros,
        const DesktopFrameTiming& timing)> on_frame;
    // Called by the encoder thread before it submits an input frame. Returning
    // true asks for an IDR frame immediately, for example after the Android
    // decoder reports it had to discard a dependent H.264 frame.
    std::function<bool()> consume_keyframe_request;
    // Optional diagnostic correlation; does not change cursor composition.
    std::function<void(std::uint64_t, int, int)> on_cursor_sample;
    bool collect_prepare_timing{};
    // An empty area pauses input while the Windows desktop is switching.
    std::function<void(DesktopCaptureArea)> on_capture_area_changed;
};

int run_windows_video_probe();

// Prints every attached DXGI output and its stable Windows device name (for
// example \\.\DISPLAY2). A virtual display is selected by this name rather
// than by a fragile adapter/output index.
int run_desktop_display_list();

// Resolves the same target-selection rule used by the desktop encoder. An
// empty device name means the current primary display.
DesktopCaptureArea resolve_desktop_capture_area(std::string_view capture_display_device = {});

int run_desktop_capture_benchmark(std::chrono::seconds duration);

int run_desktop_encode_benchmark(std::chrono::seconds duration);

int run_desktop_encode_session(
    std::chrono::seconds duration,
    const DesktopStreamCallbacks& callbacks,
    std::uint16_t output_width = 0U,
    std::uint16_t output_height = 0U,
    std::uint16_t frame_rate = 60U,
    std::string_view capture_display_device = {},
    float cursor_size_scale = 1.0F);

}  // namespace wireview::host
