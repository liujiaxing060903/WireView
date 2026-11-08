#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

constexpr std::wstring_view kPackageName = L"com.wireview.receiver";
constexpr std::wstring_view kActivityName = L"com.wireview.receiver/.MainActivity";
constexpr std::wstring_view kReversePort = L"tcp:39871";
constexpr std::uint32_t kUnlimitedSessionSeconds = 2'147'483'647U;

struct Options {
    std::filesystem::path status_file;
    // Host 的标准输出会在会话仍运行时持续镜像到此文件。控制中心即使强制停止
    // 整个 Job，也能保留停止前已经产生的 GPU、编码器和传输诊断。
    std::filesystem::path host_diagnostics_file;
    // A practical "until stopped" duration.  Keeping this a positive value
    // preserves the Host's existing CLI contract for zero-duration test paths.
    std::uint32_t duration_seconds{kUnlimitedSessionSeconds};
    std::uint16_t width{};
    std::uint16_t height{};
    std::uint16_t frame_rate{60U};
    std::wstring display;
    bool enable_input{false};
    bool device_preview{false};
    bool device_profile_probe{false};
    // R3 WiFi：非空即 WiFi 模式（格式 ip:port），跳过全部 ADB 编排。
    std::wstring wifi_endpoint;
};

struct ProcessResult {
    DWORD exit_code{ERROR_GEN_FAILURE};
    std::wstring output;
    bool started{false};
};

// Full CommandLineToArgvW-compatible quoting: every run of backslashes
// that directly precedes a double quote (or ends the argument) must be
// doubled so the child recovers the original characters. The previous
// simplified version mangled arguments such as C:\dir\ or values
// containing \", shifting every later argument of the child command line.
std::wstring quote_argument(const std::wstring& value) {
    std::wstring quoted{L"\""};
    std::size_t backslashes{};
    for (const wchar_t character : value) {
        if (character == L'\\') {
            ++backslashes;
            continue;
        }
        if (character == L'\"') {
            quoted.append(backslashes + 1U, L'\\');
            quoted.push_back(character);
        } else {
            quoted.append(backslashes, L'\\');
            quoted.push_back(character);
        }
        backslashes = 0U;
    }
    quoted.append(backslashes * 2U, L'\\');
    quoted += L'\"';
    return quoted;
}

std::filesystem::path current_executable() {
    std::wstring buffer(32'768U, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, buffer.data(),
        static_cast<DWORD>(buffer.size()));
    if (length == 0U || length >= buffer.size()) {
        return {};
    }
    buffer.resize(length);
    return buffer;
}

