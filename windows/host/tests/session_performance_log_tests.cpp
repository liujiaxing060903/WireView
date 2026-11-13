#include "../src/session_performance_log.hpp"

#include <Windows.h>
#include <stdexcept>

namespace {
std::string read(const std::filesystem::path& path) {
    std::ifstream input(path);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}
void require(bool condition, const char* message) {
    if (!condition) { throw std::runtime_error(message); }
}
}

int main() {
    try {
        const auto directory = std::filesystem::temp_directory_path() /
            ("wireview-performance-test-" + std::to_string(GetCurrentProcessId()));
        std::ostringstream console;
        std::ostringstream errors;
        std::filesystem::path first;
        {
            wireview::host::SessionPerformanceLog log(directory, GetCurrentProcessId(), console, errors);
            first = log.path();
            console << "connected\n";
            require(read(first).find(" | host | connected\n") != std::string::npos,
                "completed lines must survive before disconnect or logger destruction");
            std::thread worker([&] {
                for (int index = 0; index < 20; ++index) {
                    console << "worker=" << index << '\n';
                }
            });
            for (int index = 0; index < 20; ++index) { console << "sender=" << index << '\n'; }
            worker.join();
            errors << "decoder recovery\n";
        }
        const auto saved = read(first);
        for (int index = 0; index < 20; ++index) {
            require(saved.find(" | host | worker=" + std::to_string(index) + '\n') != std::string::npos,
                "decoder and sender diagnostic lines must not interleave");
            require(saved.find(" | host | sender=" + std::to_string(index) + '\n') != std::string::npos,
                "sender diagnostic line missing");
        }
        require(saved.find(" | error | decoder recovery\n") != std::string::npos, "stderr missing");
        {
            wireview::host::SessionPerformanceLog log(directory, GetCurrentProcessId(), console, errors);
            require(log.path() != first, "reconnecting must use a new file");
            console << "reconnected\n";
            require(read(first) == saved, "reconnecting must retain the previous connection");
            std::filesystem::remove(first);
            first = log.path();
        }
        console << "restored\n";
        require(read(first).find("restored") == std::string::npos, "console buffer not restored");
        std::filesystem::remove(first);
        std::filesystem::remove(directory);
        std::cout << "Session performance log tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
