#pragma once

#include <Windows.h>
#include <WebView2.h>
#include <wrl/client.h>

#include <functional>
#include <filesystem>
#include <string>
#include <string_view>

namespace wireview::host {

// A deliberately small WebView2 wrapper.  It owns no WireView business state:
// callers provide commands and state snapshots through the two callbacks.
class WebViewHost final {
public:
    using MessageHandler = std::function<void(std::wstring_view)>;
    using ReadyHandler = std::function<void()>;
    using ErrorHandler = std::function<void(HRESULT)>;

    bool initialize(HWND parent, const std::filesystem::path& assets_directory,
        MessageHandler message_handler, ReadyHandler ready_handler,
        ErrorHandler error_handler);
    void resize();
    void post_json(const std::wstring& json) const;
    void shutdown();
    [[nodiscard]] bool ready() const { return webview_ != nullptr; }

private:
    HWND parent_{};
    bool com_initialized_{};
    std::filesystem::path assets_directory_;
    MessageHandler message_handler_;
    ReadyHandler ready_handler_;
    ErrorHandler error_handler_;
    Microsoft::WRL::ComPtr<ICoreWebView2Environment> environment_;
    Microsoft::WRL::ComPtr<ICoreWebView2Controller> controller_;
    Microsoft::WRL::ComPtr<ICoreWebView2> webview_;
};

}  // namespace wireview::host
