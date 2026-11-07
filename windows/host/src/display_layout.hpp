// WireView 副屏桌面摆放：方位解析与偏移计算是纯逻辑（可单测），
// apply_vdd_position 走 DisplayConfig API 操作真实显示器（不进单测）。
// 本头文件刻意不引入 windows.h：long/unsigned int 与 LONG/UINT 在 Windows 上
// 是同一类型，桌面偏移与分辨率不需要 Windows 头文件即可表达。
#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace wireview::host {

// 副屏相对主屏的摆放方位。主屏恒定位于桌面 (0,0) 起点（Windows 约定）。
enum class VddPosition { Right, Left, Top, Bottom };

// 解析方位文本，接受 "right"/"left"/"top"/"bottom"，大小写不敏感；
// 非法输入返回 nullopt。
[[nodiscard]] std::optional<VddPosition> parse_vdd_position(std::string_view text);

// 返回方位的规范小写名，与 parse_vdd_position 接受的输入一一对应。
[[nodiscard]] const char* vdd_position_name(VddPosition position);

// 副屏左上角在 Windows 桌面坐标系中的偏移。主屏恒在 (0,0)。
struct DesktopOffset {
    long x = 0;
    long y = 0;
};

// right: (primary_w, 0)   left: (-vdd_w, 0)   top: (0, -vdd_h)   bottom: (0, primary_h)
[[nodiscard]] DesktopOffset compute_vdd_offset(
    VddPosition position,
    long primary_width,
    long primary_height,
    long vdd_width,
    long vdd_height);

// 竖屏方向交换宽高；输入必须已是偶数，直接交换即可。
[[nodiscard]] std::pair<unsigned int, unsigned int> swapped_dimensions(
    unsigned int width,
    unsigned int height);

// 把 vdd_device_name（格式 "\\.\DISPLAYn"）所在的活动路径摆放到位（其余路径原样保留）。
// 成功返回 true；失败返回 false 并填充 error（中文，可直接显示给用户）。
bool apply_vdd_position(const wchar_t* vdd_device_name, VddPosition position, std::wstring& error);

}  // namespace wireview::host
