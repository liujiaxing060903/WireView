#pragma once

#include <cstdint>
#include <span>
#include <vector>

namespace wireview::host {

struct AnnexBAccessUnit {
    std::vector<std::uint8_t> bytes;
    bool keyframe{false};
};

[[nodiscard]] std::vector<AnnexBAccessUnit> split_annex_b_access_units(
    std::span<const std::uint8_t> bitstream);

[[nodiscard]] std::vector<std::uint8_t> make_avc_stream_config(
    std::uint16_t width,
    std::uint16_t height,
    std::uint16_t frame_rate_numerator,
    std::uint16_t frame_rate_denominator);

}  // namespace wireview::host
