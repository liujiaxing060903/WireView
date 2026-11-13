#include "overlapped_completion.hpp"
#include <iostream>
#include <string>

int main() {
    const auto name = L"\\\\.\\pipe\\wireview-overlapped-test-" + std::to_wstring(GetCurrentProcessId());
    HANDLE server = CreateNamedPipeW(name.c_str(), PIPE_ACCESS_INBOUND | FILE_FLAG_OVERLAPPED,
        PIPE_TYPE_BYTE | PIPE_WAIT, 1U, 256U, 256U, 0U, nullptr);
    if (server == INVALID_HANDLE_VALUE) { return 1; }
    HANDLE client = CreateFileW(name.c_str(), GENERIC_WRITE, 0U, nullptr, OPEN_EXISTING, 0U, nullptr);
    if (client == INVALID_HANDLE_VALUE) { CloseHandle(server); return 1; }
    OVERLAPPED operation{};
    operation.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    BYTE data{};
    DWORD bytes{};
    bool ok = ReadFile(server, &data, 1U, &bytes, &operation) == FALSE && GetLastError() == ERROR_IO_PENDING;
    ok = ok && !wireview::host::finish_overlapped_io(server, operation, bytes, 10U) &&
        GetLastError() == ERROR_TIMEOUT && HasOverlappedIoCompleted(&operation);
    ResetEvent(operation.hEvent);
    operation.Internal = operation.InternalHigh = 0U;
    const BYTE expected = 42U;
    ok = ok && WriteFile(client, &expected, 1U, &bytes, nullptr) != FALSE;
    const BOOL read = ReadFile(server, &data, 1U, &bytes, &operation);
    ok = ok && (read != FALSE || (GetLastError() == ERROR_IO_PENDING &&
        wireview::host::finish_overlapped_io(server, operation, bytes, 1'000U))) && data == expected;
    CloseHandle(operation.hEvent);
    CloseHandle(client);
    CloseHandle(server);
    std::cout << "Real overlapped timeout/cancellation/completion: " << (ok ? "passed" : "FAILED") << '\n';
    return ok ? 0 : 1;
}
