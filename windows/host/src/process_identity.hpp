#pragma once
#include <Windows.h>
#include <cstdint>

namespace wireview::host {
inline std::uint64_t process_creation_time(HANDLE process) noexcept {
    FILETIME created{}, exited{}, kernel{}, user{};
    if (!GetProcessTimes(process, &created, &exited, &kernel, &user)) { return 0U; }
    return (static_cast<std::uint64_t>(created.dwHighDateTime) << 32U) | created.dwLowDateTime;
}

inline std::uint64_t process_creation_time(DWORD pid) noexcept {
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (process == nullptr) { return 0U; }
    const auto created = process_creation_time(process);
    CloseHandle(process);
    return created;
}

// PID alone is not identity: Windows reuses it after process exit/reboot.
inline bool recorded_process_is_alive(DWORD pid, std::uint64_t expected_creation,
    std::uint64_t record_written = 0U) noexcept {
    if (pid == 0U) { return false; }
    HANDLE process = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (process == nullptr) { return GetLastError() == ERROR_ACCESS_DENIED; }
    const auto created = process_creation_time(process);
    const bool matches = created == 0U || (expected_creation != 0U
        ? created == expected_creation : (record_written == 0U || created <= record_written));
    const bool alive = matches && WaitForSingleObject(process, 0U) == WAIT_TIMEOUT;
    CloseHandle(process);
    return alive;
}
} // namespace wireview::host
