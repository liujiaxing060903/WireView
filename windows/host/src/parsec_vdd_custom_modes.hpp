#pragma once

#include <array>
#include <cstdint>
#include <optional>

namespace wireview::host {

struct ParsecVddCustomMode {
    std::uint32_t width{};
    std::uint32_t height{};
    std::uint32_t refresh_hz{};
};

// Must be called from an elevated process. Empty slots remove only their own
// numbered key; no other Parsec configuration is touched.
void apply_parsec_vdd_custom_modes(
    const std::array<std::optional<ParsecVddCustomMode>, 5U>& modes);

// 读取 Parsec VDD 已应用的 custom 槽。驱动未安装或配置不可访问时返回 nullopt。
std::optional<std::array<std::optional<ParsecVddCustomMode>, 5U>> read_parsec_vdd_custom_modes();

void set_parsec_vdd_display_mode(
    const wchar_t* display_device,
    std::uint32_t width,
    std::uint32_t height,
    std::uint32_t refresh_hz);

}  // namespace wireview::host
