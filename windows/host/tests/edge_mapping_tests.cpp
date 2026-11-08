#include "edge_mapping.hpp"

#include <iostream>
#include <stdexcept>
#include <string>

using namespace wireview::host;

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

// 从 XML 片段中取 attribute="value" 的 value。
std::wstring attribute_value(const std::wstring& xml, const std::wstring& attribute) {
    const std::wstring needle = attribute + L"=\"";
    const std::size_t start = xml.find(needle);
    if (start == std::wstring::npos) {
        return {};
    }
    const std::size_t value_start = start + needle.size();
    const std::size_t value_end = xml.find(L'"', value_start);
    if (value_end == std::wstring::npos) {
        return {};
    }
    return xml.substr(value_start, value_end - value_start);
}

// 取指定 Zone（按 DeviceId 定位）的 <PhysicalBounds><Rect .../> 片段。
std::wstring zone_physical_bounds(const std::wstring& xml, const std::wstring& device_id) {
    const std::size_t zone_start = xml.find(L"DeviceId=\"" + device_id + L"\"");
    if (zone_start == std::wstring::npos) {
        return {};
    }
    const std::size_t bounds_start = xml.find(L"<PhysicalBounds>", zone_start);
    const std::size_t bounds_end = xml.find(L"</PhysicalBounds>", zone_start);
    if (bounds_start == std::wstring::npos || bounds_end == std::wstring::npos) {
        return {};
    }
    return xml.substr(bounds_start, bounds_end - bounds_start);
}

void test_build_rejects_overlapping_rects() {
    const std::wstring xml = build_zones_layout_xml(
        {0, 0, 2560, 1440}, {1000, 100, 2200, 2020}, L"DISPLAY1", L"DISPLAY20");
    require(xml.empty(), "overlapping monitors must produce no layout");
}

void test_horizontal_right_shares_mm_height() {
    // 主屏 2560x1440 在左，竖屏 VDD 1200x1920 在右（R1 right 摆放）。
    const std::wstring xml = build_zones_layout_xml(
        {0, 0, 2560, 1440}, {2560, 0, 3760, 1920}, L"DISPLAY1", L"DISPLAY20");

    const std::wstring primary_mm = zone_physical_bounds(xml, L"DISPLAY1");
    const std::wstring vdd_mm = zone_physical_bounds(xml, L"DISPLAY20");
    require(!primary_mm.empty() && !vdd_mm.empty(), "zones were not emitted");
    require(attribute_value(primary_mm, L"Height") == L"360", "primary mm height changed");
    // 修复点：副屏共享轴（这里是高度）毫米区间与主屏等长对齐。
    require(attribute_value(vdd_mm, L"Height") == L"360",
        "vdd mm height must equal the primary mm height on the shared axis");
    require(attribute_value(vdd_mm, L"Top") == L"0",
        "vdd mm top must align with the primary mm top on the shared axis");
    require(attribute_value(vdd_mm, L"Width") == L"300",
        "vdd free axis keeps the 4 px/mm baseline");
    // 主屏右边 ↔ 副屏左边共享区间 0..360mm。
    const std::size_t primary_zone = xml.find(L"DeviceId=\"DISPLAY1\"");
    const std::size_t vdd_zone = xml.find(L"DeviceId=\"DISPLAY20\"");
    require(primary_zone < vdd_zone, "primary zone must come first");
    const std::wstring primary_right = xml.substr(
        xml.find(L"<RightLinks>", primary_zone),
        xml.find(L"</RightLinks>", primary_zone) - xml.find(L"<RightLinks>", primary_zone));
    require(attribute_value(primary_right, L"From") == L"0" &&
        attribute_value(primary_right, L"To") == L"360",
        "primary right link must cover the shared mm range");
    require(attribute_value(primary_right, L"TargetId") == L"1",
        "primary right link must target the vdd zone");
    const std::wstring vdd_left = xml.substr(
        xml.find(L"<LeftLinks>", vdd_zone),
        xml.find(L"</LeftLinks>", vdd_zone) - xml.find(L"<LeftLinks>", vdd_zone));
    require(attribute_value(vdd_left, L"From") == L"0" &&
        attribute_value(vdd_left, L"To") == L"360",
        "vdd left link must cover the shared mm range");
    require(attribute_value(vdd_left, L"TargetId") == L"0",
        "vdd left link must target the primary zone");
}

