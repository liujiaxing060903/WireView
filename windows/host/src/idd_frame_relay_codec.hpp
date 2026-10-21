#pragma once

#include "idd_frame_relay.hpp"

#include <cstddef>
#include <cstring>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace wireview::idd_relay {

struct ParsedMessage {
    MessageType type{};
    std::vector<std::byte> payload;
};

std::vector<std::byte> encode_message(
    MessageType type,
    std::span<const std::byte> payload);

template <typename Payload>
std::vector<std::byte> encode_message(MessageType type, const Payload& payload) {
    const auto bytes = std::as_bytes(std::span{&payload, std::size_t{1U}});
    return encode_message(type, bytes);
}

ParsedMessage parse_message(std::span<const std::byte> bytes);

struct SharedTextureName {
    std::uint32_t slot{};
    std::wstring name;
};

std::vector<std::byte> encode_shared_texture_name(
    std::uint32_t slot,
    std::wstring_view name);
SharedTextureName parse_shared_texture_name(const ParsedMessage& message);

template <typename Payload>
Payload parse_payload(const ParsedMessage& message, MessageType expected_type) {
    if (message.type != expected_type || message.payload.size() != sizeof(Payload)) {
        throw std::invalid_argument("WireView Idd relay message has an unexpected type or size");
    }
    Payload payload{};
    std::memcpy(&payload, message.payload.data(), sizeof(payload));
    return payload;
}

}  // namespace wireview::idd_relay
