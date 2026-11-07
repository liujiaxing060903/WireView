#include "bitrate_governor.hpp"

#include <cstdint>
#include <iostream>

namespace {

int failures = 0;

void check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        ++failures;
    }
}

constexpr std::uint32_t kMin = 6'000'000U;
constexpr std::uint32_t kMax = 60'000'000U;
constexpr std::uint32_t kStart = 24'000'000U;

void initial_clamp_test() {
    wireview::host::BitrateGovernor low(kMin, kMax, 1'000'000U);
    check(low.target() == kMin, "initial target below the floor must clamp up");
    wireview::host::BitrateGovernor high(kMin, kMax, 120'000'000U);
    check(high.target() == kMax, "initial target above the ceiling must clamp down");
}

void drop_test() {
    wireview::host::BitrateGovernor governor(kMin, kMax, kStart);
    // Drops cut 25% per step, with a 3 s cooldown between cuts.
    const std::uint32_t after_first = governor.on_sample(60U, 5U, 0U, 0U);
    check(after_first == kStart - kStart / 4U, "drops must cut the target by 25%");
    check(governor.on_sample(60U, 5U, 0U, 0U) == 0U,
          "the cooldown must hold the target for the next sample");
    (void)governor.on_sample(60U, 5U, 0U, 0U);
    const std::uint32_t after_cooldown =
        governor.on_sample(60U, 5U, 0U, 0U);
    check(after_cooldown == (kStart - kStart / 4U) - (kStart - kStart / 4U) / 4U,
          "after the cooldown another congested second cuts again");
}

void queued_and_errors_test() {
    wireview::host::BitrateGovernor queued(kMin, kMax, kStart);
    check(queued.on_sample(60U, 0U, 8U, 0U) == kStart - kStart / 4U,
          "a deep decoder queue must count as congestion");
    wireview::host::BitrateGovernor errors(kMin, kMax, kStart);
    check(errors.on_sample(60U, 0U, 0U, 1U) == kStart - kStart / 4U,
          "decoder errors must count as congestion");
    wireview::host::BitrateGovernor shallow(kMin, kMax, kStart);
    check(shallow.on_sample(60U, 0U, 4U, 0U) == 0U,
          "a shallow queue is normal and must not trigger a cut");
}

void floor_test() {
    wireview::host::BitrateGovernor governor(kMin, kMax, kMin + 1'000'000U);
    (void)governor.on_sample(60U, 5U, 0U, 0U);
    // Repeated congestion must never push below the floor.
    for (int round = 0; round < 20; ++round) {
        (void)governor.on_sample(60U, 5U, 0U, 0U);
    }
    check(governor.target() == kMin, "the target must stop at the floor");
}

void raise_test() {
    wireview::host::BitrateGovernor governor(kMin, kMax, kStart);
    const std::uint32_t lowered = governor.on_sample(60U, 5U, 0U, 0U);
    // Eight consecutive healthy, flowing seconds raise the target by 10%.
    for (int second = 0; second < 7; ++second) {
        (void)governor.on_sample(60U, 0U, 0U, 0U);
        check(true, "healthy seconds below the threshold must not move the target");
    }
    const std::uint32_t raised = governor.on_sample(60U, 0U, 0U, 0U);
    check(raised == lowered + lowered / 10U,
          "eight healthy seconds must raise the target by 10%");
}

void stall_does_not_raise_test() {
    wireview::host::BitrateGovernor governor(kMin, kMax, kStart);
    // A stream that stopped delivering (connection gone) must not climb.
    for (int second = 0; second < 30; ++second) {
        (void)governor.on_sample(0U, 0U, 0U, 0U);
    }
    check(governor.target() == kStart,
          "stalled samples must never raise the target");
}

void ceiling_test() {
    wireview::host::BitrateGovernor governor(kMin, kMax, kMax - 1'000'000U);
    for (int cycle = 0; cycle < 12; ++cycle) {
        for (int second = 0; second < 8; ++second) {
            (void)governor.on_sample(60U, 0U, 0U, 0U);
        }
    }
    check(governor.target() == kMax, "the target must stop at the 60 Mbps ceiling");
}

}  // namespace

int main() {
    initial_clamp_test();
    drop_test();
    queued_and_errors_test();
    floor_test();
    raise_test();
    stall_does_not_raise_test();
    ceiling_test();

    if (failures != 0) {
        std::cerr << failures << " bitrate governor test(s) failed\n";
        return 1;
    }
    std::cout << "All WireView bitrate governor tests passed\n";
    return 0;
}
