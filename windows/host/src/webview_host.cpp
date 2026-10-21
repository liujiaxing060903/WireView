#include "webview_host.hpp"

#include <objbase.h>
#include <wrl.h>

namespace wireview::host {

bool WebViewHost::initialize(HWND parent, const std::filesystem::path& assets_directory,
    MessageHandler message_handler, ReadyHandler ready_handler, ErrorHandler error_handler) {
    parent_ = parent;
    assets_directory_ = assets_directory;
    message_handler_ = std::move(message_handler);
    ready_handler_ = std::move(ready_handler);
    error_handler_ = std::move(error_handler);

    const HRESULT co_result = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    com_initialized_ = SUCCEEDED(co_result);
    if (FAILED(co_result) && co_result != RPC_E_CHANGED_MODE) {
        error_handler_(co_result);
        return false;
    }

    const HRESULT create_result = CreateCoreWebView2EnvironmentWithOptions(
        nullptr, nullptr, nullptr,
        Microsoft::WRL::Callback<
            ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
            [this](HRESULT result, ICoreWebView2Environment* environment) -> HRESULT {
                if (FAILED(result) || environment == nullptr) {
                    error_handler_(FAILED(result) ? result : E_FAIL);
                    return S_OK;
                }
                environment_ = environment;
                return environment_->CreateCoreWebView2Controller(
                    parent_,
                    Microsoft::WRL::Callback<
                        ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                        [this](HRESULT controller_result,
                            ICoreWebView2Controller* controller) -> HRESULT {
                            if (FAILED(controller_result) || controller == nullptr) {
                                error_handler_(FAILED(controller_result)
                                    ? controller_result
                                    : E_FAIL);
                                return S_OK;
                            }
                            controller_ = controller;
                            // The legacy parent window still owns the Win32 paint surface.  An
                            // opaque WebView background prevents that surface from showing
                            // through while Chromium is navigating or re-compositing a page.
                            Microsoft::WRL::ComPtr<ICoreWebView2Controller2> controller2;
                            if (SUCCEEDED(controller_.As(&controller2))) {
                                controller2->put_DefaultBackgroundColor(
                                    COREWEBVIEW2_COLOR{255U, 7U, 17U, 31U});
                            }
                            if (FAILED(controller_->get_CoreWebView2(&webview_))) {
                                error_handler_(E_FAIL);
                                return S_OK;
                            }
                            Microsoft::WRL::ComPtr<ICoreWebView2Settings> settings;
                            if (SUCCEEDED(webview_->get_Settings(&settings))) {
                                settings->put_AreDevToolsEnabled(FALSE);
                                settings->put_AreDefaultContextMenusEnabled(FALSE);
                                settings->put_IsStatusBarEnabled(FALSE);
                                settings->put_IsZoomControlEnabled(FALSE);
                            }
                            Microsoft::WRL::ComPtr<ICoreWebView2_3> webview3;
                            if (FAILED(webview_.As(&webview3))) {
                                error_handler_(E_NOINTERFACE);
                                return S_OK;
                            }
                            webview3->SetVirtualHostNameToFolderMapping(
                                L"wireview.local", assets_directory_.c_str(),
                                COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_DENY_CORS);
                            webview_->add_WebMessageReceived(
                                Microsoft::WRL::Callback<
                                    ICoreWebView2WebMessageReceivedEventHandler>(
                                    [this](ICoreWebView2*,
                                        ICoreWebView2WebMessageReceivedEventArgs* args)
                                        -> HRESULT {
                                        LPWSTR message{};
                                        if (SUCCEEDED(args->TryGetWebMessageAsString(&message)) &&
                                            message != nullptr) {
                                            const std::wstring value(message);
                                            CoTaskMemFree(message);
                                            message_handler_(value);
                                        }
                                        return S_OK;
                                    }).Get(), nullptr);
                            webview_->add_NavigationCompleted(
                                Microsoft::WRL::Callback<
                                    ICoreWebView2NavigationCompletedEventHandler>(
                                    [this](ICoreWebView2*,
                                        ICoreWebView2NavigationCompletedEventArgs*) -> HRESULT {
                                        resize();
                                        ready_handler_();
                                        return S_OK;
                                    }).Get(), nullptr);
                            resize();
                            webview_->Navigate(L"https://wireview.local/index.html");
                            return S_OK;
                        }).Get());
            }).Get());
    if (FAILED(create_result)) {
        error_handler_(create_result);
        return false;
    }
    return true;
}

void WebViewHost::resize() {
    if (controller_ == nullptr || parent_ == nullptr) {
        return;
    }
    RECT bounds{};
    GetClientRect(parent_, &bounds);
    controller_->put_Bounds(bounds);
}

void WebViewHost::post_json(const std::wstring& json) const {
    if (webview_ != nullptr) {
        webview_->PostWebMessageAsJson(json.c_str());
    }
}

void WebViewHost::shutdown() {
    if (controller_ != nullptr) {
        // Close the controller before releasing COM references. This asks
        // Chromium to detach its child HWND promptly during tray-exit, rather
        // than leaving the parent window visibly blank while teardown runs.
        controller_->Close();
    }
    webview_.Reset();
    controller_.Reset();
    environment_.Reset();
    if (com_initialized_) {
        CoUninitialize();
        com_initialized_ = false;
    }
}

}  // namespace wireview::host
