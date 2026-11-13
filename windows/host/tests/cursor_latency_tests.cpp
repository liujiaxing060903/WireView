#include "../src/cursor_latency.hpp"
#include <iostream>
#include <stdexcept>

void require(bool result, const char* message) {
    if (!result) { throw std::runtime_error(message); }
}
int main() {
    try {
        wireview::host::CursorLatencyMeasurements samples;
        samples.observe(10, 20, 1000);
        require(!samples.correlate(1, 10, 20), "a stationary initial cursor is not a mouse movement");
        samples.observe(11, 20, 2000);
        require(!samples.correlate(2, 12, 20), "do not associate a frame with a different cursor position");
        require(samples.correlate(3, 11, 20), "the composed cursor must match an observed movement");
        require(samples.correlate(4, 11, 20),
            "retain a later frame of a movement in case Surface drops the first frame");
        samples.observe(12, 20, 3000);
        require(samples.correlate(5, 12, 20), "correlate a second movement");
        const auto second = samples.frame_report(5);
        require(second && second->observed_micros == 3000U, "markers must retain the original observation time");
        const auto first = samples.frame_report(4);
        require(first && first->observed_micros == 2000U,
            "reordered frame markers must match by PTS, not latest movement");
        require(first->observation_gap_micros == 1000U,
            "report actual observation gaps rather than assuming a perfect 1ms poll");
        require(samples.frame_report(3).has_value(), "all candidate frames must remain available until actual compositor matching");
        require(!samples.frame_report(3), "consume a frame marker only once");
        require(!samples.frame_report(999), "missing correlations must not produce a fake zero");
        std::cout << "Cursor latency correlation tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n'; return 1;
    }
}
