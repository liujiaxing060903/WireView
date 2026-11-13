#pragma once

#include <Windows.h>
#include <chrono>
#include <deque>
#include <future>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace wireview::host {

inline constexpr DWORD kDeviceMessageLimit = 128U * 1024U;

inline bool send_device_message(HWND receiver, HWND sender, ULONG_PTR kind,
    std::wstring_view payload, DWORD timeout = 180U, DWORD expected_pid = 0U) {
    if (receiver == nullptr || payload.empty() || payload.size() > kDeviceMessageLimit / sizeof(wchar_t)) {
        return false;
    }
    DWORD pid{};
    GetWindowThreadProcessId(receiver, &pid);
    if (pid == 0U || (expected_pid != 0U && pid != expected_pid)) { return false; }
    COPYDATASTRUCT data{kind, static_cast<DWORD>(payload.size() * sizeof(wchar_t)),
        const_cast<wchar_t*>(payload.data())};
    DWORD_PTR accepted{};
    return SendMessageTimeoutW(receiver, WM_COPYDATA, reinterpret_cast<WPARAM>(sender),
        reinterpret_cast<LPARAM>(&data), SMTO_BLOCK | SMTO_ABORTIFHUNG | SMTO_ERRORONEXIT,
        timeout, &accepted) != 0 && accepted != 0U;
}

// Only this queue's calling UI thread touches its state. The sender owns a copy
// of each payload and never touches application state from the background thread.
class DeviceCommandChannel {
public:
    void enqueue(HWND receiver, DWORD receiver_pid, HWND sender, ULONG_PTR kind, std::wstring payload) {
        queue_.push_back(Message{receiver, receiver_pid, sender, kind, std::move(payload)});
    }

    bool pending() const { return delivery_.valid() || !queue_.empty(); }

    std::optional<bool> poll() {
        std::optional<bool> result;
        if (delivery_.valid()) {
            if (delivery_.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) {
                return std::nullopt;
            }
            result = delivery_.get();
            if (!*result) { queue_.clear(); }
        }
        if (!queue_.empty()) {
            auto message = std::move(queue_.front());
            queue_.pop_front();
            try {
                delivery_ = std::async(std::launch::async, [message = std::move(message)] {
                    return send_device_message(message.receiver, message.sender, message.kind,
                        message.payload, 3'000U, message.receiver_pid);
                });
            } catch (const std::system_error&) {
                queue_.clear();
                result = false;
            }
        }
        return result;
    }

private:
    struct Message {
        HWND receiver;
        DWORD receiver_pid;
        HWND sender;
        ULONG_PTR kind;
        std::wstring payload;
    };
    std::deque<Message> queue_;
    std::future<bool> delivery_;
};

} // namespace wireview::host
