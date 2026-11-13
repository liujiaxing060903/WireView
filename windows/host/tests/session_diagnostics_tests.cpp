// Exercise the real session entry points and child-process runner without a
// phone, an ADB server, or changing the user's SDK installation.
#include "../src/session_main.cpp"

#include <iostream>
#include <iomanip>
#include <stdexcept>

int wmain(int argc, wchar_t** argv) {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    if (const auto trace_path = environment_value(L"WIREVIEW_TEST_TRACE")) {
        std::ofstream trace(*trace_path, std::ios::app);
        trace << (current_executable().filename() == L"wireview_host.exe" ? "HOST" : "ADB");
        for (int index = 1; index < argc; ++index) { trace << ' ' << to_utf8(argv[index]); }
        trace << '\n';
    }
    if (argc >= 2 && std::wstring_view(argv[1]) == L"--probe-device-profile") {
        if (environment_value(L"WIREVIEW_TEST_HOST_HANG")) { Sleep(60'000U); }
        return 0;
    }
    if (argc >= 2 && std::wstring_view(argv[1]) == L"--stream-desktop" &&
        current_executable().filename() == L"wireview_host.exe") { return 0; }
    if (argc >= 2 && (std::wstring_view(argv[1]) == L"--status-file" ||
        std::wstring_view(argv[1]) == L"--cleanup-adb-session" ||
        std::wstring_view(argv[1]).starts_with(L"--notify-"))) {
        return wWinMain(nullptr, nullptr, nullptr, 0);
    }
    if (argc == 2 && std::wstring_view(argv[1]) == L"--hang-fixture") {
        Sleep(60'000U);
        return 0;
    }
    if (argc >= 2 && std::wstring_view(argv[1]) == L"--detect-device") {
        return wWinMain(nullptr, nullptr, nullptr, 0);
    }
    if (argc >= 2 && std::wstring_view(argv[1]) == L"devices") {
        const auto mode = environment_value(L"WIREVIEW_TEST_ADB_MODE").value_or(L"");
        if (mode == L"failed") { std::cout << "cannot connect to daemon\n"; return 1; }
        if (mode == L"daemon_ready") {
            std::cout << "adb server version (40) doesn't match this client (41); killing...\r\n"
                "* daemon not running; starting now at tcp:5037\r\n* daemon started successfully\r\n";
        }
        std::cout << "List of devices attached\n";
        const bool detailed = argc >= 3 && std::wstring_view(argv[2]) == L"-l";
        const auto row = [detailed](const char* serial, const char* state, const char* details = "") {
            // Match the real ADB observed on Windows: -l pads with spaces;
            // plain devices uses a tab. Keep the CRLF from the real output.
            if (detailed) { std::cout << std::left << std::setw(22) << serial << ' ' << state << details << "\r\n"; }
            else { std::cout << serial << '\t' << state << '\n'; }
        };
        if (mode == L"offline") { row("phone", "offline"); }
        if (mode == L"unauthorized") { row("phone", "unauthorized"); }
        if (mode == L"recovery") { row("phone", "recovery"); }
        if (mode == L"ready" || mode == L"daemon_ready" || mode.starts_with(L"pidof_") || mode.starts_with(L"foreground") || mode.starts_with(L"package_") || mode == L"background") {
            row("phone", "device");
        }
        if (mode == L"multiple") {
            row("phone", "device"); row("second", "unauthorized");
        }
        if (mode == L"two_ready") {
            row("phone", "device", " product:test model:First_Device device:first transport_id:106");
            row("second", "device", " product:test model:Second_Device device:second transport_id:108");
        }
        if (mode == L"mixed_offline") {
            row("phone", "device"); row("second", "offline");
        }
        return 0;
    }
    if (argc >= 4 && std::wstring_view(argv[1]) == L"-s" && std::wstring_view(argv[3]) == L"reverse") {
        const auto mapping = current_executable().parent_path() / (std::wstring(argv[2]) + L".mapping");
        const auto mode = environment_value(L"WIREVIEW_TEST_ADB_MODE").value_or(L"");
        if (mode == L"list_failed" && argc >= 5 && std::wstring_view(argv[4]) == L"--list") { return 1; }
        if (mode == L"remove_failed" && argc >= 5 && std::wstring_view(argv[4]) == L"--remove") { return 1; }
        if (argc >= 5 && std::wstring_view(argv[4]) == L"--list") {
            std::ifstream input(mapping);
            std::string destination;
            if (input >> destination) { std::cout << "UsbFfs tcp:39871 " << destination << '\n'; }
        } else if (argc == 6 && std::wstring_view(argv[4]) == L"--remove") {
            std::filesystem::remove(mapping);
        } else if (argc == 6) {
            std::ofstream(mapping) << to_utf8(argv[5]);
        }
        return 0;
    }
    if (argc >= 5 && std::wstring_view(argv[1]) == L"-s") {
        const auto mode = environment_value(L"WIREVIEW_TEST_ADB_MODE").value_or(L"");
        if (argc >= 8 && std::wstring_view(argv[5]) == L"broadcast" &&
            std::wstring_view(argv[7]) == L"com.wireview.receiver.action.WIREVIEW_STOPPED" &&
            environment_value(L"WIREVIEW_TEST_DELAY_STOP")) {
            Sleep(600U);
            if (const auto path = environment_value(L"WIREVIEW_TEST_TRACE")) {
                std::ofstream(*path, std::ios::app) << "STOP-DELIVERED\n";
            }
        }
        if (std::wstring_view(argv[4]) == L"pm") {
            if (mode == L"package_missing_legacy") { return 0; }
            if (mode == L"package_missing") { return 1; }
            if (mode == L"package_query_error") { std::cout << "error: device offline\n"; return 1; }
            std::cout << "package:/data/app/wireview.apk\n";
            return 0;
        }
        if (std::wstring_view(argv[4]) == L"pidof") {
            if (mode == L"pidof_transport") { std::cout << "error: device offline\n"; return 1; }
            if (mode == L"pidof_absent") { return 1; }
            std::cout << "123\n";
            return 0;
        }
        if (mode == L"foreground_fail") { std::cout << "dumpsys permission denied\n"; return 1; }
        std::cout << (mode == L"background" ? "topResumedActivity: other.app/.Main\n" :
            "topResumedActivity: com.wireview.receiver/.MainActivity\n");
        return 0;
    }
    if ((argc == 3 || argc == 4) && std::wstring_view(argv[1]) == L"--detect-fixture") {
        return detect_single_adb_device(argc == 4, argv[2]);
    }
    const auto fixture = std::filesystem::temp_directory_path() /
        (L"wireview-adb-regression-" + std::to_wstring(GetCurrentProcessId()));
    std::filesystem::create_directories(fixture);
    const auto self = fixture / L"session-tests.exe";
    std::filesystem::copy_file(current_executable(), self);
    std::filesystem::copy_file(current_executable(), fixture / L"adb.exe");
    std::ofstream(fixture / L"AdbWinApi.dll").close();
    std::ofstream(fixture / L"AdbWinUsbApi.dll").close();
    const std::vector<std::wstring> environment_names{
        L"ANDROID_HOME", L"ANDROID_SDK_ROOT", L"LOCALAPPDATA", L"PATH"};
    std::vector<std::optional<std::wstring>> previous_environment;
    for (const auto& name : environment_names) {
        previous_environment.push_back(environment_value(name.c_str()));
        // Child probes must not inspect or launch the user's real SDK.
        SetEnvironmentVariableW(name.c_str(), fixture.c_str());
    }
    unsigned failures{};
    const auto check = [&](const wchar_t* mode, DWORD expected, bool require_app = false) {
        SetEnvironmentVariableW(L"WIREVIEW_TEST_ADB_MODE", mode);
        const auto diagnostic_file = fixture / L"probe.ini";
        std::vector<std::wstring> arguments{L"--detect-fixture", diagnostic_file.wstring()};
        if (require_app) { arguments.emplace_back(L"--require-app"); }
        const auto result = run_capture(self, arguments);
        if (!result.started || result.exit_code != expected) {
            std::wcerr << mode << L": expected " << expected << L", got "
                       << result.exit_code << L'\n';
            ++failures;
        }
        std::ifstream log(diagnostic_file, std::ios::binary);
        const std::string contents{std::istreambuf_iterator<char>(log), std::istreambuf_iterator<char>()};
        if (contents.find("diagnostics=adb=") == std::string::npos ||
            contents.find(expected == 0U ? "state=ready" : "state=adb_probe") == std::string::npos ||
            (std::wstring_view(mode) == L"failed" && contents.find("cannot connect to daemon") == std::string::npos) ||
            (std::wstring_view(mode) == L"pidof_transport" && contents.find("error: device offline") == std::string::npos)) {
            std::cerr << "probe did not retain dependency path / original daemon failure\n";
            ++failures;
        }
    };
    check(L"ready", 0U);
    check(L"daemon_ready", 0U);
    check(L"empty", 3U);
    check(L"unauthorized", 4U);
    check(L"offline", 8U);
    check(L"recovery", 10U);
    check(L"multiple", 5U);
    check(L"failed", 9U);
    check(L"foreground", 0U, true);
    check(L"background", 6U, true);
    check(L"pidof_absent", 6U, true);
    check(L"pidof_transport", 9U, true);
    check(L"package_missing_legacy", 11U, true);
    check(L"package_missing", 11U, true);
    check(L"package_query_error", 9U, true);
    check(L"foreground_fail", 0U, true);
    const auto selected_check = [&](const wchar_t* mode, const wchar_t* serial, DWORD expected) {
        SetEnvironmentVariableW(L"WIREVIEW_TEST_ADB_MODE", mode);
        const auto list_file = fixture / L"devices.txt";
        const auto result = run_capture(self, {L"--detect-device", L"--serial", serial,
            L"--devices-file", list_file.wstring(), L"--diagnostics-file", (fixture / L"selected.ini").wstring()});
        if (!result.started || result.exit_code != expected) {
            std::wcerr << L"selected " << serial << L" in " << mode << L": expected "
                << expected << L", got " << result.exit_code << L'\n';
            ++failures;
        }
        std::ifstream list(list_file);
        const std::string contents{std::istreambuf_iterator<char>(list), std::istreambuf_iterator<char>()};
        if (contents.find("second") == std::string::npos) {
            std::cerr << "device list did not retain all targets for the selector\n";
            ++failures;
        }
    };
    selected_check(L"multiple", L"phone", 0U);
    selected_check(L"multiple", L"second", 4U);
    selected_check(L"two_ready", L"second", 0U);
    selected_check(L"mixed_offline", L"second", 8U);
    selected_check(L"two_ready", L"missing", 3U);
    SetEnvironmentVariableW(L"WIREVIEW_TEST_ADB_MODE", L"two_ready");
    const auto trace_path = fixture / L"trace.txt";
    SetEnvironmentVariableW(L"WIREVIEW_TEST_TRACE", trace_path.c_str());
    for (const auto* notification : {L"--notify-stop", L"--notify-host-exited", L"--notify-device-identified", L"--notify-vdd-created"}) {
        std::vector<std::wstring> args{notification};
        if (std::wstring_view(notification) == L"--notify-vdd-created") {
            args.insert(args.end(), {L"1920", L"1200", L"60"});
        }
        args.insert(args.end(), {L"--serial", L"second"});
        const auto result = run_capture(self, args);
        if (!result.started || result.exit_code != 0U) { ++failures; }
    }
    std::filesystem::copy_file(current_executable(), fixture / L"wireview_host.exe");
    std::ofstream(fixture / L"phone.mapping") << "tcp:50001";
    std::ofstream(fixture / L"second.mapping") << "tcp:50002";
    const auto session = run_capture(self, {L"--status-file", (fixture / L"session.ini").wstring(),
        L"--device-profile-probe", L"--serial", L"second"});
    std::ifstream trace(trace_path);
    const std::string traced{std::istreambuf_iterator<char>(trace), std::istreambuf_iterator<char>()};
    trace.close();
    const auto port_flag = traced.find("HOST --probe-device-profile --tcp-port ");
    std::string host_port;
    if (port_flag != std::string::npos) {
        const auto start = port_flag + std::string("HOST --probe-device-profile --tcp-port ").size();
        host_port = traced.substr(start, traced.find(' ', start) - start);
    }
    if (!session.started || session.exit_code != 0U || host_port.empty() ||
        host_port == "39871" || host_port == "50001" || host_port == "50002" ||
        traced.find("ADB -s second reverse tcp:39871 tcp:" + host_port + "\n") == std::string::npos ||
        traced.find("ADB -s phone shell") != std::string::npos ||
        traced.find("ADB -s second shell am broadcast -a com.wireview.receiver.action.WIREVIEW_STOPPED") == std::string::npos ||
        traced.find("ADB -s second shell am broadcast -a com.wireview.receiver.action.HOST_EXITED") == std::string::npos ||
        traced.find("ADB -s second shell am broadcast -a com.wireview.receiver.action.DEVICE_IDENTIFIED") == std::string::npos ||
        traced.find("ADB -s second shell am broadcast -a com.wireview.receiver.action.VDD_CREATED") == std::string::npos) {
        std::cerr << "targeted profile/notifications or session port isolation failed\n";
        ++failures;
    }
    if (std::filesystem::exists(fixture / L"second.mapping") ||
        !std::filesystem::exists(fixture / L"phone.mapping") ||
        traced.find("ADB -s second reverse --remove tcp:39871") == std::string::npos) {
        std::cerr << "completed profile leaked its reverse mapping or removed another device's mapping\n";
        ++failures;
    }
    const auto cursor_session = run_capture(self, {L"--status-file", (fixture / L"cursor-session.ini").wstring(),
        L"--display", L"test-display", L"--serial", L"second", L"--cursor-size-percent", L"150"});
    {
        std::ifstream input(trace_path);
        const std::string contents{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
        if (cursor_session.exit_code != 0U || contents.find("--cursor-size-percent 150") == std::string::npos) {
            std::cerr << "selected cursor size did not reach the Host\n"; ++failures;
        }
    }
    SetEnvironmentVariableW(L"WIREVIEW_TEST_TRACE", nullptr);
    const auto record_path = fixture / L"owned-session.ini";
    const auto cleanup_status = fixture / L"cleanup.ini";
    const auto cleanup = [&] { return run_capture(self, {L"--cleanup-adb-session", record_path.wstring(),
        L"--status-file", cleanup_status.wstring()}); };
    AdbSessionRecord record{GetCurrentProcessId(), wireview::host::process_creation_time(GetCurrentProcess()), 54321U, L"second"};
    std::ofstream(fixture / L"second.mapping") << "tcp:54321";
    if (!write_adb_session(record_path, record) || cleanup().exit_code == 0U ||
        !std::filesystem::exists(fixture / L"second.mapping")) {
        std::cerr << "cleanup removed a live owner's mapping\n"; ++failures;
    }
    // Use a known exited process, never an arbitrary PID that might be alive.
    STARTUPINFOW owner_startup{}; owner_startup.cb = sizeof(owner_startup);
    PROCESS_INFORMATION owner{};
    std::wstring owner_command = quote_argument(self.wstring()) + L" --hang-fixture";
    if (!CreateProcessW(self.c_str(), owner_command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
        nullptr, fixture.c_str(), &owner_startup, &owner)) { ++failures; }
    else {
        record.owner = owner.dwProcessId;
        record.created = wireview::host::process_creation_time(owner.hProcess);
        TerminateProcess(owner.hProcess, 0U); WaitForSingleObject(owner.hProcess, 3'000U);
        CloseHandle(owner.hThread); CloseHandle(owner.hProcess);
        std::filesystem::remove(record_path);
        if (!write_adb_session(record_path, record) || cleanup().exit_code != 0U ||
            std::filesystem::exists(record_path) || std::filesystem::exists(fixture / L"second.mapping") ||
            !std::filesystem::exists(fixture / L"phone.mapping")) {
            std::cerr << "cleanup did not release only the dead owner's exact mapping\n"; ++failures;
        }
        for (const auto* failure_mode : {L"list_failed", L"remove_failed"}) {
            std::ofstream(fixture / L"second.mapping") << "tcp:54321";
            write_adb_session(record_path, record);
            SetEnvironmentVariableW(L"WIREVIEW_TEST_ADB_MODE", failure_mode);
            if (cleanup().exit_code == 0U || !std::filesystem::exists(record_path) ||
                !std::filesystem::exists(fixture / L"second.mapping")) {
                std::cerr << "cleanup failure was hidden or its retry record lost\n"; ++failures;
            }
            std::filesystem::remove(record_path);
        }
        SetEnvironmentVariableW(L"WIREVIEW_TEST_ADB_MODE", L"two_ready");
        write_adb_session(record_path, record);
        std::ofstream(fixture / L"second.mapping") << "tcp:54322";
        if (cleanup().exit_code != 0U || !std::filesystem::exists(fixture / L"second.mapping")) {
            std::cerr << "old cleanup removed a replacement mapping\n"; ++failures;
        }
        std::ofstream(record_path) << "corrupt record";
        if (cleanup().exit_code == 0U || !std::filesystem::exists(record_path)) {
            std::cerr << "invalid ownership record was accepted\n"; ++failures;
        }
        std::filesystem::remove(record_path);
        // Hold the same serial lock while a cleanup process starts. A new
        // mapping installed under this lock must survive the older cleanup.
        write_adb_session(record_path, record);
        std::ofstream(fixture / L"second.mapping") << "tcp:54321";
        HANDLE cleanup_process{};
        {
            AdbDeviceLock lock(record.serial);
            STARTUPINFOW startup{}; startup.cb = sizeof(startup);
            PROCESS_INFORMATION child{};
            std::wstring command = quote_argument(self.wstring()) + L" --cleanup-adb-session " +
                quote_argument(record_path.wstring()) + L" --status-file " + quote_argument(cleanup_status.wstring());
            if (!lock.acquired() || !CreateProcessW(self.c_str(), command.data(), nullptr, nullptr, FALSE,
                CREATE_NO_WINDOW, nullptr, fixture.c_str(), &startup, &child)) { ++failures; }
            else {
                CloseHandle(child.hThread); cleanup_process = child.hProcess;
                if (WaitForSingleObject(cleanup_process, 150U) != WAIT_TIMEOUT) {
                    std::cerr << "cleanup did not serialize with mapping setup\n"; ++failures;
                }
                std::ofstream(fixture / L"second.mapping") << "tcp:54322";
            }
        }
        if (cleanup_process != nullptr) {
            const auto waited = WaitForSingleObject(cleanup_process, 5'000U);
            DWORD code{}; GetExitCodeProcess(cleanup_process, &code); CloseHandle(cleanup_process);
            std::ifstream mapping(fixture / L"second.mapping"); std::string value; mapping >> value;
            if (waited != WAIT_OBJECT_0 || code != 0U || value != "tcp:54322") {
                std::cerr << "older serialized cleanup removed a newer mapping\n"; ++failures;
            }
        }
    }
    // Match the control center's forced-stop path: kill a complete session Job
    // after reverse was installed, then reclaim from a helper OUTSIDE that Job.
    SetEnvironmentVariableW(L"WIREVIEW_TEST_HOST_HANG", L"1");
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    STARTUPINFOW job_startup{}; job_startup.cb = sizeof(job_startup);
    PROCESS_INFORMATION job_session{};
    const auto killed_record = fixture / L"killed-session.ini";
    std::wstring job_command = quote_argument(self.wstring()) + L" --status-file " + quote_argument((fixture / L"killed.ini").wstring()) +
        L" --adb-session-file " + quote_argument(killed_record.wstring()) + L" --device-profile-probe --serial second";
    if (job == nullptr || !SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits)) ||
        !CreateProcessW(self.c_str(), job_command.data(), nullptr, nullptr, FALSE, CREATE_SUSPENDED | CREATE_NO_WINDOW,
            nullptr, fixture.c_str(), &job_startup, &job_session)) { ++failures; }
    else {
        if (!AssignProcessToJobObject(job, job_session.hProcess)) { ++failures; TerminateProcess(job_session.hProcess, 1U); }
        else {
            ResumeThread(job_session.hThread);
            bool mapped = false;
            const auto until = GetTickCount64() + 5'000U;
            while (GetTickCount64() < until) {
                AdbSessionRecord saved;
                std::ifstream input(fixture / L"second.mapping"); std::string value; input >> value;
                if (read_adb_session(killed_record, saved) && saved.owner == job_session.dwProcessId &&
                    value == "tcp:" + std::to_string(saved.port)) { mapped = true; break; }
                Sleep(20U);
            }
            if (!mapped) { std::cerr << "forced-stop fixture did not reach reverse setup\n"; ++failures; }
            TerminateJobObject(job, 0U);
            WaitForSingleObject(job_session.hProcess, 3'000U);
            const auto result = run_capture(self, {L"--cleanup-adb-session", killed_record.wstring(),
                L"--status-file", cleanup_status.wstring()});
            if (result.exit_code != 0U || std::filesystem::exists(killed_record) ||
                std::filesystem::exists(fixture / L"second.mapping") || !std::filesystem::exists(fixture / L"phone.mapping")) {
                std::cerr << "forced Job stop leaked a mapping or touched another device\n"; ++failures;
            }
        }
        CloseHandle(job_session.hThread); CloseHandle(job_session.hProcess);
    }
    if (job != nullptr) { CloseHandle(job); }
    SetEnvironmentVariableW(L"WIREVIEW_TEST_HOST_HANG", nullptr);
    const auto notice_trace = fixture / L"notice-trace.txt";
    SetEnvironmentVariableW(L"WIREVIEW_TEST_TRACE", notice_trace.c_str());
    SetEnvironmentVariableW(L"WIREVIEW_TEST_DELAY_STOP", L"1");
    STARTUPINFOW notice_startup{}; notice_startup.cb = sizeof(notice_startup);
    PROCESS_INFORMATION notice{};
    std::wstring notice_command = quote_argument(self.wstring()) + L" --cleanup-adb-session " +
        quote_argument(killed_record.wstring()) + L" --status-file " + quote_argument(cleanup_status.wstring()) + L" --notify-stop second";
    if (!CreateProcessW(self.c_str(), notice_command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
        nullptr, fixture.c_str(), &notice_startup, &notice)) { ++failures; }
    else {
        bool broadcasting = false;
        const auto until = GetTickCount64() + 3'000U;
        while (GetTickCount64() < until) {
            std::ifstream input(notice_trace);
            const std::string contents{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
            if (contents.find("ADB -s second shell am broadcast") != std::string::npos) { broadcasting = true; break; }
            Sleep(10U);
        }
        if (!broadcasting || WaitForSingleObject(notice.hProcess, 0U) != WAIT_TIMEOUT) {
            std::cerr << "cleanup completed before its delayed stop notification\n"; ++failures;
        }
        const auto next = run_capture(self, {L"--status-file", (fixture / L"next.ini").wstring(),
            L"--device-profile-probe", L"--serial", L"second"});
        if (WaitForSingleObject(notice.hProcess, 5'000U) != WAIT_OBJECT_0 || next.exit_code != 0U) { ++failures; }
        CloseHandle(notice.hThread); CloseHandle(notice.hProcess);
        std::ifstream input(notice_trace);
        const std::string contents{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
        const auto delivered = contents.find("STOP-DELIVERED");
        const auto host_started = contents.find("HOST --probe-device-profile");
        if (delivered == std::string::npos || host_started == std::string::npos || delivered >= host_started) {
            std::cerr << "old stop notification reached the device after the next Host started\n"; ++failures;
        }
    }
    SetEnvironmentVariableW(L"WIREVIEW_TEST_TRACE", nullptr);
    SetEnvironmentVariableW(L"WIREVIEW_TEST_DELAY_STOP", nullptr);
    const auto started = GetTickCount64();
    const auto timeout = run_capture(self, {L"--hang-fixture"}, {}, {}, 100U);
    if (!timeout.started || timeout.exit_code != WAIT_TIMEOUT || GetTickCount64() - started > 3'000U) {
        std::cerr << "child timeout did not terminate promptly\n";
        ++failures;
    }
    WSADATA socket_data{};
    const int socket_init = WSAStartup(MAKEWORD(2, 2), &socket_data);
    if (socket_init != 0) { ++failures; }
    else {
        // Model an old Host listener that enabled SO_REUSEADDR. A new session
        // must fail instead of sharing that port and receiving arbitrary peers.
        const SOCKET occupied = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        const int reuse = 1;
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        int length = sizeof(address);
        if (occupied == INVALID_SOCKET || setsockopt(occupied, SOL_SOCKET, SO_REUSEADDR,
                reinterpret_cast<const char*>(&reuse), sizeof(reuse)) != 0 ||
            bind(occupied, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0 ||
            getsockname(occupied, reinterpret_cast<sockaddr*>(&address), &length) != 0 || listen(occupied, 1) != 0) {
            ++failures;
        } else {
            const auto result = run_capture(current_executable().parent_path() / L"wireview_host.exe",
                {L"--probe-device-profile", L"--status-file", (fixture / L"collision.ini").wstring(),
                 L"--tcp-port", std::to_wstring(ntohs(address.sin_port))}, {}, {}, 1'500U);
            if (!result.started || result.exit_code == 0U || result.exit_code == WAIT_TIMEOUT ||
                result.output.find(L"bind") == std::wstring::npos) {
                std::cerr << "Host stole an occupied TCP port instead of failing bind\n";
                ++failures;
            }
        }
        if (occupied != INVALID_SOCKET) { closesocket(occupied); }
        WSACleanup();
    }
    std::filesystem::remove(fixture / L"AdbWinUsbApi.dll");
    const auto sdk_tools = fixture / L"sdk" / L"platform-tools";
    std::filesystem::create_directories(sdk_tools);
    std::filesystem::copy_file(current_executable(), sdk_tools / L"adb.exe");
    std::ofstream(sdk_tools / L"AdbWinApi.dll").close();
    std::ofstream(sdk_tools / L"AdbWinUsbApi.dll").close();
    SetEnvironmentVariableW(L"ANDROID_HOME", (fixture / L"sdk").c_str());
    // A broken local copy must not shadow a complete SDK ADB. If every copy
    // is incomplete, preserve the first path for an actionable diagnosis.
    check(L"ready", 0U);
    {
        std::ifstream log(fixture / L"probe.ini", std::ios::binary);
        const std::string contents{std::istreambuf_iterator<char>(log), std::istreambuf_iterator<char>()};
        if (contents.find(to_utf8((sdk_tools / L"adb.exe").wstring())) == std::string::npos) {
            std::cerr << "incomplete local ADB shadowed the complete SDK installation\n";
            ++failures;
        }
    }
    std::filesystem::remove(sdk_tools / L"AdbWinUsbApi.dll");
    check(L"ready", 7U);
    {
        // A failed dependency check must not publish yesterday's device list.
        const auto result = run_capture(self, {L"--detect-device", L"--serial", L"second",
            L"--devices-file", (fixture / L"devices.txt").wstring()});
        std::ifstream list(fixture / L"devices.txt");
        const std::string stale{std::istreambuf_iterator<char>(list), std::istreambuf_iterator<char>()};
        if (result.exit_code != 7U || !stale.empty()) {
            std::cerr << "dependency failure left a stale device list\n";
            ++failures;
        }
    }
    Options options;
    wchar_t program[] = L"session";
    wchar_t status_flag[] = L"--status-file";
    wchar_t status_value[] = L"status.ini";
    wchar_t display_flag[] = L"--display";
    wchar_t display_value[] = L"\\\\.\\DISPLAY1";
    wchar_t* args[]{program, status_flag, status_value, display_flag, display_value};
    if (!parse_arguments(5, args, &options) || options.duration_seconds != kUnlimitedSessionSeconds) {
        std::cerr << "default session regressed to a limited/30-minute duration\n";
        ++failures;
    }
    SetEnvironmentVariableW(L"WIREVIEW_TEST_ADB_MODE", nullptr);
    for (std::size_t index = 0; index < environment_names.size(); ++index) {
        SetEnvironmentVariableW(environment_names[index].c_str(), previous_environment[index]
            ? previous_environment[index]->c_str() : nullptr);
    }
    std::filesystem::remove_all(fixture);
    std::cout << "session diagnostics failures=" << failures << '\n';
    return failures == 0U ? 0 : 1;
}
