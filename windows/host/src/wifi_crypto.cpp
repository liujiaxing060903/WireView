#include "wifi_crypto.hpp"

#include <Windows.h>
#include <bcrypt.h>
#include <dpapi.h>
#include <wincrypt.h>

#include <stdexcept>
#include <string_view>

#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "crypt32.lib")

namespace wireview::host {
namespace {

[[noreturn]] void throw_nt(const char* operation, NTSTATUS status) {
    throw std::runtime_error(
        std::string(operation) + " failed: NTSTATUS 0x" +
        [](NTSTATUS value) {
            char buffer[16]{};
            (void)snprintf(buffer, sizeof(buffer), "%08lx", static_cast<unsigned long>(value));
            return std::string(buffer);
        }(status));
}

void check_nt(const char* operation, NTSTATUS status) {
    // STATUS_SUCCESS is 0; bcrypt.h does not pull in ntstatus.h.
    if (status != 0) {
        throw_nt(operation, status);
    }
}

struct AlgorithmHandle {
    BCRYPT_ALG_HANDLE handle{nullptr};

    AlgorithmHandle(LPCWSTR algorithm, DWORD flags) {
        const NTSTATUS status = BCryptOpenAlgorithmProvider(&handle, algorithm, nullptr, flags);
        check_nt("BCryptOpenAlgorithmProvider", status);
    }

    ~AlgorithmHandle() {
        if (handle != nullptr) {
            BCryptCloseAlgorithmProvider(handle, 0);
        }
    }

    AlgorithmHandle(const AlgorithmHandle&) = delete;
    AlgorithmHandle& operator=(const AlgorithmHandle&) = delete;
};

struct HashHandle {
    BCRYPT_HASH_HANDLE handle{nullptr};

    HashHandle(BCRYPT_ALG_HANDLE algorithm, const std::uint8_t* key, std::size_t key_size) {
        const NTSTATUS status = BCryptCreateHash(
            algorithm,
            &handle,
            nullptr,
            0,
            const_cast<std::uint8_t*>(key),
            static_cast<ULONG>(key_size),
            0);
        check_nt("BCryptCreateHash", status);
    }

    ~HashHandle() {
        if (handle != nullptr) {
            BCryptDestroyHash(handle);
        }
    }

