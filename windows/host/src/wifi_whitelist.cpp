#include "wifi_whitelist.hpp"

#include "wifi_crypto.hpp"

#include <Windows.h>

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <stdexcept>

namespace wireview::host {
namespace {

// File format (one field per line, entries numbered):
//   version=1
//   device.0.android_id=<hex>
//   device.0.device_name=<utf-8>
//   device.0.paired_at=<stamp>
//   device.0.key=<hex DPAPI blob>

std::string sanitize_text(const std::string& value) {
    // Keep parsing trivial: strip CR/LF so device names cannot inject lines.
    std::string clean(value);
    std::replace(clean.begin(), clean.end(), '\r', ' ');
    std::replace(clean.begin(), clean.end(), '\n', ' ');
    return clean;
}

// "device.<n>.<field>" -> n + field, or nothing for foreign lines.
bool parse_entry_line(
    const std::string& line,
    std::size_t* index,
    std::string* field) {
    if (line.rfind("device.", 0U) != 0U) {
        return false;
    }
    const auto dot_after_index = line.find('.', 7U);
    if (dot_after_index == std::string::npos) {
        return false;
    }
    const std::string index_text = line.substr(7U, dot_after_index - 7U);
    if (index_text.empty()) {
        return false;
    }
    std::size_t parsed = 0U;
    try {
        parsed = static_cast<std::size_t>(std::stoul(index_text));
    } catch (const std::exception&) {
        return false;
    }
    const auto equals = line.find('=', dot_after_index);
    if (equals == std::string::npos) {
        return false;
    }
    *index = parsed;
    *field = line.substr(dot_after_index + 1U, equals - dot_after_index - 1U);
    return true;
}

std::string line_value(const std::string& line) {
    const auto equals = line.find('=');
    return equals == std::string::npos ? std::string{} : line.substr(equals + 1U);
}

}  // namespace

void WifiWhitelist::load() {
    entries_.clear();
    std::ifstream input(path_, std::ios::binary);
    if (!input) {
        return;
    }
    std::string line;
    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        std::size_t index = 0U;
        std::string field;
        if (!parse_entry_line(line, &index, &field)) {
            continue;
        }
        while (entries_.size() <= index) {
            entries_.emplace_back();
        }
        WifiWhitelistEntry& entry = entries_[index];
        if (field == "android_id") {
            entry.android_id = line_value(line);
        } else if (field == "device_name") {
            entry.device_name = line_value(line);
        } else if (field == "paired_at") {
            entry.paired_at = line_value(line);
        } else if (field == "last_ip") {
            entry.last_ip = line_value(line);
        } else if (field == "key") {
            try {
                entry.sealed_key = from_hex(line_value(line));
            } catch (const std::runtime_error&) {
                entry.sealed_key.clear();
            }
        }
    }
    // Drop malformed trailing entries with no identity.
    entries_.erase(
        std::remove_if(
            entries_.begin(), entries_.end(),
            [](const WifiWhitelistEntry& entry) { return entry.android_id.empty(); }),
        entries_.end());
}

bool WifiWhitelist::save(std::wstring* error) const {
    std::ofstream output(path_, std::ios::binary | std::ios::trunc);
    if (!output) {
        if (error != nullptr) {
            *error = L"cannot write the WiFi whitelist file";
        }
        return false;
    }
    output << "version=1\n";
    for (std::size_t index = 0U; index < entries_.size(); ++index) {
        const WifiWhitelistEntry& entry = entries_[index];
        output << "device." << index << ".android_id=" << entry.android_id << '\n'
               << "device." << index << ".device_name="
               << sanitize_text(entry.device_name) << '\n'
               << "device." << index << ".paired_at="
               << sanitize_text(entry.paired_at) << '\n'
               << "device." << index << ".last_ip=" << sanitize_text(entry.last_ip) << '\n'
               << "device." << index << ".key=" << to_hex(entry.sealed_key) << '\n';
    }
    if (error != nullptr) {
        *error = {};
    }
    return true;
}

const WifiWhitelistEntry* WifiWhitelist::find(std::string_view android_id) const noexcept {
    const auto match = std::find_if(
        entries_.begin(), entries_.end(),
        [android_id](const WifiWhitelistEntry& entry) {
            return entry.android_id == android_id;
        });
    return match == entries_.end() ? nullptr : &*match;
}

void WifiWhitelist::upsert(
    const std::string& android_id,
    const std::string& device_name,
    std::span<const std::uint8_t> plaintext_key) {
    WifiWhitelistEntry entry;
    entry.android_id = sanitize_text(android_id);
    entry.device_name = sanitize_text(device_name);
    SYSTEMTIME time{};
    GetLocalTime(&time);
    char timestamp[32]{};
    (void)snprintf(
        timestamp, sizeof(timestamp), "%04u-%02u-%02u %02u:%02u:%02u",
        time.wYear, time.wMonth, time.wDay, time.wHour, time.wMinute, time.wSecond);
    entry.paired_at = timestamp;
    entry.sealed_key = dpapi_protect(plaintext_key);
    const auto existing = std::find_if(
        entries_.begin(), entries_.end(),
        [&entry](const WifiWhitelistEntry& candidate) {
            return candidate.android_id == entry.android_id;
        });
    if (existing != entries_.end()) {
        // Re-pairing must not lose the last-known address: carry it over
        // before replacing the entry.
        entry.last_ip = existing->last_ip;
        *existing = std::move(entry);
        return;
    }
    entries_.push_back(std::move(entry));
}

void WifiWhitelist::update_last_ip(
    std::string_view android_id,
    std::string_view ip) {
    const auto match = std::find_if(
        entries_.begin(), entries_.end(),
        [android_id](const WifiWhitelistEntry& entry) {
            return entry.android_id == android_id;
        });
    if (match != entries_.end()) {
        match->last_ip = std::string(ip);
    }
}

bool WifiWhitelist::remove(std::string_view android_id) {
    const auto match = std::find_if(
        entries_.begin(), entries_.end(),
        [android_id](const WifiWhitelistEntry& entry) {
            return entry.android_id == android_id;
        });
    if (match == entries_.end()) {
        return false;
    }
    entries_.erase(match);
    return true;
}

std::vector<std::uint8_t> WifiWhitelist::unseal(const WifiWhitelistEntry& entry) {
    if (entry.sealed_key.empty()) {
        throw std::runtime_error("whitelist entry has no sealed key");
    }
    return dpapi_unprotect(entry.sealed_key);
}

}  // namespace wireview::host
