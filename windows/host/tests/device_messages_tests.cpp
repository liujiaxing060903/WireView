#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "device_messages.hpp"
#include <algorithm>
#include <atomic>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {
constexpr UINT kBlock = WM_APP + 19U;

void require(bool condition, const char* message) {
    if (!condition) { throw std::runtime_error(message); }
}

struct Receiver {
    HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE blocked = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE resumed = CreateEventW(nullptr, TRUE, TRUE, nullptr);
    std::atomic<DWORD> block_duration{450U};
    std::atomic<bool> destroy_after_block{};
    HWND window{};
    std::mutex mutex;
    std::vector<std::wstring> received;
    std::thread worker;

    static LRESULT CALLBACK procedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
        if (message == WM_NCCREATE) {
            auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
        }
        auto* self = reinterpret_cast<Receiver*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == kBlock) {
            SetEvent(self->blocked);
            Sleep(self->block_duration.load());
            if (self->destroy_after_block.load()) { DestroyWindow(window); PostQuitMessage(0); }
            SetEvent(self->resumed);
            return 0;
        }
        if (message == WM_COPYDATA) {
            const auto* data = reinterpret_cast<const COPYDATASTRUCT*>(lparam);
            std::wstring payload(static_cast<const wchar_t*>(data->lpData), data->cbData / sizeof(wchar_t));
            std::lock_guard lock(self->mutex);
            self->received.push_back(payload);
            return payload == L"reject" ? FALSE : TRUE;
        }
        if (message == WM_CLOSE) { DestroyWindow(window); PostQuitMessage(0); return 0; }
        return DefWindowProcW(window, message, wparam, lparam);
    }

    Receiver() : worker([this] {
        WNDCLASSW type{};
        type.lpfnWndProc = procedure;
        type.hInstance = GetModuleHandleW(nullptr);
        type.lpszClassName = L"WireViewCommandDeliveryTest";
        RegisterClassW(&type);
        window = CreateWindowW(type.lpszClassName, L"", 0U, 0, 0, 0, 0,
            HWND_MESSAGE, nullptr, type.hInstance, this);
        SetEvent(ready);
        MSG message{};
        while (GetMessageW(&message, nullptr, 0U, 0U) > 0) { DispatchMessageW(&message); }
    }) {
        require(WaitForSingleObject(ready, 2'000U) == WAIT_OBJECT_0 && window != nullptr,
            "test receiver did not start");
    }

    ~Receiver() {
        PostMessageW(window, WM_CLOSE, 0U, 0);
        worker.join();
        CloseHandle(ready);
        CloseHandle(blocked);
        CloseHandle(resumed);
    }

    std::vector<std::wstring> messages() {
        std::lock_guard lock(mutex);
        return received;
    }

    void block_for(DWORD milliseconds, bool destroy = false) {
        block_duration = milliseconds;
        destroy_after_block = destroy;
        ResetEvent(blocked);
        ResetEvent(resumed);
        require(PostMessageW(window, kBlock, 0U, 0) != 0, "receiver could not be blocked");
        require(WaitForSingleObject(blocked, 2'000U) == WAIT_OBJECT_0, "receiver did not become busy");
    }
};

std::vector<bool> finish(wireview::host::DeviceCommandChannel& channel) {
    const auto deadline = GetTickCount64() + 5'000U;
    std::vector<bool> results;
    while (channel.pending() && GetTickCount64() < deadline) {
        if (const auto result = channel.poll()) { results.push_back(*result); }
        Sleep(2U);
    }
    require(!channel.pending(), "command delivery did not finish");
    return results;
}
}

int main() {
    try {
        Receiver receiver;
        receiver.block_for(450U);
        wireview::host::DeviceCommandChannel channel;
        const auto started = GetTickCount64();
        channel.enqueue(receiver.window, GetCurrentProcessId(), nullptr, 1U, L"start");
        (void)channel.poll();
        require(GetTickCount64() - started < 250U, "sending a command blocked the UI thread");
        require(finish(channel) == std::vector<bool>{true}, "busy receiver lost the start command after 180 ms");
        require(receiver.messages() == std::vector<std::wstring>{L"start"}, "start must be delivered exactly once");

        channel.enqueue(receiver.window, GetCurrentProcessId(), nullptr, 1U, L"theme-light");
        channel.enqueue(receiver.window, GetCurrentProcessId(), nullptr, 1U, L"diagnostics");
        require(finish(channel) == std::vector<bool>({true, true}), "ordered commands were not accepted");
        require(receiver.messages() == std::vector<std::wstring>({L"start", L"theme-light", L"diagnostics"}),
            "commands must keep their original order");

        channel.enqueue(receiver.window, GetCurrentProcessId(), nullptr, 1U, L"reject");
        channel.enqueue(receiver.window, GetCurrentProcessId(), nullptr, 1U, L"must-not-send");
        require(finish(channel) == std::vector<bool>{false}, "receiver rejection must fail the sequence");
        require(receiver.messages().back() == L"reject", "a failed sequence must not continue or retry");
        const auto count = receiver.messages().size();
        channel.enqueue(receiver.window, GetCurrentProcessId() + 1U, nullptr, 1U, L"wrong-process");
        require(finish(channel) == std::vector<bool>{false}, "reused window identity must fail");
        require(receiver.messages().size() == count, "wrong process must not receive a command");
        require(!wireview::host::send_device_message(receiver.window, nullptr, 1U, L""),
            "empty messages must be rejected");

        receiver.block_for(3'500U);
        channel.enqueue(receiver.window, GetCurrentProcessId(), nullptr, 1U, L"timeout");
        channel.enqueue(receiver.window, GetCurrentProcessId(), nullptr, 1U, L"after-timeout");
        const auto timeout_started = GetTickCount64();
        require(finish(channel) == std::vector<bool>{false}, "a real timeout must fail and clear the queue");
        require(GetTickCount64() - timeout_started < 4'500U, "a timeout must be bounded");
        require(WaitForSingleObject(receiver.resumed, 2'000U) == WAIT_OBJECT_0, "timed-out receiver did not resume");
        auto messages = receiver.messages();
        require(std::find(messages.begin(), messages.end(), L"after-timeout") == messages.end(),
            "a timed-out sequence must not send its next command");

        receiver.block_for(450U, true);
        channel.enqueue(receiver.window, GetCurrentProcessId(), nullptr, 1U, L"destroyed-window");
        channel.enqueue(receiver.window, GetCurrentProcessId(), nullptr, 1U, L"after-destroy");
        require(finish(channel) == std::vector<bool>{false} && !IsWindow(receiver.window),
            "window destruction during delivery must fail without retrying");
        messages = receiver.messages();
        require(std::find(messages.begin(), messages.end(), L"after-destroy") == messages.end(),
            "a destroyed receiver must not get queued follow-up commands");
        std::cout << "All device command delivery tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
