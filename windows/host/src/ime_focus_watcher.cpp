#include "ime_focus_watcher.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <uiautomation.h>

#include <chrono>

namespace wireview::host {
namespace {

// Heuristic: does this focused element accept keyboard text? Document
// controls (browser pages, whole Word windows) are deliberately NOT trusted
// here: they would pop the tablet IME on every page click. Only Edit-like
// controls pass outright; everything else must expose the Value pattern.
bool is_editable_element(IUIAutomationElement* element) {
    CONTROLTYPEID type{};
    if (element->get_CurrentControlType(&type) != S_OK) {
        return false;
    }
    switch (type) {
        case UIA_EditControlTypeId:
            return true;
        case UIA_ComboBoxControlTypeId:
        case UIA_ListControlTypeId:
        case UIA_TreeControlTypeId:
        case UIA_TableControlTypeId:
        case UIA_DataGridControlTypeId:
        case UIA_ListItemControlTypeId:
        case UIA_TreeItemControlTypeId:
            return false;
        default:
            break;
    }
    VARIANT value{};
    if (element->GetCurrentPropertyValue(
            UIA_IsValuePatternAvailablePropertyId, &value) == S_OK) {
        const bool available = value.vt == VT_BOOL && value.boolVal != VARIANT_FALSE;
        VariantClear(&value);
        return available;
    }
    return false;
}

class FocusEventHandler final : public IUIAutomationFocusChangedEventHandler {
public:
    FocusEventHandler(
        std::function<void(bool)> on_state,
        const std::atomic<std::uint64_t>* last_tablet_input_micros,
        std::uint64_t recent_window_micros,
        const std::atomic_bool* stopping)
        : on_state_(std::move(on_state)),
          last_tablet_input_micros_(last_tablet_input_micros),
          recent_window_micros_(recent_window_micros),
          stopping_(stopping) {}

    ULONG STDMETHODCALLTYPE AddRef() override {
        return reference_count_.fetch_add(1U, std::memory_order_relaxed) + 1U;
    }

    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG remaining =
            reference_count_.fetch_sub(1U, std::memory_order_relaxed) - 1U;
        if (remaining == 0U) {
            delete this;
        }
        return remaining;
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** result) override {
        if (result == nullptr) {
            return E_POINTER;
        }
        if (iid == __uuidof(IUnknown) || iid == __uuidof(IUIAutomationFocusChangedEventHandler)) {
            *result = static_cast<IUIAutomationFocusChangedEventHandler*>(this);
            AddRef();
            return S_OK;
        }
        *result = nullptr;
        return E_NOINTERFACE;
    }

    HRESULT STDMETHODCALLTYPE HandleFocusChangedEvent(
        IUIAutomationElement* source) override {
        if (source == nullptr || !on_state_ || stopping_->load()) {
            return S_OK;
        }
        // The callback runs on UIA threads after the watcher may have been
        // asked to stop; never let anything escape the COM boundary and never
        // touch state that the stopping watcher has already released.
        try {
            // The tablet IME is only interesting while the user drives the
            // pointer from the tablet; a physical mouse click into an editor
            // on the PC must not pop the tablet keyboard.
            const std::uint64_t last_tablet =
                last_tablet_input_micros_->load(std::memory_order_relaxed);
            const std::uint64_t now = static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now().time_since_epoch())
                    .count());
            const bool editable =
                last_tablet != 0U && now >= last_tablet &&
                now - last_tablet <= recent_window_micros_ &&
                is_editable_element(source);
            bool expected = !editable;
            if (reported_.compare_exchange_strong(expected, editable)) {
                on_state_(editable);
            }
        } catch (...) {
            // Swallow: a dead socket must not terminate the process through
            // a UIA callback frame.
        }
        return S_OK;
    }

private:
    std::function<void(bool)> on_state_;
    const std::atomic<std::uint64_t>* last_tablet_input_micros_;
    std::uint64_t recent_window_micros_;
    const std::atomic_bool* stopping_;
    std::atomic<ULONG> reference_count_{1U};
    std::atomic<bool> reported_{false};
};

}  // namespace

ImeFocusWatcher::~ImeFocusWatcher() {
    stop();
}

bool ImeFocusWatcher::start(
    std::function<void(bool show)> on_state,
    const std::atomic<std::uint64_t>* last_tablet_input_micros,
    std::uint64_t recent_window_micros) {
    on_state_ = std::move(on_state);
    last_tablet_input_ = last_tablet_input_micros;
    recent_window_micros_ = recent_window_micros;
    stopping_.store(false);
    try {
        thread_ = std::thread([this] { run(); });
    } catch (...) {
        return false;
    }
    return true;
}

void ImeFocusWatcher::stop() {
    if (!thread_.joinable()) {
        return;
    }
    stopping_.store(true);
    thread_.join();
}

void ImeFocusWatcher::run() {
    // MTA: UIA event handlers arrive on their own threads either way.
    HRESULT com_result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(com_result)) {
        return;
    }
    IUIAutomation* automation = nullptr;
    FocusEventHandler* handler = nullptr;
    HRESULT hr = CoCreateInstance(
        CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&automation));
    if (SUCCEEDED(hr)) {
        handler = new (std::nothrow) FocusEventHandler(
            on_state_, last_tablet_input_, recent_window_micros_, &stopping_);
        if (handler != nullptr) {
            hr = automation->AddFocusChangedEventHandler(nullptr, handler);
            if (FAILED(hr)) {
                handler->Release();
                handler = nullptr;
            }
        }
    }
    if (FAILED(hr)) {
        if (handler != nullptr) {
            handler->Release();
        }
        if (automation != nullptr) {
            automation->Release();
        }
        CoUninitialize();
        return;
    }
    while (!stopping_.load()) {
        Sleep(150U);
    }
    automation->RemoveFocusChangedEventHandler(handler);
    handler->Release();
    automation->Release();
    // Give any in-flight UIA callback a moment to drain before the watcher's
    // captured state (stack references) goes out of scope.
    Sleep(250U);
    CoUninitialize();
}

}  // namespace wireview::host
