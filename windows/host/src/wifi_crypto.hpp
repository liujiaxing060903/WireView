#pragma once

// Thin CNG (bcrypt.h) wrappers for the R3 WiFi authentication primitives.
// Pure ASCII by policy: this target is compiled without /utf-8.

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace wireview::host {

[[nodiscard]] std::vector<std::uint8_t> sha256(std::span<const std::uint8_t> data);

// HMAC-SHA256 over one message with a symmetric key of any length.
[[nodiscard]] std::vector<std::uint8_t> hmac_sha256(
    std::span<const std::uint8_t> key,
    std::span<const std::uint8_t> data);

// PBKDF2-HMAC-SHA256 (BCryptDeriveKeyPBKDF2). Returns exactly key_length
// bytes; throws std::runtime_error on CNG failure.
[[nodiscard]] std::vector<std::uint8_t> pbkdf2_sha256(
    std::span<const std::uint8_t> password,
    std::span<const std::uint8_t> salt,
    std::uint32_t iterations,
    std::size_t key_length);

// Convenience overload used by the authentication handshake: concatenates
// the domain string with the byte fields in the fixed handshake order.
[[nodiscard]] std::vector<std::uint8_t> auth_mac(
    std::span<const std::uint8_t> key,
    std::string_view domain,
    std::span<const std::uint8_t> pc_uuid,
    std::span<const std::uint8_t> android_id,
    std::span<const std::uint8_t> nonce_pc,
    std::span<const std::uint8_t> nonce_tablet);

// v2 AUTH_RESULT integrity tag:
// HMAC-SHA256(key, "WV1-result" || outcome || message || credential_view
//             || nonce_pc || nonce_tablet).
// credential_view is empty for accepted/rejected results and carries the
// on-wire (masked) credential bytes for pairing_complete.
[[nodiscard]] std::vector<std::uint8_t> result_mac(
    std::span<const std::uint8_t> key,
    std::uint8_t outcome,
    std::string_view message,
    std::span<const std::uint8_t> credential_view,
    std::span<const std::uint8_t> nonce_pc,
    std::span<const std::uint8_t> nonce_tablet);

// v2 pairing credential mask: HMAC-SHA256(kdf_key, "WV1-wrap" || nonce_pc
// || nonce_tablet); the credential travels XOR masked with it.
[[nodiscard]] std::vector<std::uint8_t> wrap_mask(
    std::span<const std::uint8_t> kdf_key,
    std::span<const std::uint8_t> nonce_pc,
    std::span<const std::uint8_t> nonce_tablet);

// CryptProtectData (DPAPI, current user scope) for credentials at rest.
[[nodiscard]] std::vector<std::uint8_t> dpapi_protect(std::span<const std::uint8_t> plain);

[[nodiscard]] std::vector<std::uint8_t> dpapi_unprotect(std::span<const std::uint8_t> blob);

// Cryptographically random bytes (BCryptGenRandom).
[[nodiscard]] std::vector<std::uint8_t> random_bytes(std::size_t count);

// Random six digit PIN as decimal text, first digit non-zero.
[[nodiscard]] std::string random_pairing_pin();

[[nodiscard]] std::string to_hex(std::span<const std::uint8_t> bytes);

[[nodiscard]] std::vector<std::uint8_t> from_hex(const std::string& text);

}  // namespace wireview::host
