#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <mfidl.h>
#include <wrl/client.h>
#include <wrl/implements.h>
#include <deque>
#include <mutex>
#include <system_error>

namespace wireview::host {
class EncoderEvents final : public Microsoft::WRL::RuntimeClass<
    Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>, IMFAsyncCallback> {
public:
    struct Event { MediaEventType type{MEUnknown}; HRESULT status{S_OK}; };
    explicit EncoderEvents(IMFMediaEventGenerator* generator) : generator_(generator),
        ready_(CreateEventW(nullptr, TRUE, FALSE, nullptr)) {
        if (!ready_) { throw std::system_error(GetLastError(), std::system_category(), "CreateEvent(encoder events)"); }
    }
    ~EncoderEvents() { CloseHandle(ready_); }
    HANDLE ready_handle() const noexcept { return ready_; }
    HRESULT start() {
        return generator_->BeginGetEvent(this, nullptr);
    }
    bool next(Event& item) {
        std::lock_guard lock(mutex_);
        if (pending_.empty()) { return false; }
        item = pending_.front();
        pending_.pop_front();
        if (pending_.empty()) { ResetEvent(ready_); }
        return true;
    }
    void stop() {
        Microsoft::WRL::ComPtr<IMFMediaEventGenerator> released;
        {
            std::lock_guard lock(mutex_);
            released.Swap(generator_);
            pending_.clear();
            ResetEvent(ready_);
        }
    }
    STDMETHODIMP GetParameters(DWORD*, DWORD*) override { return E_NOTIMPL; }
    STDMETHODIMP Invoke(IMFAsyncResult* result) override {
        Microsoft::WRL::ComPtr<IMFMediaEventGenerator> generator;
        {
            std::lock_guard lock(mutex_);
            generator = generator_;
        }
        if (!generator) { return S_OK; }
        Microsoft::WRL::ComPtr<IMFMediaEvent> event;
        Event item;
        item.status = generator->EndGetEvent(result, &event);
        if (SUCCEEDED(item.status)) {
            HRESULT status{};
            item.status = event->GetStatus(&status);
            if (SUCCEEDED(item.status)) { item.status = status; }
            if (SUCCEEDED(item.status)) { item.status = event->GetType(&item.type); }
        }
        {
            std::lock_guard lock(mutex_);
            if (!generator_) { return S_OK; }
            pending_.push_back(item);
            SetEvent(ready_);
        }
        if (SUCCEEDED(item.status)) {
            // COM/MFT calls stay outside our queue lock: even an expensive
            // driver callback must not stall the video thread's next().
            const HRESULT status = generator->BeginGetEvent(this, nullptr);
            if (FAILED(status)) {
                std::lock_guard lock(mutex_);
                if (generator_) { pending_.push_back({MEError, status}); SetEvent(ready_); }
            }
        }
        return S_OK;
    }
private:
    std::mutex mutex_;
    std::deque<Event> pending_;
    Microsoft::WRL::ComPtr<IMFMediaEventGenerator> generator_;
    HANDLE ready_{};
};
} // namespace wireview::host
