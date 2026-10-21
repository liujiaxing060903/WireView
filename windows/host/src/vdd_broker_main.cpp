#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <Shellapi.h>

#include "parsec_vdd.hpp"
#include "parsec_vdd_custom_modes.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>
#include <optional>

namespace {

class UniqueHandle {
public:
    UniqueHandle() = default;
    explicit UniqueHandle(HANDLE handle) : handle_(handle) {}
    ~UniqueHandle() {
        if (handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE) {
            CloseHandle(handle_);
        }
    }

    UniqueHandle(const UniqueHandle&) = delete;
    UniqueHandle& operator=(const UniqueHandle&) = delete;

    [[nodiscard]] HANDLE get() const noexcept { return handle_; }
    [[nodiscard]] bool valid() const noexcept {
        return handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE;
    }

private:
    HANDLE handle_{};
};

struct BrokerOptions {
    DWORD owner_process_id{};
    std::wstring stop_event_name;
    std::filesystem::path status_file;
    std::optional<wireview::host::ParsecVddCustomMode> requested_mode;
};

[[nodiscard]] wireview::host::ParsecVddCustomMode parse_mode(std::wstring_view text) {
    const std::size_t x = text.find_first_of(L"xX");
    const std::size_t at = text.find(L'@');
    if (x == std::wstring_view::npos || at == std::wstring_view::npos || x == 0U || x + 1U >= at) {
        throw std::invalid_argument("VDD mode must use WIDTHxHEIGHT@HZ");
    }
    const unsigned long width = std::stoul(std::wstring(text.substr(0U, x)));
    const unsigned long height = std::stoul(std::wstring(text.substr(x + 1U, at - x - 1U)));
    const unsigned long hz = std::stoul(std::wstring(text.substr(at + 1U)));
    if (width == 0UL || height == 0UL || hz == 0UL || width > 65535UL || height > 65535UL || hz > 1000UL) {
        throw std::invalid_argument("VDD mode is out of range");
    }
    return {static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height),
        static_cast<std::uint32_t>(hz)};
}

[[nodiscard]] BrokerOptions parse_options(int argument_count, wchar_t** arguments) {
    BrokerOptions options;
    for (int index = 1; index < argument_count; ++index) {
        const std::wstring_view option(arguments[index]);
        if (option == L"--owner-pid" && index + 1 < argument_count &&
            options.owner_process_id == 0U) {
            const unsigned long parsed = std::stoul(arguments[++index]);
            if (parsed == 0UL || parsed > static_cast<unsigned long>(MAXDWORD)) {
                throw std::invalid_argument("owner process id is invalid");
            }
            options.owner_process_id = static_cast<DWORD>(parsed);
            continue;
        }
        if (option == L"--stop-event" && index + 1 < argument_count &&
            options.stop_event_name.empty()) {
            options.stop_event_name = arguments[++index];
            continue;
        }
        if (option == L"--status-file" && index + 1 < argument_count &&
            options.status_file.empty()) {
            options.status_file = arguments[++index];
            continue;
        }
        if (option == L"--mode" && index + 1 < argument_count && !options.requested_mode) {
            options.requested_mode = parse_mode(arguments[++index]);
            continue;
        }
        throw std::invalid_argument("invalid WireView VDD broker arguments");
    }
    if (options.owner_process_id == 0U || options.stop_event_name.empty() ||
        options.status_file.empty()) {
        throw std::invalid_argument(
            "WireView VDD broker requires --owner-pid, --stop-event, and --status-file");
    }
    return options;
}

int apply_custom_modes(int argument_count, wchar_t** arguments) {
    if (argument_count != 7) {
        throw std::invalid_argument("--apply-custom-modes requires exactly five slot values");
    }
    std::array<std::optional<wireview::host::ParsecVddCustomMode>, 5U> modes;
    for (std::size_t index = 0U; index < modes.size(); ++index) {
        const std::wstring_view text(arguments[index + 2U]);
        if (text != L"-") {
            modes[index] = parse_mode(text);
        }
    }
    wireview::host::apply_parsec_vdd_custom_modes(modes);
    return 0;
}

[[nodiscard]] std::string single_line(std::string text) {
    std::replace(text.begin(), text.end(), '\r', ' ');
    std::replace(text.begin(), text.end(), '\n', ' ');
    return text;
}

void publish_status(
    const std::filesystem::path& status_file,
    std::string_view state,
    DWORD owner_process_id,
    int display_index = -1,
    std::string_view display_device = {},
    std::string_view message = {}) {
    std::error_code error;
    std::filesystem::create_directories(status_file.parent_path(), error);
    if (error) {
        throw std::runtime_error("failed to create the VDD broker status directory");
    }
    auto temporary_file = status_file;
    temporary_file += L".tmp";
    {
        std::ofstream output(temporary_file, std::ios::trunc);
        if (!output) {
            throw std::runtime_error("failed to write the VDD broker status file");
        }
        output << "state=" << state << '\n'
               << "owner_pid=" << owner_process_id << '\n'
               << "broker_pid=" << GetCurrentProcessId() << '\n';
        if (display_index >= 0) {
            output << "driver_index=" << display_index << '\n';
        }
        if (!display_device.empty()) {
            output << "display=" << display_device << '\n';
        }
        if (!message.empty()) {
            output << "message=" << single_line(std::string(message)) << '\n';
        }
    }
    std::filesystem::remove(status_file, error);
    error.clear();
    std::filesystem::rename(temporary_file, status_file, error);
    if (error) {
        std::filesystem::remove(temporary_file, error);
        throw std::runtime_error("failed to publish the VDD broker status file");
    }
}

