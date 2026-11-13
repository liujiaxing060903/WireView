#pragma once

#include <array>
#include <algorithm>
#include <cstdint>
#include <optional>
#include <span>
#include <string>

namespace wireview::host {

struct ParsecVddCustomMode {
    std::uint32_t width{};
    std::uint32_t height{};
    std::uint32_t refresh_hz{};
    bool operator==(const ParsecVddCustomMode&) const = default;
};

inline std::wstring parsec_vdd_mode_reservation_name(const ParsecVddCustomMode& mode) {
    return L"Local\\WireView.VddMode." + std::to_wstring(mode.width) + L"x" +
        std::to_wstring(mode.height) + L"@" + std::to_wstring(mode.refresh_hz);
}

inline std::optional<std::size_t> choose_parsec_vdd_custom_slot(
    const std::array<std::optional<ParsecVddCustomMode>, 5U>& modes,
    const ParsecVddCustomMode& requested, std::span<const ParsecVddCustomMode> active) {
    for (std::size_t index = 0; index < modes.size(); ++index) {
        if (modes[index] == requested) { return index; }
    }
    for (std::size_t index = 0; index < modes.size(); ++index) {
        if (!modes[index]) { return index; }
    }
    for (int pass = 0; pass < 2; ++pass) {
        for (std::size_t index = 0; index < modes.size(); ++index) {
            const auto& mode = *modes[index];
            const bool occupied = std::any_of(active.begin(), active.end(), [&](const auto& used) {
                const bool same_size = (mode.width == used.width && mode.height == used.height) ||
                    (mode.width == used.height && mode.height == used.width);
                const auto hz_gap = std::max(mode.refresh_hz, used.refresh_hz) -
                    std::min(mode.refresh_hz, used.refresh_hz);
                return same_size && (used.refresh_hz <= 1U || hz_gap <= 1U);
            });
            const bool other_orientation = mode.width == requested.height &&
                mode.height == requested.width && mode.refresh_hz == requested.refresh_hz;
            if (!occupied && (pass != 0 || !other_orientation)) { return index; }
        }
    }
    return std::nullopt;
}

// Must be called from an elevated process. Empty slots remove only their own
// numbered key; no other Parsec configuration is touched.
void apply_parsec_vdd_custom_modes(
    const std::array<std::optional<ParsecVddCustomMode>, 5U>& modes);

// Reuse, fill, or back up and replace an unused slot. Active modes are protected.
void ensure_parsec_vdd_custom_mode(const ParsecVddCustomMode& mode);

// 读取 Parsec VDD 已应用的 custom 槽。驱动未安装或配置不可访问时返回 nullopt。
std::optional<std::array<std::optional<ParsecVddCustomMode>, 5U>> read_parsec_vdd_custom_modes();

void set_parsec_vdd_display_mode(
    const wchar_t* display_device,
    std::uint32_t width,
    std::uint32_t height,
    std::uint32_t refresh_hz);

}  // namespace wireview::host
