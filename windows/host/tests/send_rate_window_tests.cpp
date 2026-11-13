#undef NDEBUG
#include "../src/send_rate_window.hpp"
#include <cassert>
#include <chrono>
#include <cmath>

int main() {
    using namespace std::chrono_literals;
    using wireview::host::SendRateWindow;
    SendRateWindow meter;
    const auto start = SendRateWindow::Clock::time_point{};
    meter.reset(start);
    for (int i = 1; i < 120; ++i) {
        assert(!meter.delivered(start + i * 1000000us / 120, 1000));
    }
    const auto fast = meter.delivered(start + 1s, 1000);
    assert(fast && std::abs(fast->fps - 120.0) < 0.001);
    for (int i = 1; i < 90; ++i) {
        assert(!meter.delivered(start + 1s + i * 1000000us / 90, 1000));
    }
    const auto slow = meter.delivered(start + 2s, 1000);
    assert(slow && std::abs(slow->fps - 90.0) < 0.001);
    assert(std::abs(slow->mbps - 0.72) < 0.001);
    meter.reset(start + 3s);
    assert(!meter.delivered(start + 3100ms, 1000));
}
