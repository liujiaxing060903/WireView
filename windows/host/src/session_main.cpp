#include <winsock2.h>
#include <windows.h>

#include "adb_diagnostics.hpp"
#include "process_identity.hpp"

#include <algorithm>
#include <chrono>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <optional>
#include <regex>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace {

using wireview::host::AdbStatus;
using wireview::host::adb_status_message;

constexpr std::wstring_view kPackageName = L"com.wireview.receiver";
constexpr std::wstring_view kActivityName = L"com.wireview.receiver/.MainActivity";
constexpr std::wstring_view kReversePort = L"tcp:39871";
constexpr std::uint32_t kUnlimitedSessionSeconds = 2'147'483'647U;

struct Options {
    std::filesystem::path status_file;
    std::filesystem::path adb_session_file;
    // Host 的标准输出会在会话仍运行时持续镜像到此文件。控制中心即使强制停止
    // 整个 Job，也能保留停止前已经产生的 GPU、编码器和传输诊断。
    std::filesystem::path host_diagnostics_file;
    // A practical "until stopped" duration.  Keeping this a positive value
    // preserves the Host's existing CLI contract for zero-duration test paths.
    std::uint32_t duration_seconds{kUnlimitedSessionSeconds};
    std::uint16_t width{};
    std::uint16_t height{};
    std::uint16_t frame_rate{60U};
    std::uint16_t tcp_port{39871U};
    unsigned cursor_size_percent{100U};
    std::wstring display;
    std::wstring serial;
    bool follow_primary{false};
    bool enable_input{false};
    bool device_preview{false};
    bool device_profile_probe{false};
};

struct ProcessResult {
    DWORD exit_code{ERROR_GEN_FAILURE};
    std::wstring output;
    bool started{false};
};

