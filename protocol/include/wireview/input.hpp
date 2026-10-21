#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace wireview::protocol {

inline constexpr std::uint8_t kInputSchemaVersion = 1;
inline constexpr std::uint8_t kInputRecordSize = 20;
inline constexpr std::size_t kInputBatchHeaderSize = 8;
inline constexpr std::uint16_t kMaxInputRecords = 64;

enum class InputAction : std::uint8_t {
    down = 1,
    move = 2,
    up = 3,
    cancel = 4,
    hover = 5,
};

enum class InputTool : std::uint8_t {
    finger = 1,
    stylus = 2,
    eraser = 3,
    mouse = 4,
};

struct InputRecord {
    InputAction action{InputAction::cancel};
    InputTool tool{InputTool::finger};
    std::uint16_t pointer_id{0};
    std::uint16_t normalized_x{0};
    std::uint16_t normalized_y{0};
    std::uint16_t pressure{0};
    std::int16_t tilt_x_hundredths{0};
    std::int16_t tilt_y_hundredths{0};
    std::uint16_t buttons{0};
    std::uint32_t time_offset_micros{0};
};

struct InputBatch {
    std::uint16_t viewport_width{0};
    std::uint16_t viewport_height{0};
    std::vector<InputRecord> records;
};

enum class InputDecodeError {
    none,
    truncated_header,
    unsupported_version,
    invalid_record_size,
    invalid_record_count,
    invalid_viewport,
    size_mismatch,
    invalid_action,
    invalid_tool,
};

struct InputDecodeResult {
    InputDecodeError error{InputDecodeError::none};
    std::optional<InputBatch> batch;

    [[nodiscard]] bool ok() const noexcept {
        return error == InputDecodeError::none && batch.has_value();
    }
};

[[nodiscard]] std::vector<std::uint8_t> encode_input_batch(const InputBatch& batch);

[[nodiscard]] InputDecodeResult decode_input_batch(std::span<const std::uint8_t> payload);

}  // namespace wireview::protocol
