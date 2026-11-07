#pragma once

// PC-side pairing whitelist for the R3 WiFi transport. Entries live in
// %LOCALAPPDATA%\WireView\wifi-whitelist.ini; the long-term credential is
// sealed with DPAPI (current user scope) and never written in plaintext.

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace wireview::host {

struct WifiWhitelistEntry {
    std::string android_id;    // ASCII hex identity reported by the tablet.
    std::string device_name;   // UTF-8 display name.
    std::string paired_at;     // Human readable timestamp.
    std::vector<std::uint8_t> sealed_key;  // DPAPI blob of the 32-byte key.
};

class WifiWhitelist {
public:
    explicit WifiWhitelist(std::wstring path) noexcept : path_(std::move(path)) {
    }

    // Reads the whitelist file; a missing or empty file yields an empty list.
    void load();

    bool save(std::wstring* error) const;

    [[nodiscard]] const std::vector<WifiWhitelistEntry>& entries() const noexcept {
        return entries_;
    }

    [[nodiscard]] const WifiWhitelistEntry* find(
        std::string_view android_id) const noexcept;

    // Seals the plaintext key with DPAPI and appends/replaces the entry.
    void upsert(
        const std::string& android_id,
        const std::string& device_name,
        std::span<const std::uint8_t> plaintext_key);

    bool remove(std::string_view android_id);

    // Unseals a stored credential; throws std::runtime_error on DPAPI failure.
    [[nodiscard]] static std::vector<std::uint8_t> unseal(
        const WifiWhitelistEntry& entry);

private:
    std::wstring path_;
    std::vector<WifiWhitelistEntry> entries_;
};

}  // namespace wireview::host
