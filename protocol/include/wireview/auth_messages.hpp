#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace wireview::protocol {

// R3 WiFi link authentication payloads (packet types 15..17). Everything in
// this header is pure byte layout; the key derivation (PBKDF2-HMAC-SHA256)
// and the MAC (HMAC-SHA256) live in platform code (CNG on Windows,
// javax.crypto on Android) and must agree on the constants below. The Java
// mirror of these builders lives in Protocol.java.
//
// Threat model (honest scope): a one-time PIN pairing plus per-connection
// challenge/response defeats passive sniffing and random LAN scanners. A
// six digit PIN gives ~2^20 entropy against an offline attacker who recorded
// the pairing exchange; PBKDF2 raises the cost but does not change the
// class. This is adequate for a personal LAN tool and NOT for hostile
// networks. See LOCAL_NOTES.md section 9.

inline constexpr char kAuthDomainAuthenticate[] = "WV1-auth";
inline constexpr char kAuthDomainPair[] = "WV1-pair";
// PBKDF2 salt = kAuthKdfSalt || pc_uuid || android_id || nonce_pc || nonce_tablet
inline constexpr char kAuthKdfSalt[] = "WV1-salt";
inline constexpr std::uint32_t kAuthPbkdf2Iterations = 10'000U;
inline constexpr std::size_t kAuthNonceSize = 16U;
inline constexpr std::size_t kAuthUuidSize = 16U;
inline constexpr std::size_t kAuthKeySize = 32U;
inline constexpr std::size_t kAuthMacSize = 32U;

// Wire string limits; keep every message far below the protocol payload cap.
inline constexpr std::size_t kMaxAuthHostName = 128U;
inline constexpr std::size_t kMaxAuthAndroidId = 64U;
inline constexpr std::size_t kMaxAuthDeviceName = 128U;
inline constexpr std::size_t kMaxAuthResultMessage = 256U;

// MAC input for the recurring authentication:
//   HMAC-SHA256(key, "WV1-auth" || pc_uuid || android_id || nonce_pc || nonce_tablet)
// MAC input during PIN pairing (key derived from the PIN):
//   HMAC-SHA256(kdf_key, "WV1-pair" || pc_uuid || android_id || nonce_pc || nonce_tablet)

enum class AuthMode : std::uint8_t {
    authenticate = 0,
    pairing = 1,
};

enum class AuthStatus : std::uint8_t {
    ok = 0,
    // The tablet does not hold a credential for this PC. The PC may retry
    // once with mode=pairing (which always runs the PIN flow).
    not_paired = 1,
};

enum class AuthResult : std::uint8_t {
    accepted = 0,
    rejected = 1,
    pairing_complete = 2,
};

struct AuthChallenge {
    AuthMode mode{AuthMode::authenticate};
    std::array<std::uint8_t, kAuthNonceSize> nonce_pc{};
    std::array<std::uint8_t, kAuthUuidSize> pc_uuid{};
    std::string host_name;  // UTF-8 display name, max 128 bytes.
};

struct AuthResponse {
    AuthStatus status{AuthStatus::ok};
    std::array<std::uint8_t, kAuthNonceSize> nonce_tablet{};
    std::string android_id;  // ASCII hex, max 64 bytes.
    std::string device_name;  // UTF-8 display name, max 128 bytes.
    // MAC over the domain string and both nonces plus both identities;
    // meaningful only when status == ok.
    std::array<std::uint8_t, kAuthMacSize> mac{};
};

struct AuthResultPayload {
    AuthResult result{AuthResult::accepted};
    // pairing_complete: the long-term credential the tablet must store.
    // accepted: display name. rejected: human readable reason.
    std::string message;
    std::array<std::uint8_t, kAuthKeySize> credential{};
};

[[nodiscard]] std::vector<std::uint8_t> encode_auth_challenge(const AuthChallenge& challenge);

[[nodiscard]] std::optional<AuthChallenge> parse_auth_challenge(
    std::span<const std::uint8_t> payload);

[[nodiscard]] std::vector<std::uint8_t> encode_auth_response(const AuthResponse& response);

[[nodiscard]] std::optional<AuthResponse> parse_auth_response(
    std::span<const std::uint8_t> payload);

[[nodiscard]] std::vector<std::uint8_t> encode_auth_result(const AuthResultPayload& result);

[[nodiscard]] std::optional<AuthResultPayload> parse_auth_result(
    std::span<const std::uint8_t> payload);

}  // namespace wireview::protocol