int run_self_test() {
    std::error_code error;
    const auto temporary_directory = std::filesystem::temp_directory_path(error);
    if (error) {
        return 1;
    }
    const auto status_file = temporary_directory /
        (L"wireview-vdd-broker-self-test-" + std::to_wstring(GetCurrentProcessId()) + L".ini");
    try {
        publish_status(status_file, "ready", 1234U, 7, R"(\\.\DISPLAY42)");
        std::ifstream input(status_file);
        const std::string contents{
            std::istreambuf_iterator<char>(input),
            std::istreambuf_iterator<char>()};
        std::filesystem::remove(status_file, error);
        return contents.find("state=ready\n") != std::string::npos &&
                contents.find("owner_pid=1234\n") != std::string::npos &&
                contents.find("broker_pid=") != std::string::npos &&
                contents.find("driver_index=7\n") != std::string::npos &&
                contents.find(R"(display=\\.\DISPLAY42)") != std::string::npos
            ? 0
            : 1;
    } catch (const std::exception&) {
        std::filesystem::remove(status_file, error);
        return 1;
    }
}

}  // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    int argument_count = 0;
    wchar_t** arguments = CommandLineToArgvW(GetCommandLineW(), &argument_count);
    if (arguments == nullptr) {
        return 1;
    }
    if (argument_count == 2 && std::wstring_view(arguments[1]) == L"--self-test") {
        LocalFree(arguments);
        return run_self_test();
    }

    if (argument_count >= 2 && std::wstring_view(arguments[1]) == L"--apply-custom-modes") {
        try {
            const int result = apply_custom_modes(argument_count, arguments);
            LocalFree(arguments);
            return result;
        } catch (const std::exception&) {
            LocalFree(arguments);
            return 1;
        }
    }

    std::filesystem::path status_file;
    DWORD owner_process_id = 0U;
    try {
        const BrokerOptions options = parse_options(argument_count, arguments);
        status_file = options.status_file;
        owner_process_id = options.owner_process_id;
        UniqueHandle owner_process(OpenProcess(SYNCHRONIZE, FALSE, options.owner_process_id));
        if (!owner_process.valid()) {
            throw std::runtime_error("the WireView control process is no longer available");
        }
        if (WaitForSingleObject(owner_process.get(), 0U) != WAIT_TIMEOUT) {
            throw std::runtime_error("the WireView control process has already exited");
        }
        UniqueHandle stop_event(OpenEventW(SYNCHRONIZE, FALSE, options.stop_event_name.c_str()));
        if (!stop_event.valid()) {
            throw std::runtime_error("the WireView VDD stop event is unavailable");
        }

        wireview::host::ParsecVddSession session;
        std::string display_device;
        try {
            session.start();
            display_device = session.wait_for_display(std::chrono::seconds(30));
        } catch (const std::exception& first_attempt_error) {
            const std::string first_detail(first_attempt_error.what());
            const bool first_display_never_attached = first_detail.find(
                "Parsec VDD created a display but WireView did not find a uniquely new attached") !=
                std::string::npos;
            if (!first_display_never_attached) {
                throw;
            }
            // On a small set of laptop GPU/display topologies, the first VDD
            // ADD is accepted but is exposed only through the existing primary
            // DISPLAY path. Remove that unusable instance and retry once; the
            // next allocation commonly receives a distinct Windows output.
            session.stop();
            publish_status(options.status_file, "retrying", options.owner_process_id, -1, {},
                "first VDD attempt did not become a separate attached Windows display; "
                "removed it and retrying once: " +
                    first_detail);
            Sleep(1'000U);
            session.start();
            display_device = session.wait_for_display(std::chrono::seconds(30));
        }
        if (options.requested_mode) {
            const auto& mode = *options.requested_mode;
            std::wstring display(display_device.begin(), display_device.end());
            wireview::host::set_parsec_vdd_display_mode(
                display.c_str(), mode.width, mode.height, mode.refresh_hz);
        }
        publish_status(
            options.status_file,
            "ready",
            options.owner_process_id,
            session.display_index(),
            display_device,
            session.creation_diagnostic());

        const HANDLE wait_handles[]{owner_process.get(), stop_event.get()};
        const DWORD wait_result = WaitForMultipleObjects(2U, wait_handles, FALSE, INFINITE);
        if (wait_result != WAIT_OBJECT_0 && wait_result != WAIT_OBJECT_0 + 1U) {
            throw std::runtime_error("waiting for WireView VDD shutdown failed");
        }
        session.stop();
        std::error_code error;
        std::filesystem::remove(options.status_file, error);
        LocalFree(arguments);
        return 0;
    } catch (const std::exception& error) {
        if (!status_file.empty()) {
            try {
                publish_status(status_file, "error", owner_process_id, -1, {}, error.what());
            } catch (const std::exception&) {
            }
        }
        LocalFree(arguments);
        return 1;
    }
}
