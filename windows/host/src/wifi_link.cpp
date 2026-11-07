#include "wifi_link.hpp"

#include "wifi_crypto.hpp"
#include "wireview/protocol.hpp"

#include <algorithm>
#include <algorithm>
#include <chrono>
#include <stdexcept>
#include <string>

namespace wireview::host {
namespace {

using wireview::protocol::AuthChallenge;
using wireview::protocol::AuthResultPayload;
using wireview::protocol::AuthResponse;

[[nodiscard]] std::uint64_t monotonic_micros() {
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(now).count());
}

void write_packet(
    ReliableByteStream& stream,
    wireview::protocol::PacketType type,
    std::span<const std::uint8_t> payload) {
    wireview::protocol::PacketHeader header{
        .type = type,
        .sequence = monotonic_micros(),
        .timestamp_micros = monotonic_micros(),
    };
    write_all(stream, wireview::protocol::encode_packet(header, payload));
}

}  // namespace

bool constant_time_equal(
    std::span<const std::uint8_t> left,
    std::span<const std::uint8_t> right) {
    if (left.size() != right.size()) {
        return false;
    }
    std::uint8_t diff = 0U;
    for (std::size_t index = 0U; index < left.size(); ++index) {
        diff |= static_cast<std::uint8_t>(left[index] ^ right[index]);
    }
    return diff == 0U;
}

WifiPeerInfo perform_wifi_handshake(
    ReliableByteStream& stream,
    const WifiLinkIdentity& identity,
    wireview::protocol::AuthMode mode,
    std::span<const std::uint8_t> key,
    std::string_view pin,
    bool expect_pairing_completion,
    std::vector<std::uint8_t>* out_credential) {
    const std::vector<std::uint8_t> nonce_pc = random_bytes(
        wireview::protocol::kAuthNonceSize);
    AuthChallenge challenge{};
    challenge.mode = mode;
    std::copy(nonce_pc.begin(), nonce_pc.end(), challenge.nonce_pc.begin());
    std::copy(identity.pc_uuid.begin(), identity.pc_uuid.end(), challenge.pc_uuid.begin());
    challenge.host_name = identity.host_name;
    write_packet(
        stream,
        wireview::protocol::PacketType::auth_challenge,
        wireview::protocol::encode_auth_challenge(challenge));

    const auto response_packet = read_packet(stream, std::chrono::seconds(5));
    if (response_packet.header.type != wireview::protocol::PacketType::auth_response) {
        throw std::runtime_error("tablet did not return AUTH_RESPONSE");
    }
    const auto response = wireview::protocol::parse_auth_response(response_packet.payload);
    if (!response) {
        throw std::runtime_error("tablet AUTH_RESPONSE payload is invalid");
    }
    const std::vector<std::uint8_t> android_id_bytes(
        response->android_id.begin(), response->android_id.end());
    if (response->status == wireview::protocol::AuthStatus::not_paired) {
        AuthResultPayload rejection{};
        rejection.result = wireview::protocol::AuthResult::rejected;
        rejection.message = mode == wireview::protocol::AuthMode::pairing
            ? "pairing cancelled on the tablet"
            : "device is not paired with this PC";
        write_packet(
            stream,
            wireview::protocol::PacketType::auth_result,
            wireview::protocol::encode_auth_result(rejection));
        throw std::runtime_error(
            "tablet reports it is not paired with this PC (android_id " +
            response->android_id + ")");
    }

    std::vector<std::uint8_t> verification_key;
    if (mode == wireview::protocol::AuthMode::pairing) {
        const std::vector<std::uint8_t> pin_bytes(pin.begin(), pin.end());
        std::vector<std::uint8_t> salt(
            wireview::protocol::kAuthKdfSalt,
            wireview::protocol::kAuthKdfSalt + std::char_traits<char>::length(
                wireview::protocol::kAuthKdfSalt));
        salt.insert(salt.end(), identity.pc_uuid.begin(), identity.pc_uuid.end());
        salt.insert(salt.end(), android_id_bytes.begin(), android_id_bytes.end());
        salt.insert(salt.end(), nonce_pc.begin(), nonce_pc.end());
        salt.insert(
            salt.end(), response->nonce_tablet.begin(), response->nonce_tablet.end());
        verification_key = pbkdf2_sha256(
            pin_bytes,
            salt,
            wireview::protocol::kAuthPbkdf2Iterations,
            wireview::protocol::kAuthKeySize);
    } else {
        if (key.size() != wireview::protocol::kAuthKeySize) {
            throw std::runtime_error("the whitelist credential is missing or malformed");
        }
        verification_key.assign(key.begin(), key.end());
    }
    const std::string& domain = mode == wireview::protocol::AuthMode::pairing
        ? wireview::protocol::kAuthDomainPair
        : wireview::protocol::kAuthDomainAuthenticate;
    const auto expected = auth_mac(
        verification_key,
        domain,
        identity.pc_uuid,
        android_id_bytes,
        nonce_pc,
        response->nonce_tablet);
    if (!constant_time_equal(expected, response->mac)) {
        AuthResultPayload rejection{};
        rejection.result = wireview::protocol::AuthResult::rejected;
        rejection.message = "authentication failed: MAC mismatch";
        write_packet(
            stream,
            wireview::protocol::PacketType::auth_result,
            wireview::protocol::encode_auth_result(rejection));
        throw std::runtime_error(
            "the tablet's authentication proof did not verify (wrong PIN or credential)");
    }

    AuthResultPayload result{};
    std::vector<std::uint8_t> credential;
    if (expect_pairing_completion) {
        credential = random_bytes(wireview::protocol::kAuthKeySize);
        result.result = wireview::protocol::AuthResult::pairing_complete;
        result.message = identity.host_name;
        std::copy(credential.begin(), credential.end(), result.credential.begin());
    } else {
        result.result = wireview::protocol::AuthResult::accepted;
        result.message = identity.host_name;
    }
    write_packet(
        stream,
        wireview::protocol::PacketType::auth_result,
        wireview::protocol::encode_auth_result(result));
    if (out_credential != nullptr) {
        *out_credential = std::move(credential);
    }
    return WifiPeerInfo{response->android_id, response->device_name};
}

}  // namespace wireview::host
