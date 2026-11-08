#include "wifi_crypto.hpp"

#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

int failures = 0;

void check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        ++failures;
    }
}

std::vector<std::uint8_t> bytes_of(const std::string& value) {
    return {value.begin(), value.end()};
}

std::vector<std::uint8_t> expected_hex(const std::string& text) {
    return wireview::host::from_hex(text);
}

// RFC 4231 test case 2: HMAC-SHA256 with key "Jefe".
void hmac_vector_test() {
    const auto key = bytes_of("Jefe");
    const auto data = bytes_of("what do ya want for nothing?");
    const auto digest = wireview::host::hmac_sha256(key, data);
    check(
        digest == expected_hex("5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843"),
        "HMAC-SHA256 must match the RFC 4231 Jefe vector");
}

// Widely published PBKDF2-HMAC-SHA256 answer vectors (password/salt ASCII).
void pbkdf2_vector_test() {
    const auto password = bytes_of("password");
    const auto salt = bytes_of("salt");
    check(
        wireview::host::pbkdf2_sha256(password, salt, 1U, 32U) ==
            expected_hex("120fb6cffcf8b32c43e7225256c4f837a86548c92ccc35480805987cb70be17b"),
        "PBKDF2 iteration 1 must match the published vector");
    check(
        wireview::host::pbkdf2_sha256(password, salt, 2U, 32U) ==
            expected_hex("ae4d0c95af6b46d32d0adff928f06dd02a303f8ef3c251dfd6e2d85a95474c43"),
        "PBKDF2 iteration 2 must match the published vector");
    check(
        wireview::host::pbkdf2_sha256(password, salt, 4096U, 32U) ==
            expected_hex("c5e478d59288c841aa530db6845c4c8d962893a001ce4e11a4963873aa98134a"),
        "PBKDF2 iteration 4096 must match the published vector");
    // Multi-block output (dkLen > 32) exercises the PBKDF2 block loop.
    check(
        wireview::host::pbkdf2_sha256(password, salt, 1U, 48U).size() == 48U,
        "PBKDF2 must return the requested derived length");
}

void sha256_vector_test() {
    const auto digest = wireview::host::sha256(bytes_of("abc"));
    check(
        digest == expected_hex("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"),
        "SHA-256 must match the FIPS 180-2 abc vector");
}

void dpapi_round_trip_test() {
    const std::vector<std::uint8_t> plain{1U, 2U, 3U, 4U, 5U};
    const auto blob = wireview::host::dpapi_protect(plain);
    check(blob != plain, "DPAPI blob must differ from the plaintext");
    check(wireview::host::dpapi_unprotect(blob) == plain, "DPAPI must round trip");
}

void random_and_hex_test() {
    const auto first = wireview::host::random_bytes(32U);
    const auto second = wireview::host::random_bytes(32U);
    check(first.size() == 32U && second.size() == 32U, "random byte counts must match");
    check(first != second, "two random buffers must differ");

    const auto pin = wireview::host::random_pairing_pin();
    check(pin.size() == 6U, "pairing PIN must be six characters");
    for (const auto character : pin) {
        check(character >= '0' && character <= '9', "pairing PIN must be decimal");
    }
    check(pin[0] >= '1' && pin[0] <= '9', "pairing PIN must not start with zero");

    const std::vector<std::uint8_t> hex_input{0x0aU, 0xffU};
    check(wireview::host::to_hex(hex_input) == "0aff", "hex encoding must be lowercase");
    check(wireview::host::from_hex("0aFF") == std::vector<std::uint8_t>({0x0aU, 0xffU}),
          "hex decoding must accept both cases");
    bool threw = false;
    try {
        (void)wireview::host::from_hex("abc");
    } catch (const std::runtime_error&) {
        threw = true;
    }
    check(threw, "odd-length hex must be rejected");
    threw = false;
    try {
        (void)wireview::host::from_hex("zz");
    } catch (const std::runtime_error&) {
        threw = true;
    }
    check(threw, "non-hex characters must be rejected");
}

void auth_mac_test() {
    // auth_mac is domain separation plus a fixed field order; verify that the
    // output equals an explicit HMAC over the concatenated message so a
    // future reordering cannot slip through silently.
    const auto key = wireview::host::from_hex(
        "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f");
    const auto pc_uuid = wireview::host::from_hex("0102030405060708090a0b0c0d0e0f10");
    const auto android_id = bytes_of("0123456789abcdef");
    const auto nonce_pc = wireview::host::from_hex("a0a1a2a3a4a5a6a7a8a9aaabacadaeaf");
    const auto nonce_tablet = wireview::host::from_hex("b0b1b2b3b4b5b6b7b8b9babbbcbdbebf");
    const auto mac = wireview::host::auth_mac(
        key, "WV1-auth", pc_uuid, android_id, nonce_pc, nonce_tablet);

    std::vector<std::uint8_t> message(bytes_of("WV1-auth"));
    message.insert(message.end(), pc_uuid.begin(), pc_uuid.end());
    message.insert(message.end(), android_id.begin(), android_id.end());
    message.insert(message.end(), nonce_pc.begin(), nonce_pc.end());
    message.insert(message.end(), nonce_tablet.begin(), nonce_tablet.end());
    check(mac == wireview::host::hmac_sha256(key, message),
          "auth_mac must be HMAC over the documented field order");
}

void result_integrity_vector_test() {
    // Known-answer vectors computed independently with .NET HMACSHA256 (see
    // the audit notes): key = 0x01..0x20, nonce_pc = 16x0x11,
    // nonce_tablet = 16x0x22, credential view = 32x0xAA, message "MI PAD 4".
    // The Java mirror in ProtocolSelfTest locks onto the same vectors, so
    // any drift between the two implementations fails at least one side.
    std::vector<std::uint8_t> key(32U, 0U);
    for (std::size_t index = 0U; index < key.size(); ++index) {
        key[index] = static_cast<std::uint8_t>(index + 1U);
    }
    const std::vector<std::uint8_t> nonce_pc(16U, 0x11U);
    const std::vector<std::uint8_t> nonce_tablet(16U, 0x22U);
    const std::vector<std::uint8_t> credential_view(32U, 0xAAU);

    const auto pair_tag = wireview::host::result_mac(
        key, 0x02U, "MI PAD 4", credential_view, nonce_pc, nonce_tablet);
    check(wireview::host::to_hex(pair_tag) ==
              "815fce97464dd863b42a7c2340333ff9db4ebf38eac2b21dbc69935bf821b337",
          "pairing result tag must match the published vector");

    const auto accepted_tag = wireview::host::result_mac(
        key, 0x00U, "MI PAD 4", {}, nonce_pc, nonce_tablet);
    check(wireview::host::to_hex(accepted_tag) ==
              "50d6386ed9be251c7c492e96ca14772ec13a7e39acee9159b4de28f7c0d14798",
          "accepted result tag must match the published vector");

    const auto mask = wireview::host::wrap_mask(key, nonce_pc, nonce_tablet);
    check(wireview::host::to_hex(mask) ==
              "82be86bae43ef7a234893d65fe275abe540413e77d419535f01425632e3aef2e",
          "wrap mask must match the published vector");
}

}  // namespace

int main() {
    hmac_vector_test();
    pbkdf2_vector_test();
    sha256_vector_test();
    dpapi_round_trip_test();
    random_and_hex_test();
    auth_mac_test();
    result_integrity_vector_test();

    if (failures != 0) {
        std::cerr << failures << " WiFi crypto test(s) failed\n";
        return 1;
    }
    std::cout << "All WireView WiFi crypto tests passed\n";
    return 0;
}