    HashHandle(const HashHandle&) = delete;
    HashHandle& operator=(const HashHandle&) = delete;
};

}  // namespace

std::vector<std::uint8_t> sha256(std::span<const std::uint8_t> data) {
    static const AlgorithmHandle algorithm(L"SHA256", 0);
    HashHandle hash(algorithm.handle, nullptr, 0U);
    if (!data.empty()) {
        check_nt(
            "BCryptHashData",
            BCryptHashData(hash.handle, const_cast<std::uint8_t*>(data.data()),
                           static_cast<ULONG>(data.size()), 0));
    }
    std::vector<std::uint8_t> digest(32U);
    check_nt(
        "BCryptFinishHash",
        BCryptFinishHash(hash.handle, digest.data(), static_cast<ULONG>(digest.size()), 0));
    return digest;
}

std::vector<std::uint8_t> hmac_sha256(
    std::span<const std::uint8_t> key,
    std::span<const std::uint8_t> data) {
    static const AlgorithmHandle algorithm(L"SHA256", BCRYPT_ALG_HANDLE_HMAC_FLAG);
    HashHandle hash(algorithm.handle, key.data(), key.size());
    if (!data.empty()) {
        check_nt(
            "BCryptHashData",
            BCryptHashData(hash.handle, const_cast<std::uint8_t*>(data.data()),
                           static_cast<ULONG>(data.size()), 0));
    }
    std::vector<std::uint8_t> digest(32U);
    check_nt(
        "BCryptFinishHash",
        BCryptFinishHash(hash.handle, digest.data(), static_cast<ULONG>(digest.size()), 0));
    return digest;
}

std::vector<std::uint8_t> pbkdf2_sha256(
    std::span<const std::uint8_t> password,
    std::span<const std::uint8_t> salt,
    std::uint32_t iterations,
    std::size_t key_length) {
    static const AlgorithmHandle algorithm(L"SHA256", BCRYPT_ALG_HANDLE_HMAC_FLAG);
    std::vector<std::uint8_t> derived(key_length);
    const NTSTATUS status = BCryptDeriveKeyPBKDF2(
        algorithm.handle,
        const_cast<std::uint8_t*>(password.data()),
        static_cast<ULONG>(password.size()),
        const_cast<std::uint8_t*>(salt.data()),
        static_cast<ULONG>(salt.size()),
        iterations,
        derived.data(),
        static_cast<ULONG>(derived.size()),
        0);
    check_nt("BCryptDeriveKeyPBKDF2", status);
    return derived;
}

std::vector<std::uint8_t> auth_mac(
    std::span<const std::uint8_t> key,
    std::string_view domain,
    std::span<const std::uint8_t> pc_uuid,
    std::span<const std::uint8_t> android_id,
    std::span<const std::uint8_t> nonce_pc,
    std::span<const std::uint8_t> nonce_tablet) {
    std::vector<std::uint8_t> message;
    message.reserve(domain.size() + pc_uuid.size() + android_id.size() +
                    nonce_pc.size() + nonce_tablet.size());
    message.insert(message.end(), domain.begin(), domain.end());
    message.insert(message.end(), pc_uuid.begin(), pc_uuid.end());
    message.insert(message.end(), android_id.begin(), android_id.end());
    message.insert(message.end(), nonce_pc.begin(), nonce_pc.end());
    message.insert(message.end(), nonce_tablet.begin(), nonce_tablet.end());
    return hmac_sha256(key, message);
}

std::vector<std::uint8_t> result_mac(
    std::span<const std::uint8_t> key,
    std::uint8_t outcome,
    std::string_view message,
    std::span<const std::uint8_t> credential_view,
    std::span<const std::uint8_t> nonce_pc,
    std::span<const std::uint8_t> nonce_tablet) {
    constexpr std::string_view kDomain{"WV1-result"};
    std::vector<std::uint8_t> data;
    data.reserve(
        kDomain.size() + 1U + message.size() + credential_view.size() + nonce_pc.size() +
        nonce_tablet.size());
    data.insert(data.end(), kDomain.begin(), kDomain.end());
    data.push_back(outcome);
    data.insert(data.end(), message.begin(), message.end());
    data.insert(data.end(), credential_view.begin(), credential_view.end());
    data.insert(data.end(), nonce_pc.begin(), nonce_pc.end());
    data.insert(data.end(), nonce_tablet.begin(), nonce_tablet.end());
    return hmac_sha256(key, data);
}

std::vector<std::uint8_t> wrap_mask(
    std::span<const std::uint8_t> kdf_key,
    std::span<const std::uint8_t> nonce_pc,
    std::span<const std::uint8_t> nonce_tablet) {
    constexpr std::string_view kDomain{"WV1-wrap"};
    std::vector<std::uint8_t> data;
    data.reserve(kDomain.size() + nonce_pc.size() + nonce_tablet.size());
    data.insert(data.end(), kDomain.begin(), kDomain.end());
    data.insert(data.end(), nonce_pc.begin(), nonce_pc.end());
    data.insert(data.end(), nonce_tablet.begin(), nonce_tablet.end());
    return hmac_sha256(kdf_key, data);
}

std::vector<std::uint8_t> dpapi_protect(std::span<const std::uint8_t> plain) {
    DATA_BLOB input{};
    input.pbData = const_cast<std::uint8_t*>(plain.data());
    input.cbData = static_cast<DWORD>(plain.size());
    DATA_BLOB output{};
    if (!CryptProtectData(&input, L"WireView WiFi credential", nullptr, nullptr, nullptr,
                          CRYPTPROTECT_UI_FORBIDDEN, &output)) {
        throw std::runtime_error("CryptProtectData failed");
    }
    std::vector<std::uint8_t> blob(output.pbData, output.pbData + output.cbData);
    LocalFree(output.pbData);
    return blob;
}

std::vector<std::uint8_t> dpapi_unprotect(std::span<const std::uint8_t> blob) {
    DATA_BLOB input{};
    input.pbData = const_cast<std::uint8_t*>(blob.data());
    input.cbData = static_cast<DWORD>(blob.size());
    DATA_BLOB output{};
    if (!CryptUnprotectData(&input, nullptr, nullptr, nullptr, nullptr,
                            CRYPTPROTECT_UI_FORBIDDEN, &output)) {
        throw std::runtime_error("CryptUnprotectData failed");
    }
    std::vector<std::uint8_t> plain(output.pbData, output.pbData + output.cbData);
    LocalFree(output.pbData);
    return plain;
}

std::vector<std::uint8_t> random_bytes(std::size_t count) {
    static const AlgorithmHandle algorithm(L"RNG", 0);
    std::vector<std::uint8_t> bytes(count);
    if (count > 0U) {
        check_nt(
            "BCryptGenRandom",
            BCryptGenRandom(
                algorithm.handle,
                bytes.data(),
                static_cast<ULONG>(bytes.size()),
                0));
    }
    return bytes;
}

std::string random_pairing_pin() {
    // First digit drawn from 1..9 so the PIN is always six digits of text
    // with no leading-zero ambiguity when the user re-types it.
    const auto first = random_bytes(1U);
    const auto rest = random_bytes(5U);
    std::string pin;
    pin.reserve(6U);
    pin.push_back(static_cast<char>('1' + (first[0] % 9U)));
    for (const auto byte : rest) {
        pin.push_back(static_cast<char>('0' + (byte % 10U)));
    }
    return pin;
}

std::string to_hex(std::span<const std::uint8_t> bytes) {
    static const char digits[] = "0123456789abcdef";
    std::string text;
    text.reserve(bytes.size() * 2U);
    for (const auto byte : bytes) {
        text.push_back(digits[byte >> 4U]);
        text.push_back(digits[byte & 0x0fU]);
    }
    return text;
}

std::vector<std::uint8_t> from_hex(const std::string& text) {
    const auto value_of = [](char character) -> int {
        if (character >= '0' && character <= '9') {
            return character - '0';
        }
        if (character >= 'a' && character <= 'f') {
            return character - 'a' + 10;
        }
        if (character >= 'A' && character <= 'F') {
            return character - 'A' + 10;
        }
        return -1;
    };
    if (text.size() % 2U != 0U) {
        throw std::runtime_error("hex text has an odd length");
    }
    std::vector<std::uint8_t> bytes;
    bytes.reserve(text.size() / 2U);
    for (std::size_t index = 0U; index < text.size(); index += 2U) {
        const int high = value_of(text[index]);
        const int low = value_of(text[index + 1U]);
        if (high < 0 || low < 0) {
            throw std::runtime_error("hex text contains a non-hex character");
        }
        bytes.push_back(static_cast<std::uint8_t>((high << 4) | low));
    }
    return bytes;
}

}  // namespace wireview::host
