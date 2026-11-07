#pragma once

// Rate governor for the R3 WiFi transport: framerate first. The Android
// decoder reports per-second health counters (received/queued/dropped/
// errors, telemetry event 4, already present upstream); the PC keeps the
// delta, the governor turns it into a bitrate target. Congestion sheds
// bitrate first; the frame rate stays untouched. The ceiling is the
// measured WiFi budget (<= 60 Mbps, see the 2026-11-06 link report).

#include <cstdint>

namespace wireview::host {

class BitrateGovernor {
public:
    BitrateGovernor(
        std::uint32_t minimum_bps,
        std::uint32_t maximum_bps,
        std::uint32_t initial_bps) noexcept;

    // One sample per second (call after diffing the decoder counters).
    // Returns the new target bitrate when it changed, or 0 when unchanged.
    [[nodiscard]] std::uint32_t on_sample(
        std::uint32_t delivered_delta,
        std::uint32_t dropped_delta,
        std::uint32_t queued,
        std::uint32_t errors) noexcept;

    [[nodiscard]] std::uint32_t target() const noexcept {
        return target_bps_;
    }

private:
    std::uint32_t minimum_bps_;
    std::uint32_t maximum_bps_;
    std::uint32_t target_bps_;
    std::uint32_t hold_ticks_{0};      // cooldown before the next step down
    std::uint32_t healthy_ticks_{0};   // consecutive healthy seconds
};

}  // namespace wireview::host
