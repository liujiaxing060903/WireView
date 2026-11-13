#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>

namespace wireview::host {

struct DisplayPreferences {
    std::wstring copy_source;
    std::wstring copy_resolution{L"auto"};
    std::uint16_t copy_frame_rate{60U};
    std::map<std::wstring, std::uint64_t> resolution_uses;
};

inline std::optional<std::pair<std::uint16_t, std::uint16_t>> preference_resolution(std::wstring_view text) {
    std::wistringstream input{std::wstring(text)};
    std::uint32_t width{}, height{};
    wchar_t separator{};
    if (!(input >> width >> separator >> height) || separator != L'x' ||
        width < 2U || height < 2U || width > 65534U || height > 65534U || width % 2U || height % 2U) {
        return std::nullopt;
    }
    input >> std::ws;
    if (!input.eof()) { return std::nullopt; }
    return std::pair{static_cast<std::uint16_t>(width), static_cast<std::uint16_t>(height)};
}

inline DisplayPreferences load_display_preferences(const std::filesystem::path& file) {
    DisplayPreferences result;
    std::wifstream input(file);
    std::wstring line;
    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == L'\r') { line.pop_back(); }
        const auto split = line.find(L'=');
        if (split == std::wstring::npos) { continue; }
        const auto key = line.substr(0U, split), value = line.substr(split + 1U);
        if (key == L"copy_source") { result.copy_source = value; }
        else if (key == L"copy_resolution" && (value == L"auto" || preference_resolution(value))) {
            result.copy_resolution = value;
        } else if (key == L"copy_frame_rate" && (value == L"60" || value == L"120")) {
            result.copy_frame_rate = value == L"120" ? 120U : 60U;
        } else if (key.starts_with(L"use.") && preference_resolution(key.substr(4U)) &&
            !value.empty() && value.find_first_not_of(L"0123456789") == std::wstring::npos) {
            try { result.resolution_uses[key.substr(4U)] = std::stoull(value); }
            catch (const std::exception&) { /* Ignore one damaged entry, retain the others. */ }
        }
    }
    return result;
}

inline bool save_display_preferences(const std::filesystem::path& file, const DisplayPreferences& preferences) {
    std::error_code error;
    std::filesystem::create_directories(file.parent_path(), error);
    if (error) { return false; }
    const auto temporary = std::filesystem::path(file.wstring() + L".tmp");
    {
        std::wofstream output(temporary, std::ios::trunc);
        output << L"copy_source=" << preferences.copy_source << L'\n'
            << L"copy_resolution=" << preferences.copy_resolution << L'\n'
            << L"copy_frame_rate=" << preferences.copy_frame_rate << L'\n';
        for (const auto& [resolution, count] : preferences.resolution_uses) {
            output << L"use." << resolution << L'=' << count << L'\n';
        }
        output.close();
        if (!output) { return false; }
    }
    return MoveFileExW(temporary.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
}

inline void record_resolution_use(DisplayPreferences& preferences, const std::wstring& resolution) {
    auto& count = preferences.resolution_uses[resolution];
    if (count != (std::numeric_limits<std::uint64_t>::max)()) { ++count; }
}

} // namespace wireview::host
