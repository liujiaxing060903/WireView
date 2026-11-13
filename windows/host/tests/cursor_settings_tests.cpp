#include "cursor_settings.hpp"
#include <cmath>
#include <iostream>
#include <Windows.h>

int main() {
    using namespace wireview::host;
    unsigned failures{};
    const auto path = std::filesystem::temp_directory_path() /
        (L"wireview-cursor-" + std::to_wstring(GetCurrentProcessId()) + L".txt");
    CursorSizes sizes;
    for (const unsigned percent : {50U, 87U, 137U, 299U, 300U}) {
        if (parse_cursor_size(std::to_wstring(percent)) != percent) { ++failures; }
    }
    for (const auto* invalid : {L"", L"49", L"301", L"100.5", L"-100", L"100%", L"999999999999"}) {
        if (parse_cursor_size(invalid).has_value()) { ++failures; }
    }
    if (cursor_size_for(sizes, "huawei") != 100U) { ++failures; }
    sizes["huawei"] = 137U;
    sizes["other"] = 87U;
    if (!save_cursor_sizes(path, sizes)) { ++failures; }
    const auto loaded = load_cursor_sizes(path);
    if (cursor_size_for(loaded, "huawei") != 137U || cursor_size_for(loaded, "other") != 87U ||
        cursor_size_for(loaded, "new-device") != 100U) { ++failures; }
    const CursorRect original{20.0F, 30.0F, 48.0F, 48.0F};
    for (const float hotspot : {0.0F, 0.25F, 14.0F / 48.0F}) {
        for (const float scale : {0.75F, 1.0F, 1.5F, 3.0F}) {
            const auto resized = resize_cursor(original, hotspot, hotspot, scale);
            if (std::abs(resized.x + resized.width * hotspot - (original.x + original.width * hotspot)) > 0.001F ||
                std::abs(resized.y + resized.height * hotspot - (original.y + original.height * hotspot)) > 0.001F ||
                resized.width != original.width * scale) { ++failures; }
        }
    }
    std::filesystem::remove(path);
    std::cout << "cursor settings/anchor failures=" << failures << '\n';
    return failures == 0U ? 0 : 1;
}