std::wstring quote_argument(const std::wstring& value) {
    std::wstring quoted{L"\""};
    for (const wchar_t character : value) {
        if (character == L'\"') {
            quoted += L"\\\"";
        } else {
            quoted += character;
        }
    }
    quoted += L"\"";
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
    wchar_t path_buffer[32'768U]{};
    const DWORD path_length = SearchPathW(nullptr, L"adb.exe", nullptr,
        static_cast<DWORD>(std::size(path_buffer)), path_buffer, nullptr);
    if (path_length > 0U && path_length < std::size(path_buffer)) {
        candidates.emplace_back(path_buffer);
    }
    std::filesystem::path incomplete;
    for (const auto& candidate : candidates) {
        std::error_code error;
        if (!std::filesystem::is_regular_file(candidate, error)) { continue; }
        if (wireview::host::adb_dependencies_complete(candidate)) { return candidate; }
        if (incomplete.empty()) { incomplete = candidate; }
    }
    // Report the actual broken path if no complete installation exists. Never
    // launch an incomplete local copy just because it was found before the SDK.
    return incomplete;
}

void write_status(const Options& options, const std::wstring& state,
    const std::wstring& message, std::uint32_t reconnect_count,
    const std::wstring& diagnostics = {}, const std::wstring& display_info = {}) {
    if (options.status_file.empty()) {
        return;
    }
    std::error_code error;
    std::filesystem::create_directories(options.status_file.parent_path(), error);
    const auto temporary = options.status_file.wstring() + L".tmp";
    std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
    if (!stream) {
        return;
    }
    const std::wstring status = L"version=1\nstate=" + state + L"\nmessage=" + message +
        L"\ninput_mode=" + (options.enable_input ? std::wstring(L"enabled") : std::wstring(L"log-only")) +
        L"\ndisplay=" + (options.follow_primary ? L"primary display" : options.display) +
        L"\nrequested_width=" + std::to_wstring(options.width) +
        L"\nrequested_height=" + std::to_wstring(options.height) +
        L"\nrequested_fps=" + std::to_wstring(options.frame_rate) +
        L"\ntransport=adb" +
        L"\nreconnect_count=" + std::to_wstring(reconnect_count) +
        L"\ndiagnostics=" + diagnostics + L"\n";
    stream << to_utf8(status + display_info);
    stream.close();
    std::filesystem::rename(temporary, options.status_file, error);
    if (error) {
        std::filesystem::remove(options.status_file, error);
        error.clear();
        std::filesystem::rename(temporary, options.status_file, error);
    }
}

ProcessResult run_capture(const std::filesystem::path& executable,
    const std::vector<std::wstring>& arguments, const std::filesystem::path& working_directory = {},
    const std::filesystem::path& live_output_file = {}, DWORD timeout_ms = 15'000U) {
    if (executable.filename() == L"adb.exe" &&
        !wireview::host::adb_dependencies_complete(executable)) {
        return {ERROR_MOD_NOT_FOUND, adb_status_message(AdbStatus::incomplete), false};
    }
    SECURITY_ATTRIBUTES security{};
    security.nLength = sizeof(security);
    security.bInheritHandle = TRUE;
    HANDLE read_pipe{};
    HANDLE write_pipe{};
    if (!CreatePipe(&read_pipe, &write_pipe, &security, 0U)) {
        const DWORD error = GetLastError();
        return {error, L"CreatePipe 系统错误=" + std::to_wstring(error), false};
    }
    SetHandleInformation(read_pipe, HANDLE_FLAG_INHERIT, 0U);
    std::wstring command = quote_argument(executable.wstring());
    for (const auto& argument : arguments) {
        command += L" ";
        command += quote_argument(argument);
    }
    std::vector<wchar_t> command_buffer(command.begin(), command.end());
    command_buffer.push_back(L'\0');
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdOutput = write_pipe;
    startup.hStdError = write_pipe;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION process{};
    const std::wstring directory = working_directory.empty() ? L"" : working_directory.wstring();
    const BOOL created = CreateProcessW(executable.c_str(), command_buffer.data(), nullptr, nullptr,
        TRUE, CREATE_NO_WINDOW, nullptr, directory.empty() ? nullptr : directory.c_str(), &startup, &process);
    const DWORD creation_error = created ? ERROR_SUCCESS : GetLastError();
    CloseHandle(write_pipe);
    if (!created) {
        CloseHandle(read_pipe);
        return {creation_error, L"CreateProcessW 系统错误=" + std::to_wstring(creation_error), false};
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
    const ULONGLONG started_at = GetTickCount64();
    bool timed_out = false;
    for (;;) {
        DWORD available{};
        if (!PeekNamedPipe(read_pipe, nullptr, 0U, nullptr, &available, nullptr)) { available = 0U; }
        if (available != 0U) {
            if (!ReadFile(read_pipe, buffer, sizeof(buffer), &read, nullptr) || read == 0U) { break; }
            bytes.append(buffer, read);
            // Preserve the startup and latest failure without retaining days
            // of Host output in RAM. The live diagnostic file stays complete.
            if (bytes.size() > 1'048'576U) { bytes.erase(32'768U, 524'288U); }
            if (live_output) {
                live_output.write(buffer, static_cast<std::streamsize>(read));
                live_output.flush();
            }
        } else if (WaitForSingleObject(process.hProcess, 10U) == WAIT_OBJECT_0) {
            // An ADB daemon can inherit the pipe. Do not wait forever for that
            // unrelated long-lived process to close the last writer.
            if (!PeekNamedPipe(read_pipe, nullptr, 0U, nullptr, &available, nullptr) || available == 0U) {
                break;
            }
        }
        if (timeout_ms != INFINITE && GetTickCount64() - started_at >= timeout_ms) {
            timed_out = true;
            TerminateProcess(process.hProcess, WAIT_TIMEOUT);
            break;
        }
    }
    live_output.close();
    CloseHandle(read_pipe);
    const ULONGLONG elapsed = GetTickCount64() - started_at;
    const DWORD remaining = timeout_ms == INFINITE ? INFINITE :
        (elapsed >= timeout_ms ? 0U : timeout_ms - static_cast<DWORD>(elapsed));
    if (WaitForSingleObject(process.hProcess, remaining) == WAIT_TIMEOUT) {
        timed_out = true;
        TerminateProcess(process.hProcess, WAIT_TIMEOUT);
        WaitForSingleObject(process.hProcess, 5'000U);
    }
    ProcessResult result{};
    result.started = true;
    GetExitCodeProcess(process.hProcess, &result.exit_code);
    if (timed_out) {
        result.exit_code = WAIT_TIMEOUT;
        bytes += "\nADB command timed out; child stopped (global ADB server untouched).";
    }
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

AdbStatus receiver_package_status(const ProcessResult& package) {
    if (!package.started || package.exit_code > 1U) {
        return AdbStatus::command_failed;
    }
    if (package.exit_code == 0U && package.output.find(L"package:") != std::wstring::npos) {
        return AdbStatus::ready;
    }
    const bool empty_output = std::all_of(package.output.begin(), package.output.end(),
        [](wchar_t character) { return iswspace(character) != 0; });
    return empty_output ? AdbStatus::app_missing : AdbStatus::command_failed;
}

std::vector<std::wstring> connected_devices(const std::wstring& output, std::wstring_view serial = {}) {
    const auto parsed = wireview::host::parse_adb_devices(output, serial);
    return parsed.status == AdbStatus::ready ? parsed.ready : std::vector<std::wstring>{};
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
        result = result * 10U + static_cast<std::uint32_t>(character - L'0');
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
        } else if (argument == L"--adb-session-file" && require_value(&value)) {
            options->adb_session_file = value;
        } else if (argument == L"--duration-seconds" && require_value(&value) && parse_unsigned(value, &number)) {
            options->duration_seconds = number;
        } else if (argument == L"--width" && require_value(&value) && parse_unsigned(value, &number) && number <= UINT16_MAX) {
            options->width = static_cast<std::uint16_t>(number);
        } else if (argument == L"--height" && require_value(&value) && parse_unsigned(value, &number) && number <= UINT16_MAX) {
            options->height = static_cast<std::uint16_t>(number);
        } else if (argument == L"--frame-rate" && require_value(&value) && parse_unsigned(value, &number) && number <= UINT16_MAX) {
            options->frame_rate = static_cast<std::uint16_t>(number);
        } else if (argument == L"--cursor-size-percent" && require_value(&value) && parse_unsigned(value, &number) && number >= 50U && number <= 300U) {
            options->cursor_size_percent = number;
        } else if (argument == L"--display" && require_value(&value)) {
            options->display = value;
        } else if (argument == L"--follow-primary") {
            options->follow_primary = true;
        } else if (argument == L"--serial" && require_value(&value) &&
            options->serial.empty() && wireview::host::valid_adb_serial(value)) {
            options->serial = value;
        } else if (argument == L"--enable-input") {
            options->enable_input = true;
        } else if (argument == L"--device-preview") {
            options->device_preview = true;
        } else if (argument == L"--device-profile-probe") {
            options->device_profile_probe = true;
        } else {
            return false;
        }
    }
    if (options->follow_primary &&
        (!options->display.empty() || options->device_preview || options->device_profile_probe)) {
        return false;
    }
    return !options->status_file.empty() && (options->device_preview || options->device_profile_probe ||
        options->follow_primary || !options->display.empty());
}

ProcessResult run_host(const Options& options, const std::filesystem::path& host,
    std::uint32_t session_seconds, std::uint32_t reconnect_count) {
    std::vector<std::wstring> arguments;
    if (options.device_profile_probe) {
        arguments.push_back(L"--probe-device-profile");
    } else if (options.device_preview) {
        arguments.insert(arguments.end(), {L"--device-preview", std::to_wstring(session_seconds)});
    } else {
        arguments.insert(arguments.end(), {L"--stream-desktop", std::to_wstring(session_seconds)});
        if (!options.follow_primary) {
            arguments.insert(arguments.end(), {L"--display", options.display});
        }
    }
    arguments.insert(arguments.end(), {L"--tcp-port", std::to_wstring(options.tcp_port)});
    if (!options.device_profile_probe) {
        if (options.width != 0U && options.height != 0U) {
            arguments.insert(arguments.end(), {L"--size", std::to_wstring(options.width) + L"x" + std::to_wstring(options.height)});
        }
        arguments.insert(arguments.end(), {L"--frame-rate", std::to_wstring(options.frame_rate)});
        if (!options.device_preview) {
            arguments.insert(arguments.end(), {L"--transport", L"adb"});
        }
    }
    if (!options.enable_input && !options.device_preview && !options.device_profile_probe) {
        arguments.push_back(L"--input-log-only");
    }
    if (!options.device_preview && !options.device_profile_probe) {
        arguments.insert(arguments.end(), {L"--cursor-size-percent", std::to_wstring(options.cursor_size_percent)});
    }
    arguments.insert(arguments.end(), {L"--status-file", options.status_file.wstring()});
    if (!options.device_profile_probe && !options.device_preview) {
        arguments.insert(arguments.end(), {L"--reconnect-count", std::to_wstring(reconnect_count)});
    }
    return run_capture(host, arguments, host.parent_path(), options.host_diagnostics_file, INFINITE);
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

std::wstring host_failure_message(const ProcessResult& result) {
    const std::wstring detail = last_output_line(result.output);
    if (detail.find(L"No WireView client connected before the timeout") != std::wstring::npos) {
        return L"Android 端 WireView 未在 5 分钟内连接到 Windows Host；请保持应用在前台，并检查 USB 调试和 ADB reverse。";
    }
    if (detail.find(L"client did not return HELLO_ACK") != std::wstring::npos) {
        return L"Android 端已建立 TCP 连接，但未完成 WireView 握手；请检查 Android 应用是否崩溃、被省电策略限制或版本不匹配。";
    }
    if (detail.find(L"requires BGRA desktop frames") != std::wstring::npos) {
        return L"Windows 捕获画面不是当前支持的 8-bit BGRA 格式。请关闭目标屏的 HDR/10-bit，切换 SDR 后重试；这不是 USB 授权失败。原始诊断：" + detail;
    }
    if (detail.empty()) {
        return L"Windows Host 异常退出（退出代码 " + std::to_wstring(result.exit_code) + L"），未输出诊断信息。";
    }
    return L"Windows Host 启动/运行失败（退出代码 " +
        std::to_wstring(result.exit_code) + L"）：" + detail;
}

std::uint16_t choose_session_port(const std::filesystem::path& adb,
    const std::vector<wireview::host::AdbDevice>& devices, std::wstring& failure) {
    // Old Android clients keep retrying their reverse mapping. Never reuse a
    // destination still mapped by an attached device, including after a crash.
    std::vector<std::wstring> excluded{std::wstring(kReversePort)};
    for (const auto& device : devices) {
        if (device.state != L"device") { continue; }
        const auto mappings = run_capture(adb, {L"-s", device.serial, L"reverse", L"--list"});
        if (!mappings.started || mappings.exit_code != 0U) {
            failure = L"无法读取设备 " + device.serial + L" 的 ADB 端口映射；为避免投错设备已停止启动。 exit=" +
                std::to_wstring(mappings.exit_code) + L"\n" + mappings.output;
            return 0U;
        }
        std::wistringstream fields(mappings.output);
        std::wstring field;
        while (fields >> field) {
            if (field.starts_with(L"tcp:")) { excluded.push_back(field); }
        }
    }
    WSADATA data{};
    const int initialized = WSAStartup(MAKEWORD(2, 2), &data);
    if (initialized != 0) { failure = L"WSAStartup error=" + std::to_wstring(initialized); return 0U; }
    std::uint16_t chosen{};
    int socket_error{};
    for (int attempt = 0; attempt < 32 && chosen == 0U; ++attempt) {
        const SOCKET socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (socket == INVALID_SOCKET) { socket_error = WSAGetLastError(); break; }
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        int length = sizeof(address);
        if (bind(socket, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0 &&
            getsockname(socket, reinterpret_cast<sockaddr*>(&address), &length) == 0) {
            const auto port = ntohs(address.sin_port);
            if (std::find(excluded.begin(), excluded.end(), L"tcp:" + std::to_wstring(port)) == excluded.end()) {
                chosen = port;
            }
        } else { socket_error = WSAGetLastError(); }
        closesocket(socket);
    }
    WSACleanup();
    if (chosen == 0U) { failure = L"无法分配独立会话端口。socket error=" + std::to_wstring(socket_error); }
    // The Host must still bind this port itself; if another process wins the
    // interval, its real bind error is propagated, never a fallback to 39871.
    return chosen;
}

// One record per native session, written BEFORE reverse is changed. The control
// center can reclaim it outside the killed Job; PID + creation time prevents
// treating a reused PID as the old owner. Never scan/delete unknown mappings.
struct AdbSessionRecord {
    DWORD owner{};
    std::uint64_t created{};
    unsigned port{};
    std::wstring serial;
};

class AdbDeviceLock {
public:
    explicit AdbDeviceLock(std::wstring_view serial) {
        std::uint64_t hash = 14695981039346656037ULL;
        for (const auto c : serial) { hash = (hash ^ static_cast<unsigned>(c)) * 1099511628211ULL; }
        const auto name = L"Local\\WireViewAdbSession-" + std::to_wstring(hash);
        handle_ = CreateMutexW(nullptr, FALSE, name.c_str());
        if (handle_ != nullptr) {
            const auto waited = WaitForSingleObject(handle_, 5'000U);
            acquired_ = waited == WAIT_OBJECT_0 || waited == WAIT_ABANDONED;
        }
    }
    ~AdbDeviceLock() { if (handle_) { if (acquired_) { ReleaseMutex(handle_); } CloseHandle(handle_); } }
    bool acquired() const { return acquired_; }
    AdbDeviceLock(const AdbDeviceLock&) = delete;
    AdbDeviceLock& operator=(const AdbDeviceLock&) = delete;
private:
    HANDLE handle_{};
    bool acquired_{};
};

bool read_adb_session(const std::filesystem::path& path, AdbSessionRecord& record) {
    std::ifstream input(path);
    std::string magic, serial, extra;
    if (!(input >> magic >> record.owner >> record.created >> record.port >> serial) ||
        magic != "WIREVIEW_ADB_SESSION_V1" || record.owner == 0U || record.created == 0U ||
        record.port == 0U || record.port > 65535U || record.port == 39871U || (input >> extra)) { return false; }
    record.serial.assign(serial.begin(), serial.end());
    return wireview::host::valid_adb_serial(record.serial);
}

bool write_adb_session(const std::filesystem::path& path, const AdbSessionRecord& record) {
    const auto temporary = path.wstring() + L".tmp";
    std::ofstream output(temporary, std::ios::trunc);
    output << "WIREVIEW_ADB_SESSION_V1 " << record.owner << ' ' << record.created << ' '
        << record.port << ' ' << to_utf8(record.serial) << '\n';
    output.close();
    return output.good() && MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_WRITE_THROUGH);
}

bool remove_owned_adb_mapping(const std::filesystem::path& path,
    const std::filesystem::path& adb, const AdbSessionRecord& record, std::wstring& failure,
    bool* replaced = nullptr) {
    const auto listed = run_capture(adb, {L"-s", record.serial, L"reverse", L"--list"});
    if (!listed.started || listed.exit_code != 0U) {
        failure = L"读取 ADB 映射失败，保留本次记录。 exit=" + std::to_wstring(listed.exit_code) + L"\n" + listed.output;
        return false;
    }
    std::wistringstream lines(listed.output);
    std::wstring line;
    bool owned = false;
    while (std::getline(lines, line)) {
        std::wistringstream fields(line);
        std::wstring transport, source, destination;
        if (fields >> transport >> source >> destination && source == kReversePort) {
            if (destination == L"tcp:" + std::to_wstring(record.port)) { owned = true; }
            else if (replaced != nullptr) { *replaced = true; }
        }
    }
    if (owned) {
        const auto removed = run_capture(adb, {L"-s", record.serial, L"reverse", L"--remove", std::wstring(kReversePort)});
        if (!removed.started || removed.exit_code != 0U) {
            failure = L"移除本次 ADB 映射失败，保留记录。 exit=" + std::to_wstring(removed.exit_code) + L"\n" + removed.output;
            return false;
        }
    }
    // Absent/replaced mappings are not ours. In particular never remove-all.
    std::error_code error;
    std::filesystem::remove(path, error);
    if (error) { failure = L"ADB 映射已释放，但无法删除本次记录：" + path.wstring(); return false; }
    return true;
}

bool notify_stopped_device(const std::filesystem::path& adb, const std::wstring& serial, std::wstring& failure) {
    const auto result = run_capture(adb, {L"-s", serial, L"shell", L"am", L"broadcast", L"-a",
        L"com.wireview.receiver.action.WIREVIEW_STOPPED"});
    if (result.started && result.exit_code == 0U) { return true; }
    failure = L"通知设备停止失败。 exit=" + std::to_wstring(result.exit_code) + L"\n" + result.output;
    return false;
}

int cleanup_adb_session(const std::filesystem::path& path, const Options& diagnostics,
    const std::wstring& stop_serial = {}) {
    std::error_code error;
    const bool missing = !std::filesystem::exists(path, error) && !error;
    if (missing && stop_serial.empty()) { return 0; }
    AdbSessionRecord record;
    std::wstring failure;
    if (missing) {
        AdbDeviceLock lock(stop_serial);
        const auto adb = find_adb();
        if (!lock.acquired()) { failure = L"设备仍被另一会话使用，未发送旧停止通知。"; }
        else if (adb.empty() || !wireview::host::adb_dependencies_complete(adb)) { failure = L"ADB 依赖缺失，无法通知设备停止。"; }
        else if (notify_stopped_device(adb, stop_serial, failure)) { return 0; }
    }
    else if (!read_adb_session(path, record) || (!stop_serial.empty() && record.serial != stop_serial)) {
        failure = L"ADB 会话记录无效或设备不匹配，未移除任何映射。";
    }
    else {
        // TerminateJobObject is asynchronous. Wait briefly for only the recorded
        // process; never kill the shared ADB server or an unrelated process.
        const auto until = GetTickCount64() + 3'000U;
        while (wireview::host::recorded_process_is_alive(record.owner, record.created) && GetTickCount64() < until) { Sleep(20U); }
        if (wireview::host::recorded_process_is_alive(record.owner, record.created)) {
            failure = L"ADB 会话进程仍在运行，未移除映射。";
        } else {
            AdbDeviceLock lock(record.serial);
            AdbSessionRecord current;
            if (!lock.acquired()) { failure = L"设备仍被另一会话使用，未移除映射。"; }
            else if (!read_adb_session(path, current) || current.owner != record.owner ||
                current.created != record.created || current.port != record.port || current.serial != record.serial) {
                failure = L"ADB 会话记录已变化，未移除映射。";
            } else {
                const auto adb = find_adb();
                bool replaced = false;
                if (adb.empty() || !wireview::host::adb_dependencies_complete(adb)) { failure = L"ADB 依赖缺失，无法清理本次映射。"; }
                else if (remove_owned_adb_mapping(path, adb, record, failure, &replaced) &&
                    (stop_serial.empty() || replaced || notify_stopped_device(adb, stop_serial, failure))) { return 0; }
            }
        }
    }
    write_status(diagnostics, L"error", L"上次连接未清理完成，请检查设备连接后再试。", 0U, compact_host_diagnostics(failure));
    return 9;
}

int run(const Options& options) {
    const auto host = current_executable().parent_path() / L"wireview_host.exe";
    if (!std::filesystem::is_regular_file(host)) {
        write_status(options, L"error", L"未找到 wireview_host.exe；请重新解压完整的 WireView 发布包。", 0U);
        return 2;
    }
    const auto deadline = std::chrono::steady_clock::now() +
        std::chrono::seconds(options.duration_seconds);
    std::uint32_t reconnect_count{};
    while (std::chrono::steady_clock::now() < deadline) {
        const auto adb = find_adb();
        if (adb.empty()) {
            write_status(options, L"error", adb_status_message(AdbStatus::missing), reconnect_count);
            return 2;
        }
        if (!wireview::host::adb_dependencies_complete(adb)) {
            write_status(options, L"error", adb_status_message(AdbStatus::incomplete), reconnect_count,
                L"adb=" + adb.wstring());
            return 7;
        }
        const auto device_query = run_capture(adb, {L"devices"});
        if (!device_query.started || device_query.exit_code != 0U) {
            write_status(options, L"error", adb_status_message(AdbStatus::command_failed), reconnect_count,
                compact_host_diagnostics(L"adb=" + adb.wstring() + L"; exit=" +
                    std::to_wstring(device_query.exit_code) + L"\n" + device_query.output));
            return 9;
        }
        const auto parsed = wireview::host::parse_adb_devices(device_query.output, options.serial);
        if (parsed.status != AdbStatus::ready) {
            write_status(options, parsed.status == AdbStatus::unauthorized
                ? L"waiting_authorization" : L"waiting_device",
                adb_status_message(parsed.status), reconnect_count,
                compact_host_diagnostics(device_query.output));
            Sleep(1000U);
            continue;
        }
        const auto& serial = parsed.ready.front();
        AdbDeviceLock device_lock(serial);
        if (!device_lock.acquired()) {
            write_status(options, L"error", L"这台设备仍有会话运行或正在清理，请稍后再试。", reconnect_count);
            return 9;
        }
        const auto package = run_capture(adb, {L"-s", serial, L"shell", L"pm", L"path", std::wstring(kPackageName)});
        const auto package_status = receiver_package_status(package);
        if (package_status != AdbStatus::ready) {
            const bool missing = package_status == AdbStatus::app_missing;
            write_status(options, missing ? L"app_missing" : L"error", adb_status_message(package_status),
                reconnect_count, compact_host_diagnostics(package.output));
            return missing ? 3 : 9;
        }
        Options host_options = options;
        std::wstring port_failure;
        host_options.tcp_port = choose_session_port(adb, parsed.devices, port_failure);
        if (host_options.tcp_port == 0U) {
            write_status(options, L"error", L"无法建立独立的 ADB 会话端口。", reconnect_count, compact_host_diagnostics(port_failure));
            return 9;
        }
        const auto record_path = options.adb_session_file.empty()
            ? std::filesystem::path(options.status_file.wstring() + L".adb-session") : options.adb_session_file;
        const AdbSessionRecord record{GetCurrentProcessId(),
            wireview::host::process_creation_time(GetCurrentProcess()), host_options.tcp_port, serial};
        if (record.created == 0U || !write_adb_session(record_path, record)) {
            write_status(options, L"error", L"无法保存本次 ADB 连接记录，已停止启动。", reconnect_count);
            return 9;
        }
        const auto finish = [&](int exit_code) {
            std::wstring failure;
            if (remove_owned_adb_mapping(record_path, adb, record, failure)) { return exit_code; }
            write_status(options, L"error", L"本次 ADB 连接清理失败；请检查设备连接后重试。", reconnect_count,
                compact_host_diagnostics(failure));
            return 9;
        };
        const auto reverse = run_capture(adb, {L"-s", serial, L"reverse", std::wstring(kReversePort),
            L"tcp:" + std::to_wstring(host_options.tcp_port)});
        if (!reverse.started || reverse.exit_code != 0U) {
            write_status(options, L"error", L"ADB reverse 端口映射失败。请检查设备连接和 ADB 服务；已停止自动重试，修正后重新启动。", reconnect_count,
                compact_host_diagnostics(reverse.output));
            return finish(9);
        }
        const auto launch = run_capture(adb, {L"-s", serial, L"shell", L"am", L"start", L"-n", std::wstring(kActivityName)});
        if (!launch.started || launch.exit_code != 0U) {
            write_status(options, L"error", L"ADB 无法启动 Android 应用。请先在设备上手动打开 WireView，查看系统安装/后台启动限制后重试。", reconnect_count,
                compact_host_diagnostics(launch.output));
            return finish(9);
        }
        Sleep(800U);
        write_status(options, L"starting", L"Android 已准备完成，正在启动电脑端镜像服务。", reconnect_count);
        const auto remaining = std::chrono::duration_cast<std::chrono::seconds>(
            deadline - std::chrono::steady_clock::now()).count();
        const auto session_seconds = static_cast<std::uint32_t>(
            std::max<std::int64_t>(1, remaining));
        const ProcessResult host_result = run_host(host_options, host, session_seconds, reconnect_count);
        if (options.device_profile_probe) {
            return finish(host_result.exit_code == 0U ? 0 : 4);
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            if (finish(0) != 0) { return 9; }
            break;
        }
        // 会话一旦断开即结束。不要在设备端被用户划掉后台后再次通过 ADB 拉起应用，
        // 也不要把拔线后的会话伪装成仍可继续的自动重连。
        write_status(options, L"error", host_failure_message(host_result), reconnect_count,
            compact_host_diagnostics(host_result.output));
        return finish(host_result.exit_code == 0U ? 0 : static_cast<int>(host_result.exit_code));
    }
    write_status(options, L"stopped", L"WireView 会话已结束。", reconnect_count);
    return 0;
}

std::wstring detect_android_display(const std::filesystem::path& adb, std::wstring_view serial) {
    const auto result = run_capture(adb, {L"-s", std::wstring(serial), L"shell", L"dumpsys", L"display"},
        {}, {}, 1500U);
    if (!result.started || result.exit_code != 0U) {
        return {};
    }
    std::wistringstream lines(result.output);
    std::wstring base, override_info, line;
    const std::wregex default_display(LR"(\bdisplayId 0\b|uniqueId "local:0".*\blayerStack 0\b)");
    while (std::getline(lines, line)) {
        if (!std::regex_search(line, default_display)) { continue; }
        if (line.find(L"mBaseDisplayInfo=DisplayInfo{") != std::wstring::npos) { base = line; }
        if (line.find(L"mOverrideDisplayInfo=DisplayInfo{") != std::wstring::npos) { override_info = line; }
    }
    std::wsmatch mode_id, rotation;
    const auto& orientation = override_info.empty() ? base : override_info;
    if (!std::regex_search(base, mode_id, std::wregex(LR"(, mode (\d+)\b)")) ||
        !std::regex_search(orientation, rotation, std::wregex(LR"(, rotation ([0-3])\b)"))) {
        return {};
    }
    const std::wregex mode_pattern(LR"(\{id=(\d+), width=(\d+), height=(\d+), fps=([0-9.]+))");
    const std::wsregex_iterator end;
    std::wsregex_iterator active = end;
    for (std::wsregex_iterator mode(base.begin(), base.end(), mode_pattern); mode != end; ++mode) {
        if ((*mode)[1] == mode_id[1]) { active = mode; break; }
    }
    if (active == end) { return {}; }
    std::wstring width = (*active)[2], height = (*active)[3];
    double max_refresh = std::wcstod((*active)[4].str().c_str(), nullptr);
    for (std::wsregex_iterator mode(base.begin(), base.end(), mode_pattern); mode != end; ++mode) {
        if ((*mode)[2] == width && (*mode)[3] == height) {
            max_refresh = (std::max)(max_refresh, std::wcstod((*mode)[4].str().c_str(), nullptr));
        }
    }
    if (rotation[1] == L"1" || rotation[1] == L"3") { std::swap(width, height); }
    std::wsmatch render_rate;
    const auto refresh = std::regex_search(base, render_rate, std::wregex(LR"(, renderFrameRate ([0-9.]+))")) &&
            std::wcstod(render_rate[1].str().c_str(), nullptr) > 0.0
        ? render_rate[1].str() : (*active)[4].str();
    return L"android_width=" + width + L"\nandroid_height=" + height +
        L"\nandroid_refresh_hz=" + refresh +
        L"\nandroid_max_refresh_hz=" + std::to_wstring(max_refresh) + L"\n";
}

int detect_single_adb_device(bool require_wireview_app, const std::filesystem::path& diagnostic_file = {},
    std::wstring_view serial = {}, const std::filesystem::path& devices_file = {}) {
    const auto adb = find_adb();
    const auto report = [&](AdbStatus status, const std::wstring& detail = {}) {
        Options diagnostics;
        diagnostics.status_file = diagnostic_file;
        write_status(diagnostics, status == AdbStatus::ready ? L"ready" : L"adb_probe",
            adb_status_message(status), 0U,
            compact_host_diagnostics(L"adb=" + adb.wstring() + L"; serial=" + std::wstring(serial) + L"\n" + detail),
            status == AdbStatus::ready && !serial.empty() && !diagnostic_file.empty()
                ? detect_android_display(adb, serial) : std::wstring{});
        return static_cast<int>(status);
    };
    std::ofstream device_list;
    if (!devices_file.empty()) {
        device_list.open(devices_file, std::ios::binary | std::ios::trunc);
        if (!device_list) { return report(AdbStatus::command_failed, L"Cannot write device list"); }
    }
    if (adb.empty()) {
        return report(AdbStatus::missing);
    }
    if (!wireview::host::adb_dependencies_complete(adb)) { return report(AdbStatus::incomplete); }
    const auto query = run_capture(adb, {L"devices", L"-l"});
    if (!query.started || query.exit_code != 0U) {
        return report(AdbStatus::command_failed, L"exit=" + std::to_wstring(query.exit_code) + L"\n" + query.output);
    }
    if (device_list.is_open()) {
        device_list << to_utf8(query.output);
        device_list.close();
        if (!device_list) { return report(AdbStatus::command_failed, L"Cannot save device list"); }
    }
    const auto parsed = wireview::host::parse_adb_devices(query.output, serial);
    if (parsed.status != AdbStatus::ready) { return report(parsed.status, query.output); }
    const auto& devices = parsed.ready;
    if (!serial.empty() || require_wireview_app) {
        const auto package = run_capture(adb, {L"-s", devices.front(), L"shell", L"pm", L"path",
            std::wstring(kPackageName)}, {}, {}, 1500U);
        const auto status = receiver_package_status(package);
        if (status != AdbStatus::ready) { return report(status, package.output); }
    }
    if (!require_wireview_app) {
        return report(AdbStatus::ready);
    }
    const auto process = run_capture(adb, {L"-s", devices.front(), L"shell", L"pidof",
        std::wstring(kPackageName)});
    const bool process_output_empty = std::all_of(process.output.begin(), process.output.end(),
        [](wchar_t character) { return iswspace(character) != 0; });
    if (!process.started || process.exit_code > 1U ||
        (process.exit_code == 1U && !process_output_empty)) {
        return report(AdbStatus::command_failed, L"pidof exit=" + std::to_wstring(process.exit_code) + L"\n" + process.output);
    }
    const bool app_running = process.started && process.exit_code == 0U &&
        std::any_of(process.output.begin(), process.output.end(), [](wchar_t character) {
            return !iswspace(character);
        });
    if (!app_running) {
        return report(AdbStatus::app_not_running);
    }
    // “划掉后台”时 Android 进程可能短暂仍在，pidof 因而不能代表活动仍可用。
    // 若系统提供 resumed/top-resumed 行，则要求当前前台确实是 WireView；旧系统或
    // 厂商删减 dumpsys 输出时安全地退回到 pidof，避免错误拒绝正常设备。
    const auto activities = run_capture(adb, {L"-s", devices.front(), L"shell", L"dumpsys",
        L"activity", L"activities"});
    if (!activities.started || activities.exit_code != 0U) {
        return report(AdbStatus::ready, L"pidof confirmed running; foreground query unavailable; exit=" +
            std::to_wstring(activities.exit_code) + L"\n" + activities.output);
    }
    if (activities.output.find(L"mKeyguardShowing=true") != std::wstring::npos ||
        activities.output.find(L"mSleeping=true") != std::wstring::npos) {
        return report(AdbStatus::app_not_running, activities.output);
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
                return report(AdbStatus::ready, activities.output);
            }
        }
        if (end == std::wstring::npos) {
            break;
        }
        start = end + 1U;
    }
    return report(found_foreground_marker || activities.output.find(L"isSleeping=true") != std::wstring::npos
            ? AdbStatus::app_not_running : AdbStatus::ready,
        activities.output);
}

int notify_android_stop(std::wstring_view serial = {}) {
    const auto adb = find_adb();
    if (adb.empty()) {
        return 1;
    }
    const auto query = run_capture(adb, {L"devices"});
    const auto devices = query.started && query.exit_code == 0U
        ? connected_devices(query.output, serial)
        : std::vector<std::wstring>{};
    if (devices.size() != 1U) {
        return 1;
    }
    AdbDeviceLock lock(devices.front());
    if (!lock.acquired()) { return 1; }
    const auto result = run_capture(adb, {L"-s", devices.front(), L"shell", L"am", L"broadcast",
        L"-a", L"com.wireview.receiver.action.WIREVIEW_STOPPED"});
    return result.started && result.exit_code == 0U ? 0 : 1;
}

int notify_android_host_exited(std::wstring_view serial = {}) {
    const auto adb = find_adb();
    if (adb.empty()) {
        return 1;
    }
    const auto query = run_capture(adb, {L"devices"});
    const auto devices = query.started && query.exit_code == 0U
        ? connected_devices(query.output, serial)
        : std::vector<std::wstring>{};
    if (devices.size() != 1U) {
        return 1;
    }
    AdbDeviceLock lock(devices.front());
    if (!lock.acquired()) { return 1; }
    const auto result = run_capture(adb, {L"-s", devices.front(), L"shell", L"am", L"broadcast",
        L"-a", L"com.wireview.receiver.action.HOST_EXITED"});
    return result.started && result.exit_code == 0U ? 0 : 1;
}

int notify_android_device_identified(std::wstring_view serial = {}) {
    const auto adb = find_adb();
    if (adb.empty()) {
        return 1;
    }
    const auto query = run_capture(adb, {L"devices"});
    const auto devices = query.started && query.exit_code == 0U
        ? connected_devices(query.output, serial)
        : std::vector<std::wstring>{};
    if (devices.size() != 1U) {
        return 1;
    }
    AdbDeviceLock lock(devices.front());
    if (!lock.acquired()) { return 1; }
    const auto result = run_capture(adb, {L"-s", devices.front(), L"shell", L"am", L"broadcast",
        L"-a", L"com.wireview.receiver.action.DEVICE_IDENTIFIED"});
    return result.started && result.exit_code == 0U ? 0 : 1;
}

int notify_android_vdd_created(wchar_t** argv, std::wstring_view serial = {}) {
    const auto adb = find_adb();
    if (adb.empty()) {
        return 1;
    }
    const auto query = run_capture(adb, {L"devices"});
    const auto devices = query.started && query.exit_code == 0U
        ? connected_devices(query.output, serial)
        : std::vector<std::wstring>{};
    if (devices.size() != 1U) {
        return 1;
    }
    const auto result = run_capture(adb, {L"-s", devices.front(), L"shell", L"am", L"broadcast",
        L"-a", L"com.wireview.receiver.action.VDD_CREATED",
        L"--ei", L"stream_width", argv[2],
        L"--ei", L"stream_height", argv[3],
        L"--ei", L"vdd_refresh", argv[4]});
    return result.started && result.exit_code == 0U ? 0 : 1;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    // Missing/corrupt dependencies must return an error, never a Windows loader
    // dialog from each background probe. Child processes inherit this mode.
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    int argc{};
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv == nullptr) {
        return 1;
    }
    if ((argc == 5 || (argc == 7 && std::wstring_view(argv[5]) == L"--notify-stop" &&
        wireview::host::valid_adb_serial(argv[6]))) && std::wstring_view(argv[1]) == L"--cleanup-adb-session" &&
        std::wstring_view(argv[3]) == L"--status-file") {
        Options diagnostics;
        diagnostics.status_file = argv[4];
        const auto path = std::filesystem::path(argv[2]);
        const std::wstring stop_serial = argc == 7 ? argv[6] : L"";
        LocalFree(argv);
        return cleanup_adb_session(path, diagnostics, stop_serial);
    }
    // A target applies equally to detect, stream, preview and notifications.
    std::wstring serial;
    for (int index = 1; index < argc; ++index) {
        if (std::wstring_view(argv[index]) != L"--serial") { continue; }
        if (!serial.empty() || index + 1 >= argc ||
            !wireview::host::valid_adb_serial(argv[index + 1])) {
            LocalFree(argv);
            return 1;
        }
        serial = argv[index + 1];
        for (int next = index + 2; next < argc; ++next) { argv[next - 2] = argv[next]; }
        argc -= 2;
        --index;
    }
    const bool detect_device = argc >= 2 &&
        std::wstring_view(argv[1]) == L"--detect-device";
    bool require_wireview_app = false;
    bool valid_detection = true;
    std::filesystem::path diagnostic_file;
    std::filesystem::path devices_file;
    if (detect_device) {
        for (int index = 2; index < argc; ++index) {
            const std::wstring_view argument(argv[index]);
            if (argument == L"--require-wireview-app" && !require_wireview_app) { require_wireview_app = true; }
            else if (argument == L"--diagnostics-file" && index + 1 < argc && diagnostic_file.empty()) { diagnostic_file = argv[++index]; }
            else if (argument == L"--devices-file" && index + 1 < argc && devices_file.empty()) { devices_file = argv[++index]; }
            else { valid_detection = false; }
        }
    }
    const bool notify_stop = argc == 2 && std::wstring_view(argv[1]) == L"--notify-stop";
    const bool notify_host_exited = argc == 2 && std::wstring_view(argv[1]) == L"--notify-host-exited";
    const bool notify_identified = argc == 2 && std::wstring_view(argv[1]) == L"--notify-device-identified";
    const bool notify_vdd = argc == 5 && std::wstring_view(argv[1]) == L"--notify-vdd-created";
    Options options;
    options.serial = serial;
    const bool valid = !detect_device && !notify_stop && !notify_host_exited && !notify_identified && !notify_vdd && parse_arguments(argc, argv, &options);
    if (detect_device) {
        LocalFree(argv);
        return valid_detection ? detect_single_adb_device(require_wireview_app, diagnostic_file, serial, devices_file) : 1;
    }
    if (notify_stop) {
        LocalFree(argv);
        return notify_android_stop(serial);
    }
    if (notify_host_exited) {
        LocalFree(argv);
        return notify_android_host_exited(serial);
    }
    if (notify_identified) {
        LocalFree(argv);
        return notify_android_device_identified(serial);
    }
    if (notify_vdd) {
        // The VDD notification consumes argv[2..4], so it must run before
        // CommandLineToArgvW's buffer is released.
        const int result = notify_android_vdd_created(argv, serial);
        LocalFree(argv);
        return result;
    }
    LocalFree(argv);
    return valid ? run(options) : 1;
}
