#pragma once

#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <optional>

namespace wireview::host {

// Positions stay in memory. Logs contain only motion/frame IDs and timings.
class CursorLatencyMeasurements {
public:
    struct Sample {
        std::uint64_t motion;
        std::uint64_t observed_micros;
        std::uint64_t observation_gap_micros;
    };

    void observe(int x, int y, std::uint64_t timestamp_micros) {
        std::lock_guard lock(mutex_);
        const auto observation_gap = initialized_ ? timestamp_micros - previous_observation_micros_ : 0U;
        previous_observation_micros_ = timestamp_micros;
        if (!initialized_) {
            x_ = x; y_ = y; initialized_ = true;
            return;
        }
        if (x == x_ && y == y_) { return; }
        x_ = x; y_ = y;
        observations_.push_back({x, y, {++motion_, timestamp_micros, observation_gap}});
        if (observations_.size() > 256U) { observations_.pop_front(); }
    }

    bool correlate(std::uint64_t frame_pts, int x, int y) {
        std::lock_guard lock(mutex_);
        for (auto item = observations_.rbegin(); item != observations_.rend(); ++item) {
            if (item->x == x && item->y == y) {
                frames_[frame_pts] = item->sample;
                // A render callback can be batched; retain up to eight seconds
                // of 120 FPS correlations, without accumulating stopped frames.
                if (frames_.size() > 1024U) { frames_.erase(frames_.begin()); }
                return true;
            }
        }
        return false;
    }

    std::optional<Sample> frame_report(std::uint64_t frame_pts) {
        std::lock_guard lock(mutex_);
        const auto found = frames_.find(frame_pts);
        if (found == frames_.end()) { return std::nullopt; }
        const auto sample = found->second;
        frames_.erase(found);
        // This is only a frame marker. Actual display time must come from
        // SurfaceFlinger, not from the buffer's presentation timestamp.
        return sample;
    }

private:
    struct Observation { int x; int y; Sample sample; };
    std::mutex mutex_;
    bool initialized_{false};
    int x_{};
    int y_{};
    std::uint64_t motion_{};
    std::uint64_t previous_observation_micros_{};
    std::deque<Observation> observations_;
    std::map<std::uint64_t, Sample> frames_;
};
} // namespace wireview::host
