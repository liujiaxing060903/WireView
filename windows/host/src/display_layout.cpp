#include "display_layout.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <cctype>
#include <cstddef>
#include <cwctype>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace wireview::host {
namespace {

// 逐字符忽略大小写比较两段宽字符文本。
[[nodiscard]] bool equals_ignore_case(std::wstring_view left, std::wstring_view right) {
    if (left.size() != right.size()) {
        return false;
    }
    for (std::size_t index = 0U; index < left.size(); ++index) {
        if (std::towlower(left[index]) != std::towlower(right[index])) {
            return false;
        }
    }
    return true;
}

// 找到与路径 sourceInfo 匹配的源模式条目；找不到返回 nullptr。
// 注意：路径位置挂在 DISPLAYCONFIG_SOURCE_MODE.position 上，不在 PATH_INFO 里。
[[nodiscard]] DISPLAYCONFIG_MODE_INFO* find_source_mode(
    std::vector<DISPLAYCONFIG_MODE_INFO>& modes,
    const DISPLAYCONFIG_PATH_INFO& path) {
    for (auto& mode : modes) {
        if (mode.infoType != DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE) {
            continue;
        }
        if (mode.adapterId.HighPart != path.sourceInfo.adapterId.HighPart ||
            mode.adapterId.LowPart != path.sourceInfo.adapterId.LowPart ||
            mode.id != path.sourceInfo.id) {
            continue;
        }
        return &mode;
    }
    return nullptr;
}

}  // namespace

std::optional<VddPosition> parse_vdd_position(std::string_view text) {
    std::string lowered(text);
    for (char& character : lowered) {
        character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    }
    if (lowered == "right") {
        return VddPosition::Right;
    }
    if (lowered == "left") {
        return VddPosition::Left;
    }
    if (lowered == "top") {
        return VddPosition::Top;
    }
    if (lowered == "bottom") {
        return VddPosition::Bottom;
    }
    return std::nullopt;
}

const char* vdd_position_name(VddPosition position) {
    if (position == VddPosition::Right) {
        return "right";
    }
    if (position == VddPosition::Left) {
        return "left";
    }
    if (position == VddPosition::Top) {
        return "top";
    }
    return "bottom";
}

DesktopOffset compute_vdd_offset(
    VddPosition position,
    long primary_width,
    long primary_height,
    long vdd_width,
    long vdd_height) {
    DesktopOffset offset;
    switch (position) {
        case VddPosition::Right:
            offset.x = primary_width;
            offset.y = 0;
            break;
        case VddPosition::Left:
            offset.x = -vdd_width;
            offset.y = 0;
            break;
        case VddPosition::Top:
            offset.x = 0;
            offset.y = -vdd_height;
            break;
        case VddPosition::Bottom:
            offset.x = 0;
            offset.y = primary_height;
            break;
    }
    return offset;
}

std::pair<unsigned int, unsigned int> swapped_dimensions(
    unsigned int width,
    unsigned int height) {
    return {height, width};
}

bool apply_vdd_position(const wchar_t* vdd_device_name, VddPosition position, std::wstring& error) {
    if (vdd_device_name == nullptr) {
        error = L"副屏显示器路径名称为空";
        return false;
    }

    UINT32 path_count = 0U;
    UINT32 mode_count = 0U;
    LONG result = GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &path_count, &mode_count);
    if (result != ERROR_SUCCESS) {
        error = L"查询显示器拓扑失败（GetDisplayConfigBufferSizes 错误码 " +
            std::to_wstring(result) + L"）";
        return false;
    }
    std::vector<DISPLAYCONFIG_PATH_INFO> paths(path_count);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(mode_count);
    result = QueryDisplayConfig(
        QDC_ONLY_ACTIVE_PATHS, &path_count, paths.data(), &mode_count, modes.data(), nullptr);
    if (result != ERROR_SUCCESS) {
        error = L"查询显示器拓扑失败（QueryDisplayConfig 错误码 " +
            std::to_wstring(result) + L"）";
        return false;
    }
    paths.resize(path_count);
    modes.resize(mode_count);

    if (paths.size() != 2U) {
        error = L"副屏摆放仅支持主屏 + WireView 副屏两块显示器（当前 " +
            std::to_wstring(paths.size()) + L" 块），已保持现有拓扑不变";
        return false;
    }

    // "\\.\DISPLAYn" 是 GDI 源名，只能从 DISPLAYCONFIG_SOURCE_DEVICE_NAME 读到；
    // TARGET_DEVICE_NAME 只提供友好名与 "\\?\DISPLAY#..." 设备路径。
    std::size_t vdd_index = paths.size();
    for (std::size_t index = 0U; index < paths.size(); ++index) {
        DISPLAYCONFIG_SOURCE_DEVICE_NAME source_name{};
        source_name.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        source_name.header.size = sizeof(source_name);
        source_name.header.adapterId = paths[index].sourceInfo.adapterId;
        source_name.header.id = paths[index].sourceInfo.id;
        if (DisplayConfigGetDeviceInfo(&source_name.header) != ERROR_SUCCESS) {
            continue;
        }
        if (equals_ignore_case(source_name.viewGdiDeviceName, vdd_device_name)) {
            vdd_index = index;
            break;
        }
    }
    if (vdd_index == paths.size()) {
        error = std::wstring(L"未找到副屏显示器路径: ") + vdd_device_name;
        return false;
    }
    const std::size_t primary_index = vdd_index == 0U ? 1U : 0U;

    DISPLAYCONFIG_MODE_INFO* vdd_mode = find_source_mode(modes, paths[vdd_index]);
    DISPLAYCONFIG_MODE_INFO* primary_mode = find_source_mode(modes, paths[primary_index]);
    if (vdd_mode == nullptr || primary_mode == nullptr) {
        error = L"读取显示器当前分辨率失败，已保持现有拓扑不变";
        return false;
    }

    const DesktopOffset offset = compute_vdd_offset(
        position,
        static_cast<LONG>(primary_mode->sourceMode.width),
        static_cast<LONG>(primary_mode->sourceMode.height),
        static_cast<LONG>(vdd_mode->sourceMode.width),
        static_cast<LONG>(vdd_mode->sourceMode.height));
    // 只写 VDD 路径源模式的位置，绝不改动另一条路径及其模式的任何字段。
    vdd_mode->sourceMode.position.x = offset.x;
    vdd_mode->sourceMode.position.y = offset.y;

    result = SetDisplayConfig(
        static_cast<UINT32>(paths.size()), paths.data(),
        static_cast<UINT32>(modes.size()), modes.data(),
        SDC_APPLY | SDC_USE_SUPPLIED_DISPLAY_CONFIG);
    if (result != ERROR_SUCCESS) {
        error = L"摆放副屏位置失败（SetDisplayConfig 错误码 " +
            std::to_wstring(result) + L"），已保持现有拓扑不变";
        return false;
    }
    return true;
}

}  // namespace wireview::host
