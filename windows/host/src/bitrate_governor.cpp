#include "bitrate_governor.hpp"

#include <algorithm>

namespace wireview::host {

namespace {
constexpr std::uint32_t kDropCooldownTicks = 3U;   // seconds between cuts
constexpr std::uint32_t kRaiseAfterHealthyTicks = 8U;
}  // namespace

BitrateGovernor::BitrateGovernor(
    std::uint32_t minimum_bps,
    std::uint32_t maximum_bps,
    std::uint32_t initial_bps) noexcept
    : minimum_bps_(minimum_bps),
      maximum_bps_(maximum_bps),
      target_bps_(std::clamp(initial_bps, minimum_bps, maximum_bps)) {
}

std::uint32_t BitrateGovernor::on_sample(
    std::uint32_t delivered_delta,
    std::uint32_t dropped_delta,
    std::uint32_t queued,
    std::uint32_t errors) noexcept {
    const bool congested = dropped_delta > 0U || errors > 0U || queued > 4U;
    std::uint32_t next = target_bps_;
    if (congested) {
        healthy_ticks_ = 0U;
        if (hold_ticks_ > 0U) {
            --hold_ticks_;
        }
        if (hold_ticks_ == 0U && target_bps_ > minimum_bps_) {
            next = std::max(minimum_bps_, target_bps_ - target_bps_ / 4U);
            hold_ticks_ = kDropCooldownTicks;
        }
    } else {
        if (hold_ticks_ > 0U) {
            --hold_ticks_;
        }
        // A dead stream (no deliveries) must not raise the target: only a
        // sustained, healthy, actually-flowing stream earns headroom.
        if (delivered_delta > 0U) {
            ++healthy_ticks_;
            if (healthy_ticks_ >= kRaiseAfterHealthyTicks &&
                target_bps_ < maximum_bps_) {
                next = std::min(maximum_bps_, target_bps_ + target_bps_ / 10U);
                healthy_ticks_ = 0U;
            }
        } else {
            healthy_ticks_ = 0U;
        }
    }
    next = std::clamp(next, minimum_bps_, maximum_bps_);
    if (next != target_bps_) {
        target_bps_ = next;
        return next;
    }
    return 0U;
}

}  // namespace wireview::host
