#include "wireview/input.hpp"

#include <stdexcept>
#include <utility>

namespace wireview::protocol {
namespace {

void write_u16(std::vector<std::uint8_t>& output, std::size_t offset, std::uint16_t value) {
    output[offset] = static_cast<std::uint8_t>(value & 0xffU);
    output[offset + 1U] = static_cast<std::uint8_t>((value >> 8U) & 0xffU);
}

void write_u32(std::vector<std::uint8_t>& output, std::size_t offset, std::uint32_t value) {
    for (std::size_t index = 0; index < 4U; ++index) {
        output[offset + index] =
            static_cast<std::uint8_t>((value >> (index * 8U)) & 0xffU);
    }
}

[[nodiscard]] std::uint16_t read_u16(
    std::span<const std::uint8_t> input,
    std::size_t offset) {
    return static_cast<std::uint16_t>(input[offset]) |
           static_cast<std::uint16_t>(
               static_cast<std::uint16_t>(input[offset + 1U]) << 8U);
}

[[nodiscard]] std::uint32_t read_u32(
    std::span<const std::uint8_t> input,
    std::size_t offset) {
    std::uint32_t value = 0;
    for (std::size_t index = 0; index < 4U; ++index) {
        value |= static_cast<std::uint32_t>(input[offset + index]) << (index * 8U);
    }
    return value;
}

[[nodiscard]] bool valid_action(InputAction action) noexcept {
    return action >= InputAction::down && action <= InputAction::hover;
}

[[nodiscard]] bool valid_tool(InputTool tool) noexcept {
    return tool >= InputTool::finger && tool <= InputTool::mouse;
}

[[nodiscard]] bool valid_batch(const InputBatch& batch) noexcept {
    if (batch.viewport_width == 0U || batch.viewport_height == 0U ||
        batch.records.empty() || batch.records.size() > kMaxInputRecords) {
        return false;
    }
    for (const auto& record : batch.records) {
        if (!valid_action(record.action) || !valid_tool(record.tool)) {
            return false;
        }
    }
    return true;
}

}  // namespace

std::vector<std::uint8_t> encode_input_batch(const InputBatch& batch) {
    if (!valid_batch(batch)) {
        throw std::invalid_argument("invalid WireView input batch");
    }

    std::vector<std::uint8_t> output(
        kInputBatchHeaderSize + batch.records.size() * kInputRecordSize, 0U);
    output[0] = kInputSchemaVersion;
    output[1] = kInputRecordSize;
    write_u16(output, 2, static_cast<std::uint16_t>(batch.records.size()));
    write_u16(output, 4, batch.viewport_width);
    write_u16(output, 6, batch.viewport_height);

    for (std::size_t index = 0; index < batch.records.size(); ++index) {
        const auto& record = batch.records[index];
        const std::size_t offset = kInputBatchHeaderSize + index * kInputRecordSize;
        output[offset] = static_cast<std::uint8_t>(record.action);
        output[offset + 1U] = static_cast<std::uint8_t>(record.tool);
        write_u16(output, offset + 2U, record.pointer_id);
        write_u16(output, offset + 4U, record.normalized_x);
        write_u16(output, offset + 6U, record.normalized_y);
        write_u16(output, offset + 8U, record.pressure);
        write_u16(
            output,
            offset + 10U,
            static_cast<std::uint16_t>(record.tilt_x_hundredths));
        write_u16(
            output,
            offset + 12U,
            static_cast<std::uint16_t>(record.tilt_y_hundredths));
        write_u16(output, offset + 14U, record.buttons);
        write_u32(output, offset + 16U, record.time_offset_micros);
    }
    return output;
}

InputDecodeResult decode_input_batch(std::span<const std::uint8_t> payload) {
    if (payload.size() < kInputBatchHeaderSize) {
        return {.error = InputDecodeError::truncated_header};
    }
    if (payload[0] != kInputSchemaVersion) {
        return {.error = InputDecodeError::unsupported_version};
    }
    if (payload[1] != kInputRecordSize) {
        return {.error = InputDecodeError::invalid_record_size};
    }

    const std::uint16_t count = read_u16(payload, 2);
    if (count == 0U || count > kMaxInputRecords) {
        return {.error = InputDecodeError::invalid_record_count};
    }
    const std::uint16_t viewport_width = read_u16(payload, 4);
    const std::uint16_t viewport_height = read_u16(payload, 6);
    if (viewport_width == 0U || viewport_height == 0U) {
        return {.error = InputDecodeError::invalid_viewport};
    }
    const std::size_t expected_size =
        kInputBatchHeaderSize + static_cast<std::size_t>(count) * kInputRecordSize;
    if (payload.size() != expected_size) {
        return {.error = InputDecodeError::size_mismatch};
    }

    InputBatch batch{
        .viewport_width = viewport_width,
        .viewport_height = viewport_height,
    };
    batch.records.reserve(count);
    for (std::uint16_t index = 0; index < count; ++index) {
        const std::size_t offset =
            kInputBatchHeaderSize + static_cast<std::size_t>(index) * kInputRecordSize;
        const auto action = static_cast<InputAction>(payload[offset]);
        const auto tool = static_cast<InputTool>(payload[offset + 1U]);
        if (!valid_action(action)) {
            return {.error = InputDecodeError::invalid_action};
        }
        if (!valid_tool(tool)) {
            return {.error = InputDecodeError::invalid_tool};
        }
        batch.records.push_back(InputRecord{
            .action = action,
            .tool = tool,
            .pointer_id = read_u16(payload, offset + 2U),
            .normalized_x = read_u16(payload, offset + 4U),
            .normalized_y = read_u16(payload, offset + 6U),
            .pressure = read_u16(payload, offset + 8U),
            .tilt_x_hundredths = static_cast<std::int16_t>(
                read_u16(payload, offset + 10U)),
            .tilt_y_hundredths = static_cast<std::int16_t>(
                read_u16(payload, offset + 12U)),
            .buttons = read_u16(payload, offset + 14U),
            .time_offset_micros = read_u32(payload, offset + 16U),
        });
    }
    return {
        .error = InputDecodeError::none,
        .batch = std::move(batch),
    };
}

}  // namespace wireview::protocol
