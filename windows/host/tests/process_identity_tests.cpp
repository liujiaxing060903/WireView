#include "process_identity.hpp"
#include <iostream>

int main() {
    using namespace wireview::host;
    const auto pid = GetCurrentProcessId();
    const auto created = process_creation_time(pid);
    const bool valid = created != 0U && recorded_process_is_alive(pid, created) &&
        !recorded_process_is_alive(pid, created - 1U) &&
        !recorded_process_is_alive(pid, 0U, created - 1U) &&
        recorded_process_is_alive(pid, 0U, created + 1U) &&
        !recorded_process_is_alive(0U, 0U);
    std::cout << "PID reuse / legacy record age: " << (valid ? "passed" : "FAILED") << '\n';
    return valid ? 0 : 1;
}