void test_vertical_below_shares_mm_width() {
    // 主屏 2560x1440 在上，横屏 VDD 1920x1200 在下（R1 bottom 摆放）。
    const std::wstring xml = build_zones_layout_xml(
        {0, 0, 2560, 1440}, {0, 1440, 1920, 2640}, L"DISPLAY1", L"DISPLAY20");

    const std::wstring vdd_mm = zone_physical_bounds(xml, L"DISPLAY20");
    require(attribute_value(vdd_mm, L"Width") == L"640",
        "vdd mm width must equal the primary mm width on the shared axis");
    require(attribute_value(vdd_mm, L"Left") == L"0",
        "vdd mm left must align with the primary mm left on the shared axis");
    require(attribute_value(vdd_mm, L"Height") == L"300",
        "vdd free axis keeps the 4 px/mm baseline");
    const std::size_t vdd_zone = xml.find(L"DeviceId=\"DISPLAY20\"");
    const std::wstring vdd_top = xml.substr(
        xml.find(L"<TopLinks>", vdd_zone),
        xml.find(L"</TopLinks>", vdd_zone) - xml.find(L"<TopLinks>", vdd_zone));
    require(attribute_value(vdd_top, L"From") == L"0" &&
        attribute_value(vdd_top, L"To") == L"640",
        "vdd top link must cover the shared mm range");
}

void test_vertical_above_uses_negative_mm() {
    // 副屏在主屏上方（top 摆放）：毫米矩形在主屏上方，共享轴仍对齐主屏宽度。
    const std::wstring xml = build_zones_layout_xml(
        {0, 0, 2560, 1440}, {0, -1920, 1920, 0}, L"DISPLAY1", L"DISPLAY20");

    const std::wstring vdd_mm = zone_physical_bounds(xml, L"DISPLAY20");
    require(attribute_value(vdd_mm, L"Top") == L"-480",
        "vdd above the primary keeps its negative mm position");
    require(attribute_value(vdd_mm, L"Width") == L"640",
        "vdd mm width must equal the primary mm width on the shared axis");
}

void test_disconnected_sides_use_catch_all() {
    const std::wstring xml = build_zones_layout_xml(
        {0, 0, 2560, 1440}, {2560, 0, 3760, 1920}, L"DISPLAY1", L"DISPLAY20");
    std::size_t catch_all = 0;
    for (std::size_t position = xml.find(L"TargetId=\"-1\"");
         position != std::wstring::npos;
         position = xml.find(L"TargetId=\"-1\"", position + 1U)) {
        ++catch_all;
    }
    require(catch_all == 6, "each zone needs three disconnected catch-all links");
}

void test_device_id_prefix_is_stripped() {
    const std::wstring xml = build_zones_layout_xml(
        {0, 0, 1920, 1080}, {1920, 0, 3120, 2400},
        L"\\\\.\\DISPLAY1", L"\\\\.\\DISPLAY20");
    require(xml.find(L"DeviceId=\"\\\\.\\DISPLAY1\"") == std::wstring::npos,
        "the \\\\.\\ prefix must not reach the daemon");
    require(xml.find(L"DeviceId=\"DISPLAY1\"") != std::wstring::npos,
        "primary device id must be present");
}

}  // namespace

int main() {
    try {
        test_build_rejects_overlapping_rects();
        test_horizontal_right_shares_mm_height();
        test_vertical_below_shares_mm_width();
        test_vertical_above_uses_negative_mm();
        test_disconnected_sides_use_catch_all();
        test_device_id_prefix_is_stripped();
    } catch (const std::exception& error) {
        std::cerr << "edge mapping test failed: " << error.what() << '\n';
        return 1;
    }
    std::cout << "edge mapping tests passed\n";
    return 0;
}
