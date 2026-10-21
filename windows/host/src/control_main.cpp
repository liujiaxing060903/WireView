#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <Shellapi.h>
#include <dwmapi.h>
#include <commctrl.h>
#include <gdiplus.h>
#include <uxtheme.h>

#include "parsec_vdd.hpp"
#include "parsec_vdd_custom_modes.hpp"
#ifdef WIREVIEW_WEBVIEW_UI
#include "webview_host.hpp"
#endif

#include <algorithm>
#include <array>
#include <cstdint>
#include <cwctype>
#include <cwchar>
#include <exception>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

constexpr int kStartMirrorButton = 1001;
constexpr int kStartInputButton = 1002;
constexpr int kStopButton = 1003;
constexpr int kStatusText = 1004;
constexpr int kResolutionSelector = 1005;
constexpr int kFrameRateSelector = 1006;
constexpr int kDisplaySelector = 1007;
constexpr int kRefreshDisplaysButton = 1008;
constexpr int kCreateVddButton = 1009;
constexpr int kRemoveVddButton = 1010;
constexpr int kRecoverVddButton = 1011;
constexpr int kPreviewDeviceButton = 1013;
constexpr int kCustomModesButton = 1014;
constexpr int kVddModeSelector = 1015;
constexpr int kIdentifyDeviceButton = 1016;
constexpr int kSimpleModeToggle = 1017;
constexpr int kSimpleMirrorButton = 1018;
constexpr int kSimpleInputButton = 1019;
constexpr int kSimpleExtendButton = 1020;
constexpr int kSimpleExtendInputButton = 1021;
constexpr int kSimpleReturnButton = 1022;
constexpr int kDisplaySettingsButton = 1023;
constexpr int kInputDisplayOnlyButton = 1024;
constexpr int kInputEnabledButton = 1025;
constexpr int kFullModeToggle = 1026;
constexpr int kSimpleDisplayOnlyButton = 1027;
constexpr int kSimpleInputEnabledButton = 1028;
constexpr int kSimpleStopButton = 1029;
constexpr int kSimpleDisplaySettingsButton = 1030;
constexpr int kDisplayConfigToggle = 1031;
constexpr int kDiagnosticsButton = 1032;
constexpr int kDiagnosticsBackButton = 1033;
constexpr int kCheckUpdateButton = 1034;
constexpr int kDiagnosticsOutput = 1035;
constexpr int kCustomModeSaveButton = 1100;
constexpr int kCustomModeEditBase = 1110;
constexpr int kWireViewAppIcon = 200;
constexpr int kCustomModeFieldTop = 128;
constexpr int kCustomModeFieldStep = 45;
constexpr UINT_PTR kProcessTimer = 1;
constexpr UINT_PTR kVddTimer = 2;
constexpr UINT_PTR kDeviceDetectTimer = 3;
constexpr UINT kTrayMessage = WM_APP + 1U;
constexpr UINT kFinalizeControlExit = WM_APP + 2U;
constexpr UINT kTrayRestore = 2001U;
constexpr UINT kTrayStartMirror = 2002U;
constexpr UINT kTrayStartInput = 2003U;
constexpr UINT kTrayStop = 2004U;
constexpr UINT kTrayExit = 2005U;
constexpr wchar_t kControlCenterInstanceMutex[] = L"Local\\WireView.ControlCenter.Singleton.1";
struct DisplayChoice {
    std::wstring device_name;
    std::wstring label;
    DWORD refresh_hz{};
    bool primary{false};
    bool parsec_vdd{false};
};

struct DeviceModelRecord {
    std::wstring normalized_model;
    std::wstring name;
    bool tablet{false};
};

struct CustomMode {
    std::uint16_t width{};
    std::uint16_t height{};
    std::uint16_t refresh_hz{};
    bool operator==(const CustomMode&) const = default;
};

struct StandardModeGroup {
    std::wstring_view resolution;
    std::initializer_list<std::uint16_t> refresh_rates;
};

// 与 Parsec VDD 安装后默认提供的模式对应；自定义槽只用于这里没有的模式。
const std::array<StandardModeGroup, 27U> kStandardModeGroups{{
    {L"4096x2160", {24U, 30U, 60U, 144U, 240U}},
    {L"3840x2160", {24U, 30U, 60U, 144U, 240U}},
    {L"3840x1600", {24U, 30U, 60U, 144U, 240U}},
    {L"3840x1080", {24U, 30U, 60U, 144U, 240U}},
    {L"3440x1440", {24U, 30U, 60U, 144U, 240U}},
    {L"3240x2160", {60U}}, {L"3200x1800", {24U, 30U, 60U, 144U, 240U}},
    {L"3000x2000", {60U}}, {L"2880x1800", {60U}},
    {L"2880x1620", {24U, 30U, 60U, 144U, 240U}}, {L"2736x1824", {60U}},
    {L"2560x1600", {24U, 30U, 60U, 144U, 240U}},
    {L"2560x1440", {24U, 30U, 60U, 144U, 240U}},
    {L"2560x1080", {24U, 30U, 60U, 144U, 240U}}, {L"2496x1664", {60U}},
    {L"2256x1504", {60U}}, {L"2048x1152", {60U, 144U, 240U}},
    {L"1920x1200", {60U, 144U, 240U}},
    {L"1920x1080", {24U, 30U, 60U, 144U, 240U}}, {L"1800x1200", {60U}},
    {L"1680x1050", {24U, 30U, 60U, 144U, 240U}},
    {L"1600x1200", {24U, 30U, 60U, 144U, 240U}},
    {L"1600x900", {60U, 144U, 240U}},
    {L"1440x900", {24U, 30U, 60U, 144U, 240U}}, {L"1366x768", {60U}},
    {L"1280x800", {24U, 30U, 60U, 144U, 240U}},
    {L"1280x720", {24U, 30U, 60U, 144U, 240U}},
}};

struct RecoveredVddSession {
    std::filesystem::path status_file;
    std::wstring display_device;
    DWORD owner_process_id{};
    DWORD broker_process_id{};
    int display_index{-1};
    bool owner_alive{false};
    bool broker_alive{false};
    bool mapping_matches{false};
};

struct ControlState {
    HANDLE shell_process{nullptr};
    HANDLE device_probe_process{nullptr};
    HANDLE session_job{nullptr};
    HWND window{nullptr};
    HWND title{nullptr};
    HWND status{nullptr};
    HWND simple_window{nullptr};
    HWND simple_status{nullptr};
    HWND simple_mode_toggle{nullptr};
    HWND full_mode_toggle{nullptr};
    HWND simple_mirror_button{nullptr};
    HWND simple_input_button{nullptr};
    HWND simple_display_only_button{nullptr};
    HWND simple_extend_button{nullptr};
    HWND simple_extend_input_button{nullptr};
    HWND display_selector{nullptr};
    HWND refresh_displays_button{nullptr};
    HWND vdd_status{nullptr};
    HWND create_vdd_button{nullptr};
    HWND remove_vdd_button{nullptr};
    HWND recover_vdd_button{nullptr};
    HWND resolution_selector{nullptr};
    HWND frame_rate_selector{nullptr};
    HWND vdd_mode_selector{nullptr};
    HWND recommendation{nullptr};
    HWND start_mirror_button{nullptr};
    HWND start_input_button{nullptr};
    HWND full_display_only_button{nullptr};
    HWND stop_button{nullptr};
    HWND display_settings_button{nullptr};
    HWND simple_stop_button{nullptr};
    HWND simple_display_settings_button{nullptr};
    HWND display_config_toggle{nullptr};
    HWND diagnostics_button{nullptr};
    HWND diagnostics_back_button{nullptr};
    HWND check_update_button{nullptr};
    HWND diagnostics_output{nullptr};
    bool tray_added{false};
    std::filesystem::path session_launcher_path;
    std::filesystem::path vdd_broker_path;
    std::filesystem::path status_file;
    std::filesystem::path host_diagnostics_file;
    std::filesystem::path last_host_diagnostics_file;
    std::filesystem::path settings_file;
    std::filesystem::path custom_modes_file;
    std::filesystem::path runtime_log_file;
    std::filesystem::path vdd_status_file;
    std::filesystem::path device_model_catalog_file;
    std::filesystem::path device_model_cache_file;
    std::wstring last_script_status;
    std::wstring current_status;
    // 最近一次会话的 Host 原始输出不能依赖临时 status 文件：手动停止会关闭
    // Job，启动器来不及写最终状态。这里保留独立实时日志的快照供诊断页导出。
    std::wstring last_host_diagnostics;
    std::wstring vdd_stop_event_name;
    std::wstring owned_display_device;
    std::vector<DisplayChoice> display_choices;
    HANDLE vdd_broker_process{nullptr};
    HANDLE vdd_stop_event{nullptr};
    int owned_display_index{-1};
    bool vdd_stop_requested{false};
    bool exiting{false};
    std::wstring last_vdd_broker_log_state;
    // Preserve the already-validated creation request. The selectors become
    // intentionally locked while the broker creates the display.
    std::uint16_t pending_vdd_width{};
    std::uint16_t pending_vdd_height{};
    std::uint16_t pending_vdd_refresh_hz{};
    bool preview_active{false};
    bool profile_probe_active{false};
    bool auto_device_profile_completed{false};
    bool auto_device_profile_attempted{false};
    // 用户点击“刷新”时，即使同一条 USB 连接此前已经自动识别过，也重新向
    // Android 应用请求一次当前屏幕模式（分辨率、当前刷新率和最高刷新率）。
    bool device_profile_refresh_requested{false};
    bool device_profile_requires_custom_mode{false};
    bool simple_mode{false};
    // WebView2 的页面选择状态。它不替代 VDD/会话后端，只用于在页面上解释
    // 当前准备启动复制还是扩展，并驱动简易模式的大卡片入口。
    bool web_extend_mode{false};
    bool display_config_expanded{false};
    bool diagnostics_page{false};
    bool session_input_enabled{false};
    bool start_after_vdd_ready{false};
    bool start_after_vdd_input{false};
    bool start_after_vdd_removal{false};
    bool start_after_vdd_removal_input{false};
    // A simple-mode extension always belongs to the VDD that this control
    // center owns. If Windows has detached that VDD, safely recreate only
    // this known instance instead of sending a stale DISPLAYn to the host.
    bool recreate_vdd_for_simple_extension{false};
    bool recreate_vdd_for_simple_extension_input{false};
    DWORD last_auto_device_probe_exit{MAXDWORD};
    std::wstring device_profile_note;
    std::wstring device_model{L"Android"};
    std::wstring device_name{L"Android 设备"};
    std::wstring device_active_refresh{L"--"};
    std::vector<DeviceModelRecord> device_model_catalog;
    bool device_model_catalog_loaded{false};
    bool device_connected{false};
    bool wireview_android_app_seen{false};
    bool device_is_tablet{true};
    bool parsec_vdd_available{false};
    std::uint32_t parsec_vdd_version{};
    std::size_t parsec_display_count{};
    std::uint16_t detected_device_width{};
    std::uint16_t detected_device_height{};
    std::wstring detected_device_max_refresh{L"--"};
    std::optional<RecoveredVddSession> recovered_vdd_session;
    std::size_t recovered_vdd_record_count{0U};
};

ControlState g_state;
std::array<std::optional<CustomMode>, 5U> g_custom_modes;
HWND g_custom_mode_editor{nullptr};
std::array<std::array<HWND, 3U>, 5U> g_custom_mode_edits{};
HWND g_diagnostics_window{nullptr};
HWND g_diagnostics_text{nullptr};
bool g_popup_light_theme{false};

#ifdef WIREVIEW_WEBVIEW_UI
std::unique_ptr<wireview::host::WebViewHost> g_webview_host;
void publish_web_state();
void handle_web_message(std::wstring_view message);
#endif

constexpr COLORREF kColorApp = RGB(7, 17, 31);
constexpr COLORREF kColorPanel = RGB(14, 24, 39);
constexpr COLORREF kColorPanelElevated = RGB(17, 29, 45);
constexpr COLORREF kColorControl = RGB(18, 29, 44);
constexpr COLORREF kColorBorder = RGB(38, 51, 72);
constexpr COLORREF kColorBorderHover = RGB(52, 71, 97);
constexpr COLORREF kColorText = RGB(244, 247, 251);
constexpr COLORREF kColorTextSecondary = RGB(215, 222, 232);
constexpr COLORREF kColorTextMuted = RGB(169, 180, 195);
constexpr COLORREF kColorTextSubtle = RGB(127, 138, 154);
constexpr COLORREF kColorBlue = RGB(61, 123, 255);
constexpr COLORREF kColorBlueStrong = RGB(47, 107, 255);
constexpr COLORREF kColorViolet = RGB(138, 92, 255);
constexpr COLORREF kColorGreen = RGB(98, 216, 117);
constexpr COLORREF kColorRed = RGB(255, 92, 100);

// Owner-drawn BUTTON controls do not receive a dependable hover flag on every
// Windows theme. Track it ourselves so every clickable surface has the same
// default / hover / pressed / disabled feedback.
HWND g_hot_button{};
HWND g_pressed_button{};

