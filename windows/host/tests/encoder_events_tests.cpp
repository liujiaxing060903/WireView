#include "encoder_events.hpp"
#include <mfapi.h>
#include <atomic>
#include <iostream>
#include <stdexcept>
using Microsoft::WRL::ComPtr;

class ProbeGenerator final : public Microsoft::WRL::RuntimeClass<
    Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>, IMFMediaEventGenerator> {
public:
    ProbeGenerator() { if (FAILED(MFCreateEventQueue(&queue))) { throw std::runtime_error("MFCreateEventQueue"); } }
    STDMETHODIMP GetEvent(DWORD flags, IMFMediaEvent** event) override {
        ++polls;
        return queue->GetEvent(flags, event);
    }
    STDMETHODIMP BeginGetEvent(IMFAsyncCallback* callback, IUnknown* state) override { return queue->BeginGetEvent(callback, state); }
    STDMETHODIMP EndGetEvent(IMFAsyncResult* result, IMFMediaEvent** event) override { return queue->EndGetEvent(result, event); }
    STDMETHODIMP QueueEvent(MediaEventType type, REFGUID extended, HRESULT status, const PROPVARIANT* value) override {
        return queue->QueueEventParamVar(type, extended, status, value);
    }
    std::atomic<unsigned> polls{};
    ComPtr<IMFMediaEventQueue> queue;
};

int main() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    MFStartup(MF_VERSION);
    unsigned failures{};
    auto generator = Microsoft::WRL::Make<ProbeGenerator>();
    auto events = Microsoft::WRL::Make<wireview::host::EncoderEvents>(generator.Get());
    if (FAILED(events->start())) { return 1; }
    const auto read = [&] {
        wireview::host::EncoderEvents::Event item;
        for (unsigned attempt=0; attempt<200; ++attempt) {
            if (events->next(item)) { return item; }
            Sleep(1U);
        }
        return wireview::host::EncoderEvents::Event{MEError, E_ABORT};
    };
    for (auto type : {METransformNeedInput, METransformHaveOutput, METransformDrainComplete}) {
        generator->QueueEvent(type, GUID_NULL, S_OK, nullptr);
    }
    if (WaitForSingleObject(events->ready_handle(), 1'000U) != WAIT_OBJECT_0) { ++failures; }
    for (auto type : {METransformNeedInput, METransformHaveOutput, METransformDrainComplete}) {
        const auto item = read();
        if (item.type != type || item.status != S_OK) { ++failures; }
    }
    if (WaitForSingleObject(events->ready_handle(), 0U) != WAIT_TIMEOUT) { ++failures; }
    generator->QueueEvent(MEError, GUID_NULL, E_FAIL, nullptr);
    if (read().status != E_FAIL) { ++failures; }
    if (generator->polls != 0U) {
        std::cerr << "render thread still calls synchronous GetEvent\n"; ++failures;
    }
    events->stop();
    generator->queue->Shutdown();
    wireview::host::EncoderEvents::Event discarded;
    if (events->next(discarded)) { ++failures; }
    events.Reset(); generator.Reset();
    MFShutdown(); CoUninitialize();
    std::cout << "encoder event order/error/stop failures=" << failures << '\n';
    return failures == 0U ? 0 : 1;
}
