#include "idd_frame_relay_codec.hpp"

#include <cstring>
#include <limits>
#include <stdexcept>

namespace wireview::idd_relay {

std::vector<std::byte> encode_message(
    MessageType type,
    std::span<const std::byte> payload) {
    MessageHeader header{};
    header.type = type;
    header.payload_size = static_cast<std::uint32_t>(payload.size());

    std::vector<std::byte> encoded(sizeof(header) + payload.size());
    std::memcpy(encoded.data(), &header, sizeof(header));
    if (!payload.empty()) {
        std::memcpy(encoded.data() + sizeof(header), payload.data(), payload.size());
    }
    return encoded;
}

ParsedMessage parse_message(std::span<const std::byte> bytes) {
    if (bytes.size() < sizeof(MessageHeader)) {
        throw std::invalid_argument("WireView Idd relay message is shorter than its header");
    }
    MessageHeader header{};
    std::memcpy(&header, bytes.data(), sizeof(header));
    if (header.magic != kMagic || header.version != kProtocolVersion) {
        throw std::invalid_argument("WireView Idd relay message has an invalid magic or version");
    }
    if (header.payload_size != bytes.size() - sizeof(MessageHeader)) {
        throw std::invalid_argument("WireView Idd relay message has an invalid payload length");
    }
    ParsedMessage parsed;
    parsed.type = header.type;
    parsed.payload.assign(bytes.begin() + sizeof(MessageHeader), bytes.end());
    return parsed;
}

std::vector<std::byte> encode_shared_texture_name(
    std::uint32_t slot,
    std::wstring_view name) {
    constexpr std::size_t kMaximumNameCodeUnits = 256U;
    if (name.empty() || name.size() > kMaximumNameCodeUnits ||
        name.find(L'\0') != std::wstring_view::npos ||
        name.size() > std::numeric_limits<std::uint32_t>::max()) {
        throw std::invalid_argument("WireView Idd relay shared texture name is invalid");
    }
    SharedTextureNameHeader header{};
    header.slot = slot;
    header.utf16_code_units = static_cast<std::uint32_t>(name.size());
    std::vector<std::byte> payload(sizeof(header) + name.size() * sizeof(wchar_t));
    std::memcpy(payload.data(), &header, sizeof(header));
    std::memcpy(payload.data() + sizeof(header), name.data(), name.size() * sizeof(wchar_t));
    return encode_message(
        MessageType::shared_texture_name,
        std::span<const std::byte>(payload.data(), payload.size()));
}

SharedTextureName parse_shared_texture_name(const ParsedMessage& message) {
    constexpr std::size_t kMaximumNameCodeUnits = 256U;
    if (message.type != MessageType::shared_texture_name ||
        message.payload.size() < sizeof(SharedTextureNameHeader)) {
        throw std::invalid_argument("WireView Idd relay shared texture name has an invalid header");
    }
    SharedTextureNameHeader header{};
    std::memcpy(&header, message.payload.data(), sizeof(header));
    if (header.utf16_code_units == 0U || header.utf16_code_units > kMaximumNameCodeUnits ||
        message.payload.size() != sizeof(header) +
            static_cast<std::size_t>(header.utf16_code_units) * sizeof(wchar_t)) {
        throw std::invalid_argument("WireView Idd relay shared texture name has an invalid length");
    }
    SharedTextureName result{};
    result.slot = header.slot;
    result.name.resize(header.utf16_code_units);
    std::memcpy(result.name.data(), message.payload.data() + sizeof(header),
        result.name.size() * sizeof(wchar_t));
    if (result.name.find(L'\0') != std::wstring::npos) {
        throw std::invalid_argument("WireView Idd relay shared texture name contains a NUL");
    }
    return result;
}

}  // namespace wireview::idd_relay
