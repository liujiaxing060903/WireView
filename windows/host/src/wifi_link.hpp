#pragma once

// PC side of the R3 WiFi authentication handshake (protocol types 15..17).
// Shared by the host session path (authenticate mode, credential from the
// whitelist) and the control center pairing flow (pairing mode, PIN).

#include "wireview/auth_messages.hpp"

#include "reliable_byte_stream.hpp"

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace wireview::host {

struct WifiPeerInfo {
    std::string android_id;   // ASCII hex as reported by the tablet.
    std::string device_name;  // UTF-8 display name.
};

struct WifiLinkIdentity {
    std::vector<std::uint8_t> pc_uuid;  // 16 bytes, stable per PC install.
    std::string host_name;              // UTF-8 display name.
};

// Sends AUTH_CHALLENGE and validates AUTH_RESPONSE.
//  - authenticate: `key` holds the 32-byte whitelist credential; the peer is
//    rejected unless its MAC verifies.
//  - pairing: `pin` holds the 6-digit PIN (typed by the user or delivered
//    over USB); the response MAC must verify against the PIN-derived key.
// When expect_pairing_completion is set, AUTH_RESULT must carry the new
// credential, which is returned via out_credential.
// Throws std::runtime_error on protocol, timeout, or MAC failure.
[[nodiscard]] WifiPeerInfo perform_wifi_handshake(
    ReliableByteStream& stream,
    const WifiLinkIdentity& identity,
    wireview::protocol::AuthMode mode,
    std::span<const std::uint8_t> key,
    std::string_view pin,
    bool expect_pairing_completion,
    std::vector<std::uint8_t>* out_credential);

// Constant-time comparison for MAC verification.
[[nodiscard]] bool constant_time_equal(
    std::span<const std::uint8_t> left,
    std::span<const std::uint8_t> right);

}  // namespace wireview::host
