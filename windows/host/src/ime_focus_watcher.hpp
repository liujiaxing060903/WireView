// UI Automation focus watcher for the Android IME bridge: reports when the
// user is "about to type" so the tablet can pop its own IME automatically.
// A state transition fires when the focused element becomes editable (or
// stops being editable) AND the last pointer interaction came from the
// tablet within the recent window; physical mouse use on the PC must not
// trigger the tablet keyboard.
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <thread>

namespace wireview::host {

class ImeFocusWatcher {
public:
    ImeFocusWatcher() = default;
    ~ImeFocusWatcher();

    ImeFocusWatcher(const ImeFocusWatcher&) = delete;
    ImeFocusWatcher& operator=(const ImeFocusWatcher&) = delete;

    // on_state(true) = show the tablet IME, on_state(false) = hide it.
    // Called from a UI Automation callback thread; keep it cheap and
    // thread-safe. Returns false when COM/UIA setup failed.
    bool start(
        std::function<void(bool show)> on_state,
        const std::atomic<std::uint64_t>* last_tablet_input_micros,
        std::uint64_t recent_window_micros = 3'000'000);

    void stop();

private:
    void run();

    std::thread thread_;
    std::function<void(bool)> on_state_;
    const std::atomic<std::uint64_t>* last_tablet_input_{nullptr};
    std::uint64_t recent_window_micros_{3'000'000};
    std::atomic_bool stopping_{false};
};

}  // namespace wireview::host
