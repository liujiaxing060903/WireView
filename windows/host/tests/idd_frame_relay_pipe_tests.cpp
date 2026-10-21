#include "idd_frame_relay_pipe.hpp"

#include <exception>
#include <iostream>

int main() {
    try {
        const int result = wireview::host::run_idd_frame_relay_pipe_loopback_test();
        if (result != 0) {
            return result;
        }
        std::cout << "WireView Idd frame relay named-pipe test passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "WireView Idd frame relay named-pipe test failed: " << error.what() << '\n';
        return 1;
    }
}
