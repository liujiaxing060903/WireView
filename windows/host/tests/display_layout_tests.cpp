#include "display_layout.hpp"

#include <iostream>
#include <optional>
#include <stdexcept>
#include <utility>

using namespace wireview::host;

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void test_parse_accepts_canonical_names() {
    require(parse_vdd_position("right") == VddPosition::Right, "right was not parsed");
    require(parse_vdd_position("left") == VddPosition::Left, "left was not parsed");
    require(parse_vdd_position("top") == VddPosition::Top, "top was not parsed");
    require(parse_vdd_position("bottom") == VddPosition::Bottom, "bottom was not parsed");
}

void test_parse_is_case_insensitive() {
    require(parse_vdd_position("RIGHT") == VddPosition::Right, "RIGHT was not parsed");
    require(parse_vdd_position("Left") == VddPosition::Left, "Left was not parsed");
    require(parse_vdd_position("ToP") == VddPosition::Top, "ToP was not parsed");
    require(parse_vdd_position("BoTtOm") == VddPosition::Bottom, "BoTtOm was not parsed");
}

void test_parse_rejects_unknown_text() {
    require(!parse_vdd_position("").has_value(), "empty text was accepted");
    require(!parse_vdd_position("corner").has_value(), "unknown text was accepted");
    require(!parse_vdd_position("right ").has_value(), "trailing space was accepted");
    require(!parse_vdd_position("right-side").has_value(), "hyphenated text was accepted");
}

void test_name_and_parse_round_trip() {
    // name -> parse 必须还原为同一方位。
    for (const VddPosition position :
         {VddPosition::Right, VddPosition::Left, VddPosition::Top, VddPosition::Bottom}) {
        const auto parsed = parse_vdd_position(vdd_position_name(position));
        require(parsed.has_value(), "canonical name was rejected by the parser");
        require(*parsed == position, "name-to-parse round trip changed the position");
    }
}

void test_compute_vdd_offset_four_positions() {
    // 主屏 1920x1080 + 竖屏 VDD 1200x1920。
    const DesktopOffset right = compute_vdd_offset(VddPosition::Right, 1920, 1080, 1200, 1920);
    require(right.x == 1920 && right.y == 0, "right offset mismatch");
    const DesktopOffset left = compute_vdd_offset(VddPosition::Left, 1920, 1080, 1200, 1920);
    require(left.x == -1200 && left.y == 0, "left offset mismatch");
    const DesktopOffset top = compute_vdd_offset(VddPosition::Top, 1920, 1080, 1200, 1920);
    require(top.x == 0 && top.y == -1920, "top offset mismatch");
    const DesktopOffset bottom = compute_vdd_offset(VddPosition::Bottom, 1920, 1080, 1200, 1920);
    require(bottom.x == 0 && bottom.y == 1080, "bottom offset mismatch");
}

void test_compute_vdd_offset_other_geometry() {
    // 主屏 2560x1440 + 横屏 VDD 1920x1080，换一组几何再验一遍。
    const DesktopOffset right = compute_vdd_offset(VddPosition::Right, 2560, 1440, 1920, 1080);
    require(right.x == 2560 && right.y == 0, "right offset mismatch");
    const DesktopOffset left = compute_vdd_offset(VddPosition::Left, 2560, 1440, 1920, 1080);
    require(left.x == -1920 && left.y == 0, "left offset mismatch");
    const DesktopOffset top = compute_vdd_offset(VddPosition::Top, 2560, 1440, 1920, 1080);
    require(top.x == 0 && top.y == -1080, "top offset mismatch");
    const DesktopOffset bottom = compute_vdd_offset(VddPosition::Bottom, 2560, 1440, 1920, 1080);
    require(bottom.x == 0 && bottom.y == 1440, "bottom offset mismatch");
}

void test_swapped_dimensions() {
    const auto first = swapped_dimensions(1920U, 1200U);
    require(first.first == 1200U && first.second == 1920U, "1920x1200 swap mismatch");
    const auto second = swapped_dimensions(2560U, 1600U);
    require(second.first == 1600U && second.second == 2560U, "2560x1600 swap mismatch");
    const auto restored = swapped_dimensions(first.first, first.second);
    require(restored.first == 1920U && restored.second == 1200U, "double swap did not restore");
}

}  // namespace

int main() {
    try {
        test_parse_accepts_canonical_names();
        test_parse_is_case_insensitive();
        test_parse_rejects_unknown_text();
        test_name_and_parse_round_trip();
        test_compute_vdd_offset_four_positions();
        test_compute_vdd_offset_other_geometry();
        test_swapped_dimensions();
        std::cout << "All WireView display layout tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Display layout test failed: " << error.what() << '\n';
        return 1;
    }
}
