#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

namespace wireview::host {
// The OVERLAPPED and its buffers must outlive a cancelled request. Merely
// closing the event on timeout lets a driver write to freed stack memory.
inline bool finish_overlapped_io(HANDLE device, OVERLAPPED& operation,
    DWORD& bytes, DWORD timeout_ms) noexcept {
    const DWORD wait = WaitForSingleObject(operation.hEvent, timeout_ms);
    if (wait == WAIT_OBJECT_0) {
        return GetOverlappedResult(device, &operation, &bytes, FALSE) != FALSE;
    }
    const DWORD failure = wait == WAIT_TIMEOUT ? ERROR_TIMEOUT : GetLastError();
    CancelIoEx(device, &operation); // Only this request, never another session.
    if (GetOverlappedResult(device, &operation, &bytes, TRUE) != FALSE) {
        // Completion won the race with cancellation: the ADD/result is valid.
        return true;
    }
    SetLastError(failure);
    return false;
}
} // namespace wireview::host
