#pragma once

#include <Windows.h>

namespace wireview::host {

// All WireView writers of desktop modes and full layout snapshots must use the
// same existing cross-process lock, so one cannot commit another's stale mode.
class LidDisplayLayoutLock {
public:
    explicit LidDisplayLayoutLock(DWORD timeout = 0U)
        : mutex_(CreateMutexW(nullptr, FALSE, L"Local\\WireView.LidDisplayLayout.1")) {
        if (mutex_ != nullptr) {
            const DWORD wait = WaitForSingleObject(mutex_, timeout);
            acquired_ = wait == WAIT_OBJECT_0 || wait == WAIT_ABANDONED;
        }
    }
    ~LidDisplayLayoutLock() {
        if (acquired_) { ReleaseMutex(mutex_); }
        if (mutex_ != nullptr) { CloseHandle(mutex_); }
    }
    LidDisplayLayoutLock(const LidDisplayLayoutLock&) = delete;
    LidDisplayLayoutLock& operator=(const LidDisplayLayoutLock&) = delete;
    [[nodiscard]] bool acquired() const { return acquired_; }

private:
    HANDLE mutex_{};
    bool acquired_{};
};

} // namespace wireview::host
