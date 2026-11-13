#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "display_layout_lock.hpp"
#include "parsec_vdd_custom_modes.hpp"
#include <chrono>
#include <future>
#include <iostream>
#include <string>

int main() {
    using namespace wireview::host;
    std::promise<void> entered;
    std::future<std::string> request;
    bool waited{};
    {
        // This stands for the existing device's read/apply critical section.
        const LidDisplayLayoutLock restore(1'000U);
        if (!restore.acquired()) { std::cerr << "layout lock unavailable\n"; return 1; }
        request = std::async(std::launch::async, [&] {
            entered.set_value();
            try {
                // A deliberately nonexistent device exercises the real entry
                // point without changing any attached Windows display.
                set_parsec_vdd_display_mode(L"\\\\.\\WIREVIEW_LAYOUT_TEST_NO_DEVICE", 2772U, 1280U, 240U);
                return std::string("unexpectedly accepted an invalid display");
            } catch (const std::exception& error) {
                return std::string(error.what());
            }
        });
        entered.get_future().wait();
        waited = request.wait_for(std::chrono::milliseconds(200)) == std::future_status::timeout;
    }
    const auto result = request.get();
    if (!waited) {
        std::cerr << "mode application bypassed the other device's layout lock\n";
        return 1;
    }
    if (result.find("ChangeDisplaySettingsExW rejected") == std::string::npos) {
        std::cerr << "mode request did not continue normally after layout unlock: " << result << '\n';
        return 1;
    }
    std::cout << "Mode application waits for another device's layout restore\n";
    return 0;
}
