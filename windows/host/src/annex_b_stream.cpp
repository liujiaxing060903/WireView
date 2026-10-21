#include "annex_b_stream.hpp"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <stdexcept>
#include <utility>

namespace wireview::host {
namespace {

struct NalRange {
    std::size_t start;
    std::size_t payload;
    std::size_t end;
    std::uint8_t type;
};

struct StartCode {
    std::size_t offset;
    std::size_t size;
};

std::optional<StartCode> find_start_code(
    std::span<const std::uint8_t> bytes,
    std::size_t position) {
    for (std::size_t index = position; index + 3U <= bytes.size(); ++index) {
        if (bytes[index] != 0U || bytes[index + 1U] != 0U) {
            continue;
        }
        if (bytes[index + 2U] == 1U) {
            return StartCode{.offset = index, .size = 3U};
        }
        if (index + 4U <= bytes.size() &&
            bytes[index + 2U] == 0U && bytes[index + 3U] == 1U) {
            return StartCode{.offset = index, .size = 4U};
        }
    }
    return std::nullopt;
}

std::vector<NalRange> find_nal_units(std::span<const std::uint8_t> bytes) {
    std::vector<NalRange> result;
    auto current = find_start_code(bytes, 0U);
    while (current) {
        const auto payload = current->offset + current->size;
        const auto next = find_start_code(bytes, payload + 1U);
        const auto end = next ? next->offset : bytes.size();
        if (payload < end) {
            result.push_back(NalRange{
                .start = current->offset,
                .payload = payload,
                .end = end,
                .type = static_cast<std::uint8_t>(bytes[payload] & 0x1fU),
            });
        }
        current = next;
    }
    return result;
}

class BitReader {
public:
    explicit BitReader(std::vector<std::uint8_t> bytes) : bytes_(std::move(bytes)) {
    }

    std::optional<std::uint32_t> read_unsigned_exp_golomb() {
        std::uint32_t leading_zeroes = 0;
        while (true) {
            const auto bit = read_bit();
            if (!bit) {
                return std::nullopt;
            }
            if (*bit != 0U) {
                break;
            }
            if (++leading_zeroes >= 32U) {
                return std::nullopt;
            }
        }

        std::uint32_t suffix = 0;
        for (std::uint32_t index = 0; index < leading_zeroes; ++index) {
            const auto bit = read_bit();
            if (!bit) {
                return std::nullopt;
            }
            suffix = (suffix << 1U) | *bit;
        }
        return ((1U << leading_zeroes) - 1U) + suffix;
    }

private:
    std::optional<std::uint32_t> read_bit() {
        if (bit_offset_ >= bytes_.size() * 8U) {
            return std::nullopt;
        }
        const auto byte = bytes_[bit_offset_ / 8U];
        const auto shift = 7U - static_cast<unsigned>(bit_offset_ % 8U);
        ++bit_offset_;
        return static_cast<std::uint32_t>((byte >> shift) & 1U);
    }

    std::vector<std::uint8_t> bytes_;
    std::size_t bit_offset_{0U};
};

std::optional<std::uint32_t> first_macroblock_in_slice(
    std::span<const std::uint8_t> bytes,
    const NalRange& nal) {
    if (nal.payload + 1U >= nal.end) {
        return std::nullopt;
    }

    std::vector<std::uint8_t> rbsp;
    rbsp.reserve(nal.end - nal.payload - 1U);
    unsigned zero_count = 0;
    for (std::size_t index = nal.payload + 1U; index < nal.end; ++index) {
        const auto byte = bytes[index];
        if (zero_count >= 2U && byte == 0x03U) {
            zero_count = 0;
            continue;
        }
        rbsp.push_back(byte);
        zero_count = byte == 0U ? zero_count + 1U : 0U;
    }
    return BitReader(std::move(rbsp)).read_unsigned_exp_golomb();
}

void write_u16(std::vector<std::uint8_t>& output, std::size_t offset, std::uint16_t value) {
    output[offset] = static_cast<std::uint8_t>(value & 0xffU);
    output[offset + 1U] = static_cast<std::uint8_t>((value >> 8U) & 0xffU);
}

void flush_access_unit(
    std::vector<AnnexBAccessUnit>& output,
    AnnexBAccessUnit& current,
    bool& has_vcl) {
    if (has_vcl && !current.bytes.empty()) {
        output.push_back(std::move(current));
        current = {};
    }
    has_vcl = false;
}

}  // namespace

std::vector<AnnexBAccessUnit> split_annex_b_access_units(
    std::span<const std::uint8_t> bitstream) {
    const auto nals = find_nal_units(bitstream);
    if (nals.empty()) {
        throw std::invalid_argument("H.264 stream does not contain Annex-B start codes");
    }

    std::vector<AnnexBAccessUnit> output;
    AnnexBAccessUnit current;
    bool has_vcl = false;

    for (const auto& nal : nals) {
        const bool is_vcl = nal.type == 1U || nal.type == 5U;
        if (is_vcl && has_vcl) {
            const auto first_macroblock = first_macroblock_in_slice(bitstream, nal);
            if (!first_macroblock || *first_macroblock == 0U) {
                flush_access_unit(output, current, has_vcl);
            }
        } else if (!is_vcl && has_vcl &&
                   (nal.type == 6U || nal.type == 7U || nal.type == 8U || nal.type == 9U)) {
            flush_access_unit(output, current, has_vcl);
        }

        current.bytes.insert(
            current.bytes.end(),
            bitstream.begin() + static_cast<std::ptrdiff_t>(nal.start),
            bitstream.begin() + static_cast<std::ptrdiff_t>(nal.end));
        current.keyframe = current.keyframe || nal.type == 5U;
        has_vcl = has_vcl || is_vcl;
    }
    flush_access_unit(output, current, has_vcl);

    if (output.empty()) {
        throw std::invalid_argument("H.264 stream does not contain video access units");
    }
    return output;
}

std::vector<std::uint8_t> make_avc_stream_config(
    std::uint16_t width,
    std::uint16_t height,
    std::uint16_t frame_rate_numerator,
    std::uint16_t frame_rate_denominator) {
    if (width == 0U || height == 0U ||
        frame_rate_numerator == 0U || frame_rate_denominator == 0U) {
        throw std::invalid_argument("video dimensions and frame rate must be non-zero");
    }

    std::vector<std::uint8_t> output(16U, 0U);
    output[0] = 1U;  // H.264/AVC
    output[1] = 1U;  // Annex-B byte stream
    write_u16(output, 4U, width);
    write_u16(output, 6U, height);
    write_u16(output, 8U, frame_rate_numerator);
    write_u16(output, 10U, frame_rate_denominator);
    return output;
}

}  // namespace wireview::host
