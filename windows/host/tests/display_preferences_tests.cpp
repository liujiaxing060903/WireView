#include "display_preferences.hpp"
#include <stdexcept>

int main() {
    using namespace wireview::host;
    const auto directory = std::filesystem::temp_directory_path() /
        (L"wireview-display-preferences-test-" + std::to_wstring(GetCurrentProcessId()));
    const auto file = directory / L"preferences.ini";
    const auto require = [](bool condition) { if (!condition) { throw std::runtime_error("display preferences check failed"); } };
    auto preferences = load_display_preferences(file);
    require(preferences.copy_resolution == L"auto" && preferences.copy_frame_rate == 60U);
    require(!preference_resolution(L"1x1080") && !preference_resolution(L"1920x1081") &&
        !preference_resolution(L"1920x1080garbage") && preference_resolution(L"1920x1080").has_value());
    preferences.copy_source = LR"(\\.\DISPLAY2)";
    preferences.copy_resolution = L"1280x720";
    preferences.copy_frame_rate = 120U;
    record_resolution_use(preferences, L"2000x1200");
    require(save_display_preferences(file, preferences));
    auto restored = load_display_preferences(file);
    require(restored.copy_source == preferences.copy_source && restored.copy_resolution == L"1280x720" &&
        restored.copy_frame_rate == 120U && restored.resolution_uses[L"2000x1200"] == 1U);
    record_resolution_use(restored, L"2000x1200");
    require(save_display_preferences(file, restored));
    require(load_display_preferences(file).resolution_uses[L"2000x1200"] == 2U);
    std::wofstream(file, std::ios::app) << L"copy_frame_rate=999\ncopy_resolution=0x0\nuse.2000x1200=-1\nuse.2560x1600=999999999999999999999999999\n";
    const auto recovered = load_display_preferences(file);
    require(recovered.copy_resolution == L"1280x720" && recovered.copy_frame_rate == 120U &&
        recovered.resolution_uses.at(L"2000x1200") == 2U);
    std::filesystem::remove(file);
    std::filesystem::remove(directory);
}
