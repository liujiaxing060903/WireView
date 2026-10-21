#pragma once

#include "wireview/input.hpp"

#include "windows_video.hpp"

#include <cstdint>
#include <map>
#include <optional>

namespace wireview::host {

struct InputInjectionMetrics {
    std::uint64_t attempts{0};
    std::uint64_t successful{0};
    std::uint64_t finger_records{0};
    std::uint64_t pen_records{0};
    std::uint64_t mouse_records{0};
    std::uint64_t total_micros{0};
    std::uint64_t maximum_micros{0};
};

class WindowsInputInjector {
public:
    explicit WindowsInputInjector(DesktopCaptureArea target = {});
    ~WindowsInputInjector();

    WindowsInputInjector(const WindowsInputInjector&) = delete;
    WindowsInputInjector& operator=(const WindowsInputInjector&) = delete;

    bool inject_batch(const wireview::protocol::InputBatch& batch);

    [[nodiscard]] std::uint64_t injected_records() const noexcept;
    [[nodiscard]] const InputInjectionMetrics& metrics() const noexcept;

private:
    struct TouchContact {
        std::uint16_t normalized_x;
        std::uint16_t normalized_y;
        std::uint16_t pressure;
        std::uint8_t action;
    };

    bool inject_touch_batch(const wireview::protocol::InputBatch& batch);
    bool inject_pen_record(
        std::uint8_t action,
        bool eraser,
        std::uint16_t pointer_id,
        std::uint16_t normalized_x,
        std::uint16_t normalized_y,
        std::uint16_t pressure,
        std::int16_t tilt_x_hundredths,
        std::int16_t tilt_y_hundredths,
        std::uint16_t buttons);
    bool inject_mouse_record(
        std::uint8_t action,
        std::uint16_t normalized_x,
        std::uint16_t normalized_y);

    void* touch_device_{nullptr};
    void* pen_device_{nullptr};
    DesktopCaptureArea target_{};
    std::map<std::uint16_t, TouchContact> active_touches_;
    std::optional<std::uint16_t> primary_touch_id_;
    std::uint64_t injected_records_{0};
    InputInjectionMetrics metrics_{};
};

}  // namespace wireview::host
