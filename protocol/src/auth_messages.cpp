#include "wireview/auth_messages.hpp"

#include <cstring>

namespace wireview::protocol {
namespace {

constexpr std::uint8_t kAuthSchemaVersion = 1U;

void put_u16(std::vector<std::uint8_t>& out, std::uint16_t value) {
    out.push_back(static_cast<std::uint8_t>(value & 0xffU));
    out.push_back(static_cast<std::uint8_t>(value >> 8U));
}

void put_bytes(std::vector<std::uint8_t>& out, std::span<const std::uint8_t> bytes) {
    out.insert(out.end(), bytes.begin(), bytes.end());
}

void put_string(std::vector<std::uint8_t>& out, const std::string& value) {
    put_u16(out, static_cast<std::uint16_t>(value.size()));
    out.insert(out.end(), value.begin(), value.end());
}

struct Reader {
    std::span<const std::uint8_t> bytes;
    std::size_t offset{0U};

    [[nodiscard]] bool read_u8(std::uint8_t* value) {
        if (offset + 1U > bytes.size()) {
            return false;
        }
        *value = bytes[offset];
        offset += 1U;
        return true;
    }

    [[nodiscard]] bool read_u16(std::uint16_t* value) {
        if (offset + 2U > bytes.size()) {
            return false;
        }
        *value = static_cast<std::uint16_t>(
            static_cast<std::uint16_t>(bytes[offset]) |
            (static_cast<std::uint16_t>(bytes[offset + 1U]) << 8U));
        offset += 2U;
        return true;
    }

    [[nodiscard]] bool read_bytes(std::size_t count, std::uint8_t* destination) {
        if (count > bytes.size() - offset) {
            return false;
        }
        std::memcpy(destination, bytes.data() + offset, count);
        offset += count;
        return true;
    }

    [[nodiscard]] bool read_string(std::size_t maximum, std::string* value) {
        std::uint16_t length = 0;
        if (!read_u16(&length) || length > maximum) {
            return false;
        }
        if (length > bytes.size() - offset) {
            return false;
        }
        value->assign(
            reinterpret_cast<const char*>(bytes.data() + offset),
            static_cast<std::size_t>(length));
        offset += length;
        return true;
    }

    [[nodiscard]] bool at_end() const {
        return offset == bytes.size();
    }
};

}  // namespace

std::vector<std::uint8_t> encode_auth_challenge(const AuthChallenge& challenge) {
    std::vector<std::uint8_t> payload;
    payload.reserve(4U + kAuthNonceSize + kAuthUuidSize + challenge.host_name.size());
    payload.push_back(kAuthSchemaVersion);
    payload.push_back(static_cast<std::uint8_t>(challenge.mode));
    put_string(payload, challenge.host_name);
    put_bytes(payload, challenge.nonce_pc);
    put_bytes(payload, challenge.pc_uuid);
    return payload;
}

std::optional<AuthChallenge> parse_auth_challenge(std::span<const std::uint8_t> payload) {
    Reader reader{payload};
    AuthChallenge challenge;
    std::uint8_t schema = 0;
    std::uint8_t mode = 0;
    if (!reader.read_u8(&schema) || schema != kAuthSchemaVersion ||
        !reader.read_u8(&mode) || mode > static_cast<std::uint8_t>(AuthMode::pairing) ||
        !reader.read_string(kMaxAuthHostName, &challenge.host_name) ||
        !reader.read_bytes(challenge.nonce_pc.size(), challenge.nonce_pc.data()) ||
        !reader.read_bytes(challenge.pc_uuid.size(), challenge.pc_uuid.data()) ||
        !reader.at_end()) {
        return std::nullopt;
    }
    challenge.mode = static_cast<AuthMode>(mode);
    return challenge;
}

std::vector<std::uint8_t> encode_auth_response(const AuthResponse& response) {
    std::vector<std::uint8_t> payload;
    payload.reserve(
        4U + kAuthNonceSize + response.android_id.size() + response.device_name.size() +
        kAuthMacSize);
    payload.push_back(kAuthSchemaVersion);
    payload.push_back(static_cast<std::uint8_t>(response.status));
    put_string(payload, response.android_id);
    put_string(payload, response.device_name);
    put_bytes(payload, response.nonce_tablet);
    put_bytes(payload, response.mac);
    return payload;
}

std::optional<AuthResponse> parse_auth_response(std::span<const std::uint8_t> payload) {
    Reader reader{payload};
    AuthResponse response;
    std::uint8_t schema = 0;
    std::uint8_t status = 0;
    if (!reader.read_u8(&schema) || schema != kAuthSchemaVersion ||
        !reader.read_u8(&status) || status > static_cast<std::uint8_t>(AuthStatus::not_paired) ||
        !reader.read_string(kMaxAuthAndroidId, &response.android_id) ||
        !reader.read_string(kMaxAuthDeviceName, &response.device_name) ||
        !reader.read_bytes(response.nonce_tablet.size(), response.nonce_tablet.data()) ||
        !reader.read_bytes(response.mac.size(), response.mac.data()) ||
        !reader.at_end()) {
        return std::nullopt;
    }
    response.status = static_cast<AuthStatus>(status);
    return response;
}

std::vector<std::uint8_t> encode_auth_result(const AuthResultPayload& result) {
    std::vector<std::uint8_t> payload;
    payload.reserve(4U + result.message.size() + kAuthKeySize);
    payload.push_back(kAuthSchemaVersion);
    payload.push_back(static_cast<std::uint8_t>(result.result));
    put_string(payload, result.message);
    if (result.result == AuthResult::pairing_complete) {
        put_bytes(payload, result.credential);
    }
    return payload;
}

std::optional<AuthResultPayload> parse_auth_result(std::span<const std::uint8_t> payload) {
    Reader reader{payload};
    AuthResultPayload result;
    std::uint8_t schema = 0;
    std::uint8_t outcome = 0;
    if (!reader.read_u8(&schema) || schema != kAuthSchemaVersion ||
        !reader.read_u8(&outcome) || outcome > static_cast<std::uint8_t>(AuthResult::pairing_complete) ||
        !reader.read_string(kMaxAuthResultMessage, &result.message)) {
        return std::nullopt;
    }
    result.result = static_cast<AuthResult>(outcome);
    if (result.result == AuthResult::pairing_complete &&
        !reader.read_bytes(result.credential.size(), result.credential.data())) {
        return std::nullopt;
    }
    return reader.at_end() ? std::optional<AuthResultPayload>(result) : std::nullopt;
}

}  // namespace wireview::protocol