void enable_per_monitor_dpi_awareness() {
    const auto set_context = reinterpret_cast<BOOL(WINAPI*)(DPI_AWARENESS_CONTEXT)>(
        GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetProcessDpiAwarenessContext"));
    if (set_context != nullptr &&
        set_context(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2) != FALSE) {
        return;
    }
    (void)SetProcessDPIAware();
}

void restore_existing_control_center() {
    // A second launch can race the first process before its main window has
    // been registered, so retry briefly instead of silently doing nothing.
    HWND existing{};
    for (int attempt = 0; attempt < 20 && existing == nullptr; ++attempt) {
        existing = FindWindowW(L"WireViewControlWindow", nullptr);
        if (existing == nullptr) {
            Sleep(50U);
        }
    }
    if (existing == nullptr) {
        return;
    }
    ShowWindow(existing, IsIconic(existing) ? SW_RESTORE : SW_SHOW);
    SetForegroundWindow(existing);
}

LRESULT CALLBACK owner_draw_button_subclass(
    HWND button, UINT message, WPARAM wparam, LPARAM lparam,
    UINT_PTR, DWORD_PTR) {
    switch (message) {
        case WM_MOUSEMOVE: {
            if (g_hot_button != button) {
                HWND previous = g_hot_button;
                g_hot_button = button;
                if (previous != nullptr) {
                    InvalidateRect(previous, nullptr, FALSE);
                }
                TRACKMOUSEEVENT tracking{sizeof(tracking), TME_LEAVE, button, 0U};
                TrackMouseEvent(&tracking);
                InvalidateRect(button, nullptr, FALSE);
            }
            break;
        }
        case WM_MOUSELEAVE:
            if (g_hot_button == button) {
                g_hot_button = nullptr;
                InvalidateRect(button, nullptr, FALSE);
            }
            break;
        case WM_LBUTTONDOWN:
            g_pressed_button = button;
            InvalidateRect(button, nullptr, FALSE);
            break;
        case WM_LBUTTONUP:
        case WM_CAPTURECHANGED:
            if (g_pressed_button == button) {
                g_pressed_button = nullptr;
                InvalidateRect(button, nullptr, FALSE);
            }
            break;
        case WM_ENABLE:
            InvalidateRect(button, nullptr, FALSE);
            break;
        case WM_NCDESTROY:
            if (g_hot_button == button) {
                g_hot_button = nullptr;
            }
            if (g_pressed_button == button) {
                g_pressed_button = nullptr;
            }
            RemoveWindowSubclass(button, owner_draw_button_subclass, 1U);
            break;
    }
    return DefSubclassProc(button, message, wparam, lparam);
}

void enable_owner_draw_button_feedback(HWND button) {
    if (button != nullptr) {
        (void)SetWindowSubclass(button, owner_draw_button_subclass, 1U, 0U);
    }
}

HFONT g_font_title{};
HFONT g_font_heading{};
HFONT g_font_device{};
HFONT g_font_body{};
HFONT g_font_small{};
HFONT g_font_button{};
HFONT g_font_popup_heading{};
HFONT g_font_popup_body{};
HFONT g_font_popup_small{};
HBRUSH g_background_brush{};
HBRUSH g_control_brush{};
HBRUSH g_popup_light_background_brush{};
HBRUSH g_popup_light_control_brush{};
ULONG_PTR g_gdiplus_token{};
std::unique_ptr<Gdiplus::Image> g_copy_screen_asset;
std::unique_ptr<Gdiplus::Image> g_extend_screen_asset;
std::unique_ptr<Gdiplus::Image> g_display_only_asset;
std::unique_ptr<Gdiplus::Image> g_touch_pen_asset;

HFONT create_ui_font(int height, int weight) {
    return CreateFontW(-height, 0, 0, 0, weight, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
}

void initialize_ui_theme() {
    if (g_font_body != nullptr) {
        return;
    }
    g_font_title = create_ui_font(20, FW_SEMIBOLD);
    g_font_heading = create_ui_font(18, FW_SEMIBOLD);
    g_font_device = create_ui_font(27, FW_SEMIBOLD);
    g_font_body = create_ui_font(15, FW_NORMAL);
    g_font_small = create_ui_font(13, FW_NORMAL);
    g_font_button = create_ui_font(16, FW_SEMIBOLD);
    // 独立弹窗可以使用更舒适的字号，不挤压控制中心的紧凑主画布。
    g_font_popup_heading = create_ui_font(22, FW_SEMIBOLD);
    g_font_popup_body = create_ui_font(17, FW_NORMAL);
    g_font_popup_small = create_ui_font(15, FW_NORMAL);
    g_background_brush = CreateSolidBrush(kColorApp);
    g_control_brush = CreateSolidBrush(kColorControl);
    g_popup_light_background_brush = CreateSolidBrush(RGB(246, 249, 253));
    g_popup_light_control_brush = CreateSolidBrush(RGB(242, 247, 253));
}

void destroy_ui_theme() {
    for (HFONT font : {g_font_title, g_font_heading, g_font_device, g_font_body,
             g_font_small, g_font_button, g_font_popup_heading, g_font_popup_body,
             g_font_popup_small}) {
        if (font != nullptr) {
            DeleteObject(font);
        }
    }
    g_font_title = nullptr;
    g_font_heading = nullptr;
    g_font_device = nullptr;
    g_font_body = nullptr;
    g_font_small = nullptr;
    g_font_button = nullptr;
    g_font_popup_heading = nullptr;
    g_font_popup_body = nullptr;
    g_font_popup_small = nullptr;
    if (g_background_brush != nullptr) {
        DeleteObject(g_background_brush);
        g_background_brush = nullptr;
    }
    if (g_control_brush != nullptr) {
        DeleteObject(g_control_brush);
        g_control_brush = nullptr;
    }
    if (g_popup_light_background_brush != nullptr) {
        DeleteObject(g_popup_light_background_brush);
        g_popup_light_background_brush = nullptr;
    }
    if (g_popup_light_control_brush != nullptr) {
        DeleteObject(g_popup_light_control_brush);
        g_popup_light_control_brush = nullptr;
    }
}

void enable_dark_title_bar(HWND window) {
    constexpr DWORD kImmersiveDarkMode = 20U;
    const BOOL enabled = TRUE;
    (void)DwmSetWindowAttribute(
        window, kImmersiveDarkMode, &enabled, static_cast<DWORD>(sizeof(enabled)));
}

void apply_control_theme(HWND control, HFONT font = nullptr) {
    if (control == nullptr) {
        return;
    }
    SendMessageW(control, WM_SETFONT,
        reinterpret_cast<WPARAM>(font != nullptr ? font : g_font_body), TRUE);
    (void)SetWindowTheme(control, L"DarkMode_Explorer", nullptr);
}

COLORREF popup_background_color() {
    return g_popup_light_theme ? RGB(246, 249, 253) : kColorApp;
}

COLORREF popup_control_color() {
    return g_popup_light_theme ? RGB(242, 247, 253) : kColorControl;
}

COLORREF popup_panel_elevated_color() {
    return g_popup_light_theme ? RGB(233, 240, 248) : kColorPanelElevated;
}

COLORREF popup_border_color() {
    return g_popup_light_theme ? RGB(190, 204, 222) : kColorBorder;
}

COLORREF popup_border_hover_color() {
    return g_popup_light_theme ? RGB(143, 167, 198) : kColorBorderHover;
}

COLORREF popup_text_color() {
    return g_popup_light_theme ? RGB(25, 39, 57) : kColorText;
}

COLORREF popup_text_secondary_color() {
    return g_popup_light_theme ? RGB(57, 74, 96) : kColorTextSecondary;
}

HBRUSH popup_background_brush() {
    return g_popup_light_theme ? g_popup_light_background_brush : g_background_brush;
}

HBRUSH popup_control_brush() {
    return g_popup_light_theme ? g_popup_light_control_brush : g_control_brush;
}

void apply_popup_control_theme(HWND control, HFONT font = nullptr) {
    if (control == nullptr) {
        return;
    }
    SendMessageW(control, WM_SETFONT,
        reinterpret_cast<WPARAM>(font != nullptr ? font : g_font_popup_body), TRUE);
    (void)SetWindowTheme(control, g_popup_light_theme ? L"Explorer" : L"DarkMode_Explorer", nullptr);
}

void apply_popup_title_bar_theme(HWND window) {
    constexpr DWORD kImmersiveDarkMode = 20U;
    const BOOL enabled = g_popup_light_theme ? FALSE : TRUE;
    (void)DwmSetWindowAttribute(
        window, kImmersiveDarkMode, &enabled, static_cast<DWORD>(sizeof(enabled)));
}

BOOL CALLBACK refresh_popup_child_theme(HWND child, LPARAM) {
    (void)SetWindowTheme(child, g_popup_light_theme ? L"Explorer" : L"DarkMode_Explorer", nullptr);
    return TRUE;
}

void refresh_popup_theme(HWND window) {
    if (window == nullptr) {
        return;
    }
    apply_popup_title_bar_theme(window);
    EnumChildWindows(window, refresh_popup_child_theme, 0);
    RedrawWindow(window, nullptr, nullptr,
        RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_UPDATENOW);
}

struct StreamSettings;
std::optional<StreamSettings> selected_stream_settings();
void stop_session();
bool start_session(HWND window, bool enable_input, bool device_preview, bool device_profile_probe);
void begin_simple_extension(bool enable_input);
void set_status(const std::wstring& text);
std::string wide_to_utf8(std::wstring_view value);
void log_control_event(std::wstring_view category, std::wstring detail);
std::wstring describe_display_choices_for_log();
void update_recommendation();
void show_custom_mode_editor(HWND owner);
void show_diagnostics_window(HWND owner);
void notify_android_of_vdd_created(const StreamSettings& settings);

struct StreamSettings {
    std::uint16_t width{};
    std::uint16_t height{};
    std::uint16_t frame_rate{60U};
    std::uint16_t vdd_refresh_hz{60U};
    std::uint16_t vdd_width{};
    std::uint16_t vdd_height{};
    std::wstring display_device;
};

std::wstring selector_text(HWND selector);
void refresh_diagnostics_output();

void save_stream_settings(const StreamSettings& settings) {
    if (g_state.settings_file.empty()) {
        return;
    }
    std::error_code error;
    std::filesystem::create_directories(g_state.settings_file.parent_path(), error);
    if (error) {
        return;
    }
    std::ofstream output(g_state.settings_file, std::ios::trunc);
    if (!output) {
        return;
    }
    if (settings.width == 0U) {
        output << "resolution=auto\n";
    } else {
        output << "resolution=" << settings.width << 'x' << settings.height << '\n';
    }
    output << "frame_rate=" << settings.frame_rate << '\n';
    output << "vdd_mode=" << settings.vdd_width << 'x' << settings.vdd_height << '@'
           << settings.vdd_refresh_hz << '\n';
    // 控制中心的日常路径固定使用 ADB；WinUSB 实验入口仍保留在命令行工具中。
    output << "transport=adb\n";
    if (!settings.display_device.empty()) {
        output << "display=";
        for (const wchar_t character : settings.display_device) {
            output << static_cast<char>(character);
        }
        output << '\n';
    }
}

std::wstring format_custom_mode(const CustomMode& mode) {
    return std::to_wstring(mode.width) + L"x" + std::to_wstring(mode.height) + L" @ " +
        std::to_wstring(mode.refresh_hz) + L" Hz";
}

std::optional<CustomMode> parse_custom_mode(std::wstring_view text) {
    const std::size_t separator = text.find_first_of(L"xX");
    const std::size_t at = text.find(L'@');
    if (separator == std::wstring_view::npos || at == std::wstring_view::npos ||
        separator == 0U || separator + 1U >= at || at + 1U >= text.size()) {
        return std::nullopt;
    }
    try {
        const unsigned long width = std::stoul(std::wstring(text.substr(0U, separator)));
        const unsigned long height = std::stoul(
            std::wstring(text.substr(separator + 1U, at - separator - 1U)));
        const unsigned long refresh = std::stoul(std::wstring(text.substr(at + 1U)));
        if (width == 0UL || height == 0UL || refresh == 0UL || width > 65535UL ||
            height > 65535UL || refresh > 1000UL || (width % 2UL) != 0UL ||
            (height % 2UL) != 0UL) {
            return std::nullopt;
        }
        return CustomMode{
            static_cast<std::uint16_t>(width),
            static_cast<std::uint16_t>(height),
            static_cast<std::uint16_t>(refresh)};
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

void save_custom_modes() {
    if (g_state.custom_modes_file.empty()) {
        return;
    }
    std::error_code error;
    std::filesystem::create_directories(g_state.custom_modes_file.parent_path(), error);
    if (error) {
        return;
    }
    std::ofstream output(g_state.custom_modes_file, std::ios::trunc);
    if (!output) {
        return;
    }
    output << "version=1\n";
    for (std::size_t index = 0U; index < g_custom_modes.size(); ++index) {
        output << "slot" << (index + 1U) << '=';
        if (g_custom_modes[index]) {
            const CustomMode& mode = *g_custom_modes[index];
            output << mode.width << 'x' << mode.height << '@' << mode.refresh_hz;
        }
        output << '\n';
    }
}

void load_custom_modes() {
    g_custom_modes.fill(std::nullopt);
    if (g_state.custom_modes_file.empty()) {
        return;
    }
    std::ifstream input(g_state.custom_modes_file);
    std::string line;
    while (std::getline(input, line)) {
        if (line.rfind("slot", 0U) != 0U || line.size() < 7U || line[5U] != '=') {
            continue;
        }
        const char slot_character = line[4U];
        if (slot_character < '1' || slot_character > '5') {
            continue;
        }
        const std::string value = line.substr(6U);
        if (value.empty()) {
            continue;
        }
        const std::wstring text(value.begin(), value.end());
        g_custom_modes[static_cast<std::size_t>(slot_character - '1')] = parse_custom_mode(text);
    }
}

std::optional<std::pair<std::uint16_t, std::uint16_t>> parse_resolution(
    std::wstring_view text) {
    const std::size_t separator = text.find_first_of(L"xX");
    if (separator == std::wstring_view::npos || separator == 0U || separator + 1U >= text.size()) {
        return std::nullopt;
    }
    try {
        const unsigned long width = std::stoul(std::wstring(text.substr(0U, separator)));
        const unsigned long height = std::stoul(std::wstring(text.substr(separator + 1U)));
        if (width == 0UL || height == 0UL || width > 65535UL || height > 65535UL ||
            (width % 2UL) != 0UL || (height % 2UL) != 0UL) {
            return std::nullopt;
        }
        return std::pair{
            static_cast<std::uint16_t>(width), static_cast<std::uint16_t>(height)};
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

bool select_resolution_option(
    HWND selector, std::pair<std::uint16_t, std::uint16_t> resolution) {
    const LRESULT count = SendMessageW(selector, CB_GETCOUNT, 0U, 0);
    for (LRESULT index = 0; index < count; ++index) {
        const LRESULT length = SendMessageW(selector, CB_GETLBTEXTLEN, index, 0);
        if (length < 0) {
            continue;
        }
        std::wstring item(static_cast<std::size_t>(length) + 1U, L'\0');
        SendMessageW(selector, CB_GETLBTEXT, index, reinterpret_cast<LPARAM>(item.data()));
        item.resize(static_cast<std::size_t>(length));
        if (const auto parsed = parse_resolution(item); parsed && *parsed == resolution) {
            SendMessageW(selector, CB_SETCURSEL, static_cast<WPARAM>(index), 0);
            return true;
        }
    }
    return false;
}

void populate_resolution_selector(std::wstring preferred = {}) {
    if (g_state.resolution_selector == nullptr) {
        return;
    }
    if (preferred.empty()) {
        preferred = selector_text(g_state.resolution_selector);
    }
    SendMessageW(g_state.resolution_selector, CB_RESETCONTENT, 0U, 0);
    const auto requested_resolution = parse_resolution(
        preferred.empty() ? std::wstring_view{L"2560x1600"} : std::wstring_view{preferred});
    std::vector<std::pair<std::uint16_t, std::uint16_t>> custom_resolutions;
    for (std::size_t index = 0U; index < g_custom_modes.size(); ++index) {
        if (g_custom_modes[index]) {
            const auto resolution = std::pair{
                g_custom_modes[index]->width, g_custom_modes[index]->height};
            custom_resolutions.push_back(resolution);
            const std::wstring label = std::to_wstring(g_custom_modes[index]->width) + L"x" +
                std::to_wstring(g_custom_modes[index]->height) + L"（自定义槽 " +
                std::to_wstring(index + 1U) + L"）";
            SendMessageW(g_state.resolution_selector, CB_ADDSTRING, 0U,
                reinterpret_cast<LPARAM>(label.c_str()));
        }
    }

    const auto is_custom_resolution = [&custom_resolutions](
                                          std::pair<std::uint16_t, std::uint16_t> resolution) {
        return std::find(custom_resolutions.begin(), custom_resolutions.end(), resolution) !=
            custom_resolutions.end();
    };
    // Keep the currently selected (or persisted) standard mode immediately after
    // custom slots, then retain the familiar built-in ordering for everything else.
    if (requested_resolution && !is_custom_resolution(*requested_resolution)) {
        for (const StandardModeGroup& group : kStandardModeGroups) {
            const auto standard_resolution = parse_resolution(group.resolution);
            if (standard_resolution && *standard_resolution == *requested_resolution) {
                SendMessageW(g_state.resolution_selector, CB_ADDSTRING, 0U,
                    reinterpret_cast<LPARAM>(group.resolution.data()));
                break;
            }
        }
    }
    for (const StandardModeGroup& group : kStandardModeGroups) {
        const auto standard_resolution = parse_resolution(group.resolution);
        if (!standard_resolution || is_custom_resolution(*standard_resolution) ||
            (requested_resolution && *standard_resolution == *requested_resolution)) {
            continue;
        }
        SendMessageW(g_state.resolution_selector, CB_ADDSTRING, 0U,
            reinterpret_cast<LPARAM>(group.resolution.data()));
    }
    if (!requested_resolution || !select_resolution_option(
            g_state.resolution_selector, *requested_resolution)) {
        SendMessageW(g_state.resolution_selector, CB_SETCURSEL, 0U, 0);
    }
}

void populate_vdd_mode_selector(std::wstring preferred = {}) {
    if (g_state.vdd_mode_selector == nullptr || g_state.resolution_selector == nullptr) {
        return;
    }
    if (preferred.empty()) {
        preferred = selector_text(g_state.vdd_mode_selector);
    }
    SendMessageW(g_state.vdd_mode_selector, CB_RESETCONTENT, 0U, 0);
    const auto resolution = parse_resolution(selector_text(g_state.resolution_selector));
    if (!resolution) {
        return;
    }
    std::vector<std::uint16_t> refresh_rates;
    for (const StandardModeGroup& group : kStandardModeGroups) {
        const auto standard_resolution = parse_resolution(group.resolution);
        if (standard_resolution && *standard_resolution == *resolution) {
            refresh_rates.insert(refresh_rates.end(), group.refresh_rates.begin(), group.refresh_rates.end());
            break;
        }
    }
    for (const auto& custom_mode : g_custom_modes) {
        if (custom_mode && custom_mode->width == resolution->first &&
            custom_mode->height == resolution->second) {
            refresh_rates.push_back(custom_mode->refresh_hz);
        }
    }
    std::sort(refresh_rates.begin(), refresh_rates.end());
    refresh_rates.erase(std::unique(refresh_rates.begin(), refresh_rates.end()), refresh_rates.end());
    for (const std::uint16_t refresh_rate : refresh_rates) {
        const std::wstring label = std::to_wstring(refresh_rate) + L" Hz";
        SendMessageW(g_state.vdd_mode_selector, CB_ADDSTRING, 0U,
            reinterpret_cast<LPARAM>(label.c_str()));
    }
    if (refresh_rates.empty()) {
        SetWindowTextW(g_state.vdd_mode_selector, L"");
        return;
    }
    const auto preferred_rate = preferred.empty() ? std::optional<std::uint16_t>{} :
        [&]() -> std::optional<std::uint16_t> {
            try {
                return static_cast<std::uint16_t>(std::stoul(preferred));
            } catch (const std::exception&) {
                return std::nullopt;
            }
        }();
    const std::uint16_t selected_rate = preferred_rate &&
            std::find(refresh_rates.begin(), refresh_rates.end(), *preferred_rate) != refresh_rates.end()
        ? *preferred_rate
        : refresh_rates.back();
    const std::wstring selected_label = std::to_wstring(selected_rate) + L" Hz";
    if (SendMessageW(g_state.vdd_mode_selector, CB_SELECTSTRING, static_cast<WPARAM>(-1),
            reinterpret_cast<LPARAM>(selected_label.c_str())) == CB_ERR) {
        SendMessageW(g_state.vdd_mode_selector, CB_SETCURSEL, 0U, 0);
    }
}

void sync_custom_modes_from_parsec_vdd() {
    // 用户正在控制中心编辑时不覆盖尚未保存的输入；关闭编辑窗口后，下一次轮询
    // 会读取 ParsecVDisplay 的 Apply 结果。
    if (g_custom_mode_editor != nullptr) {
        return;
    }
    const auto parsed_modes = wireview::host::read_parsec_vdd_custom_modes();
    if (!parsed_modes.has_value()) {
        return;
    }
    std::array<std::optional<CustomMode>, 5U> synchronized;
    for (std::size_t index = 0U; index < synchronized.size(); ++index) {
        const auto& mode = (*parsed_modes)[index];
        if (!mode || mode->width == 0U || mode->height == 0U || mode->refresh_hz == 0U ||
            mode->width > 65535U || mode->height > 65535U || mode->refresh_hz > 1000U ||
            (mode->width % 2U) != 0U || (mode->height % 2U) != 0U) {
            continue;
        }
        synchronized[index] = CustomMode{static_cast<std::uint16_t>(mode->width),
            static_cast<std::uint16_t>(mode->height), static_cast<std::uint16_t>(mode->refresh_hz)};
    }
    if (synchronized == g_custom_modes) {
        return;
    }
    g_custom_modes = synchronized;
    save_custom_modes();
    populate_resolution_selector();
    populate_vdd_mode_selector();
    update_recommendation();
    set_status(L"已同步 Parsec VDD 的自定义模式。 ");
}

void apply_detected_device_profile(
    const std::wstring& width_text,
    const std::wstring& height_text,
    const std::wstring& maximum_refresh_text) {
    try {
        const unsigned long width = std::stoul(width_text);
        const unsigned long height = std::stoul(height_text);
        const double maximum_refresh = std::wcstod(maximum_refresh_text.c_str(), nullptr);
        if (width == 0UL || height == 0UL || width > 65535UL || height > 65535UL) {
            return;
        }
        const auto resolution = std::pair{
            static_cast<std::uint16_t>(width), static_cast<std::uint16_t>(height)};
        if (g_state.detected_device_width == resolution.first &&
            g_state.detected_device_height == resolution.second &&
            !g_state.device_profile_requires_custom_mode) {
            return;
        }
        g_state.detected_device_width = resolution.first;
        g_state.detected_device_height = resolution.second;
        g_state.detected_device_max_refresh = maximum_refresh_text;
        const std::wstring profile = std::to_wstring(width) + L"×" + std::to_wstring(height) +
            L"（最高 " + maximum_refresh_text + L" Hz）";
        bool added_custom_mode = false;
        if (!select_resolution_option(g_state.resolution_selector, resolution)) {
            const auto empty_slot = std::find_if(
                g_custom_modes.begin(), g_custom_modes.end(),
                [](const std::optional<CustomMode>& mode) { return !mode.has_value(); });
            if (empty_slot == g_custom_modes.end()) {
                g_state.device_profile_requires_custom_mode = true;
                g_state.device_profile_note = L"已识别设备：" + profile +
                    L"。投屏分辨率列表没有对应项，且 5 个自定义模式槽均已占用；"
                    L"请先替换或释放一个槽，才能开始 WireView。";
                show_custom_mode_editor(g_state.window);
                return;
            }
            *empty_slot = CustomMode{
                .width = resolution.first,
                .height = resolution.second,
                .refresh_hz = 240U,
            };
            save_custom_modes();
            populate_resolution_selector(
                std::to_wstring(resolution.first) + L"x" + std::to_wstring(resolution.second));
            if (!select_resolution_option(g_state.resolution_selector, resolution)) {
                return;
            }
            added_custom_mode = true;
        }
        g_state.device_profile_requires_custom_mode = false;
        populate_vdd_mode_selector(L"240");
        SendMessageW(g_state.frame_rate_selector, CB_SETCURSEL,
            maximum_refresh >= 119.0 ? 1U : 0U, 0);
        g_state.device_profile_note = L"已识别设备：" + profile +
            L"。已自动选择对应分辨率、副屏 240 Hz 和 " +
            (maximum_refresh >= 119.0
                ? L"实验性 120 FPS 主机发送目标；设备最终屏幕帧率需实测。"
                : L"60 FPS。") +
            (added_custom_mode
                ? L"已自动写入一个 240 Hz 自定义模式槽；创建 WireView 副屏时会应用到 Parsec。"
                : L"");
        update_recommendation();
    } catch (const std::exception&) {
    }
}

std::vector<DisplayChoice> enumerate_attached_displays() {
    std::vector<DisplayChoice> choices;
    const std::vector<std::string> parsec_displays =
        wireview::host::enumerate_active_parsec_vdd_displays();
    for (DWORD index = 0U;; ++index) {
        DISPLAY_DEVICEW display{};
        display.cb = sizeof(display);
        if (EnumDisplayDevicesW(nullptr, index, &display, 0U) == FALSE) {
            break;
        }
        if ((display.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP) == 0U) {
            continue;
        }

        DEVMODEW mode{};
        mode.dmSize = sizeof(mode);
        const bool has_mode = EnumDisplaySettingsW(
            display.DeviceName, ENUM_CURRENT_SETTINGS, &mode) != FALSE;
        DisplayChoice choice;
        choice.device_name = display.DeviceName;
        choice.primary = (display.StateFlags & DISPLAY_DEVICE_PRIMARY_DEVICE) != 0U;
        const std::wstring device_description(display.DeviceString);
        const std::wstring device_name(display.DeviceName);
        choice.parsec_vdd = std::any_of(
            parsec_displays.begin(), parsec_displays.end(),
            [&device_name](const std::string& parsec_display) {
                return std::wstring(parsec_display.begin(), parsec_display.end()) == device_name;
            }) || (!g_state.owned_display_device.empty() &&
                _wcsicmp(display.DeviceName, g_state.owned_display_device.c_str()) == 0);
        choice.label = choice.device_name;
        if (has_mode) {
            choice.refresh_hz = mode.dmDisplayFrequency;
            choice.label += L" — " + std::to_wstring(mode.dmPelsWidth) + L"×" +
                std::to_wstring(mode.dmPelsHeight);
            if (mode.dmDisplayFrequency > 1U) {
                choice.label += L" @ " + std::to_wstring(mode.dmDisplayFrequency) + L" Hz";
            }
        }
        choice.label += choice.primary ? L"（主屏）" : L"（副屏）";
        if (!device_description.empty()) {
            choice.label += L" — ";
            choice.label += device_description;
        }
        choices.push_back(std::move(choice));
    }
    return choices;
}

void update_vdd_controls() {
    const auto probe = wireview::host::probe_parsec_vdd();
    const std::size_t parsec_display_count = static_cast<std::size_t>(std::count_if(
        g_state.display_choices.begin(),
        g_state.display_choices.end(),
        [](const DisplayChoice& choice) { return choice.parsec_vdd; }));
    g_state.parsec_vdd_available = probe.available;
    g_state.parsec_vdd_version = probe.version;
    g_state.parsec_display_count = parsec_display_count;
    if (g_state.vdd_status != nullptr) {
        std::wstring text;
        if (probe.available) {
            text = L"Parsec VDD：可用（版本 " + std::to_wstring(probe.version) +
                L"），当前副屏 " + std::to_wstring(parsec_display_count) + L" 个";
        } else {
            text = L"Parsec VDD：不可用或未安装";
        }
        if (!g_state.owned_display_device.empty()) {
            text += L"；WireView 持有 " + g_state.owned_display_device + L"（index " +
                std::to_wstring(g_state.owned_display_index) + L"）";
        } else if (g_state.vdd_broker_process != nullptr) {
            text += g_state.vdd_stop_requested ? L"；正在移除 WireView 副屏" :
                L"；正在创建 WireView 副屏";
        } else if (parsec_display_count != 0U) {
            text += L"；将直接复用已有副屏，禁止重复创建";
        }
        if (g_state.recovered_vdd_record_count > 1U) {
            text += L"；检测到 " + std::to_wstring(g_state.recovered_vdd_record_count) +
                L" 个残留记录，禁止自动清理";
        } else if (g_state.recovered_vdd_session) {
            const auto& recovered = *g_state.recovered_vdd_session;
            text += L"；残留候选记录 " + recovered.display_device + L"（index " +
                std::to_wstring(recovered.display_index) + L"）";
            if (recovered.owner_alive || recovered.broker_alive) {
                text += L"仍有进程存活";
            } else if (!recovered.mapping_matches) {
                text += L"映射无法确认（不会阻止创建新副屏，且不可安全清理）";
            }
        }
        SetWindowTextW(g_state.vdd_status, text.c_str());
        refresh_diagnostics_output();
    }
    InvalidateRect(g_state.window, nullptr, FALSE);
    InvalidateRect(g_state.simple_window, nullptr, FALSE);
    const bool session_idle = g_state.shell_process == nullptr || g_state.preview_active;
    if (g_state.create_vdd_button != nullptr) {
        EnableWindow(
            g_state.create_vdd_button,
            probe.available && g_state.vdd_broker_process == nullptr && session_idle &&
                parsec_display_count == 0U);
    }
    if (g_state.remove_vdd_button != nullptr) {
        EnableWindow(
            g_state.remove_vdd_button,
            g_state.vdd_broker_process != nullptr && session_idle);
    }
    if (g_state.recover_vdd_button != nullptr) {
        const bool recovery_is_safe_to_offer = g_state.recovered_vdd_record_count == 1U &&
            g_state.recovered_vdd_session &&
            !g_state.recovered_vdd_session->owner_alive &&
            !g_state.recovered_vdd_session->broker_alive &&
            g_state.recovered_vdd_session->mapping_matches &&
            g_state.vdd_broker_process == nullptr && session_idle;
        EnableWindow(g_state.recover_vdd_button, recovery_is_safe_to_offer);
    }
}

bool select_display_device(std::wstring_view device_name) {
    for (std::size_t index = 0U; index < g_state.display_choices.size(); ++index) {
        if (g_state.display_choices[index].device_name == device_name) {
            SendMessageW(
                g_state.display_selector,
                CB_SETCURSEL,
                static_cast<WPARAM>(index),
                0);
            return true;
        }
    }
    return false;
}

bool select_primary_display() {
    const auto primary = std::find_if(
        g_state.display_choices.begin(), g_state.display_choices.end(),
        [](const DisplayChoice& choice) { return choice.primary; });
    if (primary == g_state.display_choices.end()) {
        return false;
    }
    return select_display_device(primary->device_name);
}

void refresh_display_choices(std::wstring preferred_device = {}) {
    if (g_state.display_selector == nullptr) {
        return;
    }
    if (preferred_device.empty()) {
        const LRESULT selected = SendMessageW(g_state.display_selector, CB_GETCURSEL, 0U, 0);
        if (selected >= 0 && static_cast<std::size_t>(selected) < g_state.display_choices.size()) {
            preferred_device = g_state.display_choices[static_cast<std::size_t>(selected)].device_name;
        }
    }

    g_state.display_choices = enumerate_attached_displays();
    log_control_event(L"display-refresh", describe_display_choices_for_log());
    SendMessageW(g_state.display_selector, CB_RESETCONTENT, 0U, 0);
    for (const DisplayChoice& choice : g_state.display_choices) {
        SendMessageW(
            g_state.display_selector,
            CB_ADDSTRING,
            0U,
            reinterpret_cast<LPARAM>(choice.label.c_str()));
    }
    SendMessageW(g_state.display_selector, CB_SETDROPPEDWIDTH, 560U, 0);

    if (!preferred_device.empty() && select_display_device(preferred_device)) {
        update_vdd_controls();
        return;
    }
    const auto parsec_display = std::find_if(
        g_state.display_choices.begin(),
        g_state.display_choices.end(),
        [](const DisplayChoice& choice) { return choice.parsec_vdd && !choice.primary; });
    const auto secondary = parsec_display != g_state.display_choices.end()
        ? parsec_display
        : std::find_if(
              g_state.display_choices.begin(),
              g_state.display_choices.end(),
              [](const DisplayChoice& choice) { return !choice.primary; });
    const std::size_t default_index = secondary == g_state.display_choices.end() ? 0U :
        static_cast<std::size_t>(std::distance(g_state.display_choices.begin(), secondary));
    if (!g_state.display_choices.empty()) {
        SendMessageW(
            g_state.display_selector,
            CB_SETCURSEL,
            static_cast<WPARAM>(default_index),
            0);
    }
    update_vdd_controls();
}

bool apply_selected_mode_to_parsec_vdd() {
    if (g_state.shell_process != nullptr) {
        set_status(L"WireView 正在运行，无法切换 Parsec VDD 的显示模式。请先停止会话。");
        return false;
    }
    const auto resolution = parse_resolution(selector_text(g_state.resolution_selector));
    std::uint16_t refresh_hz{};
    try {
        refresh_hz = static_cast<std::uint16_t>(std::stoul(selector_text(g_state.vdd_mode_selector)));
    } catch (const std::exception&) {
    }
    if (!resolution || refresh_hz == 0U) {
        return false;
    }

    const auto first_parsec = std::find_if(
        g_state.display_choices.begin(), g_state.display_choices.end(),
        [](const DisplayChoice& choice) { return choice.parsec_vdd && !choice.primary; });
    if (first_parsec == g_state.display_choices.end()) {
        // 尚未创建副屏时，下拉框仍只保存给下一次创建使用。
        return false;
    }
    const auto another_parsec = std::find_if(
        std::next(first_parsec), g_state.display_choices.end(),
        [](const DisplayChoice& choice) { return choice.parsec_vdd && !choice.primary; });
    if (another_parsec != g_state.display_choices.end()) {
        set_status(L"检测到多个 Parsec VDD 副屏，无法判断应切换哪一个显示模式。");
        return false;
    }

    DEVMODEW current{};
    current.dmSize = sizeof(current);
    if (EnumDisplaySettingsW(first_parsec->device_name.c_str(), ENUM_CURRENT_SETTINGS, &current) == FALSE) {
        set_status(L"无法读取当前 Parsec VDD 显示模式。");
        return false;
    }
    if (current.dmPelsWidth == resolution->first && current.dmPelsHeight == resolution->second &&
        current.dmDisplayFrequency == refresh_hz) {
        return true;
    }

    bool mode_supported = false;
    for (DWORD index = 0U;; ++index) {
        DEVMODEW candidate{};
        candidate.dmSize = sizeof(candidate);
        if (EnumDisplaySettingsW(first_parsec->device_name.c_str(), index, &candidate) == FALSE) {
            break;
        }
        if (candidate.dmPelsWidth == resolution->first && candidate.dmPelsHeight == resolution->second &&
            candidate.dmDisplayFrequency == refresh_hz) {
            mode_supported = true;
            break;
        }
    }
    if (!mode_supported) {
        set_status(L"Parsec VDD 不支持 " + std::to_wstring(resolution->first) + L"×" +
            std::to_wstring(resolution->second) + L" @ " + std::to_wstring(refresh_hz) +
            L" Hz。请先在“编辑自定义分辨率”中写入对应模式。");
        return false;
    }

    current.dmPelsWidth = resolution->first;
    current.dmPelsHeight = resolution->second;
    current.dmDisplayFrequency = refresh_hz;
    current.dmFields |= DM_PELSWIDTH | DM_PELSHEIGHT | DM_DISPLAYFREQUENCY;
    const LONG result = ChangeDisplaySettingsExW(
        first_parsec->device_name.c_str(), &current, nullptr, CDS_UPDATEREGISTRY, nullptr);
    if (result != DISP_CHANGE_SUCCESSFUL) {
        set_status(L"Parsec VDD 显示模式切换失败（代码 " + std::to_wstring(result) + L"）。");
        return false;
    }
    const std::wstring device_name = first_parsec->device_name;
    refresh_display_choices(device_name);
    set_status(L"已同步 Parsec VDD：" + std::to_wstring(resolution->first) + L"×" +
        std::to_wstring(resolution->second) + L" @ " + std::to_wstring(refresh_hz) + L" Hz。");
    return true;
}

void restore_stream_settings() {
    if (g_state.settings_file.empty() || g_state.resolution_selector == nullptr ||
        g_state.frame_rate_selector == nullptr ||
        g_state.vdd_mode_selector == nullptr ||
        g_state.display_selector == nullptr) {
        return;
    }
    std::ifstream input(g_state.settings_file);
    if (!input) {
        return;
    }
    std::string line;
    std::wstring preferred_display;
    while (std::getline(input, line)) {
        if (line == "frame_rate=120") {
            SendMessageW(g_state.frame_rate_selector, CB_SETCURSEL, 1U, 0);
        } else if (line == "frame_rate=60") {
            SendMessageW(g_state.frame_rate_selector, CB_SETCURSEL, 0U, 0);
        } else if (line.rfind("vdd_mode=", 0U) == 0U) {
            const std::string value = line.substr(9U);
            if (!value.empty() && value.size() <= 32U) {
                const std::wstring mode(value.begin(), value.end());
                const auto parsed = parse_custom_mode(mode);
                if (parsed) {
                    (void)select_resolution_option(
                        g_state.resolution_selector, {parsed->width, parsed->height});
                    populate_vdd_mode_selector(std::to_wstring(parsed->refresh_hz));
                }
            }
        } else if (line.rfind("display=", 0U) == 0U) {
            const std::string value = line.substr(8U);
            preferred_display.assign(value.begin(), value.end());
        }
    }
    if (!preferred_display.empty()) {
        (void)select_display_device(preferred_display);
    }
}

void update_tray_tooltip(const std::wstring& text) {
    if (!g_state.tray_added || g_state.window == nullptr) {
        return;
    }
    NOTIFYICONDATAW icon{};
    icon.cbSize = sizeof(icon);
    icon.hWnd = g_state.window;
    icon.uID = 1U;
    icon.uFlags = NIF_TIP;
    wcsncpy_s(icon.szTip, text.c_str(), _TRUNCATE);
    Shell_NotifyIconW(NIM_MODIFY, &icon);
}

std::optional<std::filesystem::path> find_session_launcher() {
    std::wstring module_path(MAX_PATH, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, module_path.data(),
        static_cast<DWORD>(module_path.size()));
    if (length == 0U || length >= module_path.size()) {
        return std::nullopt;
    }
    module_path.resize(length);

    const auto candidate = std::filesystem::path(module_path).parent_path() /
        L"wireview_session.exe";
    return std::filesystem::exists(candidate)
        ? std::optional<std::filesystem::path>(candidate)
        : std::nullopt;
}

std::filesystem::path find_vdd_broker() {
    std::wstring module_path(MAX_PATH, L'\0');
    const DWORD length = GetModuleFileNameW(
        nullptr, module_path.data(), static_cast<DWORD>(module_path.size()));
    if (length == 0U || length >= module_path.size()) {
        return {};
    }
    module_path.resize(length);
    const auto candidate =
        std::filesystem::path(module_path).parent_path() / L"wireview_vdd_broker.exe";
    return std::filesystem::exists(candidate) ? candidate : std::filesystem::path{};
}

std::filesystem::path control_module_directory() {
    std::wstring module_path(MAX_PATH, L'\0');
    const DWORD length = GetModuleFileNameW(
        nullptr, module_path.data(), static_cast<DWORD>(module_path.size()));
    if (length == 0U || length >= module_path.size()) {
        return {};
    }
    module_path.resize(length);
    return std::filesystem::path(module_path).parent_path();
}

std::unique_ptr<Gdiplus::Image> load_control_ui_asset(std::wstring_view filename) {
    const std::filesystem::path path = control_module_directory() /
        L"assets" / L"ui" / filename;
    if (path.empty() || !std::filesystem::exists(path)) {
        return {};
    }
    auto image = std::make_unique<Gdiplus::Image>(path.c_str());
    return image->GetLastStatus() == Gdiplus::Ok ? std::move(image)
                                                  : std::unique_ptr<Gdiplus::Image>{};
}

void load_control_ui_assets() {
    g_copy_screen_asset = load_control_ui_asset(L"copy-screen.png");
    g_extend_screen_asset = load_control_ui_asset(L"extend-screen.png");
    g_display_only_asset = load_control_ui_asset(L"icon-display-only.png");
    g_touch_pen_asset = load_control_ui_asset(L"icon-touch-pen.png");
}

std::filesystem::path find_settings_file() {
    std::wstring local_app_data(32'768U, L'\0');
    const DWORD length = GetEnvironmentVariableW(
        L"LOCALAPPDATA", local_app_data.data(), static_cast<DWORD>(local_app_data.size()));
    if (length == 0U || length >= local_app_data.size()) {
        return {};
    }
    local_app_data.resize(length);
    return std::filesystem::path(local_app_data) / L"WireView" / L"control-settings.ini";
}

std::wstring control_log_timestamp() {
    SYSTEMTIME time{};
    GetLocalTime(&time);
    wchar_t buffer[40]{};
    swprintf_s(buffer, L"%04u-%02u-%02u %02u:%02u:%02u.%03u",
        time.wYear, time.wMonth, time.wDay, time.wHour, time.wMinute, time.wSecond,
        time.wMilliseconds);
    return buffer;
}

void log_control_event(std::wstring_view category, std::wstring detail) {
    if (g_state.runtime_log_file.empty()) {
        return;
    }
    std::replace(detail.begin(), detail.end(), L'\r', L' ');
    std::replace(detail.begin(), detail.end(), L'\n', L' ');
    std::error_code error;
    std::filesystem::create_directories(g_state.runtime_log_file.parent_path(), error);
    if (error) {
        return;
    }
    std::ofstream output(g_state.runtime_log_file, std::ios::app | std::ios::binary);
    if (!output) {
        return;
    }
    output << wide_to_utf8(control_log_timestamp()) << " ["
           << wide_to_utf8(category) << "] " << wide_to_utf8(detail) << '\n';
}

std::wstring describe_display_choices_for_log() {
    std::wstring result;
    for (const DisplayChoice& choice : g_state.display_choices) {
        if (!result.empty()) {
            result += L" | ";
        }
        result += choice.device_name + L"=" + choice.label +
            (choice.parsec_vdd ? L" [Parsec]" : L"");
    }
    return result.empty() ? L"<none>" : result;
}

void set_status(const std::wstring& text) {
    if (g_state.current_status == text) {
        return;
    }
    g_state.current_status = text;
    log_control_event(L"status", text);
    std::wstring display_text;
    display_text.reserve(text.size() +
        static_cast<std::size_t>(std::count(text.begin(), text.end(), L'\n')));
    for (std::size_t index = 0U; index < text.size(); ++index) {
        if (text[index] == L'\n' && (index == 0U || text[index - 1U] != L'\r')) {
            display_text.push_back(L'\r');
        }
        display_text.push_back(text[index]);
    }
    const auto write_status = [&display_text](HWND status_control) {
        if (status_control == nullptr) {
            return;
        }
        const LRESULT first_visible_line = SendMessageW(
            status_control, EM_GETFIRSTVISIBLELINE, 0U, 0);
        SetWindowTextW(status_control, display_text.c_str());
        if (first_visible_line > 0) {
            SendMessageW(status_control, EM_LINESCROLL, 0U, first_visible_line);
        }
    };
    write_status(g_state.status);
    if (g_state.simple_status != nullptr) {
        const std::size_t simple_end = text.find(L'\n');
        const std::wstring simple_text = text.substr(
            0U, simple_end == std::wstring::npos ? text.size() : simple_end);
        SetWindowTextW(g_state.simple_status, simple_text.c_str());
    }
    InvalidateRect(g_state.window, nullptr, FALSE);
    InvalidateRect(g_state.simple_window, nullptr, FALSE);
    const std::size_t line_break = text.find(L'\n');
    update_tray_tooltip(
        L"WireView: " + text.substr(0U, line_break == std::wstring::npos
            ? text.size()
            : line_break));
    refresh_diagnostics_output();
#ifdef WIREVIEW_WEBVIEW_UI
    publish_web_state();
#endif
}

std::wstring selector_text(HWND selector) {
    const int length = GetWindowTextLengthW(selector);
    std::wstring text(static_cast<std::size_t>(std::max(0, length)) + 1U, L'\0');
    const int copied = GetWindowTextW(selector, text.data(), length + 1);
    text.resize(static_cast<std::size_t>(std::max(0, copied)));
    text.erase(
        std::remove_if(text.begin(), text.end(), [](wchar_t character) {
            return character == L' ' || character == L'\t';
        }),
        text.end());
    return text;
}

void refresh_diagnostics_output() {
    if (g_state.diagnostics_output == nullptr && g_diagnostics_text == nullptr) {
        return;
    }
    const auto control_text = [](HWND control) {
        const int length = control == nullptr ? 0 : GetWindowTextLengthW(control);
        std::wstring value(static_cast<std::size_t>(std::max(0, length)) + 1U, L'\0');
        if (control != nullptr && length > 0) {
            const int copied = GetWindowTextW(control, value.data(), length + 1);
            value.resize(static_cast<std::size_t>(std::max(0, copied)));
        } else {
            value.clear();
        }
        return value;
    };
    const std::wstring vdd = control_text(g_state.vdd_status);
    std::wstring output = L"WireView 控制中心输出\r\n\r\n" +
        (g_state.current_status.empty() ? L"准备就绪。" : g_state.current_status) +
        L"\r\n\r\nParsec VDD 诊断\r\n\r\n" +
        (vdd.empty() ? L"正在检查 Parsec VDD。" : vdd) +
        L"\r\n\r\n完整日志文件（请直接发送文件）\r\n\r\n" +
        L"控制中心日志：" +
        (g_state.runtime_log_file.empty()
            ? L"路径尚未初始化。"
            : g_state.runtime_log_file.wstring()) +
        L"\r\nWindows Host 日志：" +
        (g_state.last_host_diagnostics_file.empty()
            ? L"尚未生成；请先启动一次 WireView。"
            : g_state.last_host_diagnostics_file.wstring());
    for (HWND output_control : {g_state.diagnostics_output, g_diagnostics_text}) {
        if (output_control != nullptr) {
            SetWindowTextW(output_control, output.c_str());
        }
    }
}

const DisplayChoice* selected_display_choice() {
    if (g_state.display_selector == nullptr) {
        return nullptr;
    }
    const LRESULT index = SendMessageW(g_state.display_selector, CB_GETCURSEL, 0U, 0);
    if (index < 0 || static_cast<std::size_t>(index) >= g_state.display_choices.size()) {
        return nullptr;
    }
    return &g_state.display_choices[static_cast<std::size_t>(index)];
}

std::optional<bool> running_on_ac_power() {
    SYSTEM_POWER_STATUS status{};
    if (GetSystemPowerStatus(&status) == FALSE) {
        return std::nullopt;
    }
    if (status.ACLineStatus == 0U) {
        return false;
    }
    if (status.ACLineStatus == 1U) {
        return true;
    }
    return std::nullopt;
}

std::wstring high_frame_rate_environment_warning(
    std::wstring_view display_device,
    DWORD display_refresh_hz,
    std::optional<bool> on_ac_power) {
    const bool refresh_too_low = display_refresh_hz != 0U && display_refresh_hz < 120U;
    const bool on_battery = on_ac_power && !*on_ac_power;
    if (!refresh_too_low && !on_battery) {
        return {};
    }

    const std::wstring display = display_device.empty()
        ? L"目标显示器"
        : std::wstring(display_device);
    if (refresh_too_low && on_battery) {
        return L"警告：" + display + L" 当前 " +
            std::to_wstring(display_refresh_hz) +
            L" Hz 且电脑未接电源；120 FPS 可能严重不稳。";
    }
    if (refresh_too_low) {
        return L"警告：" + display + L" 当前 " +
            std::to_wstring(display_refresh_hz) +
            L" Hz；120 FPS 前请调至至少 120 Hz。";
    }
    return L"警告：电脑正在使用电池；120 FPS 可能降速，建议接通电源。";
}

void update_recommendation() {
    if (g_state.recommendation == nullptr || g_state.resolution_selector == nullptr ||
        g_state.frame_rate_selector == nullptr ||
        g_state.vdd_mode_selector == nullptr ||
        g_state.display_selector == nullptr) {
        return;
    }
    const auto selected_resolution = parse_resolution(selector_text(g_state.resolution_selector));
    std::uint16_t selected_refresh_hz{};
    try {
        selected_refresh_hz = static_cast<std::uint16_t>(
            std::stoul(selector_text(g_state.vdd_mode_selector)));
    } catch (const std::exception&) {
    }
    const LRESULT frame_rate_index = SendMessageW(
        g_state.frame_rate_selector, CB_GETCURSEL, 0U, 0);
    const bool high_frame_rate = frame_rate_index == 1;
    const bool native_reference = selected_resolution && selected_resolution->first == 2560U &&
        selected_resolution->second == 1600U;
    const DisplayChoice* display = selected_display_choice();
    const std::wstring environment_warning = high_frame_rate_environment_warning(
        display == nullptr ? std::wstring_view{} : std::wstring_view(display->device_name),
        display == nullptr ? 0U : display->refresh_hz,
        running_on_ac_power());
    const std::wstring high_frame_rate_caveat =
        L"120 FPS 仅为主机发送目标，设备最终屏幕帧率需实测。";
    const std::wstring text = selected_refresh_hz != 0U &&
                (high_frame_rate ? 120U : 60U) > selected_refresh_hz
        ? L"警告：副屏刷新率为 " + std::to_wstring(selected_refresh_hz) +
            L" Hz；多出的编码帧会重复画面。" + high_frame_rate_caveat
        : high_frame_rate && !environment_warning.empty()
        ? environment_warning + L" " + high_frame_rate_caveat
        : high_frame_rate
            ? L"实验性 120 FPS：" + high_frame_rate_caveat
            : native_reference
                ? L"提示：2560x1600 是参考机的清晰、长期稳定档。"
                : L"提示：投屏分辨率同时用于 Windows 副屏和设备端视频输出。";
    SetWindowTextW(g_state.recommendation, text.c_str());
}

void add_tray_icon(HWND window) {
    NOTIFYICONDATAW icon{};
    icon.cbSize = sizeof(icon);
    icon.hWnd = window;
    icon.uID = 1U;
    icon.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    icon.uCallbackMessage = kTrayMessage;
    icon.hIcon = LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(kWireViewAppIcon));
    if (icon.hIcon == nullptr) {
        icon.hIcon = LoadIconW(nullptr, MAKEINTRESOURCEW(32512));
    }
    wcsncpy_s(icon.szTip, L"WireView: 准备就绪", _TRUNCATE);
    g_state.tray_added = Shell_NotifyIconW(NIM_ADD, &icon) != FALSE;
}

void remove_tray_icon() {
    if (!g_state.tray_added || g_state.window == nullptr) {
        return;
    }
    NOTIFYICONDATAW icon{};
    icon.cbSize = sizeof(icon);
    icon.hWnd = g_state.window;
    icon.uID = 1U;
    Shell_NotifyIconW(NIM_DELETE, &icon);
    g_state.tray_added = false;
}

void restore_window(HWND window) {
    ShowWindow(window, SW_RESTORE);
    SetForegroundWindow(window);
}

void begin_control_center_exit(HWND window) {
    if (g_state.exiting) {
        return;
    }
    g_state.exiting = true;
    log_control_event(L"exit-request", L"用户从托盘请求退出控制中心。先隐藏主窗口，再释放后台资源。 ");
    // The WebView controller can take time to close on some machines. Hiding
    // first prevents a blank, non-interactive top-level window from remaining
    // visible while Windows tears down Chromium or a VDD broker.
    ShowWindow(window, SW_HIDE);
    for (HWND auxiliary : {g_state.simple_window, g_diagnostics_window, g_custom_mode_editor}) {
        if (auxiliary != nullptr && auxiliary != window && IsWindow(auxiliary) != FALSE) {
            DestroyWindow(auxiliary);
        }
    }
    if (PostMessageW(window, kFinalizeControlExit, 0U, 0) == FALSE) {
        log_control_event(L"exit-request", L"无法投递延迟销毁消息，改为立即销毁。系统错误 " +
            std::to_wstring(GetLastError()));
        DestroyWindow(window);
    }
}

std::wstring utf8_to_wide(std::string_view value) {
    if (value.empty()) {
        return {};
    }
    const int length = MultiByteToWideChar(
        CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (length <= 0) {
        return {};
    }
    std::wstring result(static_cast<std::size_t>(length), L'\0');
    return MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
               result.data(), length) > 0
        ? result
        : std::wstring{};
}

std::string wide_to_utf8(std::wstring_view value) {
    if (value.empty()) {
        return {};
    }
    const int length = WideCharToMultiByte(
        CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (length <= 0) {
        return {};
    }
    std::string result(static_cast<std::size_t>(length), '\0');
    return WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
               result.data(), length, nullptr, nullptr) > 0
        ? result
        : std::string{};
}

std::wstring normalize_device_model(std::wstring value) {
    std::wstring normalized;
    normalized.reserve(value.size());
    bool previous_was_space = false;
    for (wchar_t character : value) {
        if (character == L'‐' || character == L'‑' || character == L'‒' ||
            character == L'–' || character == L'—' || character == L'−') {
            character = L'-';
        }
        if (iswspace(character) != 0) {
            if (!normalized.empty()) {
                previous_was_space = true;
            }
            continue;
        }
        if (previous_was_space) {
            normalized.push_back(L' ');
            previous_was_space = false;
        }
        normalized.push_back(static_cast<wchar_t>(towupper(character)));
    }
    return normalized;
}

void load_device_model_catalog() {
    if (g_state.device_model_catalog_loaded) {
        return;
    }
    g_state.device_model_catalog_loaded = true;
    std::ifstream input(g_state.device_model_catalog_file, std::ios::binary);
    std::string line;
    while (std::getline(input, line)) {
        if (line.empty() || line.starts_with('#')) {
            continue;
        }
        const std::size_t first_tab = line.find('\t');
        const std::size_t second_tab = first_tab == std::string::npos
            ? std::string::npos : line.find('\t', first_tab + 1U);
        if (first_tab == std::string::npos || second_tab == std::string::npos) {
            continue;
        }
        const std::wstring model = utf8_to_wide(line.substr(0U, first_tab));
        const std::wstring name = utf8_to_wide(line.substr(first_tab + 1U, second_tab - first_tab - 1U));
        const std::string_view category(line.data() + second_tab + 1U, line.size() - second_tab - 1U);
        if (!model.empty() && !name.empty() && (category == "phone" || category == "tablet")) {
            g_state.device_model_catalog.push_back(
                DeviceModelRecord{model, name, category == "tablet"});
        }
    }
}

std::wstring device_cache_key(
    std::wstring_view model, std::wstring_view manufacturer, std::uint16_t width, std::uint16_t height) {
    return normalize_device_model(std::wstring(manufacturer)) + L"|" +
        normalize_device_model(std::wstring(model)) + L"|" + std::to_wstring(width) + L"x" +
        std::to_wstring(height);
}

std::optional<DeviceModelRecord> lookup_cached_device_model(std::wstring_view key) {
    std::ifstream input(g_state.device_model_cache_file, std::ios::binary);
    std::string line;
    const std::string lookup_key = wide_to_utf8(key);
    while (std::getline(input, line)) {
        const std::size_t first_tab = line.find('\t');
        const std::size_t second_tab = first_tab == std::string::npos
            ? std::string::npos : line.find('\t', first_tab + 1U);
        if (first_tab == std::string::npos || second_tab == std::string::npos ||
            line.substr(0U, first_tab) != lookup_key) {
            continue;
        }
        const std::wstring name = utf8_to_wide(line.substr(first_tab + 1U, second_tab - first_tab - 1U));
        const std::string_view category(line.data() + second_tab + 1U, line.size() - second_tab - 1U);
        if (!name.empty() && (category == "phone" || category == "tablet")) {
            return DeviceModelRecord{L"", name, category == "tablet"};
        }
    }
    return std::nullopt;
}

void cache_device_model(std::wstring_view key, const DeviceModelRecord& record) {
    if (g_state.device_model_cache_file.empty()) {
        return;
    }
    std::error_code error;
    std::filesystem::create_directories(g_state.device_model_cache_file.parent_path(), error);
    if (error) {
        return;
    }
    std::ofstream output(g_state.device_model_cache_file, std::ios::app | std::ios::binary);
    if (output) {
        output << wide_to_utf8(key) << '\t' << wide_to_utf8(record.name) << '\t'
               << (record.tablet ? "tablet" : "phone") << '\n';
    }
}

std::optional<DeviceModelRecord> resolve_known_device_model(
    std::wstring_view model, std::wstring_view manufacturer, std::uint16_t width, std::uint16_t height) {
    const std::wstring key = device_cache_key(model, manufacturer, width, height);
    if (const auto cached = lookup_cached_device_model(key); cached.has_value()) {
        return cached;
    }
    load_device_model_catalog();
    const std::wstring normalized_model = normalize_device_model(std::wstring(model));
    const auto found = std::find_if(g_state.device_model_catalog.begin(), g_state.device_model_catalog.end(),
        [&normalized_model](const DeviceModelRecord& record) {
            return record.normalized_model == normalized_model;
        });
    if (found == g_state.device_model_catalog.end()) {
        return std::nullopt;
    }
    cache_device_model(key, *found);
    return *found;
}

void reset_android_device_identity() {
    g_state.device_model = L"Android";
    g_state.device_name = L"Android 设备";
    g_state.device_active_refresh = L"--";
    g_state.device_is_tablet = true;
    g_state.detected_device_width = 0U;
    g_state.detected_device_height = 0U;
    g_state.detected_device_max_refresh = L"--";
    g_state.device_profile_requires_custom_mode = false;
    g_state.device_profile_note.clear();
}

std::wstring friendly_android_device_name(
    const std::wstring& model, const std::wstring& manufacturer, bool tablet,
    std::uint16_t width, std::uint16_t height) {
    if (const auto known = resolve_known_device_model(model, manufacturer, width, height);
        known.has_value()) {
        g_state.device_is_tablet = known->tablet;
        return known->name;
    }
    if (!model.empty() && _wcsicmp(model.c_str(), L"Android") != 0) {
        if (!manufacturer.empty() && model.find(manufacturer) == std::wstring::npos) {
            return manufacturer + L" " + model;
        }
        return model;
    }
    return tablet ? L"Android 平板" : L"Android 手机";
}

std::wstring compact_refresh_rate(std::wstring text) {
    const std::size_t decimal = text.find(L'.');
    if (decimal != std::wstring::npos) {
        while (!text.empty() && text.back() == L'0') {
            text.pop_back();
        }
        if (!text.empty() && text.back() == L'.') {
            text.pop_back();
        }
    }
    return text.empty() ? L"--" : text;
}

std::optional<std::wstring> read_script_status() {
    if (g_state.status_file.empty()) {
        return std::nullopt;
    }
    std::ifstream input(g_state.status_file, std::ios::binary);
    if (!input) {
        return std::nullopt;
    }
    std::string bytes{
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>()};
    if (bytes.size() >= 3U &&
        static_cast<unsigned char>(bytes[0]) == 0xEFU &&
        static_cast<unsigned char>(bytes[1]) == 0xBBU &&
        static_cast<unsigned char>(bytes[2]) == 0xBFU) {
        bytes.erase(0U, 3U);
    }
    while (!bytes.empty() && (bytes.back() == '\r' || bytes.back() == '\n')) {
        bytes.pop_back();
    }
    if (bytes.empty()) {
        return std::nullopt;
    }
    const int wide_length = MultiByteToWideChar(
        CP_UTF8,
        0,
        bytes.data(),
        static_cast<int>(bytes.size()),
        nullptr,
        0);
    if (wide_length <= 0) {
        return std::nullopt;
    }
    std::wstring result(static_cast<std::size_t>(wide_length), L'\0');
    if (MultiByteToWideChar(
            CP_UTF8,
            0,
            bytes.data(),
            static_cast<int>(bytes.size()),
            result.data(),
            wide_length) <= 0) {
        return std::nullopt;
    }
    if (!result.starts_with(L"version=1\n") && !result.starts_with(L"version=1\r\n")) {
        return result;
    }

    const auto value_for = [&result](std::wstring_view key) {
        const std::wstring prefix = std::wstring(key) + L"=";
        std::size_t start = 0U;
        while (start < result.size()) {
            const std::size_t end = result.find(L'\n', start);
            const std::size_t length =
                (end == std::wstring::npos ? result.size() : end) - start;
            std::wstring_view line(result.data() + start, length);
            if (!line.empty() && line.back() == L'\r') {
                line.remove_suffix(1U);
            }
            if (line.starts_with(prefix)) {
                return std::wstring(line.substr(prefix.size()));
            }
            if (end == std::wstring::npos) {
                break;
            }
            start = end + 1U;
        }
        return std::wstring{};
    };

    const std::wstring state = value_for(L"state");
    const std::wstring message = value_for(L"message");
    const std::wstring diagnostics = value_for(L"diagnostics");
    const std::wstring input_mode = value_for(L"input_mode");
    const std::wstring display = value_for(L"display");
    const std::wstring actual_width = value_for(L"actual_width");
    const std::wstring actual_height = value_for(L"actual_height");
    const std::wstring actual_fps = value_for(L"actual_fps");
    const std::wstring requested_width = value_for(L"requested_width");
    const std::wstring requested_height = value_for(L"requested_height");
    const std::wstring requested_fps = value_for(L"requested_fps");
    const std::wstring transport = value_for(L"transport");
    const std::wstring client = value_for(L"client");
    const std::wstring reconnects = value_for(L"reconnects");
    const std::wstring first_frame_ms = value_for(L"first_frame_ms");
    const std::wstring surface_samples = value_for(L"surface_samples");
    const std::wstring surface_p50_ms = value_for(L"surface_p50_ms");
    const std::wstring surface_p95_ms = value_for(L"surface_p95_ms");
    const std::wstring delivered_frames = value_for(L"delivered_frames");
    const std::wstring output_fps = value_for(L"output_fps");
    const std::wstring bitrate_mbps = value_for(L"bitrate_mbps");
    const std::wstring decoder_name = value_for(L"decoder_name");
    const std::wstring decoder_hardware = value_for(L"decoder_hardware");
    const std::wstring decoder_software = value_for(L"decoder_software");
    const std::wstring decoder_low_latency = value_for(L"decoder_low_latency");
    const std::wstring android_width = value_for(L"android_width");
    const std::wstring android_height = value_for(L"android_height");
    const std::wstring android_refresh_hz = value_for(L"android_refresh_hz");
    const std::wstring android_max_refresh_hz = value_for(L"android_max_refresh_hz");
    const std::wstring android_manufacturer = value_for(L"android_manufacturer");
    const std::wstring android_device_type = value_for(L"android_device_type");
    const std::wstring decoder_health_seen = value_for(L"decoder_health_seen");
    const std::wstring decoder_active = value_for(L"decoder_active");
    const std::wstring decoder_first_frame = value_for(L"decoder_first_frame");
    const std::wstring decoder_received_frames = value_for(L"decoder_received_frames");
    const std::wstring decoder_queued_frames = value_for(L"decoder_queued_frames");
    const std::wstring decoder_dropped_frames = value_for(L"decoder_dropped_frames");
    const std::wstring decoder_errors = value_for(L"decoder_errors");
    const bool android_connection_lost = state == L"waiting_device" ||
        state == L"waiting_usb" || state == L"waiting_authorization" ||
        state == L"waiting_adb" || state == L"reconnecting" || state == L"stopped" ||
        state == L"completed" || state == L"error";

    if (android_connection_lost) {
        g_state.device_connected = false;
        reset_android_device_identity();
    } else if (!client.empty()) {
        g_state.device_model = client;
        if (!android_device_type.empty()) {
            g_state.device_is_tablet = android_device_type != L"phone";
        } else if (_wcsicmp(client.c_str(), L"PKB110") == 0) {
            g_state.device_is_tablet = false;
        }
        g_state.device_connected = true;
        g_state.wireview_android_app_seen = true;
    }
    if (!android_refresh_hz.empty() && std::wcstod(android_refresh_hz.c_str(), nullptr) > 0.0) {
        g_state.device_active_refresh = compact_refresh_rate(android_refresh_hz);
    }

    std::wstring state_label = message;
    if (state == L"starting") {
        state_label = L"正在启动电脑端";
    } else if (state == L"waiting_usb") {
        state_label = L"等待已授权的 Android 设备";
    } else if (state == L"waiting_client") {
        state_label = L"等待设备端 WireView 应用连接";
    } else if (state == L"connected") {
        state_label = L"已连接，正在准备视频";
    } else if (state == L"streaming") {
        state_label = L"正在传输";
    } else if (state == L"recovering" || state == L"reconnecting") {
        state_label = L"连接中断，正在重新连接";
    } else if (state == L"waiting_device") {
        state_label = L"Android 设备已断开";
    } else if (state == L"waiting_authorization") {
        state_label = L"Android 设备未授权 USB 调试";
    } else if (state == L"waiting_adb") {
        state_label = L"未找到 Android Platform Tools";
    } else if (state == L"completed") {
        state_label = L"会话已结束";
    } else if (state == L"identifying_device") {
        state_label = L"正在识别已连接设备";
    } else if (state == L"error") {
        state_label = L"Windows Host 启动/运行失败";
    }
    const std::wstring input_label = input_mode == L"enabled"
        ? L"输入已启用"
        : L"安全模式";

    const bool has_actual_mode = !actual_width.empty() && actual_width != L"0" &&
        !actual_height.empty() && actual_height != L"0";
    const bool has_requested_size = !requested_width.empty() && requested_width != L"0" &&
        !requested_height.empty() && requested_height != L"0";
    std::wstring mode = has_actual_mode
        ? actual_width + L"×" + actual_height + L" @" + actual_fps + L" FPS"
        : has_requested_size
            ? L"请求 " + requested_width + L"×" + requested_height + L" @" +
                  requested_fps + L" FPS"
            : L"原生自动 @" + requested_fps + L" FPS";

    std::wstring formatted = L"状态：" + state_label + L" · " + input_label + L"\n" +
        L"目标：" + (display.empty() ? L"未知" : display) + L" · " + mode;
    const std::wstring transport_label = transport == L"winusb" ? L"WinUSB" : L"ADB/TCP";
    formatted += L"\n连接：" + transport_label + L" · " +
        (client.empty() ? L"未握手" : client) +
        L" · 重连 " + (reconnects.empty() ? L"0" : reconnects) + L" 次";
    if (!first_frame_ms.empty() && first_frame_ms != L"0") {
        formatted += L" · 首帧 " + first_frame_ms + L" ms";
    }
    if (!delivered_frames.empty() && delivered_frames != L"0") {
        formatted += L"\n主机发送：" + output_fps + L" FPS · " + bitrate_mbps +
            L" Mbps · " + delivered_frames + L" 帧";
    }
    if (!surface_samples.empty() && surface_samples != L"0") {
        formatted += L"\nSurface：" + surface_samples + L" 样本 · P50 " +
            surface_p50_ms + L" ms · P95 " + surface_p95_ms + L" ms";
    } else if (!message.empty() && state != L"starting" && state != L"waiting_usb" &&
               state != L"waiting_client") {
        formatted += L"\n" + message;
    }
    const bool has_device_profile = !android_width.empty() && android_width != L"0" &&
        !android_height.empty() && android_height != L"0";
    if (has_device_profile && !android_connection_lost) {
        apply_detected_device_profile(android_width, android_height, android_max_refresh_hz);
    }
    if (!client.empty() && !android_connection_lost) {
        g_state.device_name = friendly_android_device_name(
            g_state.device_model, android_manufacturer, g_state.device_is_tablet,
            g_state.detected_device_width, g_state.detected_device_height);
    }
    if (has_device_profile && decoder_name.empty()) {
        formatted += L"\n已识别设备：" + android_width + L"×" + android_height + L" @ " +
            android_refresh_hz + L" Hz（最高 " + android_max_refresh_hz + L" Hz）";
    }
    if (!g_state.device_profile_note.empty()) {
        formatted += L"\n" + g_state.device_profile_note;
    }
    if (decoder_health_seen == L"1") {
        std::wstring health_label;
        if (!decoder_errors.empty() && decoder_errors != L"0") {
            health_label = L"异常";
        } else if (state == L"streaming" && decoder_active != L"1") {
            health_label = L"异常";
        } else if (!decoder_dropped_frames.empty() && decoder_dropped_frames != L"0") {
            health_label = L"警告";
        } else if (decoder_first_frame != L"1") {
            health_label = L"启动中";
        } else {
            health_label = L"正常";
        }
        formatted += L"\n健康：" + health_label + L" · 解码输入 " +
            (decoder_queued_frames.empty() ? L"0" : decoder_queued_frames) + L"/" +
            (decoder_received_frames.empty() ? L"0" : decoder_received_frames) +
            L" · 输入丢帧 " +
            (decoder_dropped_frames.empty() ? L"0" : decoder_dropped_frames) +
            L" · 解码错误 " + (decoder_errors.empty() ? L"0" : decoder_errors);
    }
    if (!decoder_name.empty()) {
        const std::wstring decoder_kind = decoder_hardware == L"1"
            ? L"硬件"
            : decoder_software == L"1" ? L"软件" : L"未知实现";
        formatted += L"\n平板：" + android_width + L"×" + android_height + L" @" +
            android_refresh_hz + L" Hz（最高 " + android_max_refresh_hz + L" Hz）" +
            L" · 解码：" + decoder_name + L"（" + decoder_kind + L"）";

        const auto decimal_value = [](const std::wstring& text) {
            wchar_t* end = nullptr;
            const double value = std::wcstod(text.c_str(), &end);
            return end != text.c_str() ? value : 0.0;
        };
        const double requested_rate = decimal_value(requested_fps);
        const double active_refresh = decimal_value(android_refresh_hz);
        const double maximum_refresh = decimal_value(android_max_refresh_hz);
        std::wstring compatibility;
        if (decoder_hardware != L"1") {
            compatibility = L"未确认硬件 AVC，建议先使用 60 FPS 或降低分辨率";
        } else if (requested_rate >= 119.0 && maximum_refresh < 119.0) {
            compatibility = L"平板刷新率不足 120 Hz，120 FPS 无法完整显示";
        } else if (requested_rate >= 119.0 && active_refresh < 119.0) {
            compatibility = L"平板支持 120 Hz，但系统当前仍为 60 Hz；请检查高刷新率设置";
        } else if (requested_rate >= 119.0) {
            compatibility = L"120 FPS 主机发送条件已匹配；最终屏幕帧率仍需实测";
        } else if (active_refresh < 59.0) {
            compatibility = L"当前刷新率不足 60 Hz，建议检查平板显示设置";
        } else {
            compatibility = L"60 FPS 档已匹配";
        }
        compatibility += decoder_low_latency == L"1"
            ? L" · 标准低延迟标记=是"
            : L" · 标准低延迟标记=否（以首帧实测为准）";
        formatted += L"\n兼容性：" + compatibility;
    }
    if (state == L"starting" || state == L"waiting_client" || state == L"connected" ||
        state == L"streaming" || state == L"recovering") {
        formatted += L"\n操作：镜像会话运行中；需要结束请点击“停止 WireView”。";
    }
    if (!diagnostics.empty()) {
        std::wstring expanded;
        expanded.reserve(diagnostics.size());
        std::size_t cursor = 0U;
        while (cursor < diagnostics.size()) {
            const std::size_t marker = diagnostics.find(L"[[nl]]", cursor);
            if (marker == std::wstring::npos) {
                expanded.append(diagnostics, cursor, std::wstring::npos);
                break;
            }
            expanded.append(diagnostics, cursor, marker - cursor);
            expanded.push_back(L'\n');
            cursor = marker + 6U;
        }
        formatted += L"\n\nWindows Host 原始诊断（退出前）：\n" + expanded;
    }
    return formatted;
}

int run_script_status_self_test() {
    std::error_code error;
    const auto temporary_directory = std::filesystem::temp_directory_path(error);
    if (error) {
        return 1;
    }
    const auto status_file = temporary_directory /
        (L"wireview-control-status-self-test-" +
         std::to_wstring(GetCurrentProcessId()) + L".ini");
    g_state.status_file = status_file;
    {
        std::ofstream output(status_file, std::ios::trunc);
        output << "version=1\n"
               << "state=streaming\n"
               << "message=Desktop stream is active\n"
               << R"(display=\\.\DISPLAY6)" << '\n'
               << "input_mode=log-only\n"
               << "transport=winusb\n"
               << "requested_width=0\nrequested_height=0\nrequested_fps=60\n"
               << "actual_width=2560\nactual_height=1600\nactual_fps=60\n"
               << "client=DBR-W10\nreconnects=2\nfirst_frame_ms=12\n"
               << "surface_samples=8\nsurface_p50_ms=21.50\nsurface_p95_ms=43.25\n"
               << "delivered_frames=240\noutput_fps=59.94\nbitrate_mbps=24.10\n"
               << "decoder_name=c2.qti.avc.decoder\ndecoder_hardware=1\n"
               << "decoder_software=0\ndecoder_low_latency=0\ndecoder_1080p60=1\n"
               << "android_width=2560\nandroid_height=1600\n"
               << "android_refresh_hz=120.00\nandroid_max_refresh_hz=120.00\n"
               << "decoder_health_seen=1\ndecoder_active=1\ndecoder_first_frame=1\n"
               << "decoder_received_frames=240\ndecoder_queued_frames=240\n"
               << "decoder_dropped_frames=0\ndecoder_errors=0\n"
               << "diagnostics=Capture adapter: Intel(R) test GPU[[nl]]"
               << "Encoder: test H.264 MFT[[nl]]"
               << "WireView host error: test HRESULT 0x8000FFFF\n";
    }
    const auto structured = read_script_status();
    const bool structured_ok = structured &&
        structured->find(L"正在传输") != std::wstring::npos &&
        structured->find(LR"(\\.\DISPLAY6)") != std::wstring::npos &&
        structured->find(L"2560×1600 @60 FPS") != std::wstring::npos &&
        structured->find(L"重连 2 次") != std::wstring::npos &&
        structured->find(L"连接：WinUSB · DBR-W10") != std::wstring::npos &&
        structured->find(L"主机发送：59.94 FPS") != std::wstring::npos &&
        structured->find(L"P95 43.25 ms") != std::wstring::npos &&
        structured->find(L"健康：正常 · 解码输入 240/240 · 输入丢帧 0 · 解码错误 0") !=
            std::wstring::npos &&
        structured->find(L"c2.qti.avc.decoder（硬件）") != std::wstring::npos &&
        structured->find(L"60 FPS 档已匹配") != std::wstring::npos &&
        structured->find(L"标准低延迟标记=否（以首帧实测为准）") != std::wstring::npos &&
        structured->find(L"Windows Host 原始诊断（退出前）：\nCapture adapter: Intel(R) test GPU") !=
            std::wstring::npos &&
        structured->find(L"WireView host error: test HRESULT 0x8000FFFF") != std::wstring::npos;
    {
        std::ofstream output(status_file, std::ios::trunc);
        output << "version=1\nstate=streaming\nmessage=Desktop stream is active\n"
               << R"(display=\\.\DISPLAY6)" << '\n'
               << "input_mode=log-only\nrequested_width=1920\nrequested_height=1200\n"
               << "requested_fps=120\nactual_width=1920\nactual_height=1200\nactual_fps=120\n"
               << "client=DBR-W10\nreconnects=0\nfirst_frame_ms=20\n"
               << "decoder_name=c2.qti.avc.decoder\ndecoder_hardware=1\n"
               << "decoder_software=0\ndecoder_low_latency=0\ndecoder_1080p60=1\n"
               << "android_width=2560\nandroid_height=1600\n"
               << "android_refresh_hz=60.00\nandroid_max_refresh_hz=120.00\n"
               << "decoder_health_seen=1\ndecoder_active=1\ndecoder_first_frame=1\n"
               << "decoder_received_frames=120\ndecoder_queued_frames=117\n"
               << "decoder_dropped_frames=3\ndecoder_errors=0\n";
    }
    const auto refresh_limited = read_script_status();
    const bool refresh_limited_ok = refresh_limited &&
        refresh_limited->find(
            L"平板支持 120 Hz，但系统当前仍为 60 Hz；请检查高刷新率设置") !=
            std::wstring::npos &&
        refresh_limited->find(L"健康：警告 · 解码输入 117/120 · 输入丢帧 3") !=
            std::wstring::npos;
    {
        std::ofstream output(status_file, std::ios::trunc);
        output << "legacy status text\n";
    }
    const auto legacy = read_script_status();
    const bool legacy_ok = legacy && *legacy == L"legacy status text";
    const bool environment_warning_copy_ok =
        high_frame_rate_environment_warning(LR"(\\.\DISPLAY6)", 60U, false) ==
            L"警告：" LR"(\\.\DISPLAY6)" L" 当前 60 Hz 且电脑未接电源；120 FPS 可能严重不稳。" &&
        high_frame_rate_environment_warning(LR"(\\.\DISPLAY6)", 60U, true) ==
            L"警告：" LR"(\\.\DISPLAY6)" L" 当前 60 Hz；120 FPS 前请调至至少 120 Hz。" &&
        high_frame_rate_environment_warning(LR"(\\.\DISPLAY6)", 240U, false) ==
            L"警告：电脑正在使用电池；120 FPS 可能降速，建议接通电源。" &&
        high_frame_rate_environment_warning(LR"(\\.\DISPLAY6)", 240U, true).empty();
    std::filesystem::remove(status_file, error);
    g_state.status_file.clear();
    return structured_ok && refresh_limited_ok && legacy_ok &&
        environment_warning_copy_ok ? 0 : 1;
}

struct VddBrokerStatus {
    std::string state;
    std::wstring display_device;
    std::wstring message;
    DWORD owner_process_id{};
    DWORD broker_process_id{};
    int display_index{-1};
};

std::optional<VddBrokerStatus> read_vdd_broker_status(
    const std::filesystem::path& status_file) {
    if (status_file.empty()) {
        return std::nullopt;
    }
    std::ifstream input(status_file);
    if (!input) {
        return std::nullopt;
    }
    VddBrokerStatus result;
    std::string line;
    while (std::getline(input, line)) {
        if (line.rfind("state=", 0U) == 0U) {
            result.state = line.substr(6U);
        } else if (line.rfind("display=", 0U) == 0U) {
            const std::string value = line.substr(8U);
            result.display_device.assign(value.begin(), value.end());
        } else if (line.rfind("driver_index=", 0U) == 0U) {
            try {
                result.display_index = std::stoi(line.substr(13U));
            } catch (const std::exception&) {
                return std::nullopt;
            }
        } else if (line.rfind("owner_pid=", 0U) == 0U) {
            try {
                const unsigned long value = std::stoul(line.substr(10U));
                if (value > static_cast<unsigned long>(MAXDWORD)) {
                    return std::nullopt;
                }
                result.owner_process_id = static_cast<DWORD>(value);
            } catch (const std::exception&) {
                return std::nullopt;
            }
        } else if (line.rfind("broker_pid=", 0U) == 0U) {
            try {
                const unsigned long value = std::stoul(line.substr(11U));
                if (value > static_cast<unsigned long>(MAXDWORD)) {
                    return std::nullopt;
                }
                result.broker_process_id = static_cast<DWORD>(value);
            } catch (const std::exception&) {
                return std::nullopt;
            }
        } else if (line.rfind("message=", 0U) == 0U) {
            const std::string value = line.substr(8U);
            result.message.assign(value.begin(), value.end());
        }
    }
    return result.state.empty() ? std::nullopt : std::optional<VddBrokerStatus>(result);
}

std::wstring describe_vdd_failure(const std::wstring& detail) {
    if (detail.find(L"Parsec VDD created a display but") != std::wstring::npos) {
        return L"Parsec VDD 已接受创建请求，但 WireView 在 30 秒内没有找到唯一新增的已附加显示器。"
            L"该机的 Windows 显示拓扑可能延迟刷新或未公开 Parsec 标志；请保留下面的原始枚举信息。\n"
            L"原始诊断：" + detail;
    }
    if (detail.find(L"DeviceIoControl(Parsec VDD keep-alive)") != std::wstring::npos) {
        return L"Parsec VDD 驱动在运行中停止响应：" + detail +
            L"。请检查设备管理器中的驱动状态，必要时重启后重装或降级到 VDD 0.41 驱动。";
    }
    return detail.empty() ? L"VDD 守护进程未返回具体错误。" : detail;
}

[[nodiscard]] bool process_is_alive(DWORD process_id) noexcept {
    if (process_id == 0U) {
        return false;
    }
    HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, process_id);
    if (process == nullptr) {
        // Access denied is treated conservatively as an existing process.
        return GetLastError() == ERROR_ACCESS_DENIED;
    }
    const bool alive = WaitForSingleObject(process, 0U) == WAIT_TIMEOUT;
    CloseHandle(process);
    return alive;
}

[[nodiscard]] bool recovery_display_mapping_matches(
    const std::wstring& display_device) noexcept {
    return std::any_of(
        g_state.display_choices.begin(),
        g_state.display_choices.end(),
        [&display_device](const DisplayChoice& choice) {
            return choice.parsec_vdd && choice.device_name == display_device;
        });
}

void scan_vdd_recovery_records() {
    g_state.recovered_vdd_session.reset();
    g_state.recovered_vdd_record_count = 0U;
    if (g_state.settings_file.empty()) {
        update_vdd_controls();
        return;
    }

    std::error_code error;
    const auto directory = g_state.settings_file.parent_path();
    if (!std::filesystem::exists(directory, error) || error) {
        update_vdd_controls();
        return;
    }
    auto newest_write_time = (std::filesystem::file_time_type::min)();
    for (std::filesystem::directory_iterator iterator(directory, error), end;
         !error && iterator != end;
         iterator.increment(error)) {
        if (!iterator->is_regular_file(error) || error) {
            continue;
        }
        const std::wstring filename = iterator->path().filename().wstring();
        if (!filename.starts_with(L"vdd-session-") || !filename.ends_with(L".ini") ||
            iterator->path() == g_state.vdd_status_file) {
            continue;
        }
        const auto status = read_vdd_broker_status(iterator->path());
        if (!status || status->state != "ready" || status->display_index < 0 ||
            status->display_index > 15 || status->display_device.empty()) {
            continue;
        }
        const bool owner_alive = process_is_alive(status->owner_process_id);
        const bool broker_alive = process_is_alive(status->broker_process_id);
        const bool mapping_matches = recovery_display_mapping_matches(status->display_device);
        if (!owner_alive && !broker_alive && !mapping_matches) {
            // The broker already removed (or never attached) this display and
            // only its status file survived a control-center shutdown. The
            // driver index may since have been reused, so never remove a VDD
            // by that stale index. Dropping metadata is safe and prevents an
            // unconfirmable historical record from permanently blocking all
            // future display creation.
            std::error_code remove_error;
            const bool removed = std::filesystem::remove(iterator->path(), remove_error);
            log_control_event(
                L"vdd-recovery",
                L"忽略已消失副屏的孤立记录：" + iterator->path().wstring() +
                    L"；display=" + status->display_device + L"；index=" +
                    std::to_wstring(status->display_index) +
                    (removed
                        ? L"；已删除记录，未操作 Parsec VDD。"
                        : L"；记录删除失败但不阻止创建，未操作 Parsec VDD；系统错误=" +
                            std::to_wstring(remove_error.value())));
            continue;
        }
        ++g_state.recovered_vdd_record_count;
        const auto write_time = iterator->last_write_time(error);
        if (error) {
            error.clear();
        }
        if (g_state.recovered_vdd_session && write_time <= newest_write_time) {
            continue;
        }
        newest_write_time = write_time;
        g_state.recovered_vdd_session = RecoveredVddSession{
            .status_file = iterator->path(),
            .display_device = status->display_device,
            .owner_process_id = status->owner_process_id,
            .broker_process_id = status->broker_process_id,
            .display_index = status->display_index,
            .owner_alive = owner_alive,
            .broker_alive = broker_alive,
            .mapping_matches = mapping_matches,
        };
    }
    update_vdd_controls();
}

void close_vdd_broker_handles() {
    if (g_state.vdd_broker_process != nullptr) {
        CloseHandle(g_state.vdd_broker_process);
        g_state.vdd_broker_process = nullptr;
    }
    if (g_state.vdd_stop_event != nullptr) {
        CloseHandle(g_state.vdd_stop_event);
        g_state.vdd_stop_event = nullptr;
    }
    g_state.vdd_stop_event_name.clear();
    g_state.owned_display_device.clear();
    g_state.owned_display_index = -1;
    g_state.vdd_stop_requested = false;
    g_state.pending_vdd_width = 0U;
    g_state.pending_vdd_height = 0U;
    g_state.pending_vdd_refresh_hz = 0U;
    g_state.start_after_vdd_ready = false;
    g_state.start_after_vdd_input = false;
    g_state.recreate_vdd_for_simple_extension = false;
    g_state.recreate_vdd_for_simple_extension_input = false;
}

void update_vdd_configuration_controls() {
    const bool locked = g_state.vdd_broker_process != nullptr ||
        !g_state.owned_display_device.empty();
    if (g_state.resolution_selector != nullptr) {
        EnableWindow(g_state.resolution_selector, !locked && g_state.shell_process == nullptr);
    }
    if (g_state.vdd_mode_selector != nullptr) {
        EnableWindow(g_state.vdd_mode_selector, !locked && g_state.shell_process == nullptr);
    }
}

bool apply_custom_modes_with_elevation(HWND window) {
    if (g_state.vdd_broker_path.empty()) {
        return false;
    }
    std::wstring arguments = L"--apply-custom-modes";
    for (const auto& mode : g_custom_modes) {
        arguments += L" ";
        if (!mode) {
            arguments += L"-";
        } else {
            arguments += std::to_wstring(mode->width) + L"x" +
                std::to_wstring(mode->height) + L"@" + std::to_wstring(mode->refresh_hz);
        }
    }
    SHELLEXECUTEINFOW execute{};
    execute.cbSize = sizeof(execute);
    execute.fMask = SEE_MASK_NOCLOSEPROCESS;
    execute.hwnd = window;
    execute.lpVerb = L"runas";
    execute.lpFile = g_state.vdd_broker_path.c_str();
    execute.lpParameters = arguments.c_str();
    execute.lpDirectory = g_state.vdd_broker_path.parent_path().c_str();
    execute.nShow = SW_HIDE;
    log_control_event(L"vdd-custom-modes", L"请求管理员权限写入 Parsec 自定义模式。");
    if (ShellExecuteExW(&execute) == FALSE || execute.hProcess == nullptr) {
        log_control_event(L"vdd-custom-modes", L"管理员授权被取消或启动失败，系统错误 " +
            std::to_wstring(GetLastError()));
        return false;
    }
    const DWORD waited = WaitForSingleObject(execute.hProcess, 30'000U);
    DWORD exit_code = 1U;
    if (waited == WAIT_OBJECT_0) {
        (void)GetExitCodeProcess(execute.hProcess, &exit_code);
    }
    CloseHandle(execute.hProcess);
    log_control_event(L"vdd-custom-modes", L"自定义模式写入进程结束，退出代码 " +
        std::to_wstring(exit_code));
    return waited == WAIT_OBJECT_0 && exit_code == 0U;
}

bool custom_modes_match_parsec_vdd() {
    const auto applied = wireview::host::read_parsec_vdd_custom_modes();
    if (!applied.has_value()) {
        return false;
    }
    for (std::size_t index = 0U; index < g_custom_modes.size(); ++index) {
        const auto& requested = g_custom_modes[index];
        const auto& current = (*applied)[index];
        if (requested.has_value() != current.has_value()) {
            return false;
        }
        if (requested && (requested->width != current->width ||
            requested->height != current->height || requested->refresh_hz != current->refresh_hz)) {
            return false;
        }
    }
    return true;
}

bool start_vdd_broker(HWND window, bool confirm_custom_modes = true) {
    log_control_event(L"vdd-create", L"收到创建副屏请求；当前显示器：" +
        describe_display_choices_for_log());
    if (g_state.device_profile_requires_custom_mode) {
        set_status(L"请先在“编辑自定义分辨率”中替换或释放一个自定义槽，再创建副屏。");
        show_custom_mode_editor(window);
        return false;
    }
    if (g_state.shell_process != nullptr) {
        if (g_state.preview_active) {
            stop_session();
        } else {
            set_status(L"先停止当前 WireView 视频会话");
            return false;
        }
    }
    if (g_state.vdd_broker_process != nullptr) {
        set_status(L"WireView 已经持有或正在创建一块副屏。");
        return false;
    }
    if (g_state.recovered_vdd_record_count != 0U) {
        set_status(L"检测到未处理的 WireView VDD 残留记录，已拒绝创建新副屏。");
        return false;
    }
    const auto current_displays = enumerate_attached_displays();
    if (std::any_of(
            current_displays.begin(),
            current_displays.end(),
            [](const DisplayChoice& choice) { return choice.parsec_vdd; })) {
        g_state.display_choices = current_displays;
        update_vdd_controls();
        set_status(L"已存在 Parsec 副屏，请直接选择复用；已拒绝重复创建。");
        return false;
    }
    if (g_state.vdd_broker_path.empty()) {
        set_status(L"未找到 wireview_vdd_broker.exe，请重新构建 Windows 程序。");
        return false;
    }
    const auto probe = wireview::host::probe_parsec_vdd();
    log_control_event(L"vdd-probe", L"available=" + std::to_wstring(probe.available ? 1U : 0U) +
        L" version=" + std::to_wstring(probe.version) + L" detail=" +
        std::wstring(probe.detail.begin(), probe.detail.end()));
    if (!probe.available) {
        set_status(L"Parsec VDD 不可用，无法创建 WireView 副屏。");
        return false;
    }
    const auto requested_stream = selected_stream_settings();
    if (!requested_stream) {
        return false;
    }
    g_state.pending_vdd_width = requested_stream->width;
    g_state.pending_vdd_height = requested_stream->height;
    g_state.pending_vdd_refresh_hz = requested_stream->vdd_refresh_hz;
    const bool has_custom_mode = std::any_of(
        g_custom_modes.begin(), g_custom_modes.end(),
        [](const std::optional<CustomMode>& mode) { return mode.has_value(); });
    // 自定义模式是用户已在控制中心保存的配置。创建副屏时直接按“继续”执行，
    // 避免简易、完整和自动配置入口反复弹出相同的系统确认框。
    (void)confirm_custom_modes;
    if (has_custom_mode && !custom_modes_match_parsec_vdd() &&
        !apply_custom_modes_with_elevation(window)) {
        set_status(L"未获得管理员授权或 Parsec VDD 自定义模式写入失败；没有创建副屏。 ");
        return false;
    }
    if (has_custom_mode) {
        log_control_event(L"vdd-custom-modes", custom_modes_match_parsec_vdd()
            ? L"自定义模式已同步，跳过管理员写入。"
            : L"自定义模式已在本次创建前写入。 ");
    }

    const std::wstring unique_suffix = std::to_wstring(GetCurrentProcessId()) + L"-" +
        std::to_wstring(GetTickCount64());
    g_state.vdd_stop_event_name = L"Local\\WireViewVddStop-" + unique_suffix;
    g_state.vdd_stop_event = CreateEventW(
        nullptr, TRUE, FALSE, g_state.vdd_stop_event_name.c_str());
    if (g_state.vdd_stop_event == nullptr) {
        g_state.vdd_stop_event_name.clear();
        set_status(L"无法创建 WireView VDD 停止事件。");
        return false;
    }

    std::error_code error;
    std::filesystem::path status_directory;
    if (!g_state.settings_file.empty()) {
        status_directory = g_state.settings_file.parent_path();
    } else {
        status_directory = std::filesystem::temp_directory_path(error);
        if (error) {
            CloseHandle(g_state.vdd_stop_event);
            g_state.vdd_stop_event = nullptr;
            g_state.vdd_stop_event_name.clear();
            set_status(L"无法创建 WireView VDD 会话记录。");
            return false;
        }
    }
    g_state.vdd_status_file =
        status_directory / (L"vdd-session-" + unique_suffix + L".ini");
    std::filesystem::remove(g_state.vdd_status_file, error);
    g_state.last_vdd_broker_log_state.clear();
    std::wstring argument = L"\"" + g_state.vdd_broker_path.wstring() +
        L"\" --owner-pid " + std::to_wstring(GetCurrentProcessId()) +
        L" --stop-event \"" + g_state.vdd_stop_event_name +
        L"\" --status-file \"" + g_state.vdd_status_file.wstring() + L"\"";
    if (requested_stream->vdd_width != 0U) {
        argument += L" --mode " + std::to_wstring(requested_stream->vdd_width) + L"x" +
            std::to_wstring(requested_stream->vdd_height) + L"@" +
            std::to_wstring(requested_stream->vdd_refresh_hz);
    }
    const auto create_broker_process = [&](DWORD creation_flags, PROCESS_INFORMATION& process) {
        std::vector<wchar_t> command(argument.begin(), argument.end());
        command.push_back(L'\0');
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        return CreateProcessW(
            nullptr,
            command.data(),
            nullptr,
            nullptr,
            FALSE,
            creation_flags,
            nullptr,
            g_state.vdd_broker_path.parent_path().c_str(),
            &startup,
            &process) != FALSE;
    };

    PROCESS_INFORMATION process{};
    if (!create_broker_process(CREATE_NO_WINDOW | CREATE_BREAKAWAY_FROM_JOB, process)) {
        const DWORD first_error = GetLastError();
        if (first_error != ERROR_ACCESS_DENIED ||
            !create_broker_process(CREATE_NO_WINDOW, process)) {
            CloseHandle(g_state.vdd_stop_event);
            g_state.vdd_stop_event = nullptr;
            g_state.vdd_stop_event_name.clear();
            g_state.vdd_status_file.clear();
            set_status(L"无法启动 WireView VDD 守护进程。");
            return false;
        }
    }
    CloseHandle(process.hThread);
    g_state.vdd_broker_process = process.hProcess;
    log_control_event(L"vdd-broker", L"已启动，PID=" + std::to_wstring(process.dwProcessId) +
        L"，请求模式=" + std::to_wstring(requested_stream->vdd_width) + L"x" +
        std::to_wstring(requested_stream->vdd_height) + L"@" +
        std::to_wstring(requested_stream->vdd_refresh_hz));
    update_vdd_configuration_controls();
    if (SetTimer(window, kVddTimer, 250U, nullptr) == 0U) {
        SetEvent(g_state.vdd_stop_event);
        close_vdd_broker_handles();
        set_status(L"无法监控 WireView VDD 守护进程。");
        return false;
    }
    update_vdd_controls();
    set_status(L"正在创建一块仅属于当前 WireView 控制中心的副屏……");
    return true;
}

void request_vdd_removal() {
    if (g_state.shell_process != nullptr) {
        set_status(L"先停止当前 WireView 视频会话");
        return;
    }
    if (g_state.vdd_broker_process == nullptr || g_state.vdd_stop_event == nullptr) {
        set_status(L"当前没有由 WireView 创建并持有的副屏。");
        return;
    }
    if (SetEvent(g_state.vdd_stop_event) == FALSE) {
        set_status(L"无法通知 VDD 守护进程移除副屏。");
        return;
    }
    g_state.vdd_stop_requested = true;
    update_vdd_controls();
    set_status(L"正在安全移除 WireView 创建的副屏……");
}

void recover_stale_vdd_session(HWND window) {
    scan_vdd_recovery_records();
    if (g_state.recovered_vdd_record_count != 1U || !g_state.recovered_vdd_session) {
        set_status(L"没有唯一、可确认的 WireView VDD 残留记录。");
        return;
    }
    const RecoveredVddSession recovered = *g_state.recovered_vdd_session;
    if (recovered.owner_alive || recovered.broker_alive || !recovered.mapping_matches) {
        set_status(L"残留会话仍有进程存活或显示器映射无法确认，已拒绝清理。");
        return;
    }
    const std::wstring confirmation =
        L"将按 WireView 残留候选记录移除以下 Parsec VDD：\n\n" +
        recovered.display_device + L"\ndriver index " +
        std::to_wstring(recovered.display_index) +
        L"\n\nParsec 驱动会复用 index，WireView 无法自动证明它仍属于原会话。"
        L"只有你确认该 index 没有被 Parsec/Sunshine 等其他程序重新使用时才继续。"
        L"此操作不会按名称批量删除其他 Parsec 显示器。";
    if (MessageBoxW(
            window,
            confirmation.c_str(),
            L"确认清理 WireView 残留副屏",
            MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES) {
        set_status(L"已取消清理残留副屏。");
        return;
    }
    try {
        wireview::host::remove_parsec_vdd_display(recovered.display_index);
        std::error_code error;
        std::filesystem::remove(recovered.status_file, error);
        g_state.recovered_vdd_session.reset();
        g_state.recovered_vdd_record_count = 0U;
        for (int attempt = 0; attempt < 30; ++attempt) {
            const auto displays = enumerate_attached_displays();
            const bool still_attached = std::any_of(
                displays.begin(),
                displays.end(),
                [&recovered](const DisplayChoice& choice) {
                    return choice.parsec_vdd &&
                        choice.device_name == recovered.display_device;
                });
            if (!still_attached) {
                break;
            }
            Sleep(100U);
        }
        refresh_display_choices();
        scan_vdd_recovery_records();
        set_status(L"已按精确 driver index 清理 WireView 残留副屏。");
    } catch (const std::exception& error) {
        const std::string message(error.what());
        const std::wstring wide_message(message.begin(), message.end());
        set_status(L"清理 WireView 残留副屏失败：" + wide_message);
    }
}

void poll_vdd_broker(HWND window) {
    if (g_state.vdd_broker_process == nullptr) {
        KillTimer(window, kVddTimer);
        return;
    }
    const auto broker_status = read_vdd_broker_status(g_state.vdd_status_file);
    if (broker_status) {
        const std::wstring fingerprint = std::wstring(broker_status->state.begin(), broker_status->state.end()) +
            L"|display=" + broker_status->display_device + L"|index=" +
            std::to_wstring(broker_status->display_index) + L"|message=" + broker_status->message;
        if (fingerprint != g_state.last_vdd_broker_log_state) {
            g_state.last_vdd_broker_log_state = fingerprint;
            log_control_event(L"vdd-broker-status", fingerprint);
        }
    }
    if (broker_status && broker_status->state == "ready" &&
        g_state.owned_display_device.empty() && !broker_status->display_device.empty() &&
        broker_status->display_index >= 0) {
        g_state.owned_display_device = broker_status->display_device;
        g_state.owned_display_index = broker_status->display_index;
        refresh_display_choices(g_state.owned_display_device);
        update_vdd_configuration_controls();
        if (g_state.pending_vdd_width != 0U && g_state.pending_vdd_height != 0U &&
            g_state.pending_vdd_refresh_hz != 0U) {
            StreamSettings settings{};
            settings.width = g_state.pending_vdd_width;
            settings.height = g_state.pending_vdd_height;
            settings.vdd_refresh_hz = g_state.pending_vdd_refresh_hz;
            notify_android_of_vdd_created(settings);
        } else if (const auto settings = selected_stream_settings()) {
            // Fallback for an older/recovered broker session with no saved request.
            notify_android_of_vdd_created(*settings);
        }
        const bool topology_repaired = broker_status->message.find(
            L"requested Windows DisplaySwitch.exe /extend") != std::wstring::npos;
        set_status(
            (topology_repaired
                ? L"已检测到副屏被 Windows 设为复制，已自动切换为扩展；WireView 副屏已创建并选中："
                : L"WireView 副屏已创建并选中：") +
            g_state.owned_display_device + L"。");
        if (g_state.start_after_vdd_ready) {
            const bool enable_input = g_state.start_after_vdd_input;
            g_state.start_after_vdd_ready = false;
            g_state.start_after_vdd_input = false;
            (void)start_session(window, enable_input, false, false);
        }
    }
    if (broker_status && broker_status->state == "retrying") {
        set_status(L"首次 Parsec VDD 创建未成为独立的 Windows 显示器，已尝试切换扩展拓扑、移除该次实例，并正在进行第 2 次创建（最多等待 30 秒）。");
    }

    DWORD exit_code = STILL_ACTIVE;
    if (GetExitCodeProcess(g_state.vdd_broker_process, &exit_code) != FALSE &&
        exit_code == STILL_ACTIVE) {
        return;
    }
    log_control_event(L"vdd-broker-exit", L"退出代码 " + std::to_wstring(exit_code) +
        L"；最后状态=" + g_state.last_vdd_broker_log_state);

    const bool removal_requested = g_state.vdd_stop_requested;
    const bool start_after_removal = g_state.start_after_vdd_removal;
    const bool start_after_removal_input = g_state.start_after_vdd_removal_input;
    const bool recreate_for_simple_extension = g_state.recreate_vdd_for_simple_extension;
    const bool recreate_for_simple_extension_input =
        g_state.recreate_vdd_for_simple_extension_input;
    std::wstring failure_message;
    if (broker_status && broker_status->state == "error") {
        failure_message = describe_vdd_failure(broker_status->message);
    }
    KillTimer(window, kVddTimer);
    close_vdd_broker_handles();
    g_state.start_after_vdd_removal = false;
    g_state.start_after_vdd_removal_input = false;
    update_vdd_configuration_controls();
    std::error_code error;
    std::filesystem::remove(g_state.vdd_status_file, error);
    g_state.vdd_status_file.clear();
    refresh_display_choices();
    scan_vdd_recovery_records();
    if (!failure_message.empty()) {
        set_status(L"WireView 副屏操作失败：" + failure_message);
    } else if (removal_requested) {
        set_status(L"WireView 创建的副屏已安全移除。");
        if (recreate_for_simple_extension) {
            set_status(L"原 WireView 副屏已不再附加到 Windows，正在重新创建扩展屏幕……");
            begin_simple_extension(recreate_for_simple_extension_input);
        } else if (start_after_removal) {
            if (!select_primary_display()) {
                set_status(L"副屏已移除，但未找到电脑主显示器。请在完整模式中刷新显示器。 ");
            } else {
                (void)start_session(window, start_after_removal_input, false, false);
            }
        }
    } else {
        set_status(L"WireView VDD 守护进程已结束，未保留其副屏。");
    }
}

void shutdown_vdd_broker_on_exit() {
    if (g_state.vdd_broker_process == nullptr) {
        return;
    }
    if (g_state.vdd_stop_event != nullptr) {
        (void)SetEvent(g_state.vdd_stop_event);
    }
    log_control_event(L"vdd-shutdown", L"已发送停止事件，最多等待 5 秒。 ");
    const DWORD wait_result = WaitForSingleObject(g_state.vdd_broker_process, 5'000U);
    log_control_event(L"vdd-shutdown", L"等待结果=" + std::to_wstring(wait_result));
    if (wait_result == WAIT_OBJECT_0 && !g_state.vdd_status_file.empty()) {
        std::error_code error;
        std::filesystem::remove(g_state.vdd_status_file, error);
    }
    close_vdd_broker_handles();
}

void update_session_action_controls() {
    const bool mirroring = g_state.shell_process != nullptr &&
        !g_state.preview_active && !g_state.profile_probe_active;
    const HMODULE user32 = GetModuleHandleW(L"user32.dll");
    const bool input_supported = user32 != nullptr &&
        GetProcAddress(user32, "CreateSyntheticPointerDevice") != nullptr &&
        GetProcAddress(user32, "InjectSyntheticPointerInput") != nullptr &&
        GetProcAddress(user32, "DestroySyntheticPointerDevice") != nullptr;
    if (g_state.start_mirror_button != nullptr) {
        EnableWindow(g_state.start_mirror_button, !mirroring);
    }
    if (g_state.start_input_button != nullptr) {
        EnableWindow(g_state.start_input_button, !mirroring && input_supported);
    }
    if (g_state.full_display_only_button != nullptr) {
        EnableWindow(g_state.full_display_only_button, !mirroring);
    }
    if (g_state.stop_button != nullptr) {
        EnableWindow(g_state.stop_button, g_state.shell_process != nullptr);
    }
    if (g_state.simple_stop_button != nullptr) {
        EnableWindow(g_state.simple_stop_button, g_state.shell_process != nullptr);
    }
    if (g_state.simple_mirror_button != nullptr) {
        EnableWindow(g_state.simple_mirror_button, !mirroring);
    }
    if (g_state.simple_input_button != nullptr) {
        EnableWindow(g_state.simple_input_button, !mirroring && input_supported);
    }
    if (g_state.simple_display_only_button != nullptr) {
        EnableWindow(g_state.simple_display_only_button, !mirroring);
    }
    const bool can_begin_extension = !mirroring &&
        (g_state.vdd_broker_process == nullptr || !g_state.owned_display_device.empty());
    if (g_state.simple_extend_button != nullptr) {
        EnableWindow(g_state.simple_extend_button, can_begin_extension);
    }
    if (g_state.simple_extend_input_button != nullptr) {
        EnableWindow(g_state.simple_extend_input_button, can_begin_extension && input_supported);
    }
}

void stop_session() {
    // 在关闭 Job 前读取会话启动器实时镜像的 Host 输出；关闭 Job 会同时终止
    // wireview_session 与 wireview_host，不能等到之后再向 status 文件索取最终结果。
    if (!g_state.host_diagnostics_file.empty()) {
        std::ifstream input(g_state.host_diagnostics_file, std::ios::binary);
        std::string bytes{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
        if (!bytes.empty()) {
            const int length = MultiByteToWideChar(CP_UTF8, 0, bytes.data(),
                static_cast<int>(bytes.size()), nullptr, 0);
            if (length > 0) {
                std::wstring output(static_cast<std::size_t>(length), L'\0');
                if (MultiByteToWideChar(CP_UTF8, 0, bytes.data(), static_cast<int>(bytes.size()),
                        output.data(), length) > 0) {
                    g_state.last_host_diagnostics = std::move(output);
                }
            }
        }
    }
    if (g_state.session_job != nullptr) {
        TerminateJobObject(g_state.session_job, 0U);
        CloseHandle(g_state.session_job);
        g_state.session_job = nullptr;
    }
    if (g_state.shell_process != nullptr) {
        CloseHandle(g_state.shell_process);
        g_state.shell_process = nullptr;
    }
    g_state.preview_active = false;
    g_state.profile_probe_active = false;
    if (!g_state.status_file.empty()) {
        std::error_code error;
        std::filesystem::remove(g_state.status_file, error);
        g_state.status_file.clear();
        g_state.last_script_status.clear();
    }
    g_state.host_diagnostics_file.clear();
    if (g_state.frame_rate_selector != nullptr) {
        EnableWindow(g_state.frame_rate_selector, TRUE);
    }
    if (g_state.display_selector != nullptr) {
        EnableWindow(g_state.display_selector, TRUE);
    }
    if (g_state.refresh_displays_button != nullptr) {
        EnableWindow(g_state.refresh_displays_button, TRUE);
    }
    update_session_action_controls();
    update_vdd_configuration_controls();
    update_vdd_controls();
}

std::optional<StreamSettings> selected_stream_settings() {
    if (g_state.resolution_selector == nullptr || g_state.frame_rate_selector == nullptr ||
        g_state.vdd_mode_selector == nullptr ||
        g_state.display_selector == nullptr) {
        return std::nullopt;
    }
    StreamSettings settings;
    const LRESULT frame_rate_index = SendMessageW(
        g_state.frame_rate_selector, CB_GETCURSEL, 0U, 0);
    settings.frame_rate = frame_rate_index == 1 ? 120U : 60U;
    const auto resolution = parse_resolution(selector_text(g_state.resolution_selector));
    if (!resolution) {
        set_status(L"请选择有效的投屏分辨率。 ");
        return std::nullopt;
    }
    try {
        const unsigned long refresh = std::stoul(selector_text(g_state.vdd_mode_selector));
        if (refresh == 0UL || refresh > 1000UL) {
            throw std::out_of_range("refresh");
        }
        settings.vdd_refresh_hz = static_cast<std::uint16_t>(refresh);
    } catch (const std::exception&) {
        set_status(L"请选择该分辨率支持的副屏刷新率。 ");
        return std::nullopt;
    }
    settings.width = resolution->first;
    settings.height = resolution->second;
    settings.vdd_width = resolution->first;
    settings.vdd_height = resolution->second;
    const LRESULT display_index = SendMessageW(
        g_state.display_selector, CB_GETCURSEL, 0U, 0);
    if (display_index < 0 ||
        static_cast<std::size_t>(display_index) >= g_state.display_choices.size()) {
        set_status(L"没有可用的 Windows 显示器，请刷新显示器列表。");
        return std::nullopt;
    }
    settings.display_device =
        g_state.display_choices[static_cast<std::size_t>(display_index)].device_name;
    return settings;
}

bool start_session(
    HWND window, bool enable_input, bool device_preview = false, bool device_profile_probe = false) {
    if (!device_profile_probe && g_state.device_profile_requires_custom_mode) {
        set_status(L"请先在“编辑自定义分辨率”中替换或释放一个自定义槽，再开始 WireView。");
        show_custom_mode_editor(window);
        return false;
    }
    if (g_state.shell_process != nullptr) {
        if (g_state.preview_active && !device_preview) {
            stop_session();
        } else {
            set_status(L"先停止当前 WireView 视频会话");
            return false;
        }
    }
    if (g_state.shell_process != nullptr) {
        set_status(L"先停止当前 WireView 视频会话");
        return false;
    }
    if (g_state.session_launcher_path.empty()) {
        set_status(L"未找到 wireview_session.exe。请重新解压完整的 WireView 发布包。");
        return false;
    }
    if (device_profile_probe) {
        g_state.device_profile_note.clear();
    }
    const auto settings = selected_stream_settings();
    if (!settings) {
        return false;
    }
    if (!device_preview && !device_profile_probe) {
        const auto current_displays = enumerate_attached_displays();
        const bool target_is_attached = std::any_of(
            current_displays.begin(), current_displays.end(),
            [&settings](const DisplayChoice& choice) {
                return choice.device_name == settings->display_device;
            });
        if (!target_is_attached) {
            // The selector is persisted and may describe a DISPLAYn that
            // Windows removed while the control center was minimized. Never
            // launch the host with that stale value: it cannot capture it.
            refresh_display_choices();
            set_status(L"目标显示器 " + settings->display_device +
                L" 已不再附加到 Windows。已刷新列表；请重新选择可用显示器，"
                L"或在简易模式点击“扩展屏幕”以重新创建 WireView 副屏。 ");
            return false;
        }
    }
    save_stream_settings(*settings);

    std::error_code error;
    const auto temporary_directory = std::filesystem::temp_directory_path(error);
    if (error) {
        set_status(L"无法创建 WireView 状态文件。 ");
        return false;
    }
    g_state.status_file = temporary_directory /
        (L"wireview-control-" + std::to_wstring(GetCurrentProcessId()) + L".status");
    const auto host_diagnostics_directory = g_state.runtime_log_file.empty()
        ? temporary_directory
        : g_state.runtime_log_file.parent_path();
    g_state.host_diagnostics_file = host_diagnostics_directory /
        (L"host-" + std::to_wstring(GetCurrentProcessId()) + L".log");
    g_state.last_host_diagnostics_file = g_state.host_diagnostics_file;
    std::filesystem::remove(g_state.status_file, error);
    std::filesystem::remove(g_state.host_diagnostics_file, error);
    std::wstring argument = L"\"" + g_state.session_launcher_path.wstring() +
        L"\" --status-file \"" + g_state.status_file.wstring() +
        L"\" --host-diagnostics-file \"" + g_state.host_diagnostics_file.wstring() +
        // The preview deliberately remains bounded; normal mirroring has no
        // practical wall-clock limit and ends only when the user stops it.
        L"\" --duration-seconds " + (device_preview ? L"300" : L"2147483647");
    if (enable_input && !device_preview) {
        argument += L" --enable-input";
    }
    if (device_profile_probe) {
        argument += L" --device-profile-probe";
    } else if (device_preview) {
        argument += L" --device-preview";
    }
    if (!device_profile_probe && settings->width != 0U) {
        argument += L" --width " + std::to_wstring(settings->width) +
            L" --height " + std::to_wstring(settings->height);
    }
    if (!device_profile_probe) {
        argument += L" --frame-rate " + std::to_wstring(settings->frame_rate);
    }
    if (!device_preview && !device_profile_probe) {
        argument += L" --display \"" + settings->display_device + L"\"";
    }
    std::vector<wchar_t> command(argument.begin(), argument.end());
    command.push_back(L'\0');

    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (job == nullptr) {
        set_status(L"无法创建 WireView 会话。");
        return false;
    }
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!SetInformationJobObject(
            job,
            JobObjectExtendedLimitInformation,
            &limits,
            sizeof(limits))) {
        CloseHandle(job);
        std::filesystem::remove(g_state.status_file, error);
        g_state.status_file.clear();
        set_status(L"无法配置 WireView 会话。");
        return false;
    }

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    const auto working_directory = g_state.session_launcher_path.parent_path().wstring();
    if (!CreateProcessW(
            g_state.session_launcher_path.c_str(),
            command.data(),
            nullptr,
            nullptr,
            FALSE,
            CREATE_NO_WINDOW,
            nullptr,
            working_directory.c_str(),
            &startup,
            &process)) {
        const DWORD launch_error = GetLastError();
        CloseHandle(job);
        set_status(L"无法启动原生 WireView 会话入口（系统错误 " +
            std::to_wstring(launch_error) + L"）。");
        return false;
    }
    CloseHandle(process.hThread);
    if (!AssignProcessToJobObject(job, process.hProcess)) {
        TerminateProcess(process.hProcess, 1U);
        CloseHandle(process.hProcess);
        CloseHandle(job);
        std::filesystem::remove(g_state.status_file, error);
        g_state.status_file.clear();
        set_status(L"无法托管 WireView 会话进程。");
        return false;
    }

    g_state.session_job = job;
    g_state.shell_process = process.hProcess;
    g_state.preview_active = device_preview;
    g_state.profile_probe_active = device_profile_probe;
    EnableWindow(g_state.display_selector, FALSE);
    EnableWindow(g_state.refresh_displays_button, FALSE);
    EnableWindow(g_state.resolution_selector, FALSE);
    EnableWindow(g_state.frame_rate_selector, FALSE);
    EnableWindow(g_state.vdd_mode_selector, FALSE);
    update_session_action_controls();
    update_vdd_controls();
    SetTimer(window, kProcessTimer, 500U, nullptr);
    const std::wstring mode = settings->width == 0U
        ? L"原生自动"
        : std::to_wstring(settings->width) + L"x" + std::to_wstring(settings->height);
    const std::wstring frame_rate_caveat = settings->frame_rate >= 120U
        ? L"（实验性主机发送目标；设备最终屏幕帧率需实测）"
        : L"";
    if (device_profile_probe) {
        set_status(L"正在识别已连接设备的分辨率和刷新率；不会显示预览框或创建副屏。 ");
    } else if (device_preview) {
        set_status(L"正在连接设备预览：" + mode + L" @ " +
            std::to_wstring(settings->frame_rate) + L" FPS。设备端将显示白框；框外保持黑边，"
            L"不会创建副屏或开始视频流。开始镜像会自动关闭预览。");
    } else {
    set_status(enable_input
        ? L"WireView 正在启动：" + settings->display_device + L"，平板输入已启用，" + mode + L" @" +
              std::to_wstring(settings->frame_rate) + L" FPS。镜像开始后请点击“停止 WireView”结束。" + frame_rate_caveat
        : L"WireView 正在启动：" + settings->display_device + L"，安全模式，" + mode + L" @" +
              std::to_wstring(settings->frame_rate) + L" FPS。镜像开始后请点击“停止 WireView”结束。" + frame_rate_caveat);
    }
    return true;
}

void start_device_detection_probe() {
    if (g_state.device_probe_process != nullptr || g_state.shell_process != nullptr ||
        g_state.session_launcher_path.empty()) {
        return;
    }
    std::wstring command = L"\"" + g_state.session_launcher_path.wstring() +
        L"\" --detect-device";
    if (g_state.wireview_android_app_seen) {
        command += L" --require-wireview-app";
    }
    std::vector<wchar_t> buffer(command.begin(), command.end());
    buffer.push_back(L'\0');
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (CreateProcessW(g_state.session_launcher_path.c_str(), buffer.data(), nullptr, nullptr,
            FALSE, CREATE_NO_WINDOW, nullptr, g_state.session_launcher_path.parent_path().c_str(),
            &startup, &process)) {
        CloseHandle(process.hThread);
        g_state.device_probe_process = process.hProcess;
    }
}

void poll_device_detection(HWND window) {
    if (g_state.device_probe_process == nullptr) {
        start_device_detection_probe();
        return;
    }
    DWORD exit_code = STILL_ACTIVE;
    if (!GetExitCodeProcess(g_state.device_probe_process, &exit_code) || exit_code == STILL_ACTIVE) {
        return;
    }
    CloseHandle(g_state.device_probe_process);
    g_state.device_probe_process = nullptr;
    if (exit_code != 0U) {
        const bool was_connected = g_state.device_connected;
        g_state.device_connected = false;
        reset_android_device_identity();
        if (was_connected) {
            InvalidateRect(g_state.window, nullptr, FALSE);
            InvalidateRect(g_state.simple_window, nullptr, FALSE);
        }
        if (exit_code != g_state.last_auto_device_probe_exit && g_state.shell_process == nullptr) {
            const std::wstring reason = exit_code == 2U
                ? L"未找到 adb.exe。请安装 Android Platform Tools。"
                : exit_code == 4U
                    ? L"Android 设备未授权 USB 调试；请在设备上允许此电脑。"
                    : exit_code == 5U
                        ? L"检测到多台 Android 设备；请仅保留一台设备连接。"
                        : exit_code == 6U
                            ? L"WireView Android 应用未运行。请在设备上重新打开 WireView。"
                            : L"未检测到已授权的 Android 设备。请检查 USB 数据线、端口和 USB 调试授权。";
            set_status((g_state.auto_device_profile_completed || exit_code == 6U)
                ? L"连接已断开：" + reason + L"\n上次识别结果已失效。"
                : reason);
        }
        g_state.last_auto_device_probe_exit = exit_code;
        g_state.auto_device_profile_completed = false;
        // 这次识别对应的物理 USB 连接已经失效。重新插线并重新授权后，
        // 下一次成功探测必须再次自动执行“识别已连接设备”。
        g_state.auto_device_profile_attempted = false;
        if (exit_code != 6U) {
            g_state.wireview_android_app_seen = false;
        }
        return;
    }
    g_state.last_auto_device_probe_exit = 0U;
    const bool was_connected = g_state.device_connected;
    g_state.device_connected = true;
    if (!was_connected) {
        InvalidateRect(g_state.window, nullptr, FALSE);
        InvalidateRect(g_state.simple_window, nullptr, FALSE);
    }
    if ((!g_state.auto_device_profile_attempted || g_state.device_profile_refresh_requested) &&
        g_state.shell_process == nullptr) {
        // A profile server closes immediately after it reports the device mode.
        // Mark this physical USB connection before launching it, otherwise the
        // close/retry loop makes Android alternate between two UI states.
        g_state.auto_device_profile_attempted = true;
        g_state.auto_device_profile_completed = true;
        g_state.device_profile_refresh_requested = false;
        (void)start_session(window, false, false, true);
    }
}

void notify_android_of_user_stop() {
    if (g_state.session_launcher_path.empty()) {
        return;
    }
    std::wstring command = L"\"" + g_state.session_launcher_path.wstring() +
        L"\" --notify-stop";
    std::vector<wchar_t> buffer(command.begin(), command.end());
    buffer.push_back(L'\0');
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (CreateProcessW(g_state.session_launcher_path.c_str(), buffer.data(), nullptr, nullptr,
            FALSE, CREATE_NO_WINDOW, nullptr, g_state.session_launcher_path.parent_path().c_str(),
            &startup, &process)) {
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
    }
}

void notify_android_of_host_exit() {
    if (g_state.session_launcher_path.empty()) {
        return;
    }
    std::wstring command = L"\"" + g_state.session_launcher_path.wstring() +
        L"\" --notify-host-exited";
    std::vector<wchar_t> buffer(command.begin(), command.end());
    buffer.push_back(L'\0');
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (CreateProcessW(g_state.session_launcher_path.c_str(), buffer.data(), nullptr, nullptr,
            FALSE, CREATE_NO_WINDOW, nullptr, g_state.session_launcher_path.parent_path().c_str(),
            &startup, &process)) {
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
    }
}

void notify_android_of_device_identified() {
    if (g_state.session_launcher_path.empty()) {
        return;
    }
    std::wstring command = L"\"" + g_state.session_launcher_path.wstring() +
        L"\" --notify-device-identified";
    std::vector<wchar_t> buffer(command.begin(), command.end());
    buffer.push_back(L'\0');
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (CreateProcessW(g_state.session_launcher_path.c_str(), buffer.data(), nullptr, nullptr,
            FALSE, CREATE_NO_WINDOW, nullptr, g_state.session_launcher_path.parent_path().c_str(),
            &startup, &process)) {
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
    }
}

void notify_android_of_vdd_created(const StreamSettings& settings) {
    if (g_state.session_launcher_path.empty()) {
        return;
    }
    std::wstring command = L"\"" + g_state.session_launcher_path.wstring() +
        L"\" --notify-vdd-created " + std::to_wstring(settings.width) + L" " +
        std::to_wstring(settings.height) + L" " + std::to_wstring(settings.vdd_refresh_hz);
    std::vector<wchar_t> buffer(command.begin(), command.end());
    buffer.push_back(L'\0');
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (CreateProcessW(g_state.session_launcher_path.c_str(), buffer.data(), nullptr, nullptr,
            FALSE, CREATE_NO_WINDOW, nullptr, g_state.session_launcher_path.parent_path().c_str(),
            &startup, &process)) {
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
    }
}

void fill_rounded_rect(
    HDC dc, const RECT& rectangle, COLORREF fill, COLORREF border, int radius = 12) {
    HBRUSH fill_brush = CreateSolidBrush(fill);
    HPEN border_pen = CreatePen(PS_SOLID, 1, border);
    const HGDIOBJ old_brush = SelectObject(dc, fill_brush);
    const HGDIOBJ old_pen = SelectObject(dc, border_pen);
    RoundRect(dc, rectangle.left, rectangle.top, rectangle.right, rectangle.bottom,
        radius * 2, radius * 2);
    SelectObject(dc, old_pen);
    SelectObject(dc, old_brush);
    DeleteObject(border_pen);
    DeleteObject(fill_brush);
}

void paint_text(HDC dc, std::wstring_view text, RECT rectangle, HFONT font,
    COLORREF color, UINT format = DT_LEFT | DT_VCENTER | DT_SINGLELINE) {
    const HGDIOBJ old_font = SelectObject(dc, font != nullptr ? font : g_font_body);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, color);
    DrawTextW(dc, text.data(), static_cast<int>(text.size()), &rectangle, format);
    SelectObject(dc, old_font);
}

void paint_status_dot(HDC dc, int x, int y, bool connected) {
    HBRUSH brush = CreateSolidBrush(connected ? kColorGreen : kColorTextSubtle);
    const HGDIOBJ old_brush = SelectObject(dc, brush);
    const HGDIOBJ old_pen = SelectObject(dc, GetStockObject(NULL_PEN));
    Ellipse(dc, x, y, x + 10, y + 10);
    SelectObject(dc, old_pen);
    SelectObject(dc, old_brush);
    DeleteObject(brush);
}

void paint_device_thumbnail(HDC dc, const RECT& bounds, bool tablet, bool connected) {
    const int width = tablet ? 126 : 62;
    const int height = tablet ? 82 : 116;
    const int left = bounds.left + (bounds.right - bounds.left - width) / 2;
    const int top = bounds.top + (bounds.bottom - bounds.top - height) / 2;
    RECT shell{left, top, left + width, top + height};
    fill_rounded_rect(dc, shell, RGB(27, 37, 52), RGB(111, 124, 143), tablet ? 10 : 14);
    RECT screen{left + 7, top + 7, left + width - 7, top + height - 7};
    fill_rounded_rect(dc, screen, RGB(9, 18, 32), RGB(30, 45, 67), tablet ? 6 : 9);
    if (connected) {
        paint_status_dot(dc, shell.right - 13, shell.bottom - 13, true);
    }
}

void paint_screen_icon(HDC dc, RECT rectangle, COLORREF accent, bool dashed) {
    HPEN pen = CreatePen(dashed ? PS_DASH : PS_SOLID, 2, accent);
    const HGDIOBJ old_pen = SelectObject(dc, pen);
    const HGDIOBJ old_brush = SelectObject(dc, GetStockObject(NULL_BRUSH));
    RoundRect(dc, rectangle.left, rectangle.top, rectangle.right, rectangle.bottom, 12, 12);
    MoveToEx(dc, rectangle.left + 1, rectangle.bottom - 16, nullptr);
    LineTo(dc, rectangle.right - 1, rectangle.bottom - 16);
    SelectObject(dc, old_brush);
    SelectObject(dc, old_pen);
    DeleteObject(pen);
}

void paint_ui_asset(HDC dc, Gdiplus::Image* image, const RECT& bounds) {
    if (image == nullptr || image->GetLastStatus() != Gdiplus::Ok ||
        bounds.right <= bounds.left || bounds.bottom <= bounds.top) {
        return;
    }
    Gdiplus::Graphics graphics(dc);
    graphics.SetCompositingMode(Gdiplus::CompositingModeSourceOver);
    graphics.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
    graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);
    graphics.DrawImage(image, Gdiplus::Rect(bounds.left, bounds.top,
        bounds.right - bounds.left, bounds.bottom - bounds.top));
}

void paint_secondary_action_icon(HDC dc, int id, const RECT& bounds, COLORREF color) {
    const int size = 18;
    const int left = bounds.left + 16;
    const int top = (bounds.top + bounds.bottom - size) / 2;
    HPEN pen = CreatePen(PS_SOLID, 2, color);
    const HGDIOBJ old_pen = SelectObject(dc, pen);
    const HGDIOBJ old_brush = SelectObject(dc, GetStockObject(NULL_BRUSH));
    if (id == kDisplaySettingsButton || id == kSimpleDisplaySettingsButton) {
        Rectangle(dc, left, top, left + size, top + 12);
        MoveToEx(dc, left + size / 2, top + 12, nullptr);
        LineTo(dc, left + size / 2, top + 17);
        MoveToEx(dc, left + 4, top + 17, nullptr);
        LineTo(dc, left + size - 4, top + 17);
    } else if (id == kPreviewDeviceButton) {
        Ellipse(dc, left, top, left + size, top + size);
        MoveToEx(dc, left + size - 2, top + size - 2, nullptr);
        LineTo(dc, left + size + 5, top + size + 5);
    } else if (id == kCustomModesButton) {
        Rectangle(dc, left, top + 2, left + size, top + size - 2);
        MoveToEx(dc, left + 5, top + 5, nullptr);
        LineTo(dc, left + 5, top + size - 5);
        MoveToEx(dc, left + 13, top + 5, nullptr);
        LineTo(dc, left + 13, top + size - 5);
    } else if (id == kCreateVddButton) {
        Ellipse(dc, left, top, left + size, top + size);
        MoveToEx(dc, left + size / 2, top + 4, nullptr);
        LineTo(dc, left + size / 2, top + size - 4);
        MoveToEx(dc, left + 4, top + size / 2, nullptr);
        LineTo(dc, left + size - 4, top + size / 2);
    } else if (id == kRemoveVddButton || id == kRecoverVddButton) {
        Rectangle(dc, left + 3, top + 5, left + size - 3, top + size);
        MoveToEx(dc, left + 2, top + 4, nullptr);
        LineTo(dc, left + size - 2, top + 4);
        MoveToEx(dc, left + 7, top + 1, nullptr);
        LineTo(dc, left + size - 7, top + 1);
    } else if (id == kDiagnosticsButton) {
        Rectangle(dc, left + 2, top, left + size - 2, top + size);
        MoveToEx(dc, left + 5, top + 6, nullptr);
        LineTo(dc, left + size - 5, top + 6);
        MoveToEx(dc, left + 5, top + 11, nullptr);
        LineTo(dc, left + size - 5, top + 11);
    }
    SelectObject(dc, old_brush);
    SelectObject(dc, old_pen);
    DeleteObject(pen);
}

struct SimpleLayout {
    RECT client{};
    RECT device_card{};
    RECT mirror{};
    RECT extend{};
    RECT display_only{};
    RECT input{};
    RECT stop{};
    RECT display_settings{};
    RECT return_full{};
    RECT footer{};
};

SimpleLayout simple_layout(HWND window) {
    SimpleLayout layout{};
    GetClientRect(window, &layout.client);
    const int right = layout.client.right - 24;
    const int bottom = layout.client.bottom;
    layout.device_card = RECT{24, 88, right, 264};
    layout.mirror = RECT{40, 356, 710, 658};
    layout.extend = RECT{730, 356, right - 16, 658};
    layout.display_only = RECT{40, 744, 724, 840};
    layout.input = RECT{724, 744, right - 16, 840};
    layout.display_settings = RECT{24, bottom - 154, right - 250, bottom - 78};
    layout.stop = RECT{right - 234, bottom - 154, right, bottom - 78};
    layout.return_full = RECT{0, 0, 0, 0};
    layout.footer = RECT{24, bottom - 62, right, bottom - 18};
    return layout;
}

struct FullLayout {
    RECT client{};
    RECT device_card{};
    RECT config_card{};
    RECT vdd_card{};
    RECT input_card{};
    RECT summary_card{};
    RECT action_card{};
    RECT tools_card{};
    RECT log_card{};
};

FullLayout full_layout(HWND window) {
    FullLayout layout{};
    GetClientRect(window, &layout.client);
    const int right = layout.client.right - 24;
    const bool expanded = g_state.display_config_expanded;
    const int config_bottom = expanded ? 456 : 278;
    const int input_top = expanded ? 472 : 294;
    const int input_bottom = expanded ? 616 : 582;
    const int summary_top = expanded ? 632 : 598;
    const int summary_bottom = expanded ? 786 : 798;
    const int tools_top = expanded ? 802 : 814;
    layout.device_card = RECT{24, 88, 708, 278};
    layout.config_card = RECT{724, 88, right, config_bottom};
    layout.vdd_card = RECT{24, 294, 708, 582};
    layout.input_card = RECT{724, input_top, right, input_bottom};
    layout.summary_card = RECT{24, summary_top, 890, summary_bottom};
    layout.action_card = RECT{906, summary_top, right, summary_bottom};
    layout.tools_card = RECT{24, tools_top, 708, layout.client.bottom - 78};
    layout.log_card = RECT{724, tools_top, right, layout.client.bottom - 78};
    return layout;
}

void move_control(HWND control, const RECT& rectangle) {
    if (control != nullptr) {
        MoveWindow(control, rectangle.left, rectangle.top,
            rectangle.right - rectangle.left, rectangle.bottom - rectangle.top, TRUE);
    }
}

void paint_simple_window(HWND window, HDC dc) {
    const SimpleLayout layout = simple_layout(window);
    const RECT& client = layout.client;
    FillRect(dc, &client, g_background_brush);
    paint_text(dc, L"WireView 有线副屏 — USB 平板副屏", RECT{24, 12, 580, 48},
        g_font_title, kColorText);
    paint_text(dc, L"简易模式", RECT{42, 52, 146, 82}, g_font_heading, kColorBlue);
    paint_text(dc, L"完整模式", RECT{176, 52, 282, 82}, g_font_heading, kColorTextMuted);
    HPEN active_pen = CreatePen(PS_SOLID, 3, kColorBlue);
    HGDIOBJ old_pen = SelectObject(dc, active_pen);
    MoveToEx(dc, 40, 88, nullptr);
    LineTo(dc, 122, 88);
    SelectObject(dc, old_pen);
    DeleteObject(active_pen);

    fill_rounded_rect(dc, layout.device_card, kColorPanel, RGB(38, 69, 120), 14);
    paint_device_thumbnail(dc, RECT{54, 106, 344, 246},
        g_state.device_is_tablet, g_state.device_connected);
    const std::wstring connected = g_state.device_connected ? L"设备已连接" : L"等待设备连接";
    paint_text(dc, connected, RECT{380, 124, 620, 156}, g_font_body,
        g_state.device_connected ? kColorGreen : kColorTextMuted);
    paint_text(dc, g_state.device_model, RECT{380, 158, 850, 204}, g_font_device, kColorText);
    const std::wstring detail = g_state.device_connected
        ? L"USB 已连接 · " +
              (g_state.detected_device_width == 0U ? std::wstring(L"正在读取分辨率")
                  : std::to_wstring(g_state.detected_device_width) + L"×" +
                        std::to_wstring(g_state.detected_device_height)) +
              L" · " + g_state.device_active_refresh + L" Hz"
        : L"请连接 USB 数据线并允许 USB 调试";
    paint_text(dc, detail, RECT{380, 210, 920, 240}, g_font_body, kColorTextSecondary);

    const RECT display_card{24, 282, client.right - 24, client.bottom - 172};
    fill_rounded_rect(dc, display_card, kColorPanel, kColorBorder, 14);
    paint_text(dc, L"显示方式", RECT{40, 298, 160, 332}, g_font_heading, kColorText);
    paint_text(dc, L"选择本次启动的显示方式（启动后生效）", RECT{156, 300, 520, 332},
        g_font_small, kColorTextMuted);
    paint_text(dc, L"本次会话输入", RECT{40, 678, 218, 712}, g_font_heading, kColorText);
    paint_text(dc, L"启动 WireView 时应用此设置", RECT{192, 680, 490, 712},
        g_font_small, kColorTextMuted);
    const RECT input_shell{40, 738, client.right - 40, 854};
    fill_rounded_rect(dc, input_shell, kColorControl, kColorBorder, 12);

    fill_rounded_rect(dc, layout.display_settings, kColorPanel, kColorBorder, 12);
    paint_text(dc, L"▣   Windows 显示设置", RECT{56, layout.display_settings.top,
            420, layout.display_settings.bottom}, g_font_body, kColorTextSecondary);
    paint_text(dc, L"点击打开系统显示设置", RECT{client.right - 330, layout.display_settings.top,
            client.right - 54, layout.display_settings.bottom}, g_font_small, kColorBlue,
        DT_RIGHT | DT_VCENTER | DT_SINGLELINE);

    fill_rounded_rect(dc, layout.footer, kColorPanel, kColorBorder, 10);
    paint_status_dot(dc, 42, layout.footer.top + 17, g_state.device_connected);
    paint_text(dc, g_state.device_connected ? L"就绪：设备已连接，可启动" : L"等待 Android 设备连接",
        RECT{62, layout.footer.top, 490, layout.footer.bottom}, g_font_small, kColorTextMuted);
    paint_text(dc, L"副屏分辨率：" + selector_text(g_state.resolution_selector) + L" · 刷新率：" +
            selector_text(g_state.vdd_mode_selector) + L" · Parsec VDD " +
            (g_state.parsec_vdd_available ? L"可用" : L"不可用"),
        RECT{510, layout.footer.top, client.right - 42, layout.footer.bottom}, g_font_small,
        kColorTextMuted, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
}

void paint_full_window_reference(HWND window, HDC dc) {
    const FullLayout layout = full_layout(window);
    const RECT& client = layout.client;
    FillRect(dc, &client, g_background_brush);
    paint_text(dc, L"WireView 有线副屏 — USB 平板副屏", RECT{24, 12, 580, 48},
        g_font_title, kColorText);
    paint_text(dc, L"简易模式", RECT{42, 52, 146, 82}, g_font_heading, kColorTextMuted);
    paint_text(dc, L"完整模式", RECT{176, 52, 282, 82}, g_font_heading, kColorBlue);
    HPEN active_pen = CreatePen(PS_SOLID, 3, kColorBlue);
    HGDIOBJ old_pen = SelectObject(dc, active_pen);
    MoveToEx(dc, 174, 88, nullptr);
    LineTo(dc, 256, 88);
    SelectObject(dc, old_pen);
    DeleteObject(active_pen);

    for (const RECT card : {layout.device_card, layout.config_card, layout.vdd_card,
             layout.input_card, layout.summary_card, layout.action_card, layout.tools_card,
             layout.log_card}) {
        fill_rounded_rect(dc, card, kColorPanel, kColorBorder, 12);
    }

    paint_text(dc, L"1", RECT{42, 108, 66, 138}, g_font_body, kColorBlue, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    paint_text(dc, L"设备概览", RECT{76, 102, 280, 140}, g_font_heading, kColorText);
    paint_device_thumbnail(dc, RECT{44, 150, 210, 260}, g_state.device_is_tablet, g_state.device_connected);
    paint_text(dc, g_state.device_connected ? L"设备已连接" : L"等待设备连接",
        RECT{248, 150, 420, 180}, g_font_body,
        g_state.device_connected ? kColorGreen : kColorTextMuted);
    paint_text(dc, g_state.device_model, RECT{248, 180, 630, 220}, g_font_device, kColorText);
    const std::wstring device_detail = g_state.device_connected
        ? L"USB 已连接 · " +
              (g_state.detected_device_width == 0U ? std::wstring(L"正在读取分辨率")
                  : std::to_wstring(g_state.detected_device_width) + L"×" +
                        std::to_wstring(g_state.detected_device_height)) +
              L" · " + g_state.device_active_refresh + L" Hz"
        : L"请连接 USB 数据线并允许 USB 调试";
    paint_text(dc, device_detail, RECT{248, 224, 664, 252}, g_font_body, kColorTextSecondary);

    paint_text(dc, L"2", RECT{742, 108, 766, 138}, g_font_body, kColorBlue, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    paint_text(dc, L"显示配置", RECT{776, 102, 980, 140}, g_font_heading, kColorText);
    const int config_column = 157;
    const std::array<std::wstring, 4U> config_labels{L"目标显示器", L"分辨率", L"刷新率", L"视频帧率"};
    const std::array<std::wstring, 4U> config_values{
        selected_display_choice() ? selected_display_choice()->device_name : L"未选择",
        selector_text(g_state.resolution_selector), selector_text(g_state.vdd_mode_selector),
        selector_text(g_state.frame_rate_selector)};
    for (int index = 0; index < 4; ++index) {
        const int left = 748 + index * config_column;
        paint_text(dc, config_labels[static_cast<std::size_t>(index)], RECT{left, 172, left + 146, 198},
            g_font_small, kColorTextMuted);
        paint_text(dc, config_values[static_cast<std::size_t>(index)], RECT{left, 200, left + 146, 232},
            g_font_body, kColorTextSecondary);
    }
    paint_text(dc, g_state.display_config_expanded ? L"⌃" : L"›",
        RECT{layout.config_card.right - 54, 166, layout.config_card.right - 26, 224},
        g_font_heading, kColorTextMuted, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    if (g_state.display_config_expanded) {
        HPEN divider = CreatePen(PS_SOLID, 1, kColorBorder);
        old_pen = SelectObject(dc, divider);
        MoveToEx(dc, 746, 250, nullptr);
        LineTo(dc, layout.config_card.right - 22, 250);
        SelectObject(dc, old_pen);
        DeleteObject(divider);
        paint_text(dc, L"展开后可直接修改本次会话参数", RECT{748, 262,
            layout.config_card.right - 34, 286}, g_font_small, kColorTextMuted);
    }

    paint_text(dc, L"3", RECT{42, 314, 66, 344}, g_font_body, kColorBlue, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    paint_text(dc, L"WireView 副屏 / Parsec VDD", RECT{76, 308, 430, 346}, g_font_heading, kColorText);
    const bool extension = !g_state.owned_display_device.empty();
    paint_text(dc, L"当前 WireView 副屏数量", RECT{168, 374, 402, 402}, g_font_small, kColorTextMuted);
    paint_text(dc, extension ? L"1 个" : L"0 个", RECT{168, 400, 322, 452}, g_font_device, kColorText);
    paint_text(dc, L"启动方式（由副屏数量决定）", RECT{168, 456, 410, 480}, g_font_small, kColorTextMuted);
    paint_text(dc, extension ? L"扩展屏幕 模式" : L"复制屏幕 模式", RECT{168, 482, 416, 520},
        g_font_heading, extension ? kColorViolet : kColorBlue);
    paint_text(dc, L"VDD", RECT{64, 382, 144, 446}, g_font_device, kColorViolet,
        DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    paint_text(dc, L"创建、移除和残留清理不会影响其他显示器。",
        RECT{42, 536, 676, 562}, g_font_small, kColorTextMuted);

    paint_text(dc, L"4", RECT{742, layout.input_card.top + 20, 766, layout.input_card.top + 50},
        g_font_body, kColorBlue, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    paint_text(dc, L"本次会话输入（启动时应用）", RECT{776, layout.input_card.top + 14,
            1110, layout.input_card.top + 52}, g_font_heading, kColorText);
    const int input_shell_top = layout.input_card.top +
        (g_state.display_config_expanded ? 64 : 78);
    const int input_shell_height = g_state.display_config_expanded ? 80 : 106;
    const RECT input_shell{748, input_shell_top, layout.input_card.right - 22,
        input_shell_top + input_shell_height};
    fill_rounded_rect(dc, input_shell, kColorControl, kColorBorder, 12);
    if (!g_state.display_config_expanded) {
        paint_text(dc, L"该设置仅在本次启动时生效，下次启动可重新选择。",
            RECT{748, layout.input_card.bottom - 48, layout.input_card.right - 26,
                layout.input_card.bottom - 20}, g_font_small, kColorTextMuted);
    }

    paint_text(dc, L"5", RECT{42, layout.summary_card.top + 20, 66, layout.summary_card.top + 50},
        g_font_body, kColorBlue, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    paint_text(dc, L"会话摘要 / 即将启动", RECT{76, layout.summary_card.top + 14, 404,
            layout.summary_card.top + 52}, g_font_heading, kColorText);
    const RECT summary_inner{42, layout.summary_card.top + 64, 872, layout.summary_card.bottom - 48};
    fill_rounded_rect(dc, summary_inner, kColorControl, kColorBorder, 10);
    paint_text(dc, extension ? L"扩展屏幕" : L"复制屏幕", RECT{62, summary_inner.top + 16,
            196, summary_inner.bottom - 12},
        g_font_heading, extension ? kColorViolet : kColorBlue);
    paint_text(dc, L"副屏数量\n" + std::to_wstring(extension ? 1 : 0) + L" 个",
        RECT{210, summary_inner.top + 10, 316, summary_inner.bottom - 10}, g_font_small,
        kColorTextSecondary, DT_LEFT | DT_WORDBREAK);
    paint_text(dc, L"目标显示器\n" + (selected_display_choice() ? selected_display_choice()->device_name : L"未选择"),
        RECT{336, summary_inner.top + 10, 530, summary_inner.bottom - 10}, g_font_small,
        kColorTextSecondary, DT_LEFT | DT_WORDBREAK);
    paint_text(dc, std::wstring(g_state.session_input_enabled ? L"触控与手写笔" : L"仅显示") + L"\n" +
            selector_text(g_state.frame_rate_selector),
        RECT{616, summary_inner.top + 10, 838, summary_inner.bottom - 10}, g_font_small,
        kColorTextSecondary, DT_LEFT | DT_WORDBREAK);
    paint_text(dc, L"点击“启动 WireView”后，将创建副屏并按以上配置运行。",
        RECT{48, layout.summary_card.bottom - 38, 858, layout.summary_card.bottom - 14},
        g_font_small, kColorTextMuted);

    paint_text(dc, L"6  启动与停止", RECT{926, layout.action_card.top + 14, 1220,
            layout.action_card.top + 52}, g_font_heading, kColorText);
    paint_text(dc, L"7  高级工具", RECT{42, layout.tools_card.top + 14, 300,
            layout.tools_card.top + 52}, g_font_heading, kColorText);
    paint_text(dc, L"8  诊断与日志", RECT{742, layout.log_card.top + 14, 1050,
            layout.log_card.top + 52}, g_font_heading, kColorText);
    paint_text(dc, L"连接诊断、日志查看与导出", RECT{778, layout.log_card.top + 66, 1140,
            layout.log_card.top + 92},
        g_font_body, kColorTextSecondary);
    paint_text(dc, L"›", RECT{layout.log_card.right - 64, layout.log_card.top + 44,
            layout.log_card.right - 30, layout.log_card.top + 108},
        g_font_heading, kColorTextMuted, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    const RECT footer{24, client.bottom - 62, client.right - 24, client.bottom - 18};
    fill_rounded_rect(dc, footer, kColorPanel, kColorBorder, 10);
    paint_status_dot(dc, 42, footer.top + 17, g_state.device_connected);
    paint_text(dc, g_state.device_connected ? L"就绪：设备已连接，可启动" : L"等待 Android 设备连接",
        RECT{62, footer.top, 520, footer.bottom}, g_font_small, kColorTextMuted);
    paint_text(dc, L"版本 1.0.0", RECT{client.right - 264, footer.top,
            client.right - 132, footer.bottom}, g_font_small, kColorTextMuted,
        DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
}

void paint_diagnostics_page(HWND window, HDC dc) {
    RECT client{};
    GetClientRect(window, &client);
    FillRect(dc, &client, g_background_brush);
    paint_text(dc, L"WireView 有线副屏 — 诊断与日志", RECT{24, 12, 580, 48},
        g_font_title, kColorText);
    paint_text(dc, L"用于查看连接状态、会话输出与 Parsec VDD 诊断信息。",
        RECT{40, 96, client.right - 40, 126}, g_font_small, kColorTextMuted);
    fill_rounded_rect(dc, RECT{24, 128, client.right - 24, client.bottom - 24},
        kColorPanel, kColorBorder, 14);
}

// 旧版手绘布局仍保留在参考绘制调用之后，便于历史比对；当前逻辑在前者完成后
// 返回。MSVC 19.44 会把这段历史代码报告为 C4702，而项目以 /WX 构建。
#pragma warning(push)
#pragma warning(disable : 4702)
void paint_full_window(HWND window, HDC dc) {
    paint_full_window_reference(window, dc);
    return;
    const FullLayout layout = full_layout(window);
    const RECT& client = layout.client;
    FillRect(dc, &client, g_background_brush);
    paint_text(dc, L"WireView 有线副屏 — USB 平板副屏", RECT{24, 12, 560, 48},
        g_font_title, kColorText);
    paint_text(dc, L"完整模式", RECT{148, 52, 280, 82},
        g_font_heading, kColorBlue);
    paint_text(dc, L"简易模式", RECT{24, 52, 136, 82},
        g_font_body, kColorTextMuted);

    for (const RECT rectangle : {layout.device_card, layout.config_card, layout.vdd_card,
             layout.input_card, layout.summary_card, layout.action_card, layout.tools_card,
             layout.log_card}) {
        fill_rounded_rect(dc, rectangle, kColorPanel, kColorBorder, 12);
    }

    paint_text(dc, L"1  设备概览", RECT{layout.device_card.left + 18, 98,
            layout.device_card.left + 256, 136},
        g_font_heading, kColorText);
    paint_device_thumbnail(dc, RECT{layout.device_card.left + 18, 132,
            layout.device_card.left + 166, 238},
        g_state.device_is_tablet, g_state.device_connected);
    paint_status_dot(dc, layout.device_card.left + 190, 150, g_state.device_connected);
    paint_text(dc, g_state.device_connected ? L"已连接" : L"未连接",
        RECT{layout.device_card.left + 208, 136, layout.device_card.left + 356, 170}, g_font_body,
        g_state.device_connected ? kColorGreen : kColorTextMuted);
    paint_text(dc, g_state.device_name, RECT{layout.device_card.left + 188, 168,
            layout.device_card.right - 20, 205},
        g_font_device, kColorText);
    std::wstring device_detail = L"型号：" + g_state.device_model;
    if (g_state.detected_device_width != 0U) {
        device_detail += L" · " + std::to_wstring(g_state.detected_device_width) + L"×" +
            std::to_wstring(g_state.detected_device_height) + L" · " +
            g_state.device_active_refresh + L" Hz";
    }
    paint_text(dc, device_detail, RECT{layout.device_card.left + 188, 205,
            layout.device_card.right - 20, 234},
        g_font_small, kColorTextMuted);

    const int config_left = layout.config_card.left + 18;
    const int config_width = layout.config_card.right - layout.config_card.left - 36;
    const int config_column = std::max(100, (config_width - 18) / 4);
    paint_text(dc, L"2  显示配置", RECT{config_left, 98, config_left + 244, 136},
        g_font_heading, kColorText);
    paint_text(dc, L"目标显示器", RECT{config_left, 136, config_left + config_column, 164},
        g_font_small, kColorTextMuted);
    paint_text(dc, L"投屏分辨率", RECT{config_left + config_column + 6, 136,
            config_left + config_column * 2 + 6, 164}, g_font_small, kColorTextMuted);
    paint_text(dc, L"副屏刷新率", RECT{config_left + config_column * 2 + 12, 136,
            config_left + config_column * 3 + 12, 164}, g_font_small, kColorTextMuted);
    paint_text(dc, L"视频帧率", RECT{config_left + config_column * 3 + 18, 136,
            layout.config_card.right - 18, 164}, g_font_small, kColorTextMuted);

    paint_text(dc, L"3  WireView 副屏 / Parsec VDD", RECT{layout.vdd_card.left + 18, 276,
            layout.vdd_card.left + 406, 314},
        g_font_heading, kColorText);
    const std::wstring count = std::to_wstring(g_state.owned_display_device.empty() ? 0 : 1);
    paint_text(dc, L"当前 WireView 副屏", RECT{layout.vdd_card.left + 24, 326,
            layout.vdd_card.left + 246, 352},
        g_font_small, kColorTextMuted);
    paint_text(dc, count + L" 个", RECT{layout.vdd_card.left + 24, 350,
            layout.vdd_card.left + 196, 392},
        g_font_device, kColorText);
    paint_text(dc, L"当前启动方式", RECT{layout.vdd_card.left + 226, 326,
            layout.vdd_card.left + 406, 352},
        g_font_small, kColorTextMuted);
    paint_text(dc, g_state.owned_display_device.empty() ? L"复制屏幕" : L"扩展屏幕",
        RECT{layout.vdd_card.left + 226, 350, layout.vdd_card.left + 426, 392}, g_font_heading,
        g_state.owned_display_device.empty() ? kColorBlue : kColorViolet);
    const int vdd_detail_left = layout.vdd_card.right - 152;
    paint_text(dc, L"Parsec VDD", RECT{vdd_detail_left, 326, layout.vdd_card.right - 22, 352},
        g_font_small, kColorTextMuted);
    paint_status_dot(dc, vdd_detail_left, 362, g_state.parsec_vdd_available);
    const std::wstring vdd_state = g_state.parsec_vdd_available
        ? L"可用 · 版本 " + std::to_wstring(g_state.parsec_vdd_version)
        : L"不可用或未安装";
    paint_text(dc, vdd_state, RECT{vdd_detail_left + 18, 348, layout.vdd_card.right - 22, 382},
        g_font_body, g_state.parsec_vdd_available ? kColorGreen : kColorTextMuted);
    paint_text(dc, L"系统副屏 " + std::to_wstring(g_state.parsec_display_count) + L" 个",
        RECT{vdd_detail_left, 382, layout.vdd_card.right - 22, 412}, g_font_small, kColorTextSecondary);

    paint_text(dc, L"4  本次会话输入", RECT{layout.input_card.left + 18, 276,
            layout.input_card.left + 332, 314},
        g_font_heading, kColorText);
    paint_text(dc, L"该设置在启动 WireView 时应用，不会实时改变当前会话。",
        RECT{layout.input_card.left + 20, 444, layout.input_card.right - 20, 476},
        g_font_small, kColorTextMuted);

    paint_text(dc, L"5  会话摘要 / 即将启动", RECT{layout.summary_card.left + 18, 518,
            layout.summary_card.left + 396, 556},
        g_font_heading, kColorText);
    const StreamSettings* unused = nullptr;
    (void)unused;
    const std::wstring display_name = selected_display_choice() != nullptr
        ? selected_display_choice()->device_name : L"未选择";
    const std::wstring resolution = g_state.resolution_selector != nullptr
        ? selector_text(g_state.resolution_selector) : L"未选择";
    const std::wstring refresh = g_state.vdd_mode_selector != nullptr
        ? selector_text(g_state.vdd_mode_selector) : L"--";
    const std::wstring fps = g_state.frame_rate_selector != nullptr
        ? selector_text(g_state.frame_rate_selector) : L"--";
    paint_text(dc, g_state.owned_display_device.empty() ? L"复制屏幕" : L"扩展屏幕",
        RECT{layout.summary_card.left + 24, 566, layout.summary_card.left + 146, 600}, g_font_heading,
        g_state.owned_display_device.empty() ? kColorBlue : kColorViolet);
    paint_text(dc, L"目标 " + display_name, RECT{layout.summary_card.left + 156, 562,
            layout.summary_card.left + 446, 590},
        g_font_small, kColorTextSecondary);
    paint_text(dc, resolution + L" · " + refresh + L" · " + fps,
        RECT{layout.summary_card.left + 156, 590, layout.summary_card.right - 198, 620},
        g_font_body, kColorText);
    paint_text(dc, g_state.session_input_enabled ? L"输入：触控与手写笔" : L"输入：仅显示",
        RECT{layout.summary_card.right - 180, 566, layout.summary_card.right - 26, 620},
        g_font_small, kColorTextSecondary,
        DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
    paint_text(dc, L"启动时将使用以上配置；运行期间可随时停止。",
        RECT{layout.summary_card.left + 24, 632, layout.summary_card.right - 26, 660},
        g_font_small, kColorTextMuted);

    paint_text(dc, L"6  启动与停止", RECT{layout.action_card.left + 18, 518,
            layout.action_card.left + 294, 556},
        g_font_heading, kColorText);
    paint_text(dc, L"7  高级工具", RECT{layout.tools_card.left + 18, 706,
            layout.tools_card.left + 276, 744},
        g_font_heading, kColorText);
    paint_text(dc, L"8  诊断与日志", RECT{layout.log_card.left + 18, 706,
            layout.log_card.left + 314, 744},
        g_font_heading, kColorText);
}
#pragma warning(pop)

void paint_gradient_button(HDC dc, const RECT& rectangle, bool disabled, bool hovered, bool pressed) {
    if (disabled) {
        fill_rounded_rect(dc, rectangle, RGB(35, 43, 55), kColorBorder, 10);
        return;
    }
    const unsigned short left_red = pressed ? 0x5800U : (hovered ? 0x8A00U : 0x7A00U);
    const unsigned short left_green = pressed ? 0x3100U : (hovered ? 0x5600U : 0x4500U);
    const unsigned short left_blue = pressed ? 0xB600U : (hovered ? 0xFF00U : 0xF600U);
    const unsigned short right_red = pressed ? 0x2300U : (hovered ? 0x3C00U : 0x2F00U);
    const unsigned short right_green = pressed ? 0x5400U : (hovered ? 0x8200U : 0x6B00U);
    const unsigned short right_blue = pressed ? 0xB800U : (hovered ? 0xFF00U : 0xFF00U);
    TRIVERTEX vertices[2]{
        {rectangle.left, rectangle.top, left_red, left_green, left_blue, 0xFF00},
        {rectangle.right, rectangle.bottom, right_red, right_green, right_blue, 0xFF00},
    };
    GRADIENT_RECT gradient{0, 1};
    HRGN clip = CreateRoundRectRgn(rectangle.left, rectangle.top,
        rectangle.right + 1, rectangle.bottom + 1, 20, 20);
    SelectClipRgn(dc, clip);
    GradientFill(dc, vertices, 2U, &gradient, 1U, GRADIENT_FILL_RECT_H);
    SelectClipRgn(dc, nullptr);
    DeleteObject(clip);
}

bool paint_owner_draw_button_unbuffered(const DRAWITEMSTRUCT& item) {
    if (item.CtlType == ODT_COMBOBOX) {
        RECT bounds = item.rcItem;
        HBRUSH background = CreateSolidBrush(
            (item.itemState & ODS_SELECTED) != 0U ? kColorPanelElevated : kColorControl);
        FillRect(item.hDC, &bounds, background);
        DeleteObject(background);
        std::wstring text_value;
        LRESULT item_index = static_cast<LRESULT>(item.itemID);
        if (item_index < 0) {
            item_index = SendMessageW(item.hwndItem, CB_GETCURSEL, 0U, 0);
        }
        if (item_index >= 0) {
            const LRESULT length = SendMessageW(
                item.hwndItem, CB_GETLBTEXTLEN, static_cast<WPARAM>(item_index), 0);
            if (length >= 0) {
                text_value.resize(static_cast<std::size_t>(length) + 1U);
                const LRESULT copied = SendMessageW(item.hwndItem, CB_GETLBTEXT,
                    static_cast<WPARAM>(item_index),
                    reinterpret_cast<LPARAM>(text_value.data()));
                text_value.resize(copied > 0 ? static_cast<std::size_t>(copied) : 0U);
            }
        }
        bounds.left += 10;
        bounds.right -= 4;
        paint_text(item.hDC, text_value, bounds, g_font_body,
            (item.itemState & ODS_DISABLED) != 0U ? kColorTextSubtle : kColorTextSecondary);
        return true;
    }
    if (item.CtlType != ODT_BUTTON) {
        return false;
    }
    const bool disabled = (item.itemState & ODS_DISABLED) != 0U;
    const bool hovered = !disabled && g_hot_button == item.hwndItem;
    const bool pressed = !disabled && ((item.itemState & ODS_SELECTED) != 0U ||
        g_pressed_button == item.hwndItem);
    const int id = static_cast<int>(item.CtlID);
    RECT bounds = item.rcItem;
    const bool simple_surface = g_state.simple_mode &&
        (id == kSimpleMirrorButton || id == kSimpleExtendButton ||
            id == kSimpleDisplayOnlyButton || id == kSimpleInputEnabledButton ||
            id == kSimpleStopButton || id == kSimpleDisplaySettingsButton);
    HBRUSH surface_brush = CreateSolidBrush(simple_surface ? kColorApp : kColorPanel);
    FillRect(item.hDC, &bounds, surface_brush);
    DeleteObject(surface_brush);
    if (pressed) {
        OffsetRect(&bounds, 0, 1);
    }

    if (id == kSimpleMirrorButton || id == kSimpleExtendButton) {
        const bool copy = id == kSimpleMirrorButton;
        const COLORREF accent = copy ? kColorBlue : kColorViolet;
        const COLORREF card_fill = disabled ? kColorControl
            : pressed ? RGB(16, 28, 45)
            : hovered ? RGB(25, 42, 68) : kColorPanel;
        fill_rounded_rect(item.hDC, bounds, card_fill,
            disabled ? kColorBorder : (hovered ? RGB(105, 150, 255) : accent), 14);
        const int center = (bounds.left + bounds.right) / 2;
        const int content_top = bounds.top + std::max(30,
            (static_cast<int>(bounds.bottom) - static_cast<int>(bounds.top) - 202) / 2);
        const RECT illustration_bounds{center - 180, content_top - 24, center + 180,
            content_top + 132};
        paint_ui_asset(item.hDC,
            copy ? g_copy_screen_asset.get() : g_extend_screen_asset.get(), illustration_bounds);
        paint_text(item.hDC, copy ? L"复制屏幕" : L"扩展屏幕",
            RECT{bounds.left + 20, content_top + 134, bounds.right - 20, content_top + 172},
            g_font_heading, disabled ? kColorTextSubtle : kColorText,
            DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        paint_text(item.hDC,
            copy ? L"副屏显示与主屏相同的内容" : L"将副屏作为扩展显示区域",
            RECT{bounds.left + 20, content_top + 170, bounds.right - 20, content_top + 200},
            g_font_body, kColorTextMuted, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        paint_text(item.hDC,
            copy ? L"推荐用于演示与分享" : L"推荐用于效率与创作",
            RECT{bounds.left + 20, content_top + 200, bounds.right - 20, content_top + 230},
            g_font_small, accent, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        return true;
    }

    if (id == kInputDisplayOnlyButton || id == kInputEnabledButton ||
        id == kSimpleDisplayOnlyButton || id == kSimpleInputEnabledButton) {
        const bool input_enabled = id == kInputEnabledButton || id == kSimpleInputEnabledButton;
        const bool selected = input_enabled
            ? g_state.session_input_enabled : !g_state.session_input_enabled;
        const COLORREF accent = kColorBlue;
        const COLORREF input_fill = disabled ? RGB(30, 38, 50)
            : pressed ? RGB(19, 34, 57)
            : selected ? (hovered ? RGB(35, 63, 106) : RGB(25, 45, 77))
            : hovered ? RGB(31, 45, 66) : kColorControl;
        fill_rounded_rect(item.hDC, bounds, input_fill,
            selected ? accent : (hovered ? kColorBorderHover : kColorBorder), 10);
        wchar_t text[128]{};
        GetWindowTextW(item.hwndItem, text, static_cast<int>(std::size(text)));
        const bool simple_input = id == kSimpleDisplayOnlyButton || id == kSimpleInputEnabledButton;
        const int center_y = (bounds.top + bounds.bottom) / 2;
        if (simple_input) {
            const int dot_x = bounds.left + 28;
            if (selected) {
                HBRUSH dot = CreateSolidBrush(kColorBlue);
                const HGDIOBJ old_brush = SelectObject(item.hDC, dot);
                const HGDIOBJ old_pen = SelectObject(item.hDC, GetStockObject(NULL_PEN));
                Ellipse(item.hDC, dot_x - 9, center_y - 9, dot_x + 9, center_y + 9);
                SelectObject(item.hDC, old_pen);
                SelectObject(item.hDC, old_brush);
                DeleteObject(dot);
            } else {
                HPEN ring = CreatePen(PS_SOLID, 2, kColorTextSubtle);
                const HGDIOBJ old_pen = SelectObject(item.hDC, ring);
                const HGDIOBJ old_brush = SelectObject(item.hDC, GetStockObject(NULL_BRUSH));
                Ellipse(item.hDC, dot_x - 9, center_y - 9, dot_x + 9, center_y + 9);
                SelectObject(item.hDC, old_brush);
                SelectObject(item.hDC, old_pen);
                DeleteObject(ring);
            }
        }
        const int icon_size = simple_input ? 34 : 30;
        const int icon_left = simple_input ? bounds.left + 66 : bounds.left + 42;
        const RECT icon_bounds{icon_left, center_y - icon_size / 2,
            icon_left + icon_size, center_y + icon_size / 2};
        paint_ui_asset(item.hDC,
            input_enabled ? g_touch_pen_asset.get() : g_display_only_asset.get(), icon_bounds);
        const RECT text_bounds{icon_bounds.right + 10, bounds.top, bounds.right - 16, bounds.bottom};
        paint_text(item.hDC, text, text_bounds, g_font_button,
            disabled ? kColorTextSubtle : (selected ? kColorText : kColorTextSecondary),
            DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        return true;
    }

    wchar_t text[256]{};
    GetWindowTextW(item.hwndItem, text, static_cast<int>(std::size(text)));
    if (id == kSimpleModeToggle || id == kFullModeToggle) {
        const bool active = (id == kSimpleModeToggle) == g_state.simple_mode;
        FillRect(item.hDC, &bounds, g_background_brush);
        paint_text(item.hDC, text, bounds, g_font_heading,
            active ? kColorBlue : (hovered ? kColorTextSecondary : kColorTextMuted),
            DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        return true;
    }
    if (id == kDisplayConfigToggle || id == kDiagnosticsButton) {
        FillRect(item.hDC, &bounds, g_control_brush);
        paint_text(item.hDC, text, bounds, g_font_heading,
            hovered ? kColorText : kColorTextMuted, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        return true;
    }
    if (id == kStartMirrorButton) {
        paint_gradient_button(item.hDC, bounds, disabled, hovered, pressed);
        paint_text(item.hDC, text, bounds, g_font_heading,
            disabled ? kColorTextSubtle : kColorText,
            DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        return true;
    }
    if (id == kCustomModeSaveButton) {
        if (g_popup_light_theme) {
            const COLORREF fill = disabled ? RGB(202, 211, 225)
                : pressed ? RGB(74, 91, 184)
                : hovered ? RGB(91, 109, 215) : RGB(76, 94, 198);
            fill_rounded_rect(item.hDC, bounds, fill, RGB(91, 109, 215), 9);
        } else {
            paint_gradient_button(item.hDC, bounds, disabled, hovered, pressed);
        }
        paint_text(item.hDC, text, bounds, g_font_popup_body,
            disabled ? kColorTextSubtle : kColorText,
            DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        return true;
    }
    const bool danger = id == kStopButton || id == kSimpleStopButton ||
        id == kRemoveVddButton || id == kRecoverVddButton;
    const COLORREF generic_fill = disabled ? RGB(30, 38, 50)
        : pressed ? RGB(18, 29, 44)
        : hovered ? (danger ? RGB(67, 38, 50) : RGB(30, 47, 70)) : kColorControl;
    const COLORREF generic_border = disabled ? kColorBorder
        : danger ? (hovered ? RGB(166, 75, 91) : RGB(90, 49, 59))
        : (hovered ? kColorBorderHover : kColorBorder);
    fill_rounded_rect(item.hDC, bounds,
        generic_fill, generic_border, 8);
    const bool has_secondary_icon = id == kDisplaySettingsButton ||
        id == kSimpleDisplaySettingsButton || id == kPreviewDeviceButton ||
        id == kCustomModesButton || id == kCreateVddButton || id == kRemoveVddButton ||
        id == kRecoverVddButton || id == kDiagnosticsButton;
    if (has_secondary_icon) {
        paint_secondary_action_icon(item.hDC, id, bounds,
            disabled ? kColorTextSubtle : (danger ? kColorRed : kColorTextSecondary));
        bounds.left += 30;
    }
    paint_text(item.hDC, text, bounds, g_font_body,
        disabled ? kColorTextSubtle : (danger ? kColorRed : kColorTextSecondary),
        DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    return true;
}

bool paint_owner_draw_button(const DRAWITEMSTRUCT& item) {
    if (item.CtlType != ODT_BUTTON || item.hDC == nullptr) {
        return paint_owner_draw_button_unbuffered(item);
    }
    const int width = std::max(1L, item.rcItem.right - item.rcItem.left);
    const int height = std::max(1L, item.rcItem.bottom - item.rcItem.top);
    HDC buffer_dc = CreateCompatibleDC(item.hDC);
    if (buffer_dc == nullptr) {
        return paint_owner_draw_button_unbuffered(item);
    }
    HBITMAP bitmap = CreateCompatibleBitmap(item.hDC, width, height);
    if (bitmap == nullptr) {
        DeleteDC(buffer_dc);
        return paint_owner_draw_button_unbuffered(item);
    }
    const HGDIOBJ previous_bitmap = SelectObject(buffer_dc, bitmap);
    DRAWITEMSTRUCT buffered_item = item;
    buffered_item.hDC = buffer_dc;
    buffered_item.rcItem = RECT{0, 0, width, height};
    // 圆角按钮的缓冲位图默认未初始化。自定义模式弹窗为浅色时，若不先铺上
    // 弹窗面板底色，圆角外四个像素区域会把黑色垃圾像素一起拷回窗口。
    if (item.CtlID == kCustomModeSaveButton) {
        HBRUSH background = CreateSolidBrush(popup_control_color());
        FillRect(buffer_dc, &buffered_item.rcItem, background);
        DeleteObject(background);
    }
    const bool painted = paint_owner_draw_button_unbuffered(buffered_item);
    BitBlt(item.hDC, item.rcItem.left, item.rcItem.top, width, height, buffer_dc, 0, 0, SRCCOPY);
    SelectObject(buffer_dc, previous_bitmap);
    DeleteObject(bitmap);
    DeleteDC(buffer_dc);
    return painted;
}

void paint_window_buffered(HWND window, HDC target, void (*painter)(HWND, HDC)) {
    RECT client{};
    GetClientRect(window, &client);
    const int width = std::max(1L, client.right - client.left);
    const int height = std::max(1L, client.bottom - client.top);
    HDC buffer_dc = CreateCompatibleDC(target);
    if (buffer_dc == nullptr) {
        painter(window, target);
        return;
    }
    HBITMAP bitmap = CreateCompatibleBitmap(target, width, height);
    if (bitmap == nullptr) {
        DeleteDC(buffer_dc);
        painter(window, target);
        return;
    }
    const HGDIOBJ previous_bitmap = SelectObject(buffer_dc, bitmap);
    painter(window, buffer_dc);
    BitBlt(target, 0, 0, width, height, buffer_dc, 0, 0, SRCCOPY);
    SelectObject(buffer_dc, previous_bitmap);
    DeleteObject(bitmap);
    DeleteDC(buffer_dc);
}

void layout_simple_controls(HWND window) {
    const SimpleLayout layout = simple_layout(window);
    move_control(g_state.simple_mirror_button, layout.mirror);
    move_control(g_state.simple_extend_button, layout.extend);
    move_control(g_state.simple_display_only_button, layout.display_only);
    move_control(g_state.simple_input_button, layout.input);
    move_control(g_state.simple_stop_button, layout.stop);
    move_control(g_state.simple_display_settings_button, layout.display_settings);
    move_control(g_state.simple_status, RECT{62, layout.footer.top + 5,
        layout.footer.right - 48, layout.footer.bottom - 5});
}

void layout_full_controls(HWND window) {
    const FullLayout layout = full_layout(window);
    const int config_left = 748;
    const int config_column = 157;
    move_control(g_state.display_config_toggle, RECT{layout.config_card.right - 72, 156,
        layout.config_card.right - 22, 236});
    move_control(g_state.display_selector, RECT{config_left, 296, config_left + 146, 336});
    move_control(g_state.resolution_selector, RECT{config_left + config_column, 296,
        config_left + config_column + 146, 336});
    move_control(g_state.vdd_mode_selector, RECT{config_left + config_column * 2, 296,
        config_left + config_column * 2 + 146, 336});
    move_control(g_state.frame_rate_selector, RECT{config_left + config_column * 3, 296,
        config_left + config_column * 3 + 146, 336});

    move_control(g_state.create_vdd_button, RECT{42, 524, 246, 566});
    move_control(g_state.remove_vdd_button, RECT{258, 524, 462, 566});
    move_control(g_state.recover_vdd_button, RECT{474, 524, 690, 566});

    const int input_top = layout.input_card.top + (g_state.display_config_expanded ? 64 : 78);
    const int input_height = g_state.display_config_expanded ? 80 : 106;
    const int input_mid = (layout.input_card.left + layout.input_card.right) / 2;
    move_control(g_state.full_display_only_button, RECT{748, input_top, input_mid, input_top + input_height});
    move_control(g_state.start_input_button, RECT{input_mid, input_top, layout.input_card.right - 22,
        input_top + input_height});
    move_control(g_state.start_mirror_button, RECT{926, layout.action_card.top + 50,
        layout.action_card.right - 22, layout.action_card.top + 120});
    move_control(g_state.stop_button, RECT{926, layout.action_card.bottom - 66,
        layout.action_card.right - 22, layout.action_card.bottom - 18});
    move_control(GetDlgItem(window, kPreviewDeviceButton), RECT{42, layout.tools_card.top + 56,
        254, layout.tools_card.top + 116});
    move_control(GetDlgItem(window, kCustomModesButton), RECT{272, layout.tools_card.top + 56,
        484, layout.tools_card.top + 116});
    move_control(g_state.display_settings_button, RECT{502, layout.tools_card.top + 56,
        694, layout.tools_card.top + 116});
    move_control(g_state.diagnostics_button, RECT{layout.log_card.right - 76,
        layout.log_card.top + 44, layout.log_card.right - 24, layout.log_card.top + 122});
    move_control(g_state.check_update_button, RECT{layout.client.right - 134,
        layout.client.bottom - 56, layout.client.right - 34, layout.client.bottom - 24});
}

void show_control(HWND control, bool visible) {
    if (control != nullptr) {
        ShowWindow(control, visible ? SW_SHOWNA : SW_HIDE);
    }
}

void apply_control_center_view(HWND window) {
    const bool diagnostics = g_state.diagnostics_page;
    const bool simple = g_state.simple_mode && !diagnostics;
    const bool full = !g_state.simple_mode && !diagnostics;

    for (HWND control : {g_state.simple_mode_toggle, g_state.full_mode_toggle}) {
        show_control(control, !diagnostics);
    }
    for (HWND control : {g_state.simple_mirror_button, g_state.simple_extend_button,
             g_state.simple_display_only_button, g_state.simple_input_button,
             g_state.simple_stop_button, g_state.simple_display_settings_button}) {
        show_control(control, simple);
    }
    show_control(g_state.simple_status, false);
    for (HWND control : {g_state.display_config_toggle, g_state.create_vdd_button,
             g_state.remove_vdd_button, g_state.recover_vdd_button, g_state.start_mirror_button,
             g_state.start_input_button, g_state.full_display_only_button, g_state.stop_button,
             g_state.display_settings_button, g_state.diagnostics_button,
             g_state.check_update_button, GetDlgItem(window, kPreviewDeviceButton),
             GetDlgItem(window, kCustomModesButton)}) {
        show_control(control, full);
    }
    const bool show_config_fields = full && g_state.display_config_expanded;
    for (HWND selector : {g_state.display_selector, g_state.resolution_selector,
             g_state.vdd_mode_selector, g_state.frame_rate_selector}) {
        show_control(selector, show_config_fields);
    }
    show_control(g_state.diagnostics_back_button, diagnostics);
    show_control(g_state.diagnostics_output, diagnostics);
    show_control(g_state.status, false);
    show_control(g_state.vdd_status, false);
    show_control(g_state.refresh_displays_button, false);
    show_control(GetDlgItem(window, kIdentifyDeviceButton), false);

    if (simple) {
        layout_simple_controls(window);
    } else if (full) {
        layout_full_controls(window);
    }
    InvalidateRect(window, nullptr, FALSE);
}

void begin_simple_extension(bool enable_input) {
    if (!g_state.owned_display_device.empty()) {
        // The simple "扩展屏幕" card must never inherit the target selected
        // in full mode: it always streams the VDD owned by this control
        // center. Refresh first so a detached/stale DISPLAYn cannot reach
        // wireview_host and produce a misleading capture error.
        const std::wstring owned_display = g_state.owned_display_device;
        refresh_display_choices(owned_display);
        const auto owned = std::find_if(
            g_state.display_choices.begin(), g_state.display_choices.end(),
            [&owned_display](const DisplayChoice& choice) {
                return !choice.primary && choice.device_name == owned_display;
            });
        if (owned != g_state.display_choices.end() &&
            select_display_device(owned_display)) {
            (void)start_session(g_state.window, enable_input, false, false);
            return;
        }

        // The broker gives us exact ownership of this VDD. If Windows no
        // longer exposes it as an attached output, removing and recreating
        // only that instance is safe; do not touch any other Parsec display.
        log_control_event(L"simple-extend-recover", L"WireView 持有的副屏不再附加：" +
            owned_display + L"；将安全重建该副屏。");
        g_state.recreate_vdd_for_simple_extension = true;
        g_state.recreate_vdd_for_simple_extension_input = enable_input;
        request_vdd_removal();
        if (!g_state.vdd_stop_requested) {
            g_state.recreate_vdd_for_simple_extension = false;
            g_state.recreate_vdd_for_simple_extension_input = false;
        }
        return;
    }
    g_state.start_after_vdd_ready = true;
    g_state.start_after_vdd_input = enable_input;
    if (!start_vdd_broker(g_state.window, false)) {
        g_state.start_after_vdd_ready = false;
        g_state.start_after_vdd_input = false;
    }
}

void begin_simple_copy(bool enable_input) {
    if (!g_state.owned_display_device.empty()) {
        g_state.start_after_vdd_removal = true;
        g_state.start_after_vdd_removal_input = enable_input;
        request_vdd_removal();
        if (!g_state.vdd_stop_requested) {
            g_state.start_after_vdd_removal = false;
            g_state.start_after_vdd_removal_input = false;
        }
        return;
    }
    refresh_display_choices();
    if (!select_primary_display()) {
        set_status(L"未找到电脑主显示器，无法开始复制。 ");
        return;
    }
    (void)start_session(g_state.window, enable_input, false, false);
}

void return_to_full_mode() {
    g_state.simple_mode = false;
    g_state.diagnostics_page = false;
    if (g_state.window != nullptr) {
        apply_control_center_view(g_state.window);
    }
}

void open_windows_display_settings() {
    const auto result = reinterpret_cast<INT_PTR>(ShellExecuteW(
        nullptr, L"open", L"ms-settings:display", nullptr, nullptr, SW_SHOWNORMAL));
    if (result <= 32) {
        set_status(L"无法打开 Windows“系统 → 屏幕”设置。 ");
    }
}

#ifdef WIREVIEW_WEBVIEW_UI
std::wstring json_escape(std::wstring_view text) {
    std::wstring escaped;
    escaped.reserve(text.size() + 16U);
    for (const wchar_t character : text) {
        switch (character) {
            case L'\\': escaped += L"\\\\"; break;
            case L'\"': escaped += L"\\\""; break;
            case L'\n': escaped += L"\\n"; break;
            case L'\r': escaped += L"\\r"; break;
            case L'\t': escaped += L"\\t"; break;
            default:
                if (character >= 0 && character < 0x20) {
                    escaped += L' ';
                } else {
                    escaped += character;
                }
        }
    }
    return escaped;
}

std::wstring combo_options_as_json(HWND selector) {
    std::wstring json = L"[";
    const LRESULT count = selector == nullptr
        ? 0
        : SendMessageW(selector, CB_GETCOUNT, 0U, 0);
    for (LRESULT index = 0; index < count; ++index) {
        const LRESULT length = SendMessageW(selector, CB_GETLBTEXTLEN,
            static_cast<WPARAM>(index), 0);
        if (length < 0) {
            continue;
        }
        std::wstring text(static_cast<std::size_t>(length) + 1U, L'\0');
        SendMessageW(selector, CB_GETLBTEXT, static_cast<WPARAM>(index),
            reinterpret_cast<LPARAM>(text.data()));
        text.resize(static_cast<std::size_t>(length));
        if (json.size() > 1U) {
            json += L',';
        }
        json += L"\"" + json_escape(text) + L"\"";
    }
    json += L"]";
    return json;
}

int combo_selected_index(HWND selector) {
    if (selector == nullptr) {
        return 0;
    }
    const LRESULT selected = SendMessageW(selector, CB_GETCURSEL, 0U, 0);
    return selected < 0 ? 0 : static_cast<int>(selected);
}

std::wstring web_diagnostics_text() {
    const std::wstring status = g_state.current_status.empty()
        ? L"准备就绪。"
        : g_state.current_status;
    std::wstring vdd;
    if (g_state.vdd_status != nullptr) {
        const int length = GetWindowTextLengthW(g_state.vdd_status);
        vdd.resize(static_cast<std::size_t>(std::max(0, length)) + 1U, L'\0');
        const int copied = GetWindowTextW(g_state.vdd_status, vdd.data(), length + 1);
        vdd.resize(static_cast<std::size_t>(std::max(0, copied)));
    }
    return L"WireView 控制中心输出\n\n" + status + L"\n\nParsec VDD 诊断\n\n" +
        (vdd.empty() ? L"正在检查 Parsec VDD。" : vdd) +
        L"\n\n完整日志文件（请直接发送文件）\n\n" +
        L"控制中心日志：" +
        (g_state.runtime_log_file.empty()
            ? L"路径尚未初始化。"
            : g_state.runtime_log_file.wstring()) +
        L"\nWindows Host 日志：" +
        (g_state.last_host_diagnostics_file.empty()
            ? L"尚未生成；请先启动一次 WireView。"
            : g_state.last_host_diagnostics_file.wstring());
}

void publish_web_state() {
    if (!g_webview_host || !g_webview_host->ready()) {
        return;
    }
    const DisplayChoice* selected_display = selected_display_choice();
    const std::wstring frame_rate = combo_selected_index(g_state.frame_rate_selector) == 1
        ? L"120 FPS（实验）"
        : L"60 FPS";
    const bool running = g_state.shell_process != nullptr && !g_state.preview_active &&
        !g_state.profile_probe_active;
    std::wstring json = L"{";
    const auto add_string = [&json](std::wstring_view key, std::wstring_view value) {
        if (json.size() > 1U) {
            json += L',';
        }
        json += L"\"" + std::wstring(key) + L"\":\"" + json_escape(value) + L"\"";
    };
    const auto add_bool = [&json](std::wstring_view key, bool value) {
        if (json.size() > 1U) {
            json += L',';
        }
        json += L"\"" + std::wstring(key) + L"\":" + (value ? L"true" : L"false");
    };
    const auto add_number = [&json](std::wstring_view key, std::size_t value) {
        if (json.size() > 1U) {
            json += L',';
        }
        json += L"\"" + std::wstring(key) + L"\":" + std::to_wstring(value);
    };
    const auto add_array = [&json](std::wstring_view key, const std::wstring& value) {
        if (json.size() > 1U) {
            json += L',';
        }
        json += L"\"" + std::wstring(key) + L"\":" + value;
    };
    add_bool(L"connected", g_state.device_connected);
    add_bool(L"running", running);
    add_bool(L"canStartSession", !g_state.device_profile_requires_custom_mode);
    add_bool(L"inputEnabled", g_state.session_input_enabled);
    add_string(L"mode", g_state.web_extend_mode ? L"extend" : L"copy");
    add_string(L"deviceName", g_state.device_name);
    add_string(L"deviceCategory", g_state.device_is_tablet ? L"tablet" : L"phone");
    add_string(L"deviceRefresh", g_state.device_active_refresh);
    add_string(L"status", g_state.current_status.empty() ? L"准备就绪。" : g_state.current_status);
    add_string(L"resolution", selector_text(g_state.resolution_selector));
    add_string(L"vddMode", selector_text(g_state.vdd_mode_selector));
    add_string(L"frameRate", frame_rate);
    add_string(L"displayName", selected_display == nullptr ? L"--" : selected_display->device_name);
    add_bool(L"parsecAvailable", g_state.parsec_vdd_available);
    add_number(L"parsecVersion", g_state.parsec_vdd_version);
    add_number(L"parsecDisplayCount", g_state.parsec_display_count);
    add_array(L"displays", combo_options_as_json(g_state.display_selector));
    add_array(L"resolutions", combo_options_as_json(g_state.resolution_selector));
    add_array(L"vddModes", combo_options_as_json(g_state.vdd_mode_selector));
    add_number(L"selectedDisplay", static_cast<std::size_t>(combo_selected_index(g_state.display_selector)));
    add_number(L"selectedResolution", static_cast<std::size_t>(combo_selected_index(g_state.resolution_selector)));
    add_number(L"selectedVddMode", static_cast<std::size_t>(combo_selected_index(g_state.vdd_mode_selector)));
    add_string(L"diagnostics", web_diagnostics_text());
    json += L"}";
    g_webview_host->post_json(json);
}

bool parse_web_selector_index(std::wstring_view message, std::wstring_view prefix,
    HWND selector, int& index) {
    if (!message.starts_with(prefix) || selector == nullptr) {
        return false;
    }
    try {
        const int parsed = std::stoi(std::wstring(message.substr(prefix.size())));
        const LRESULT count = SendMessageW(selector, CB_GETCOUNT, 0U, 0);
        if (parsed < 0 || parsed >= count) {
            return false;
        }
        index = parsed;
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

void handle_web_message(std::wstring_view message) {
    log_control_event(L"web-click", std::wstring(message));
    if (message == L"page-ready") {
        publish_web_state();
        return;
    }
    if (message == L"theme-light" || message == L"theme-dark") {
        g_popup_light_theme = message == L"theme-light";
        refresh_popup_theme(g_custom_mode_editor);
        refresh_popup_theme(g_diagnostics_window);
        return;
    }
    if (message == L"mode-copy") {
        g_state.web_extend_mode = false;
        begin_simple_copy(g_state.session_input_enabled);
    } else if (message == L"mode-extend") {
        g_state.web_extend_mode = true;
        begin_simple_extension(g_state.session_input_enabled);
    } else if (message == L"input-off") {
        g_state.session_input_enabled = false;
    } else if (message == L"input-on") {
        g_state.session_input_enabled = true;
    } else if (message == L"start") {
        (void)start_session(g_state.window, g_state.session_input_enabled, false, false);
    } else if (message == L"stop") {
        notify_android_of_user_stop();
        stop_session();
        set_status(L"WireView 已停止。");
    } else if (message == L"display-settings") {
        open_windows_display_settings();
    } else if (message == L"refresh-displays") {
        refresh_display_choices();
        scan_vdd_recovery_records();
        if (g_state.shell_process == nullptr) {
            set_status(L"正在刷新显示器、Parsec VDD，并重新读取 Android 设备信息…");
            g_state.device_profile_refresh_requested = true;
            // 先确认 ADB 连接；确认后会启动一次不显示画面、不创建副屏的
            // 设备信息探测，由 Android 应用重新上报当前分辨率与刷新率。
            poll_device_detection(g_state.window);
        } else {
            set_status(g_state.display_choices.empty()
                ? L"未发现已附加到桌面的显示器；WireView 运行中，设备连接状态会持续监测。"
                : L"显示器与 Parsec VDD 副屏状态已刷新；WireView 运行中，设备连接状态会持续监测。"
            );
        }
    } else if (message == L"preview") {
        (void)start_session(g_state.window, false, true, false);
    } else if (message == L"custom-modes") {
        sync_custom_modes_from_parsec_vdd();
        show_custom_mode_editor(g_state.window);
    } else if (message == L"diagnostics") {
        show_diagnostics_window(g_state.window);
    } else if (message == L"create-vdd") {
        (void)start_vdd_broker(g_state.window);
    } else if (message == L"remove-vdd") {
        request_vdd_removal();
    } else if (message == L"recover-vdd") {
        recover_stale_vdd_session(g_state.window);
    } else {
        int index{};
        if (parse_web_selector_index(message, L"select-display|", g_state.display_selector, index)) {
            SendMessageW(g_state.display_selector, CB_SETCURSEL, static_cast<WPARAM>(index), 0);
            update_vdd_controls();
            update_recommendation();
        } else if (parse_web_selector_index(message, L"select-resolution|", g_state.resolution_selector, index)) {
            SendMessageW(g_state.resolution_selector, CB_SETCURSEL, static_cast<WPARAM>(index), 0);
            populate_vdd_mode_selector();
            (void)apply_selected_mode_to_parsec_vdd();
            update_recommendation();
        } else if (parse_web_selector_index(message, L"select-vdd|", g_state.vdd_mode_selector, index)) {
            SendMessageW(g_state.vdd_mode_selector, CB_SETCURSEL, static_cast<WPARAM>(index), 0);
            (void)apply_selected_mode_to_parsec_vdd();
            update_recommendation();
        } else if (message == L"select-fps|60" || message == L"select-fps|120") {
            SendMessageW(g_state.frame_rate_selector, CB_SETCURSEL,
                message.ends_with(L"120") ? 1U : 0U, 0);
            update_recommendation();
        } else {
            return;
        }
    }
    publish_web_state();
}

void hide_native_controls_for_web() {
    // 保留这些 HWND 作为已经验证过的后端选择器与状态容器；WebView2 版本中
    // 它们绝不能成为可见界面的一部分，所有展示和交互改由页面完成。
    for (HWND control : {g_state.simple_mode_toggle, g_state.full_mode_toggle,
             g_state.simple_mirror_button, g_state.simple_extend_button,
             g_state.simple_display_only_button, g_state.simple_input_button,
             g_state.simple_stop_button, g_state.simple_display_settings_button,
             g_state.display_selector, g_state.refresh_displays_button,
             g_state.resolution_selector, g_state.vdd_mode_selector,
             g_state.frame_rate_selector, g_state.start_mirror_button,
             g_state.start_input_button, g_state.full_display_only_button,
             g_state.stop_button, g_state.display_settings_button,
             g_state.create_vdd_button, g_state.remove_vdd_button,
             g_state.recover_vdd_button, g_state.display_config_toggle,
             g_state.diagnostics_button, g_state.diagnostics_back_button,
             g_state.check_update_button, g_state.status, g_state.vdd_status,
             g_state.diagnostics_output, GetDlgItem(g_state.window, kIdentifyDeviceButton),
             GetDlgItem(g_state.window, kPreviewDeviceButton),
             GetDlgItem(g_state.window, kCustomModesButton)}) {
        if (control != nullptr) {
            ShowWindow(control, SW_HIDE);
        }
    }
}
#endif

LRESULT CALLBACK simple_window_procedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
        case WM_CREATE: {
            g_state.simple_window = window;
            enable_dark_title_bar(window);
            g_state.simple_mirror_button = CreateWindowW(
                L"BUTTON", L"复制屏幕", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                24, 278, 548, 232, window,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(kSimpleMirrorButton)),
                nullptr, nullptr);
            g_state.simple_extend_button = CreateWindowW(
                L"BUTTON", L"扩展屏幕", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                588, 278, 548, 232, window,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(kSimpleExtendButton)),
                nullptr, nullptr);
            g_state.simple_display_only_button = CreateWindowW(
                L"BUTTON", L"仅显示", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                24, 566, 556, 64, window,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(kInputDisplayOnlyButton)),
                nullptr, nullptr);
            g_state.simple_input_button = CreateWindowW(
                L"BUTTON", L"触控与手写笔", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                580, 566, 556, 64, window,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(kInputEnabledButton)),
                nullptr, nullptr);
            g_state.simple_stop_button = CreateWindowW(
                L"BUTTON", L"■  停止 WireView", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                24, 650, 230, 44, window,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(kStopButton)),
                nullptr, nullptr);
            g_state.simple_display_settings_button = CreateWindowW(
                L"BUTTON", L"Windows 显示设置", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                270, 650, 250, 44, window,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(kDisplaySettingsButton)),
                nullptr, nullptr);
            CreateWindowW(
                L"BUTTON", L"切换到完整模式  →", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                886, 650, 250, 44, window,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(kSimpleReturnButton)),
                nullptr, nullptr);
            RECT simple_client{};
            GetClientRect(window, &simple_client);
            g_state.simple_status = CreateWindowW(
                L"STATIC", L"准备就绪", WS_CHILD | WS_VISIBLE | SS_LEFT | SS_CENTERIMAGE,
                62, simple_client.bottom - 54, simple_client.right - 110, 30,
                window, nullptr, nullptr, nullptr);
            for (HWND control : {g_state.simple_mirror_button, g_state.simple_extend_button,
                     g_state.simple_display_only_button, g_state.simple_input_button,
                     g_state.simple_stop_button, g_state.simple_display_settings_button,
                     g_state.simple_status}) {
                apply_control_theme(control, control == g_state.simple_status ? g_font_small : g_font_button);
            }
            for (HWND button : {g_state.simple_mirror_button, g_state.simple_extend_button,
                     g_state.simple_display_only_button, g_state.simple_input_button,
                     g_state.simple_stop_button, g_state.simple_display_settings_button,
                     GetDlgItem(window, kSimpleReturnButton)}) {
                enable_owner_draw_button_feedback(button);
            }
            layout_simple_controls(window);
            set_status(g_state.current_status.empty() ? L"准备就绪。" : g_state.current_status);
            update_session_action_controls();
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT paint{};
            HDC dc = BeginPaint(window, &paint);
            paint_window_buffered(window, dc, paint_simple_window);
            EndPaint(window, &paint);
            return 0;
        }
        case WM_DRAWITEM:
            if (paint_owner_draw_button(
                    *reinterpret_cast<const DRAWITEMSTRUCT*>(lparam))) {
                return TRUE;
            }
            break;
        case WM_CTLCOLORSTATIC: {
            HDC dc = reinterpret_cast<HDC>(wparam);
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, kColorTextMuted);
            return reinterpret_cast<LRESULT>(g_background_brush);
        }
        case WM_COMMAND:
            log_control_event(L"simple-command", L"id=" +
                std::to_wstring(LOWORD(wparam)) + L" notify=" + std::to_wstring(HIWORD(wparam)));
            switch (LOWORD(wparam)) {
                case kSimpleReturnButton:
                    return_to_full_mode();
                    return 0;
                case kSimpleMirrorButton:
                    begin_simple_copy(g_state.session_input_enabled);
                    return 0;
                case kSimpleExtendButton:
                    begin_simple_extension(g_state.session_input_enabled);
                    return 0;
                case kInputDisplayOnlyButton:
                    g_state.session_input_enabled = false;
                    InvalidateRect(window, nullptr, FALSE);
                    InvalidateRect(g_state.window, nullptr, FALSE);
                    return 0;
                case kInputEnabledButton:
                    g_state.session_input_enabled = true;
                    InvalidateRect(window, nullptr, FALSE);
                    InvalidateRect(g_state.window, nullptr, FALSE);
                    return 0;
                case kStopButton:
                    notify_android_of_user_stop();
                    stop_session();
                    set_status(L"WireView 已停止。");
                    return 0;
                case kDisplaySettingsButton:
                    open_windows_display_settings();
                    return 0;
            }
            break;
        case WM_CLOSE:
            // 简易窗口关闭时仅隐藏到托盘，保持简易模式以便托盘恢复。
            ShowWindow(window, SW_HIDE);
            return 0;
        case WM_GETMINMAXINFO: {
            auto* info = reinterpret_cast<MINMAXINFO*>(lparam);
            info->ptMinTrackSize.x = 980;
            info->ptMinTrackSize.y = 700;
            return 0;
        }
        case WM_SIZE:
            if (wparam != SIZE_MINIMIZED) {
                layout_simple_controls(window);
                InvalidateRect(window, nullptr, FALSE);
            }
            return 0;
        case WM_DPICHANGED: {
            const RECT* suggested = reinterpret_cast<const RECT*>(lparam);
            SetWindowPos(window, nullptr, suggested->left, suggested->top,
                suggested->right - suggested->left, suggested->bottom - suggested->top,
                SWP_NOZORDER | SWP_NOACTIVATE);
            return 0;
        }
        case WM_DESTROY:
            g_state.simple_window = nullptr;
            g_state.simple_status = nullptr;
            g_state.simple_mirror_button = nullptr;
            g_state.simple_input_button = nullptr;
            g_state.simple_extend_button = nullptr;
            g_state.simple_extend_input_button = nullptr;
            g_state.simple_display_only_button = nullptr;
            g_state.simple_stop_button = nullptr;
            g_state.simple_display_settings_button = nullptr;
            return 0;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

void open_simple_mode(HWND full_window) {
    g_state.simple_mode = true;
    g_state.diagnostics_page = false;
    apply_control_center_view(full_window);
}

LRESULT CALLBACK custom_mode_editor_procedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
        case WM_CREATE: {
            const auto add_text = [window](const wchar_t* text, int x, int y, int width, int height,
                                      HFONT font) {
                HWND control = CreateWindowW(L"STATIC", text, WS_CHILD | WS_VISIBLE,
                    x, y, width, height, window, nullptr, nullptr, nullptr);
                apply_popup_control_theme(control, font);
                return control;
            };
            add_text(L"自定义 Parsec VDD 模式", 26, 22, 560, 32, g_font_popup_heading);
            add_text(L"每个槽分别输入宽度、高度和刷新率；留空可删除该槽。\n"
                     L"保存后可选择投屏分辨率和刷新率；创建副屏时才会写入 Parsec VDD。",
                26, 62, 560, 52, g_font_popup_small);
            for (std::size_t index = 0U; index < g_custom_mode_edits.size(); ++index) {
                const int y = kCustomModeFieldTop + static_cast<int>(index) * kCustomModeFieldStep;
                const std::wstring label = L"自定义槽 " + std::to_wstring(index + 1U) + L"：";
                add_text(label.c_str(), 26, y + 3, 122, 30, g_font_popup_body);
                const std::array<std::wstring, 3U> values{
                    g_custom_modes[index] ? std::to_wstring(g_custom_modes[index]->width) : L"",
                    g_custom_modes[index] ? std::to_wstring(g_custom_modes[index]->height) : L"",
                    g_custom_modes[index] ? std::to_wstring(g_custom_modes[index]->refresh_hz) : L""};
                const std::array<int, 3U> left{154, 300, 448};
                const std::array<int, 3U> width{112, 112, 94};
                for (std::size_t field = 0U; field < 3U; ++field) {
                    g_custom_mode_edits[index][field] = CreateWindowExW(
                        0, L"EDIT", values[field].c_str(),
                        WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL | ES_NUMBER,
                        left[field], y, width[field], 32, window,
                        reinterpret_cast<HMENU>(static_cast<INT_PTR>(kCustomModeEditBase + index * 3U + field)),
                        nullptr, nullptr);
                    apply_popup_control_theme(g_custom_mode_edits[index][field], g_font_popup_body);
                }
                add_text(L"×", 272, y + 2, 20, 30, g_font_popup_body);
                add_text(L"@", 418, y + 2, 20, 30, g_font_popup_body);
                add_text(L"Hz", 550, y + 2, 34, 30, g_font_popup_body);
            }
            return 0;
        }
        case WM_ERASEBKGND:
            return TRUE;
        case WM_PAINT: {
            PAINTSTRUCT paint{};
            HDC dc = BeginPaint(window, &paint);
            RECT client{};
            GetClientRect(window, &client);
            FillRect(dc, &client, popup_background_brush());
            fill_rounded_rect(dc, RECT{12, 12, client.right - 12, client.bottom - 12},
                popup_control_color(), popup_border_color(), 13);
            // 三个独立数字框分别对应宽度、高度和刷新率，避免用户手动输入分隔符。
            for (std::size_t index = 0U; index < g_custom_mode_edits.size(); ++index) {
                const int y = kCustomModeFieldTop + static_cast<int>(index) * kCustomModeFieldStep;
                for (const RECT bounds : {RECT{150, y - 4, 270, y + 36},
                         RECT{296, y - 4, 416, y + 36}, RECT{444, y - 4, 546, y + 36}}) {
                    fill_rounded_rect(dc, bounds, popup_panel_elevated_color(), popup_border_hover_color(), 7);
                }
            }
            const RECT save_bounds{414, 392, 584, 434};
            const COLORREF save_fill = g_popup_light_theme ? RGB(76, 94, 198) : kColorBlue;
            const COLORREF save_border = g_popup_light_theme ? RGB(91, 109, 215) : kColorBorderHover;
            fill_rounded_rect(dc, save_bounds, save_fill, save_border, 9);
            paint_text(dc, L"保存 5 个槽", save_bounds, g_font_popup_body, kColorText,
                DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            EndPaint(window, &paint);
            return 0;
        }
        case WM_DRAWITEM:
            if (paint_owner_draw_button(*reinterpret_cast<const DRAWITEMSTRUCT*>(lparam))) {
                return TRUE;
            }
            break;
        case WM_CTLCOLORSTATIC: {
            HDC dc = reinterpret_cast<HDC>(wparam);
            SetBkColor(dc, popup_control_color());
            SetTextColor(dc, popup_text_secondary_color());
            return reinterpret_cast<LRESULT>(popup_control_brush());
        }
        case WM_CTLCOLOREDIT: {
            HDC dc = reinterpret_cast<HDC>(wparam);
            SetBkColor(dc, popup_control_color());
            SetTextColor(dc, popup_text_color());
            return reinterpret_cast<LRESULT>(popup_control_brush());
        }
        case WM_LBUTTONUP: {
            const POINT point{static_cast<short>(LOWORD(lparam)), static_cast<short>(HIWORD(lparam))};
            const RECT save_bounds{414, 392, 584, 434};
            if (PtInRect(&save_bounds, point) != FALSE) {
                SendMessageW(window, WM_COMMAND, MAKEWPARAM(kCustomModeSaveButton, BN_CLICKED), 0);
                return 0;
            }
            break;
        }
        case WM_COMMAND:
            if (LOWORD(wparam) == kCustomModeSaveButton) {
                log_control_event(L"custom-modes", L"用户保存自定义 Parsec VDD 模式槽。 ");
                std::array<std::optional<CustomMode>, 5U> updated;
                for (std::size_t index = 0U; index < updated.size(); ++index) {
                    const std::array<std::wstring, 3U> values{
                        selector_text(g_custom_mode_edits[index][0]),
                        selector_text(g_custom_mode_edits[index][1]),
                        selector_text(g_custom_mode_edits[index][2])};
                    if (values[0].empty() && values[1].empty() && values[2].empty()) {
                        continue;
                    }
                    const auto parse_number = [](const std::wstring& value, unsigned long maximum)
                        -> std::optional<unsigned long> {
                        if (value.empty() || !std::all_of(value.begin(), value.end(),
                                [](wchar_t character) { return iswdigit(character) != 0; })) {
                            return std::nullopt;
                        }
                        try {
                            const unsigned long parsed = std::stoul(value);
                            return parsed != 0UL && parsed <= maximum ? std::optional{parsed} : std::nullopt;
                        } catch (const std::exception&) {
                            return std::nullopt;
                        }
                    };
                    const auto width = parse_number(values[0], 65535UL);
                    const auto height = parse_number(values[1], 65535UL);
                    const auto refresh = parse_number(values[2], 1000UL);
                    if (!width || !height || !refresh || (*width % 2UL) != 0UL || (*height % 2UL) != 0UL) {
                        MessageBoxW(window,
                            L"每个非空槽都要完整填写宽度、高度和刷新率；宽高必须为正偶数。",
                            L"WireView 自定义模式", MB_OK | MB_ICONWARNING);
                        return 0;
                    }
                    updated[index] = CustomMode{static_cast<std::uint16_t>(*width),
                        static_cast<std::uint16_t>(*height), static_cast<std::uint16_t>(*refresh)};
                }
                g_custom_modes = updated;
                save_custom_modes();
                const bool applied_to_parsec = apply_custom_modes_with_elevation(window);
                populate_resolution_selector();
                populate_vdd_mode_selector();
                if (g_state.device_profile_requires_custom_mode &&
                    g_state.detected_device_width != 0U && g_state.detected_device_height != 0U) {
                    apply_detected_device_profile(
                        std::to_wstring(g_state.detected_device_width),
                        std::to_wstring(g_state.detected_device_height),
                        g_state.detected_device_max_refresh);
                }
                update_recommendation();
                if (!applied_to_parsec) {
                    set_status(L"自定义槽已保存到控制中心，但未能写入 Parsec VDD；请允许管理员授权后重试。");
                } else if (g_state.device_profile_requires_custom_mode) {
                    set_status(L"当前设备分辨率仍无可用自定义槽，请替换或释放一个槽后再保存。");
                } else if (!g_state.device_profile_note.empty()) {
                    set_status(g_state.device_profile_note + L" 已同步到 Parsec VDD。 ");
                } else {
                    set_status(L"已保存 5 个自定义槽，并同步到 Parsec VDD。 ");
                }
                DestroyWindow(window);
                return 0;
            }
            break;
        case WM_CLOSE:
            DestroyWindow(window);
            return 0;
        case WM_DESTROY:
            g_custom_mode_editor = nullptr;
            for (auto& row : g_custom_mode_edits) {
                row.fill(nullptr);
            }
            return 0;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

void show_custom_mode_editor(HWND owner) {
    if (g_custom_mode_editor != nullptr) {
        ShowWindow(g_custom_mode_editor, SW_RESTORE);
        SetForegroundWindow(g_custom_mode_editor);
        return;
    }
    g_custom_mode_editor = CreateWindowExW(
        WS_EX_DLGMODALFRAME,
        L"WireViewCustomModeEditor",
        L"WireView · 自定义 Parsec VDD 模式",
        WS_CAPTION | WS_SYSMENU | WS_POPUP | WS_VISIBLE,
        CW_USEDEFAULT, CW_USEDEFAULT, 640, 510, owner, nullptr,
        GetModuleHandleW(nullptr), nullptr);
    if (g_custom_mode_editor == nullptr) {
        set_status(L"无法打开自定义模式窗口（系统错误 " +
            std::to_wstring(GetLastError()) + L"）。 ");
    } else {
        apply_popup_title_bar_theme(g_custom_mode_editor);
    }
}

LRESULT CALLBACK diagnostics_window_procedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
        case WM_CREATE: {
            apply_popup_title_bar_theme(window);
            const HICON application_icon = LoadIconW(
                GetModuleHandleW(nullptr), MAKEINTRESOURCEW(kWireViewAppIcon));
            if (application_icon != nullptr) {
                SendMessageW(window, WM_SETICON, ICON_SMALL,
                    reinterpret_cast<LPARAM>(application_icon));
                SendMessageW(window, WM_SETICON, ICON_BIG,
                    reinterpret_cast<LPARAM>(application_icon));
            }
            HWND title = CreateWindowW(L"STATIC", L"诊断与日志", WS_CHILD | WS_VISIBLE,
                24, 20, 420, 34, window, nullptr, nullptr, nullptr);
            HWND subtitle = CreateWindowW(L"STATIC",
                L"连接状态、会话输出和 Parsec VDD 诊断会自动更新。",
                WS_CHILD | WS_VISIBLE, 24, 58, 720, 28, window, nullptr, nullptr, nullptr);
            apply_popup_control_theme(title, g_font_popup_heading);
            apply_popup_control_theme(subtitle, g_font_popup_small);
            g_diagnostics_text = CreateWindowExW(
                0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_BORDER |
                    ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL,
                24, 96, 752, 458, window, nullptr, nullptr, nullptr);
            apply_popup_control_theme(g_diagnostics_text, g_font_popup_body);
            refresh_diagnostics_output();
            return 0;
        }
        case WM_GETMINMAXINFO: {
            auto* size_info = reinterpret_cast<MINMAXINFO*>(lparam);
            size_info->ptMinTrackSize.x = 680;
            size_info->ptMinTrackSize.y = 460;
            return 0;
        }
        case WM_SIZE: {
            if (g_diagnostics_text != nullptr) {
                const int width = LOWORD(lparam);
                const int height = HIWORD(lparam);
                SetWindowPos(g_diagnostics_text, nullptr, 24, 96,
                    (std::max)(1, width - 48), (std::max)(1, height - 120),
                    SWP_NOZORDER | SWP_NOACTIVATE);
            }
            // 让圆角面板和静态标题在旧尺寸区域一并清除并重绘，避免拖动缩放时
            // 留下上一帧的圆角边框残影。
            RedrawWindow(window, nullptr, nullptr,
                RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_UPDATENOW);
            return 0;
        }
        case WM_ERASEBKGND:
            return TRUE;
        case WM_PAINT: {
            PAINTSTRUCT paint{};
            HDC dc = BeginPaint(window, &paint);
            RECT client{};
            GetClientRect(window, &client);
            FillRect(dc, &client, popup_background_brush());
            fill_rounded_rect(dc, RECT{12, 12, client.right - 12, client.bottom - 12},
                popup_control_color(), popup_border_color(), 13);
            EndPaint(window, &paint);
            return 0;
        }
        case WM_CTLCOLORSTATIC: {
            HDC dc = reinterpret_cast<HDC>(wparam);
            SetBkColor(dc, popup_control_color());
            SetTextColor(dc, popup_text_secondary_color());
            return reinterpret_cast<LRESULT>(popup_control_brush());
        }
        case WM_CTLCOLOREDIT: {
            HDC dc = reinterpret_cast<HDC>(wparam);
            SetBkColor(dc, popup_control_color());
            SetTextColor(dc, popup_text_secondary_color());
            return reinterpret_cast<LRESULT>(popup_control_brush());
        }
        case WM_CLOSE:
            DestroyWindow(window);
            return 0;
        case WM_DESTROY:
            g_diagnostics_window = nullptr;
            g_diagnostics_text = nullptr;
            return 0;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

void show_diagnostics_window(HWND owner) {
    if (g_diagnostics_window != nullptr) {
        ShowWindow(g_diagnostics_window, SW_RESTORE);
        SetForegroundWindow(g_diagnostics_window);
        refresh_diagnostics_output();
        return;
    }
    g_diagnostics_window = CreateWindowExW(
        WS_EX_DLGMODALFRAME,
        L"WireViewDiagnosticsWindow",
        L"WireView · 诊断与日志",
        WS_OVERLAPPEDWINDOW | WS_VISIBLE,
        CW_USEDEFAULT, CW_USEDEFAULT, 900, 680, owner, nullptr,
        GetModuleHandleW(nullptr), nullptr);
    if (g_diagnostics_window == nullptr) {
        set_status(L"无法打开诊断与日志窗口（系统错误 " +
            std::to_wstring(GetLastError()) + L"）。 ");
    }
}

LRESULT CALLBACK window_procedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
        case WM_CREATE: {
            enable_dark_title_bar(window);
            const HICON application_icon = LoadIconW(
                GetModuleHandleW(nullptr), MAKEINTRESOURCEW(kWireViewAppIcon));
            if (application_icon != nullptr) {
                SendMessageW(window, WM_SETICON, ICON_SMALL,
                    reinterpret_cast<LPARAM>(application_icon));
                SendMessageW(window, WM_SETICON, ICON_BIG,
                    reinterpret_cast<LPARAM>(application_icon));
            }
            g_state.simple_mode_toggle = CreateWindowW(
                L"BUTTON", L"简易模式",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                40, 50, 106, 32,
                window,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(kSimpleModeToggle)),
                nullptr,
                nullptr);
            g_state.full_mode_toggle = CreateWindowW(
                L"BUTTON", L"完整模式",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                174, 50, 106, 32,
                window,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(kFullModeToggle)),
                nullptr,
                nullptr);
            g_state.simple_mirror_button = CreateWindowW(
                L"BUTTON", L"复制屏幕", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                40, 356, 670, 302, window,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(kSimpleMirrorButton)), nullptr, nullptr);
            g_state.simple_extend_button = CreateWindowW(
                L"BUTTON", L"扩展屏幕", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                730, 356, 670, 302, window,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(kSimpleExtendButton)), nullptr, nullptr);
            g_state.simple_display_only_button = CreateWindowW(
                L"BUTTON", L"仅显示", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                40, 744, 684, 96, window,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(kSimpleDisplayOnlyButton)), nullptr, nullptr);
            g_state.simple_input_button = CreateWindowW(
                L"BUTTON", L"触控与手写笔", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                724, 744, 676, 96, window,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(kSimpleInputEnabledButton)), nullptr, nullptr);
            g_state.simple_display_settings_button = CreateWindowW(
                L"BUTTON", L"Windows 显示设置", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                24, 884, 1150, 76, window,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(kSimpleDisplaySettingsButton)), nullptr, nullptr);
            g_state.simple_stop_button = CreateWindowW(
                L"BUTTON", L"■  停止 WireView", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                1180, 884, 232, 76, window,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(kSimpleStopButton)), nullptr, nullptr);
            g_state.simple_status = CreateWindowW(
                L"STATIC", L"准备就绪。", WS_CHILD | SS_LEFT | SS_CENTERIMAGE,
                62, 984, 1320, 34, window, nullptr, nullptr, nullptr);
            g_state.display_selector = CreateWindowW(
                L"COMBOBOX", L"",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST |
                    CBS_OWNERDRAWFIXED | CBS_HASSTRINGS | WS_VSCROLL,
                656, 164, 174, 240,
                window,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(kDisplaySelector)),
                nullptr,
                nullptr);
            g_state.refresh_displays_button = CreateWindowW(
                L"BUTTON", L"刷新",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                1192, 204, 44, 30,
                window,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(kRefreshDisplaysButton)),
                nullptr,
                nullptr);
            g_state.display_config_toggle = CreateWindowW(
                L"BUTTON", L"›", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                1330, 156, 50, 80, window,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(kDisplayConfigToggle)),
                nullptr, nullptr);
            refresh_display_choices();
            g_state.resolution_selector = CreateWindowW(
                L"COMBOBOX", L"",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST |
                    CBS_OWNERDRAWFIXED | CBS_HASSTRINGS | WS_VSCROLL,
                838, 164, 168, 280,
                window,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(kResolutionSelector)),
                nullptr,
                nullptr);
            populate_resolution_selector();
            g_state.vdd_mode_selector = CreateWindowW(
                L"COMBOBOX", L"",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST |
                    CBS_OWNERDRAWFIXED | CBS_HASSTRINGS,
                1018, 164, 108, 180,
                window,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(kVddModeSelector)),
                nullptr,
                nullptr);
            populate_vdd_mode_selector();
            g_state.frame_rate_selector = CreateWindowW(
                L"COMBOBOX", L"",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST |
                    CBS_OWNERDRAWFIXED | CBS_HASSTRINGS,
                1138, 164, 98, 180,
                window,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(kFrameRateSelector)),
                nullptr,
                nullptr);
            SendMessageW(
                g_state.frame_rate_selector, CB_ADDSTRING, 0U,
                reinterpret_cast<LPARAM>(L"60 FPS"));
            SendMessageW(
                g_state.frame_rate_selector, CB_ADDSTRING, 0U,
                reinterpret_cast<LPARAM>(L"120（实验）"));
            SendMessageW(g_state.frame_rate_selector, CB_SETCURSEL, 0U, 0);
            CreateWindowW(
                L"BUTTON", L"刷新设备",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                478, 102, 120, 34,
                window,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdentifyDeviceButton)),
                nullptr,
                nullptr);
            restore_stream_settings();
            g_state.recommendation = CreateWindowW(
                L"STATIC", L"", WS_CHILD,
                0, 0, 1, 1,
                window, nullptr, nullptr, nullptr);
            update_recommendation();
            g_state.start_mirror_button = CreateWindowW(
                L"BUTTON", L"▶  启动 WireView",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                824, 558, 408, 56,
                window,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(kStartMirrorButton)),
                nullptr,
                nullptr);
            g_state.start_input_button = CreateWindowW(
                L"BUTTON", L"触控与手写笔",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                946, 350, 286, 72,
                window,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(kInputEnabledButton)),
                nullptr,
                nullptr);
            g_state.full_display_only_button = CreateWindowW(
                L"BUTTON", L"仅显示",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                658, 350, 288, 72,
                window,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(kInputDisplayOnlyButton)),
                nullptr,
                nullptr);
            g_state.stop_button = CreateWindowW(
                L"BUTTON", L"■  停止 WireView",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                824, 626, 260, 38,
                window,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(kStopButton)),
                nullptr,
                nullptr);
            g_state.display_settings_button = CreateWindowW(
                L"BUTTON", L"Windows 显示设置",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                526, 750, 240, 44,
                window,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(kDisplaySettingsButton)),
                nullptr,
                nullptr);
            g_state.diagnostics_button = CreateWindowW(
                L"BUTTON", L"›", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                1330, 858, 52, 78, window,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(kDiagnosticsButton)), nullptr, nullptr);
            g_state.check_update_button = CreateWindowW(
                L"BUTTON", L"检查更新", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                1290, 984, 100, 32, window,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(kCheckUpdateButton)), nullptr, nullptr);
            g_state.diagnostics_back_button = CreateWindowW(
                L"BUTTON", L"←  返回控制中心", WS_CHILD | WS_TABSTOP | BS_OWNERDRAW,
                38, 54, 180, 40, window,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(kDiagnosticsBackButton)), nullptr, nullptr);
            g_state.diagnostics_output = CreateWindowExW(
                WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | ES_MULTILINE | ES_READONLY |
                    ES_AUTOVSCROLL | WS_VSCROLL,
                40, 128, 1344, 800, window,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(kDiagnosticsOutput)), nullptr, nullptr);
            g_state.vdd_status = CreateWindowW(
                L"STATIC", L"正在检查 Parsec VDD……",
                WS_CHILD | SS_LEFT,
                0, 0, 1, 1,
                window, nullptr, nullptr, nullptr);
            g_state.create_vdd_button = CreateWindowW(
                L"BUTTON", L"＋  创建副屏",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                42, 430, 170, 42,
                window,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(kCreateVddButton)),
                nullptr,
                nullptr);
            g_state.remove_vdd_button = CreateWindowW(
                L"BUTTON", L"－  移除副屏",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                224, 430, 170, 42,
                window,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(kRemoveVddButton)),
                nullptr,
                nullptr);
            g_state.recover_vdd_button = CreateWindowW(
                L"BUTTON", L"清理残留副屏",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                406, 430, 192, 42,
                window,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(kRecoverVddButton)),
                nullptr,
                nullptr);
            CreateWindowW(
                L"BUTTON", L"预览所选画面范围",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                42, 750, 224, 44, window,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(kPreviewDeviceButton)),
                nullptr, nullptr);
            CreateWindowW(
                L"BUTTON", L"编辑自定义分辨率",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                278, 750, 236, 44, window,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(kCustomModesButton)),
                nullptr, nullptr);
            g_state.status = CreateWindowW(
                L"STATIC", L"准备就绪。",
                WS_CHILD | WS_VISIBLE | SS_LEFT,
                824, 746, 408, 54, window,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(kStatusText)),
                nullptr, nullptr);
            for (HWND control : {g_state.simple_mode_toggle, g_state.full_mode_toggle,
                     g_state.simple_mirror_button, g_state.simple_extend_button,
                     g_state.simple_display_only_button, g_state.simple_input_button,
                     g_state.simple_stop_button, g_state.simple_display_settings_button,
                     g_state.simple_status, g_state.display_selector,
                     g_state.refresh_displays_button, g_state.resolution_selector,
                     g_state.vdd_mode_selector, g_state.frame_rate_selector,
                     g_state.start_mirror_button, g_state.start_input_button,
                     g_state.full_display_only_button, g_state.stop_button,
                     g_state.display_settings_button, g_state.vdd_status,
                     g_state.create_vdd_button, g_state.remove_vdd_button,
                     g_state.recover_vdd_button, g_state.status, g_state.display_config_toggle,
                     g_state.diagnostics_button, g_state.check_update_button,
                     g_state.diagnostics_back_button, g_state.diagnostics_output}) {
                apply_control_theme(control, control == g_state.status || control == g_state.vdd_status
                    || control == g_state.simple_status || control == g_state.diagnostics_output
                    ? g_font_small : g_font_body);
            }
            for (HWND selector : {g_state.display_selector, g_state.resolution_selector,
                     g_state.vdd_mode_selector, g_state.frame_rate_selector}) {
                SendMessageW(selector, CB_SETITEMHEIGHT, static_cast<WPARAM>(-1), 28);
                SendMessageW(selector, CB_SETITEMHEIGHT, 0U, 26);
            }
            for (HWND button : {g_state.simple_mode_toggle, g_state.full_mode_toggle,
                     g_state.simple_mirror_button, g_state.simple_extend_button,
                     g_state.simple_display_only_button, g_state.simple_input_button,
                     g_state.simple_stop_button, g_state.simple_display_settings_button,
                     g_state.refresh_displays_button,
                     GetDlgItem(window, kIdentifyDeviceButton), g_state.start_mirror_button,
                     g_state.start_input_button, g_state.full_display_only_button, g_state.stop_button,
                     g_state.display_settings_button, g_state.create_vdd_button,
                     g_state.remove_vdd_button, g_state.recover_vdd_button,
                     GetDlgItem(window, kPreviewDeviceButton), GetDlgItem(window, kCustomModesButton),
                     g_state.display_config_toggle, g_state.diagnostics_button,
                     g_state.check_update_button, g_state.diagnostics_back_button}) {
                enable_owner_draw_button_feedback(button);
            }
            scan_vdd_recovery_records();
            update_session_action_controls();
            g_state.window = window;
            add_tray_icon(window);
            g_state.simple_mode = true;
            EnableWindow(g_state.check_update_button, FALSE);
            apply_control_center_view(window);
