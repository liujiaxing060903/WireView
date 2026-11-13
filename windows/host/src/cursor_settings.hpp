#pragma once
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <string>
#include <string_view>

namespace wireview::host {
using CursorSizes = std::map<std::string, unsigned>;

inline std::optional<unsigned> parse_cursor_size(std::wstring_view value) {
    if (value.empty() || value.size() > 3U) { return std::nullopt; }
    unsigned percent{};
    for (const wchar_t digit : value) {
        if (digit < L'0' || digit > L'9') { return std::nullopt; }
        percent = percent * 10U + static_cast<unsigned>(digit - L'0');
    }
    return percent >= 50U && percent <= 300U ? std::optional<unsigned>{percent} : std::nullopt;
}

inline CursorSizes load_cursor_sizes(const std::filesystem::path& path) {
    CursorSizes sizes;
    std::ifstream input(path);
    std::string serial;
    unsigned percent{};
    while (input >> serial >> percent) {
        if (percent >= 50U && percent <= 300U) { sizes[serial] = percent; }
    }
    return sizes;
}

inline unsigned cursor_size_for(const CursorSizes& sizes, std::string_view serial) {
    const auto found = sizes.find(std::string(serial));
    return found == sizes.end() ? 100U : found->second;
}

inline bool save_cursor_sizes(const std::filesystem::path& path, const CursorSizes& sizes) {
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    if (error) { return false; }
    std::ofstream output(path, std::ios::trunc);
    for (const auto& [serial, percent] : sizes) { output << serial << ' ' << percent << '\n'; }
    output.close();
    return output.good();
}

struct CursorRect { float x, y, width, height; };

// Keep the same point in the bitmap under the mouse when its size changes.
// Hotspot coordinates are fractions of the original bitmap dimensions.
inline CursorRect resize_cursor(CursorRect rect, float hotspot_x, float hotspot_y, float scale) {
    rect.x -= (scale - 1.0F) * rect.width * hotspot_x;
    rect.y -= (scale - 1.0F) * rect.height * hotspot_y;
    rect.width *= scale;
    rect.height *= scale;
    return rect;
}
} // namespace wireview::host