std::optional<std::wstring> environment_value(const wchar_t* name) {
    std::wstring value(32'768U, L'\0');
    const DWORD length = GetEnvironmentVariableW(name, value.data(),
        static_cast<DWORD>(value.size()));
    if (length == 0U || length >= value.size()) {
        return std::nullopt;
    }
    value.resize(length);
    return value;
}

std::string to_utf8(const std::wstring& value) {
    if (value.empty()) {
        return {};
    }
    const int length = WideCharToMultiByte(CP_UTF8, 0, value.data(),
        static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (length <= 0) {
        return {};
    }
    std::string result(static_cast<std::size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
        result.data(), length, nullptr, nullptr);
    return result;
}

// Widens narrow ASCII diagnostics (exception messages) back to wide text
// for the status file.
std::wstring widen_ascii(std::string_view value) {
    std::wstring result(value.size(), L'\0');
    for (std::size_t index = 0U; index < value.size(); ++index) {
        result[index] = static_cast<wchar_t>(static_cast<unsigned char>(value[index]));
    }
    return result;
}

// Append-only diagnostics for the reverse supervisor. The supervisor used
// to fail silently, which made "replug never recovers" undiagnosable: the
// only evidence was a missing mapping. One line per state change plus a
// periodic heartbeat lands here, next to the other WireView logs.
void log_supervisor_line(const std::filesystem::path& status_file,
    const std::string& line) {
    const std::filesystem::path directory = status_file.parent_path();
    std::error_code make_error;
    std::filesystem::create_directories(directory, make_error);
    const std::filesystem::path log_file = directory / L"reverse-supervisor.log";
    std::ofstream stream(log_file, std::ios::app);
    if (!stream) {
        return;
    }
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(now).count();
    stream << seconds << ' ' << line << '\n';
}

std::filesystem::path find_adb() {
    std::vector<std::filesystem::path> candidates;
    const auto executable_directory = current_executable().parent_path();
    candidates.push_back(executable_directory / L"adb.exe");
    std::filesystem::path parent = executable_directory;
    for (int level = 0; level < 6 && !parent.empty(); ++level) {
        candidates.push_back(parent / L"platform-tools" / L"adb.exe");
        parent = parent.parent_path();
    }
    for (const wchar_t* variable : {L"ANDROID_HOME", L"ANDROID_SDK_ROOT"}) {
        if (const auto value = environment_value(variable)) {
            candidates.push_back(std::filesystem::path(*value) / L"platform-tools" / L"adb.exe");
        }
    }
    if (const auto local_app_data = environment_value(L"LOCALAPPDATA")) {
        candidates.push_back(std::filesystem::path(*local_app_data) /
            L"Android" / L"Sdk" / L"platform-tools" / L"adb.exe");
    }
    for (const auto& candidate : candidates) {
        std::error_code error;
        if (std::filesystem::is_regular_file(candidate, error)) {
            return candidate;
        }
    }
    wchar_t path_buffer[32'768U]{};
    if (SearchPathW(nullptr, L"adb.exe", nullptr,
            static_cast<DWORD>(std::size(path_buffer)), path_buffer, nullptr) > 0U) {
        return path_buffer;
    }
    return {};
}

void write_status(const Options& options, const std::wstring& state,
    const std::wstring& message, std::uint32_t reconnect_count,
    const std::wstring& diagnostics = {}) {
    if (options.status_file.empty()) {
        return;
    }
    // The status file is the session's only feedback channel; every failure
    // below leaves a debugger-visible trace so a stalled control center can
    // be diagnosed instead of polling a stale state forever without a hint.
    std::error_code error;
    if (!std::filesystem::create_directories(options.status_file.parent_path(), error) &&
        error) {
        OutputDebugStringA(("WireView session: creating the status directory failed (error " +
            std::to_string(error.value()) + "): " +
            to_utf8(options.status_file.parent_path().wstring()) + "\n").c_str());
        error.clear();
    }
    const auto temporary = options.status_file.wstring() + L".tmp";
    std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
    if (!stream) {
        OutputDebugStringA(("WireView session: opening the status temporary file failed: " +
            to_utf8(temporary) + "\n").c_str());
        return;
    }
    const std::wstring status = L"version=1\nstate=" + state + L"\nmessage=" + message +
        L"\ninput_mode=" + (options.enable_input ? std::wstring(L"enabled") : std::wstring(L"log-only")) +
        L"\ndisplay=" + options.display +
        L"\nrequested_width=" + std::to_wstring(options.width) +
        L"\nrequested_height=" + std::to_wstring(options.height) +
        L"\nrequested_fps=" + std::to_wstring(options.frame_rate) +
        L"\ntransport=" + (options.wifi_endpoint.empty() ? L"adb" : L"wifi") +
        L"\nreconnect_count=" + std::to_wstring(reconnect_count) +
        L"\ndiagnostics=" + diagnostics + L"\n";
    stream << to_utf8(status);
    stream.close();
    std::filesystem::rename(temporary, options.status_file, error);
    if (error) {
        // The control center may briefly hold the target file open while
        // polling; remove and rename once more before giving up with a trace.
        OutputDebugStringA(("WireView session: publishing the status file failed (error " +
            std::to_string(error.value()) + "), retrying: " +
            to_utf8(options.status_file.wstring()) + "\n").c_str());
        error.clear();
        std::filesystem::remove(options.status_file, error);
        error.clear();
        std::filesystem::rename(temporary, options.status_file, error);
        if (error) {
            OutputDebugStringA(("WireView session: the status file rename retry failed (error " +
                std::to_string(error.value()) + "): " +
                to_utf8(options.status_file.wstring()) + "\n").c_str());
        }
    }
}

// adb.exe is the machine-wide Android debug bridge server: once started it
// outlives WireView and is shared with other tools on the machine. When
// this session runs inside the control center's kill-on-close job, every
// spawned adb (and the detached server it forks) would be terminated with
// the session, so adb launches request CREATE_BREAKAWAY_FROM_JOB. The
// host executable never matches this predicate and always stays in the job.
bool is_adb_executable(const std::filesystem::path& executable) {
    constexpr std::wstring_view kAdbFileName = L"adb.exe";
    const std::wstring filename = executable.filename().wstring();
    if (filename.size() != kAdbFileName.size()) {
        return false;
    }
    for (std::size_t index = 0U; index < kAdbFileName.size(); ++index) {
        const auto lowered = static_cast<wchar_t>(towlower(filename[index]));
        if (lowered != kAdbFileName[index]) {
            return false;
        }
    }
    return true;
}

std::vector<wchar_t> build_command_line(const std::filesystem::path& executable,
    const std::vector<std::wstring>& arguments) {
    std::wstring command = quote_argument(executable.wstring());
    for (const auto& argument : arguments) {
        command += L" ";
        command += quote_argument(argument);
    }
    std::vector<wchar_t> buffer(command.begin(), command.end());
    buffer.push_back(L'\0');
    return buffer;
}

BOOL spawn_child_process(const std::filesystem::path& executable, wchar_t* command_line,
    const std::filesystem::path& working_directory, STARTUPINFOW* startup,
    PROCESS_INFORMATION* process) {
    const bool wants_breakaway = is_adb_executable(executable);
    const std::wstring directory = working_directory.empty()
        ? std::wstring{}
        : working_directory.wstring();
    const wchar_t* directory_argument = directory.empty() ? nullptr : directory.c_str();
    const DWORD base_flags = CREATE_NO_WINDOW;
    BOOL created = FALSE;
    if (wants_breakaway) {
        // adb escapes the kill-on-close job so the system singleton adb
        // server survives a session stop (it would otherwise take down
        // Android Studio and every other adb user on the machine).
        created = CreateProcessW(executable.c_str(), command_line, nullptr, nullptr, TRUE,
            base_flags | CREATE_BREAKAWAY_FROM_JOB, nullptr, directory_argument, startup,
            process);
        if (!created) {
            // A job created without JOB_OBJECT_LIMIT_BREAKAWAY_OK refuses
            // the escape (for example a session launched by an older
            // control center); degrade to a regular in-job child. A retry
            // for unrelated failure causes just fails again harmlessly.
            created = CreateProcessW(executable.c_str(), command_line, nullptr, nullptr,
                TRUE, base_flags, nullptr, directory_argument, startup, process);
        }
    } else {
        created = CreateProcessW(executable.c_str(), command_line, nullptr, nullptr, TRUE,
            base_flags, nullptr, directory_argument, startup, process);
    }
    return created;
}

// Bounded variant for housekeeping adb calls: a hung adbd half-open
// pipe must not freeze the caller (audit P1 - the reverse supervisor
// join used to wait forever on such a freeze). The child is killed on
// timeout; its remaining output is drained afterwards (the pipe closes
// on death).
ProcessResult run_capture_bounded(
    const std::filesystem::path& executable,
    const std::vector<std::wstring>& arguments,
    const std::filesystem::path& working_directory = {},
    const std::filesystem::path& live_output_file = {},
    DWORD timeout_milliseconds = 15'000U) {
    (void)live_output_file;  // the bounded path never mirrors live output
    SECURITY_ATTRIBUTES security{};
    security.nLength = sizeof(security);
    security.bInheritHandle = TRUE;
    HANDLE read_pipe{};
    HANDLE write_pipe{};
    if (!CreatePipe(&read_pipe, &write_pipe, &security, 0U)) {
        return {};
    }
    SetHandleInformation(read_pipe, HANDLE_FLAG_INHERIT, 0U);
    std::vector<wchar_t> command_buffer = build_command_line(executable, arguments);
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdOutput = write_pipe;
    startup.hStdError = write_pipe;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION process{};
    const BOOL created = spawn_child_process(executable, command_buffer.data(),
        working_directory, &startup, &process);
    CloseHandle(write_pipe);
    if (!created) {
        CloseHandle(read_pipe);
        return {};
    }
    const auto deadline = GetTickCount64() + timeout_milliseconds;
    // Drain the pipe concurrently: children that write more than the pipe
    // capacity (e.g. "dumpsys activity activities") would otherwise block
    // on a full pipe, miss the deadline and get killed with the output
    // still stuck in the kernel buffer.
    std::string bytes;
    std::thread pipe_reader([&bytes, read_pipe] {
        char buffer[1024];
        DWORD read = 0U;
        while (ReadFile(read_pipe, buffer, sizeof(buffer), &read, nullptr) && read > 0U) {
            bytes.append(buffer, read);
        }
    });
    while (WaitForSingleObject(process.hProcess, 200) == WAIT_TIMEOUT) {
        if (GetTickCount64() >= deadline) {
            TerminateProcess(process.hProcess, 1U);
            WaitForSingleObject(process.hProcess, 5'000U);
            break;
        }
    }
    ProcessResult result{};
    result.started = true;
    GetExitCodeProcess(process.hProcess, &result.exit_code);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    // Closing the read end first is essential when this run spawned the adb
    // server: the resident server inherits the pipe handles, so the write
    // end never runs dry and the reader's ReadFile would block forever.
    // Closing read_pipe fails that pending ReadFile immediately and the
    // join completes; the collected bytes are already safe in 'bytes'.
    CloseHandle(read_pipe);
    pipe_reader.join();
    if (!bytes.empty()) {
        const int length = MultiByteToWideChar(CP_UTF8, 0, bytes.data(),
            static_cast<int>(bytes.size()), nullptr, 0);
        if (length > 0) {
            result.output.resize(static_cast<std::size_t>(length));
            MultiByteToWideChar(CP_UTF8, 0, bytes.data(), static_cast<int>(bytes.size()),
                result.output.data(), length);
        }
    }
    return result;
}
ProcessResult run_capture(const std::filesystem::path& executable,
    const std::vector<std::wstring>& arguments, const std::filesystem::path& working_directory = {},
    const std::filesystem::path& live_output_file = {}) {
    SECURITY_ATTRIBUTES security{};
    security.nLength = sizeof(security);
    security.bInheritHandle = TRUE;
    HANDLE read_pipe{};
    HANDLE write_pipe{};
    if (!CreatePipe(&read_pipe, &write_pipe, &security, 0U)) {
        return {};
    }
    SetHandleInformation(read_pipe, HANDLE_FLAG_INHERIT, 0U);
    std::vector<wchar_t> command_buffer = build_command_line(executable, arguments);
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdOutput = write_pipe;
    startup.hStdError = write_pipe;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION process{};
    const BOOL created = spawn_child_process(executable, command_buffer.data(),
        working_directory, &startup, &process);
    CloseHandle(write_pipe);
    if (!created) {
        CloseHandle(read_pipe);
        return {};
    }
    std::ofstream live_output;
    if (!live_output_file.empty()) {
        std::error_code error;
        std::filesystem::create_directories(live_output_file.parent_path(), error);
        live_output.open(live_output_file, std::ios::binary | std::ios::trunc);
    }
    std::string bytes;
    char buffer[1024];
    DWORD read{};
    while (ReadFile(read_pipe, buffer, sizeof(buffer), &read, nullptr) && read > 0U) {
        bytes.append(buffer, read);
        if (live_output) {
            live_output.write(buffer, static_cast<std::streamsize>(read));
            live_output.flush();
        }
    }
    live_output.close();
    CloseHandle(read_pipe);
    WaitForSingleObject(process.hProcess, INFINITE);
    ProcessResult result{};
    result.started = true;
    GetExitCodeProcess(process.hProcess, &result.exit_code);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    if (!bytes.empty()) {
        const int count = MultiByteToWideChar(CP_UTF8, 0, bytes.data(),
            static_cast<int>(bytes.size()), nullptr, 0);
        if (count > 0) {
            result.output.resize(static_cast<std::size_t>(count));
            MultiByteToWideChar(CP_UTF8, 0, bytes.data(), static_cast<int>(bytes.size()),
                result.output.data(), count);
        }
    }
    return result;
}

std::vector<std::wstring> connected_devices(const std::wstring& output) {
    std::vector<std::wstring> devices;
    std::size_t begin{};
    while (begin < output.size()) {
        const std::size_t end = output.find_first_of(L"\r\n", begin);
        const std::wstring_view line(output.data() + begin,
            (end == std::wstring::npos ? output.size() : end) - begin);
        const std::size_t separator = line.find(L'\t');
        if (separator != std::wstring_view::npos && line.substr(separator + 1U) == L"device") {
            devices.emplace_back(line.substr(0U, separator));
        }
        begin = end == std::wstring::npos ? output.size() : end + 1U;
    }
    return devices;
}

bool parse_unsigned(std::wstring_view value, std::uint32_t* target) {
    if (value.empty()) {
        return false;
    }
    std::uint32_t result{};
    for (const wchar_t character : value) {
        if (character < L'0' || character > L'9') {
            return false;
        }
        const std::uint32_t digit = static_cast<std::uint32_t>(character - L'0');
        // Reject values that would wrap around uint32_t; without this a
        // --duration-seconds of 4294967297 would silently become 1 second.
        if (result > (UINT32_MAX - digit) / 10U) {
            return false;
        }
        result = result * 10U + digit;
    }
    *target = result;
    return true;
}

bool parse_arguments(int argc, wchar_t** argv, Options* options) {
    for (int index = 1; index < argc; ++index) {
        const std::wstring_view argument(argv[index]);
        const auto require_value = [&](std::wstring* value) -> bool {
            if (index + 1 >= argc) {
                return false;
            }
            *value = argv[++index];
            return true;
        };
        std::wstring value;
        std::uint32_t number{};
        if (argument == L"--status-file" && require_value(&value)) {
            options->status_file = value;
        } else if (argument == L"--host-diagnostics-file" && require_value(&value)) {
            options->host_diagnostics_file = value;
        } else if (argument == L"--duration-seconds" && require_value(&value) && parse_unsigned(value, &number)) {
            options->duration_seconds = number;
        } else if (argument == L"--width" && require_value(&value) && parse_unsigned(value, &number) && number <= UINT16_MAX) {
            options->width = static_cast<std::uint16_t>(number);
        } else if (argument == L"--height" && require_value(&value) && parse_unsigned(value, &number) && number <= UINT16_MAX) {
            options->height = static_cast<std::uint16_t>(number);
        } else if (argument == L"--frame-rate" && require_value(&value) && parse_unsigned(value, &number) && number <= UINT16_MAX) {
            options->frame_rate = static_cast<std::uint16_t>(number);
        } else if (argument == L"--display" && require_value(&value)) {
            options->display = value;
        } else if (argument == L"--enable-input") {
            options->enable_input = true;
        } else if (argument == L"--device-preview") {
            options->device_preview = true;
        } else if (argument == L"--device-profile-probe") {
            options->device_profile_probe = true;
        } else if (argument == L"--transport" && require_value(&value)) {
            if (value != L"wifi") {
                return false;
            }
            // WiFi 端点随 --transport 之后传入（见下）。
            if (index + 1 >= argc || argv[index + 1][0] == L'-') {
                return false;
            }
            options->wifi_endpoint = argv[++index];
            // The downstream WiFi transport is IPv4-only (it fills a
            // sockaddr_in). An endpoint with more than one colon is an
            // IPv6 literal or malformed; reject it here, at parse time,
            // instead of failing later with a misleading ip:port split.
            if (std::count(options->wifi_endpoint.begin(), options->wifi_endpoint.end(),
                    L':') > 1) {
                throw std::invalid_argument("IPv6 endpoints are not supported; use IPv4:port");
            }
        } else {
            return false;
        }
    }
    return !options->status_file.empty() &&
        (options->device_preview || options->device_profile_probe || !options->display.empty());
}

ProcessResult run_host(const Options& options, const std::filesystem::path& host,
    std::uint32_t session_seconds, std::uint32_t reconnect_count) {
    (void)reconnect_count;  // the reconnecting entry tracks its own interruptions
    std::vector<std::wstring> arguments;
    if (options.device_profile_probe) {
        arguments.push_back(L"--probe-device-profile");
    } else if (options.device_preview) {
        // The host rejects --device-preview durations above 1800 seconds,
        // while sessions default to an "until stopped" duration. Clamp the
        // forwarded preview duration to that CLI contract; the mirror and
        // reconnect paths below have no such cap.
        constexpr std::uint32_t kMaximumPreviewSeconds = 1'800U;
        arguments.insert(arguments.end(), {L"--device-preview",
            std::to_wstring((std::min)(session_seconds, kMaximumPreviewSeconds))});
    } else {
        arguments.insert(arguments.end(), {L"--stream-desktop", std::to_wstring(session_seconds)});
        arguments.push_back(L"--display");
        arguments.push_back(options.display);
    }
    if (!options.device_profile_probe) {
        if (options.width != 0U && options.height != 0U) {
            arguments.insert(arguments.end(), {L"--size", std::to_wstring(options.width) + L"x" + std::to_wstring(options.height)});
        }
        arguments.insert(arguments.end(), {L"--frame-rate", std::to_wstring(options.frame_rate)});
        if (!options.device_preview) {
            if (!options.wifi_endpoint.empty()) {
                // R3 WiFi：PC 作为客户端直连平板；凭据经环境变量传递。
                // 会话用重连入口：通道故障自动重拨，不再整场终止。
                const auto colon = options.wifi_endpoint.rfind(L':');
                if (colon == std::wstring::npos || colon == 0U ||
                    colon + 1U >= options.wifi_endpoint.size()) {
                    return ProcessResult{};
                }
                arguments[0] = L"--reconnect-desktop";
                arguments.insert(arguments.end(), {L"--transport", L"wifi"});
                arguments.insert(arguments.end(), {L"--remote-ip", options.wifi_endpoint.substr(0U, colon)});
                arguments.insert(arguments.end(), {L"--remote-port", options.wifi_endpoint.substr(colon + 1U)});
            } else {
                // USB 会话用重连入口：adb reverse 抖动/平板短暂断开时自动
                // 恢复（监督线程保 reverse 映射，host 循环重 accept）。
                arguments[0] = L"--reconnect-desktop";
                arguments.insert(arguments.end(), {L"--transport", L"adb"});
            }
        }
    }
    if (!options.enable_input && !options.device_preview && !options.device_profile_probe) {
        arguments.push_back(L"--input-log-only");
    }
    arguments.insert(arguments.end(), {L"--status-file", options.status_file.wstring()});
    // The reconnecting entry tracks interruptions itself; reconnect_count is
    // only consumed by the single-shot --stream-desktop path.
    if (!options.device_profile_probe && !options.device_preview &&
        options.wifi_endpoint.empty()) {
        // (kept for the winusb/legacy single-shot variants; harmless)
    }
    return run_capture(host, arguments, host.parent_path(), options.host_diagnostics_file);
}

std::wstring last_output_line(const std::wstring& output) {
    std::size_t end = output.size();
    while (end > 0U && iswspace(output[end - 1U])) {
        --end;
    }
    if (end == 0U) {
        return {};
    }
    const std::size_t begin_marker = output.find_last_of(L"\r\n", end - 1U);
    const std::size_t begin = begin_marker == std::wstring::npos ? 0U : begin_marker + 1U;
    return output.substr(begin, end - begin);
}

// Status files are line-oriented. Keep the complete Host console output in one
// value and restore its line breaks in the control center. This deliberately
// retains startup information (GPU, MFT name, control settings) before the
// final HRESULT, which is usually essential for encoder compatibility reports.
std::wstring compact_host_diagnostics(std::wstring output) {
    while (!output.empty() && iswspace(output.back())) {
        output.pop_back();
    }
    std::wstring compact;
    compact.reserve(output.size());
    for (std::size_t index = 0; index < output.size(); ++index) {
        const wchar_t character = output[index];
        if (character == L'\r') {
            if (index + 1U < output.size() && output[index + 1U] == L'\n') {
                ++index;
            }
            compact += L"[[nl]]";
        } else if (character == L'\n') {
            compact += L"[[nl]]";
        } else if (character != L'\0') {
            compact.push_back(character);
        }
    }
    constexpr std::size_t kMaximumDiagnosticCharacters = 12'000U;
    if (compact.size() <= kMaximumDiagnosticCharacters) {
        return compact;
    }
    constexpr std::size_t kHeadCharacters = 8'500U;
    constexpr std::size_t kTailCharacters = 3'000U;
    return compact.substr(0U, kHeadCharacters) +
        L"[[nl]][…Windows Host 输出过长，已保留开头和结尾…][[nl]]" +
        compact.substr(compact.size() - kTailCharacters);
}

std::wstring host_failure_message(const ProcessResult& result, bool wifi_session = false) {
    const std::wstring detail = last_output_line(result.output);
    if (detail.find(L"No WireView client connected before the timeout") != std::wstring::npos ||
        detail.find(L"connect timed out") != std::wstring::npos ||
        detail.find(L"connect failed") != std::wstring::npos) {
        if (wifi_session) {
            return L"电脑端无法通过 WiFi 连接平板：请确认两端在同一局域网、平板已开启 WiFi 投屏开关，且 IP 正确（可用重新配对更新）。";
        }
        return L"Android 端 WireView 未在 5 分钟内连接到 Windows Host；请保持应用在前台，并检查 USB 调试和 ADB reverse。";
    }
    if (detail.find(L"client did not return HELLO_ACK") != std::wstring::npos) {
        return L"Android 端已建立 TCP 连接，但未完成 WireView 握手；请检查 Android 应用是否崩溃、被省电策略限制或版本不匹配。";
    }
    if (detail.empty()) {
        return L"Windows Host 异常退出（退出代码 " + std::to_wstring(result.exit_code) + L"），未输出诊断信息。";
    }
    return L"Windows Host 启动/运行失败（退出代码 " +
        std::to_wstring(result.exit_code) + L"）：" + detail;
}

int run(const Options& options) {
    const auto host = current_executable().parent_path() / L"wireview_host.exe";
    if (!std::filesystem::is_regular_file(host)) {
        write_status(options, L"error", L"未找到 wireview_host.exe；请重新解压完整的 WireView 发布包。", 0U);
        return 2;
    }
    // R3 WiFi 模式：没有 ADB 设备编排（reverse/启动应用/设备检测都不适用），
    // PC 直接作为客户端连接平板端点，凭据经环境变量传递。
    if (!options.wifi_endpoint.empty()) {
        write_status(options, L"starting", L"正在通过 WiFi 连接平板…", 0U);
        const ProcessResult host_result = run_host(options, host, options.duration_seconds, 0U);
        if (host_result.exit_code == 0U) {
            write_status(options, L"stopped", L"WireView WiFi 会话已结束。", 0U,
                compact_host_diagnostics(host_result.output));
            return 0;
        }
        write_status(options, L"error", host_failure_message(host_result, true), 0U,
            compact_host_diagnostics(host_result.output));
        return static_cast<int>(host_result.exit_code);
    }
    const auto deadline = std::chrono::steady_clock::now() +
        std::chrono::seconds(options.duration_seconds);
    std::uint32_t reconnect_count{};
    while (std::chrono::steady_clock::now() < deadline) {
        const auto adb = find_adb();
        if (adb.empty()) {
            write_status(options, L"waiting_adb", L"未找到 adb.exe。请安装官方 Android SDK Platform Tools，或放到发布包的 platform-tools 文件夹。", reconnect_count);
            Sleep(1000U);
            continue;
        }
        const auto device_query = run_capture_bounded(adb, {L"devices"});
        const auto devices = device_query.started ? connected_devices(device_query.output) : std::vector<std::wstring>{};
        if (devices.size() != 1U) {
            if (device_query.output.find(L"\tunauthorized") != std::wstring::npos) {
                write_status(options, L"waiting_authorization", L"设备未授权 USB 调试。请在 Android 设备上允许此电脑。", reconnect_count);
            } else if (devices.empty()) {
                write_status(options, L"waiting_device", L"未检测到已授权的 Android 设备。请检查 USB 数据线、端口和 USB 调试授权。", reconnect_count);
            } else {
                write_status(options, L"waiting_device", L"检测到多台 Android 设备；请仅保留一台设备连接。", reconnect_count);
            }
            Sleep(1000U);
            continue;
        }
        const auto& serial = devices.front();
        const auto package = run_capture_bounded(adb, {L"-s", serial, L"shell", L"pm", L"path", std::wstring(kPackageName)});
        if (!package.started || package.exit_code != 0U || package.output.find(L"package:") == std::wstring::npos) {
            write_status(options, L"error", L"未检测到 WireView Android 应用；请先安装 APK。", reconnect_count);
            return 3;
        }
        const auto reverse = run_capture_bounded(adb, {L"-s", serial, L"reverse", std::wstring(kReversePort), std::wstring(kReversePort)});
        if (!reverse.started || reverse.exit_code != 0U) {
            write_status(options, L"waiting", L"ADB 端口映射失败，正在重试。", reconnect_count);
            Sleep(1000U);
            continue;
        }
        const auto launch = run_capture_bounded(adb, {L"-s", serial, L"shell", L"am", L"start", L"-n", std::wstring(kActivityName)});
        if (!launch.started || launch.exit_code != 0U) {
            write_status(options, L"waiting", L"无法启动 WireView Android 应用，正在重试。", reconnect_count);
            Sleep(1000U);
            continue;
        }
        Sleep(800U);
        write_status(options, L"starting", L"Android 已准备完成，正在启动电脑端镜像服务。", reconnect_count);
        const auto remaining = std::chrono::duration_cast<std::chrono::seconds>(
            deadline - std::chrono::steady_clock::now()).count();
        const auto session_seconds = static_cast<std::uint32_t>(
            std::max<std::int64_t>(1, remaining));
        // R3：host 重连会话运行期间监督 adb reverse 映射。拔线会让 adbd
        // 清掉全部 reverse；重插后若不重建，host 的 listener 永远等不到
        // 连接。每 2s 先查 reverse --list，缺失才重建（直接重跑 reverse
        // 会先删旧映射，踢断正在工作的连接）。
        std::atomic_bool reverse_supervisor_running{true};
        std::thread reverse_supervisor([&]() {
            const std::string serial_narrow = to_utf8(serial);
            bool last_mapped = true;
            auto last_heartbeat = std::chrono::steady_clock::now() - std::chrono::seconds(60);
            while (reverse_supervisor_running.load()) {
                const auto listing = run_capture_bounded(
                    adb, {L"-s", serial, L"reverse", L"--list"},
                    {}, {}, 15'000U);
                const bool mapped = listing.started && listing.exit_code == 0U &&
                    listing.output.find(std::wstring(kReversePort)) != std::wstring::npos;
                bool rebuild_ok = true;
                if (!mapped) {
                    const auto rebuild = run_capture_bounded(
                        adb, {L"-s", serial, L"reverse",
                              std::wstring(kReversePort), std::wstring(kReversePort)},
                        {}, {}, 15'000U);
                    rebuild_ok = rebuild.started && rebuild.exit_code == 0U;
                }
                const auto now = std::chrono::steady_clock::now();
                if (mapped != last_mapped || now - last_heartbeat >= std::chrono::seconds(15)) {
                    last_mapped = mapped;
                    last_heartbeat = now;
                    log_supervisor_line(options.status_file,
                        "serial=" + serial_narrow +
                        " mapped=" + (mapped ? "1" : "0") +
                        (mapped ? "" : (rebuild_ok ? " rebuild=ok" : " rebuild=failed")) +
                        " list_exit=" + std::to_string(listing.exit_code) +
                        " list_started=" + (listing.started ? "1" : "0"));
                }
                for (int tick = 0; tick < 20 && reverse_supervisor_running.load(); ++tick) {
                    Sleep(100U);
                }
            }
        });
        const ProcessResult host_result = run_host(options, host, session_seconds, reconnect_count);
        reverse_supervisor_running.store(false);
        if (reverse_supervisor.joinable()) {
            // The supervisor may be parked inside a bounded adb call
            // (15 s worst case); the join still completes because the
            // bounded call kills the child on timeout.
            reverse_supervisor.join();
        }
        if (options.device_profile_probe) {
            return host_result.exit_code == 0U ? 0 : 4;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            break;
        }
        if (host_result.exit_code == 2U) {
            // The host gave up its in-process accept loop: no tablet showed
            // up for several rounds, which for the adb listener means the
            // reverse mapping is gone (a yanked or glitched cable clears
            // it) and the tablet cannot dial in until it is re-established.
            // Hand control back to this loop - it re-detects the device,
            // re-runs adb reverse and relaunches the host, the same
            // recovery path the pre-reconnect sessions used. am start on a
            // foreground activity is a harmless bring-to-front, not a
            // restart. Every other exit code stays terminal: a real host
            // failure must not be disguised as a waiting state.
            write_status(options, L"waiting",
                L"正在重设 ADB 端口映射并等待平板重新连接……", reconnect_count,
                compact_host_diagnostics(host_result.output));
            Sleep(500U);
            continue;
        }
        // 会话一旦断开即结束。不要在设备端被用户划掉后台后再次通过 ADB 拉起应用，
        // 也不要把拔线后的会话伪装成仍可继续的自动重连。
        write_status(options, L"error", host_failure_message(host_result), reconnect_count,
            compact_host_diagnostics(host_result.output));
        return host_result.exit_code == 0U ? 0 : static_cast<int>(host_result.exit_code);
    }
    write_status(options, L"stopped", L"WireView 会话已结束。", reconnect_count);
    return 0;
}

int detect_single_adb_device(bool require_wireview_app) {
    const auto adb = find_adb();
    if (adb.empty()) {
        return 2;
    }
    const auto query = run_capture_bounded(adb, {L"devices"});
    if (!query.started || query.exit_code != 0U) {
        return 2;
    }
    if (query.output.find(L"\tunauthorized") != std::wstring::npos) {
        return 4;
    }
    const auto devices = connected_devices(query.output);
    if (devices.empty()) {
        return 3;
    }
    if (devices.size() != 1U) {
        return 5;
    }
    if (!require_wireview_app) {
        return 0;
    }
    const auto process = run_capture_bounded(adb, {L"-s", devices.front(), L"shell", L"pidof",
        std::wstring(kPackageName)});
    const bool app_running = process.started && process.exit_code == 0U &&
        std::any_of(process.output.begin(), process.output.end(), [](wchar_t character) {
            return !iswspace(character);
        });
    if (!app_running) {
        return 6;
    }
    // “划掉后台”时 Android 进程可能短暂仍在，pidof 因而不能代表活动仍可用。
    // 若系统提供 resumed/top-resumed 行，则要求当前前台确实是 WireView；旧系统或
    // 厂商删减 dumpsys 输出时安全地退回到 pidof，避免错误拒绝正常设备。
    const auto activities = run_capture_bounded(adb, {L"-s", devices.front(), L"shell", L"dumpsys",
        L"activity", L"activities"});
    if (!activities.started || activities.exit_code != 0U) {
        return 0;
    }
    std::size_t start = 0U;
    bool found_foreground_marker = false;
    while (start < activities.output.size()) {
        const std::size_t end = activities.output.find(L'\n', start);
        const std::wstring_view line(activities.output.data() + start,
            (end == std::wstring::npos ? activities.output.size() : end) - start);
        if (line.find(L"mResumedActivity") != std::wstring::npos ||
            line.find(L"topResumedActivity") != std::wstring::npos) {
            found_foreground_marker = true;
            if (line.find(kPackageName) != std::wstring::npos) {
                return 0;
            }
        }
        if (end == std::wstring::npos) {
            break;
        }
        start = end + 1U;
    }
    return found_foreground_marker ? 6 : 0;
}

int notify_android_stop() {
    const auto adb = find_adb();
    if (adb.empty()) {
        return 1;
    }
    const auto query = run_capture_bounded(adb, {L"devices"});
    const auto devices = query.started && query.exit_code == 0U
        ? connected_devices(query.output)
        : std::vector<std::wstring>{};
    if (devices.size() != 1U) {
        return 1;
    }
    const auto result = run_capture_bounded(adb, {L"-s", devices.front(), L"shell", L"am", L"broadcast",
        L"-a", L"com.wireview.receiver.action.WIREVIEW_STOPPED"});
    return result.started && result.exit_code == 0U ? 0 : 1;
}

int notify_android_host_exited() {
    const auto adb = find_adb();
    if (adb.empty()) {
        return 1;
    }
    const auto query = run_capture_bounded(adb, {L"devices"});
    const auto devices = query.started && query.exit_code == 0U
        ? connected_devices(query.output)
        : std::vector<std::wstring>{};
    if (devices.size() != 1U) {
        return 1;
    }
    const auto result = run_capture_bounded(adb, {L"-s", devices.front(), L"shell", L"am", L"broadcast",
        L"-a", L"com.wireview.receiver.action.HOST_EXITED"});
    return result.started && result.exit_code == 0U ? 0 : 1;
}

int notify_pairing_approved(wchar_t** argv) {
    // R3：把配对 PIN 经 USB（adb broadcast）投递给平板。线缆本身是带外信道，
    // WiFi 侧攻击者看不到该值；投递失败时用户仍可手输 PIN。
    const std::wstring pin = argv[2];
    if (pin.empty() || pin.size() > 32U) {
        return 1;
    }
    const auto adb = find_adb();
    if (adb.empty()) {
        return 1;
    }
    const auto query = run_capture_bounded(adb, {L"devices"});
    const auto devices = query.started && query.exit_code == 0U
        ? connected_devices(query.output)
        : std::vector<std::wstring>{};
    if (devices.size() != 1U) {
        return 1;
    }
    const auto result = run_capture_bounded(adb, {L"-s", devices.front(), L"shell", L"am", L"broadcast",
        L"-a", L"com.wireview.receiver.action.WIREVIEW_PAIRING_APPROVED",
        L"--es", L"pin", pin});
    return result.started && result.exit_code == 0U ? 0 : 1;
}

int notify_android_device_identified() {
    const auto adb = find_adb();
    if (adb.empty()) {
        return 1;
    }
    const auto query = run_capture_bounded(adb, {L"devices"});
    const auto devices = query.started && query.exit_code == 0U
        ? connected_devices(query.output)
        : std::vector<std::wstring>{};
    if (devices.size() != 1U) {
        return 1;
    }
    const auto result = run_capture_bounded(adb, {L"-s", devices.front(), L"shell", L"am", L"broadcast",
        L"-a", L"com.wireview.receiver.action.DEVICE_IDENTIFIED"});
    return result.started && result.exit_code == 0U ? 0 : 1;
}

int notify_android_vdd_created(wchar_t** argv) {
    const auto adb = find_adb();
    if (adb.empty()) {
        return 1;
    }
    const auto query = run_capture_bounded(adb, {L"devices"});
    const auto devices = query.started && query.exit_code == 0U
        ? connected_devices(query.output)
        : std::vector<std::wstring>{};
    if (devices.size() != 1U) {
        return 1;
    }
    const auto result = run_capture_bounded(adb, {L"-s", devices.front(), L"shell", L"am", L"broadcast",
        L"-a", L"com.wireview.receiver.action.VDD_CREATED",
        L"--ei", L"stream_width", argv[2],
        L"--ei", L"stream_height", argv[3],
        L"--ei", L"vdd_refresh", argv[4]});
    return result.started && result.exit_code == 0U ? 0 : 1;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    int argc{};
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv == nullptr) {
        return 1;
    }
    const bool detect_device = (argc == 2 || argc == 3) &&
        std::wstring_view(argv[1]) == L"--detect-device";
    const bool require_wireview_app = detect_device && argc == 3 &&
        std::wstring_view(argv[2]) == L"--require-wireview-app";
    const bool notify_stop = argc == 2 && std::wstring_view(argv[1]) == L"--notify-stop";
    const bool notify_host_exited = argc == 2 && std::wstring_view(argv[1]) == L"--notify-host-exited";
    const bool notify_identified = argc == 2 && std::wstring_view(argv[1]) == L"--notify-device-identified";
    const bool notify_vdd = argc == 5 && std::wstring_view(argv[1]) == L"--notify-vdd-created";
    const bool notify_pairing = argc == 3 && std::wstring_view(argv[1]) == L"--notify-pairing-approved";
    Options options;
    bool valid = false;
    try {
        valid = !detect_device && !notify_stop && !notify_host_exited && !notify_identified &&
            !notify_vdd && !notify_pairing && parse_arguments(argc, argv, &options);
    } catch (const std::exception& error) {
        // Argument validation (for example IPv6 WiFi endpoints) reports
        // specific reasons by exception; surface them through the status
        // file when it was already parsed, and never let them escape
        // wWinMain as a silent std::terminate.
        OutputDebugStringA((std::string("WireView session: ") + error.what() + "\n").c_str());
        if (!options.status_file.empty()) {
            write_status(options, L"error", widen_ascii(error.what()), 0U);
        }
        LocalFree(argv);
        return 1;
    }
    if (detect_device) {
        LocalFree(argv);
        return require_wireview_app || argc == 2 ? detect_single_adb_device(require_wireview_app) : 1;
    }
    if (notify_pairing) {
        const int result = notify_pairing_approved(argv);
        LocalFree(argv);
        return result;
    }
    if (notify_stop) {
        LocalFree(argv);
        return notify_android_stop();
    }
    if (notify_host_exited) {
        LocalFree(argv);
        return notify_android_host_exited();
    }
    if (notify_identified) {
        LocalFree(argv);
        return notify_android_device_identified();
    }
    if (notify_vdd) {
        // The VDD notification consumes argv[2..4], so it must run before
        // CommandLineToArgvW's buffer is released.
        const int result = notify_android_vdd_created(argv);
        LocalFree(argv);
        return result;
    }
    LocalFree(argv);
    return valid ? run(options) : 1;
}