#ifdef WIREVIEW_WEBVIEW_UI
            std::wstring executable_path(32'768U, L'\0');
            const DWORD executable_length = GetModuleFileNameW(
                nullptr, executable_path.data(), static_cast<DWORD>(executable_path.size()));
            executable_path.resize(executable_length);
            const std::filesystem::path web_assets =
                std::filesystem::path(executable_path).parent_path() / L"web";
            if (!std::filesystem::exists(web_assets / L"index.html")) {
                MessageBoxW(window,
                    L"未找到 WireView Web 控制中心资源。请重新解压完整发布包，或运行"
                    L"“WireView 原生控制中心.exe”。",
                    L"WireView 控制中心", MB_OK | MB_ICONERROR);
                PostMessageW(window, WM_CLOSE, 0U, 0);
                return 0;
            }
            g_webview_host = std::make_unique<wireview::host::WebViewHost>();
            hide_native_controls_for_web();
            (void)g_webview_host->initialize(
                window, web_assets,
                [](std::wstring_view command) { handle_web_message(command); },
                []() { publish_web_state(); },
                [window](HRESULT error) {
                    const std::wstring text =
                        L"无法初始化 Microsoft Edge WebView2 Evergreen Runtime（错误 " +
                        std::to_wstring(static_cast<unsigned long>(error)) + L"）。\n\n"
                        L"请安装 WebView2 Evergreen Runtime 后重试，或运行同目录的"
                        L"“WireView 原生控制中心.exe”使用兼容界面。";
                    MessageBoxW(window, text.c_str(), L"WireView 控制中心",
                        MB_OK | MB_ICONERROR);
                    PostMessageW(window, WM_CLOSE, 0U, 0);
                });
#endif
            SetTimer(window, kDeviceDetectTimer, 2'000U, nullptr);
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT paint{};
            HDC dc = BeginPaint(window, &paint);
#ifdef WIREVIEW_WEBVIEW_UI
            // WebView2 是此目标中唯一的可见内容；不再让父窗口绘制旧 GDI 页面，
            // 避免页面导航或重组时露出 legacy 布局。
            (void)dc;
#else
            if (g_state.diagnostics_page) {
                paint_window_buffered(window, dc, paint_diagnostics_page);
            } else if (g_state.simple_mode) {
                paint_window_buffered(window, dc, paint_simple_window);
            } else {
                paint_window_buffered(window, dc, paint_full_window);
            }
#endif
            EndPaint(window, &paint);
            return 0;
        }
        case WM_DRAWITEM:
            if (paint_owner_draw_button(
                    *reinterpret_cast<const DRAWITEMSTRUCT*>(lparam))) {
                return TRUE;
            }
            break;
        case WM_CTLCOLORSTATIC: {
            HDC dc = reinterpret_cast<HDC>(wparam);
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, kColorTextMuted);
            return reinterpret_cast<LRESULT>(g_background_brush);
        }
        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORLISTBOX: {
            HDC dc = reinterpret_cast<HDC>(wparam);
            SetBkColor(dc, kColorControl);
            SetTextColor(dc, kColorText);
            return reinterpret_cast<LRESULT>(g_control_brush);
        }
        case WM_COMMAND:
            log_control_event(L"native-command", L"id=" +
                std::to_wstring(LOWORD(wparam)) + L" notify=" + std::to_wstring(HIWORD(wparam)));
            if (LOWORD(wparam) == kRefreshDisplaysButton) {
                refresh_display_choices();
                scan_vdd_recovery_records();
                set_status(g_state.display_choices.empty()
                    ? L"未发现已附加到桌面的显示器。"
                    : L"显示器列表已刷新；默认优先复用现有副屏。");
                return 0;
            }
            if (LOWORD(wparam) == kFrameRateSelector && HIWORD(wparam) == CBN_SELCHANGE) {
                update_recommendation();
                InvalidateRect(window, nullptr, FALSE);
                return 0;
            }
            if (LOWORD(wparam) == kResolutionSelector && HIWORD(wparam) == CBN_SELCHANGE) {
                populate_vdd_mode_selector();
                update_recommendation();
                InvalidateRect(window, nullptr, FALSE);
                return 0;
            }
            if (LOWORD(wparam) == kVddModeSelector && HIWORD(wparam) == CBN_SELCHANGE) {
                update_recommendation();
                InvalidateRect(window, nullptr, FALSE);
                return 0;
            }
            if (LOWORD(wparam) == kDisplaySelector && HIWORD(wparam) == CBN_SELCHANGE) {
                update_recommendation();
                InvalidateRect(window, nullptr, FALSE);
                return 0;
            }
            switch (LOWORD(wparam)) {
                case kPreviewDeviceButton:
                    start_session(window, false, true);
                    return 0;
                case kIdentifyDeviceButton:
                    start_session(window, false, false, true);
                    return 0;
                case kSimpleModeToggle:
                    open_simple_mode(window);
                    return 0;
                case kFullModeToggle:
                    return_to_full_mode();
                    return 0;
                case kDisplayConfigToggle:
                    g_state.display_config_expanded = !g_state.display_config_expanded;
                    apply_control_center_view(window);
                    return 0;
                case kDiagnosticsButton:
                    show_diagnostics_window(window);
                    return 0;
                case kDiagnosticsBackButton:
                    g_state.diagnostics_page = false;
                    apply_control_center_view(window);
                    return 0;
                case kCustomModesButton:
                    sync_custom_modes_from_parsec_vdd();
                    show_custom_mode_editor(window);
                    return 0;
                case kStartMirrorButton:
                    start_session(window, g_state.session_input_enabled);
                    return 0;
                case kStartInputButton:
                    start_session(window, true);
                    return 0;
                case kInputDisplayOnlyButton:
                    g_state.session_input_enabled = false;
                    InvalidateRect(window, nullptr, FALSE);
                    InvalidateRect(g_state.simple_window, nullptr, FALSE);
                    return 0;
                case kInputEnabledButton:
                    g_state.session_input_enabled = true;
                    InvalidateRect(window, nullptr, FALSE);
                    InvalidateRect(g_state.simple_window, nullptr, FALSE);
                    return 0;
                case kSimpleDisplayOnlyButton:
                    g_state.session_input_enabled = false;
                    InvalidateRect(window, nullptr, FALSE);
                    return 0;
                case kSimpleInputEnabledButton:
                    g_state.session_input_enabled = true;
                    InvalidateRect(window, nullptr, FALSE);
                    return 0;
                case kSimpleMirrorButton:
                    begin_simple_copy(g_state.session_input_enabled);
                    return 0;
                case kSimpleExtendButton:
                    begin_simple_extension(g_state.session_input_enabled);
                    return 0;
                case kStopButton:
                case kSimpleStopButton:
                    notify_android_of_user_stop();
                    stop_session();
                    set_status(L"WireView 已停止。");
                    return 0;
                case kDisplaySettingsButton:
                case kSimpleDisplaySettingsButton:
                    open_windows_display_settings();
                    return 0;
                case kCreateVddButton:
                    (void)start_vdd_broker(window);
                    return 0;
                case kRemoveVddButton:
                    request_vdd_removal();
                    return 0;
                case kRecoverVddButton:
                    recover_stale_vdd_session(window);
                    return 0;
                case kTrayRestore:
                    restore_window(window);
                    return 0;
                case kTrayStartMirror:
                    start_session(window, false);
                    return 0;
                case kTrayStartInput:
                    start_session(window, true);
                    return 0;
                case kTrayStop:
                    notify_android_of_user_stop();
                    stop_session();
                    set_status(L"WireView 已停止。");
                    return 0;
                case kTrayExit:
                    begin_control_center_exit(window);
                    return 0;
            }
            break;
        case kFinalizeControlExit:
            log_control_event(L"exit-finalize", L"开始销毁主窗口。 ");
            if (DestroyWindow(window) == FALSE) {
                log_control_event(L"exit-finalize", L"DestroyWindow 失败，系统错误 " +
                    std::to_wstring(GetLastError()));
            }
            return 0;
        case WM_POWERBROADCAST:
            update_recommendation();
            return TRUE;
        case WM_TIMER:
            if (wparam == kVddTimer) {
                poll_vdd_broker(window);
#ifdef WIREVIEW_WEBVIEW_UI
                publish_web_state();
#endif
                return 0;
            }
            if (wparam == kDeviceDetectTimer) {
                poll_device_detection(window);
                sync_custom_modes_from_parsec_vdd();
#ifdef WIREVIEW_WEBVIEW_UI
                publish_web_state();
#endif
                return 0;
            }
            if (wparam == kProcessTimer && g_state.shell_process != nullptr) {
                if (const auto script_status = read_script_status();
                    script_status && *script_status != g_state.last_script_status) {
                    g_state.last_script_status = *script_status;
                    set_status(*script_status);
                }
                DWORD exit_code = STILL_ACTIVE;
                if (!GetExitCodeProcess(g_state.shell_process, &exit_code) ||
                    exit_code != STILL_ACTIVE) {
                    const bool completed_profile_probe = g_state.profile_probe_active;
                    const std::wstring final_status = read_script_status().value_or(
                        g_state.last_script_status);
                    if (completed_profile_probe && !g_state.device_profile_note.empty()) {
                        notify_android_of_device_identified();
                    }
                    KillTimer(window, kProcessTimer);
                    stop_session();
                    set_status(completed_profile_probe && !g_state.device_profile_note.empty()
                        ? g_state.device_profile_note
                        : (!final_status.empty()
                            ? final_status
                            : L"WireView 会话已结束。"));
                }
            }
            return 0;
        case WM_SIZE:
            if (wparam == SIZE_MINIMIZED) {
                ShowWindow(window, SW_HIDE);
            }
