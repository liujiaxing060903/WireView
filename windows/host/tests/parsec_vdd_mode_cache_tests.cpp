#include "parsec_vdd_custom_modes.hpp"
#include <iostream>
#include <vector>

int main() {
    using namespace wireview::host;
    using Mode = ParsecVddCustomMode;
    std::array<std::optional<Mode>, 5U> modes{
        Mode{2772, 1280, 240}, Mode{2374, 1080, 240}, Mode{1080, 2374, 240},
        Mode{2248, 1080, 60}, Mode{1080, 2248, 60}};
    const auto require = [](bool condition, const char* reason) {
        if (!condition) { std::cerr << reason << '\n'; }
        return condition;
    };
    const Mode portrait{1280, 2772, 240};
    if (!require(choose_parsec_vdd_custom_slot(modes, portrait, {}) == 1U,
            "a full cache must accept portrait without evicting its landscape mode")) { return 1; }
    const std::vector<Mode> active{*modes[1]};
    if (!require(choose_parsec_vdd_custom_slot(modes, portrait, active) == 3U,
            "keep both orientations of another device's active mode")) { return 1; }
    const std::vector<Mode> rotated{{1280, 2772, 239}};
    if (!require(choose_parsec_vdd_custom_slot(modes, Mode{1600, 2560, 240}, rotated) == 1U,
            "protect a rotated active mode even when Windows reports 239 Hz")) { return 1; }
    std::vector<Mode> all_active;
    for (const auto& mode : modes) { all_active.push_back(*mode); }
    if (!require(!choose_parsec_vdd_custom_slot(modes, portrait, all_active),
            "never overwrite five occupied modes")) { return 1; }
    modes[4].reset();
    if (!require(choose_parsec_vdd_custom_slot(modes, portrait, active) == 4U,
            "use an empty slot before replacing saved modes")) { return 1; }
    if (!require(choose_parsec_vdd_custom_slot(modes, *modes[2], active) == 2U,
            "reuse an installed mode before consuming an empty slot")) { return 1; }
    std::cout << "Full mode cache accepts new devices and protects active modes\n";
    return 0;
}
