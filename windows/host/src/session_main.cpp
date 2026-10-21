#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <optional>
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
    std::error_code error;
    std::filesystem::create_directories(options.status_file.parent_path(), error);
    const auto temporary = options.status_file.wstring() + L".tmp";
    std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
    if (!stream) {
        return;
    }
    const std::wstring status = L"version=1\nstate=" + state + L"\nmessage=" + message +
        L"\ninput_mode=" + (options.enable_input ? std::wstring(L"enabled") : std::wstring(L"log-only")) +
        L"\ndisplay=" + options.display +
        L"\nrequested_width=" + std::to_wstring(options.width) +
        L"\nrequested_height=" + std::to_wstring(options.height) +
        L"\nrequested_fps=" + std::to_wstring(options.frame_rate) +
        L"\ntransport=adb" +
        L"\nreconnect_count=" + std::to_wstring(reconnect_count) +
        L"\ndiagnostics=" + diagnostics + L"\n";
    stream << to_utf8(status);
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
        } else {
            return false;
        }
    }
    return !options->status_file.empty() &&
        (options->device_preview || options->device_profile_probe || !options->display.empty());
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
        arguments.push_back(L"--display");
        arguments.push_back(options.display);
    }
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
    arguments.insert(arguments.end(), {L"--status-file", options.status_file.wstring()});
    if (!options.device_profile_probe && !options.device_preview) {
        arguments.insert(arguments.end(), {L"--reconnect-count", std::to_wstring(reconnect_count)});
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

std::wstring host_failure_message(const ProcessResult& result) {
    const std::wstring detail = last_output_line(result.output);
    if (detail.find(L"No WireView client connected before the timeout") != std::wstring::npos) {
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
        const auto device_query = run_capture(adb, {L"devices"});
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
        const auto package = run_capture(adb, {L"-s", serial, L"shell", L"pm", L"path", std::wstring(kPackageName)});
        if (!package.started || package.exit_code != 0U || package.output.find(L"package:") == std::wstring::npos) {
            write_status(options, L"error", L"未检测到 WireView Android 应用；请先安装 APK。", reconnect_count);
            return 3;
        }
        const auto reverse = run_capture(adb, {L"-s", serial, L"reverse", std::wstring(kReversePort), std::wstring(kReversePort)});
        if (!reverse.started || reverse.exit_code != 0U) {
            write_status(options, L"waiting", L"ADB 端口映射失败，正在重试。", reconnect_count);
            Sleep(1000U);
            continue;
        }
        const auto launch = run_capture(adb, {L"-s", serial, L"shell", L"am", L"start", L"-n", std::wstring(kActivityName)});
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
        const ProcessResult host_result = run_host(options, host, session_seconds, reconnect_count);
        if (options.device_profile_probe) {
            return host_result.exit_code == 0U ? 0 : 4;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            break;
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
    const auto query = run_capture(adb, {L"devices"});
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
    const auto process = run_capture(adb, {L"-s", devices.front(), L"shell", L"pidof",
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
    const auto activities = run_capture(adb, {L"-s", devices.front(), L"shell", L"dumpsys",
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
    const auto query = run_capture(adb, {L"devices"});
    const auto devices = query.started && query.exit_code == 0U
        ? connected_devices(query.output)
        : std::vector<std::wstring>{};
    if (devices.size() != 1U) {
        return 1;
    }
    const auto result = run_capture(adb, {L"-s", devices.front(), L"shell", L"am", L"broadcast",
        L"-a", L"com.wireview.receiver.action.WIREVIEW_STOPPED"});
    return result.started && result.exit_code == 0U ? 0 : 1;
}

int notify_android_host_exited() {
    const auto adb = find_adb();
    if (adb.empty()) {
        return 1;
    }
    const auto query = run_capture(adb, {L"devices"});
    const auto devices = query.started && query.exit_code == 0U
        ? connected_devices(query.output)
        : std::vector<std::wstring>{};
    if (devices.size() != 1U) {
        return 1;
    }
    const auto result = run_capture(adb, {L"-s", devices.front(), L"shell", L"am", L"broadcast",
        L"-a", L"com.wireview.receiver.action.HOST_EXITED"});
    return result.started && result.exit_code == 0U ? 0 : 1;
}

int notify_android_device_identified() {
    const auto adb = find_adb();
    if (adb.empty()) {
        return 1;
    }
    const auto query = run_capture(adb, {L"devices"});
    const auto devices = query.started && query.exit_code == 0U
        ? connected_devices(query.output)
        : std::vector<std::wstring>{};
    if (devices.size() != 1U) {
        return 1;
    }
    const auto result = run_capture(adb, {L"-s", devices.front(), L"shell", L"am", L"broadcast",
        L"-a", L"com.wireview.receiver.action.DEVICE_IDENTIFIED"});
    return result.started && result.exit_code == 0U ? 0 : 1;
}

int notify_android_vdd_created(wchar_t** argv) {
    const auto adb = find_adb();
    if (adb.empty()) {
        return 1;
    }
    const auto query = run_capture(adb, {L"devices"});
    const auto devices = query.started && query.exit_code == 0U
        ? connected_devices(query.output)
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
    Options options;
    const bool valid = !detect_device && !notify_stop && !notify_host_exited && !notify_identified && !notify_vdd && parse_arguments(argc, argv, &options);
    if (detect_device) {
        LocalFree(argv);
        return require_wireview_app || argc == 2 ? detect_single_adb_device(require_wireview_app) : 1;
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