#ifndef WIREVIEW_WEBVIEW_UI
            else {
                apply_control_center_view(window);
            }
#endif
#ifdef WIREVIEW_WEBVIEW_UI
            if (g_webview_host) {
                g_webview_host->resize();
            }
#endif
            return 0;
        case WM_GETMINMAXINFO: {
            auto* info = reinterpret_cast<MINMAXINFO*>(lparam);
            info->ptMinTrackSize.x = 1040;
            info->ptMinTrackSize.y = 760;
            return 0;
        }
        case WM_DPICHANGED: {
            const RECT* suggested = reinterpret_cast<const RECT*>(lparam);
            SetWindowPos(window, nullptr, suggested->left, suggested->top,
                suggested->right - suggested->left, suggested->bottom - suggested->top,
                SWP_NOZORDER | SWP_NOACTIVATE);
            return 0;
        }
        case WM_CLOSE:
            ShowWindow(window, SW_HIDE);
            return 0;
        case kTrayMessage:
            if (lparam == WM_LBUTTONDBLCLK) {
                log_control_event(L"tray", L"双击恢复控制中心。 ");
                restore_window(window);
            } else if (lparam == WM_RBUTTONUP) {
                log_control_event(L"tray", L"打开托盘菜单。 ");
                HMENU menu = CreatePopupMenu();
                if (menu != nullptr) {
                    AppendMenuW(menu, MF_STRING, kTrayRestore, L"显示 WireView");
                    AppendMenuW(menu, MF_SEPARATOR, 0U, nullptr);
                    AppendMenuW(menu, MF_STRING, kTrayStartMirror, L"开始安全镜像");
                    AppendMenuW(menu, MF_STRING, kTrayStartInput, L"开始并启用输入");
                    AppendMenuW(menu, MF_STRING, kTrayStop, L"停止 WireView");
                    AppendMenuW(menu, MF_SEPARATOR, 0U, nullptr);
                    AppendMenuW(menu, MF_STRING, kTrayExit, L"退出控制中心");
                    POINT point{};
                    GetCursorPos(&point);
                    SetForegroundWindow(window);
                    TrackPopupMenu(menu, TPM_RIGHTBUTTON, point.x, point.y, 0, window, nullptr);
                    DestroyMenu(menu);
                }
            }
            return 0;
        case WM_DESTROY:
            g_state.exiting = true;
            log_control_event(L"destroy", L"收到 WM_DESTROY，开始停止会话、VDD 与 WebView。 ");
            if (g_state.simple_window != nullptr && IsWindow(g_state.simple_window) != FALSE) {
                DestroyWindow(g_state.simple_window);
            }
