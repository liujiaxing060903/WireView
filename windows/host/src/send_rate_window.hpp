#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace wireview::host {

class SendRateWindow {
public:
    using Clock = std::chrono::steady_clock;
    struct Rate {
        double fps;
        double mbps;
    };

    void reset(Clock::time_point now) {
        started_ = now;
        frames_ = 0;
        bytes_ = 0;
    }

    std::optional<Rate> delivered(Clock::time_point now, std::size_t bytes) {
        ++frames_;
        bytes_ += bytes;
        const auto elapsed = now - started_;
        if (elapsed < std::chrono::seconds(1)) {
            return std::nullopt;
        }
        const double seconds = std::chrono::duration<double>(elapsed).count();
        const Rate result{static_cast<double>(frames_) / seconds,
            static_cast<double>(bytes_) * 8.0 / seconds / 1'000'000.0};
        reset(now);
        return result;
    }

private:
    Clock::time_point started_{};
    std::uint64_t frames_{};
    std::uint64_t bytes_{};
};

}  // namespace wireview::host