#ifdef WIREVIEW_WEBVIEW_UI
            if (g_webview_host) {
                log_control_event(L"destroy", L"正在关闭 WebView2 控制器。 ");
                g_webview_host->shutdown();
                g_webview_host.reset();
                log_control_event(L"destroy", L"WebView2 控制器已关闭。 ");
            }
#endif
            // 退出控制中心代表电脑端离线，不等同于用户在控制中心内主动停止会话。
            // 设备端据此提示等待电脑端，而不会错误显示“WireView 已停止”。
            notify_android_of_host_exit();
            KillTimer(window, kProcessTimer);
            KillTimer(window, kVddTimer);
            KillTimer(window, kDeviceDetectTimer);
            if (g_state.device_probe_process != nullptr) {
                TerminateProcess(g_state.device_probe_process, 0U);
                CloseHandle(g_state.device_probe_process);
                g_state.device_probe_process = nullptr;
            }
            stop_session();
            shutdown_vdd_broker_on_exit();
            remove_tray_icon();
            log_control_event(L"destroy", L"退出清理完成，投递消息循环结束。 ");
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show_command) {
    int argument_count = 0;
    wchar_t** arguments = CommandLineToArgvW(GetCommandLineW(), &argument_count);
    if (arguments != nullptr && argument_count == 2 &&
        std::wstring_view(arguments[1]) == L"--self-test-status") {
        LocalFree(arguments);
        return run_script_status_self_test();
    }
    if (arguments != nullptr) {
        LocalFree(arguments);
    }
    HANDLE instance_mutex = CreateMutexW(nullptr, TRUE, kControlCenterInstanceMutex);
    if (instance_mutex == nullptr) {
        return 1;
    }
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        restore_existing_control_center();
        CloseHandle(instance_mutex);
        return 0;
    }
    enable_per_monitor_dpi_awareness();
    Gdiplus::GdiplusStartupInput gdiplus_startup{};
    if (Gdiplus::GdiplusStartup(&g_gdiplus_token, &gdiplus_startup, nullptr) != Gdiplus::Ok) {
        g_gdiplus_token = 0U;
    }
    g_state.session_launcher_path = find_session_launcher().value_or(std::filesystem::path{});
    g_state.vdd_broker_path = find_vdd_broker();
    g_state.settings_file = find_settings_file();
    if (!g_state.settings_file.empty()) {
        SYSTEMTIME time{};
        GetLocalTime(&time);
        wchar_t name[80]{};
        swprintf_s(name, L"control-%04u%02u%02u-%lu.log", time.wYear, time.wMonth,
            time.wDay, GetCurrentProcessId());
        g_state.runtime_log_file = g_state.settings_file.parent_path() / L"logs" / name;
        log_control_event(L"startup", L"WireView 控制中心启动；PID=" +
            std::to_wstring(GetCurrentProcessId()) + L"；日志=" + g_state.runtime_log_file.wstring());
    }
    std::wstring executable_path(32'768U, L'\0');
    const DWORD executable_length = GetModuleFileNameW(
        nullptr, executable_path.data(), static_cast<DWORD>(executable_path.size()));
    executable_path.resize(executable_length);
    if (!executable_path.empty()) {
        g_state.device_model_catalog_file = std::filesystem::path(executable_path).parent_path() /
            L"data/device-models/android-cn-mobile.tsv";
    }
    std::array<wchar_t, 32'768U> local_app_data{};
    const DWORD local_app_data_length = GetEnvironmentVariableW(
        L"LOCALAPPDATA", local_app_data.data(), static_cast<DWORD>(local_app_data.size()));
    if (local_app_data_length > 0U && local_app_data_length < local_app_data.size()) {
        g_state.device_model_cache_file = std::filesystem::path(local_app_data.data()) /
            L"WireView/device-model-cache.tsv";
    }
    if (!g_state.settings_file.empty()) {
        g_state.custom_modes_file = g_state.settings_file.parent_path() / L"custom-modes.ini";
    }
    load_custom_modes();
    initialize_ui_theme();
    load_control_ui_assets();

    const WNDCLASSW window_class{
        .lpfnWndProc = window_procedure,
        .hInstance = instance,
        .hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512)),
        .hbrBackground = g_background_brush,
        .lpszClassName = L"WireViewControlWindow",
    };
    if (RegisterClassW(&window_class) == 0U) {
        return 1;
    }
    const WNDCLASSW simple_window_class{
        .lpfnWndProc = simple_window_procedure,
        .hInstance = instance,
        .hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512)),
        .hbrBackground = g_background_brush,
        .lpszClassName = L"WireViewSimpleWindow",
    };
    if (RegisterClassW(&simple_window_class) == 0U) {
        return 1;
    }
    const WNDCLASSW custom_mode_class{
        .lpfnWndProc = custom_mode_editor_procedure,
        .hInstance = instance,
        .hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512)),
        .hbrBackground = g_background_brush,
        .lpszClassName = L"WireViewCustomModeEditor",
    };
    if (RegisterClassW(&custom_mode_class) == 0U) {
        return 1;
    }
    const WNDCLASSW diagnostics_window_class{
        .lpfnWndProc = diagnostics_window_procedure,
        .hInstance = instance,
        .hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512)),
        .hbrBackground = g_background_brush,
        .lpszClassName = L"WireViewDiagnosticsWindow",
    };
    if (RegisterClassW(&diagnostics_window_class) == 0U) {
        return 1;
    }
    // The WebView2 control center is designed as one wide, complete canvas.
    // Use that canvas size by default, but never place part of the fixed-size
    // window outside the current monitor's usable work area.
    POINT cursor{};
    (void)GetCursorPos(&cursor);
    const HMONITOR monitor = MonitorFromPoint(cursor, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO monitor_info{};
    monitor_info.cbSize = sizeof(monitor_info);
    if (GetMonitorInfoW(monitor, &monitor_info) == FALSE) {
        monitor_info.rcWork = RECT{0, 0, 1600, 1040};
    }
    const int work_width = monitor_info.rcWork.right - monitor_info.rcWork.left;
    const int work_height = monitor_info.rcWork.bottom - monitor_info.rcWork.top;
#ifdef WIREVIEW_WEBVIEW_UI
    // This release targets the current 2560x1600 / 175% desktop first.
    // Keep a deliberate, centered fixed-size window instead of multiplying it
    // by DPI and effectively turning the control center into a full-screen app.
    constexpr int desired_canvas_width = 1600;
    constexpr int desired_canvas_height = 1040;
#else
    // Keep the independent legacy GDI fallback at its historical size.
    constexpr int desired_canvas_width = 1440;
    constexpr int desired_canvas_height = 1080;
#endif
    constexpr int window_margin = 24;
    const int window_width = (std::min)(desired_canvas_width, (std::max)(800, work_width - window_margin));
    const int window_height = (std::min)(desired_canvas_height, (std::max)(600, work_height - window_margin));
    const int window_x = monitor_info.rcWork.left + (work_width - window_width) / 2;
    const int window_y = monitor_info.rcWork.top + (work_height - window_height) / 2;

    HWND window = CreateWindowExW(
        0,
        window_class.lpszClassName,
        L"WireView 控制中心",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN,
        window_x,
        window_y,
        window_width,
        window_height,
        nullptr,
        nullptr,
        instance,
        nullptr);
    if (window == nullptr) {
        return 1;
    }
    const int initial_show_command = show_command == SW_HIDE ? SW_SHOWNORMAL : show_command;
    ShowWindow(window, initial_show_command);
    UpdateWindow(window);

    MSG message{};
    while (GetMessageW(&message, nullptr, 0U, 0U) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    destroy_ui_theme();
    g_copy_screen_asset.reset();
    g_extend_screen_asset.reset();
    g_display_only_asset.reset();
    g_touch_pen_asset.reset();
    if (g_gdiplus_token != 0U) {
        Gdiplus::GdiplusShutdown(g_gdiplus_token);
        g_gdiplus_token = 0U;
    }
    CloseHandle(instance_mutex);
    return 0;
}
