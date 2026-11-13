#pragma once
#include <Windows.h>
#include <algorithm>
#include <iterator>
#include <string>
#include <vector>

namespace wireview::host::screen_layout {
struct Screen {
    unsigned number{};
    std::wstring source;
    std::wstring monitor;
    RECT bounds{};
    bool primary{};
};

inline std::vector<Screen> read() {
    constexpr UINT32 flags = QDC_ONLY_ACTIVE_PATHS | QDC_VIRTUAL_MODE_AWARE;
    for (int attempt = 0; attempt < 2; ++attempt) {
        UINT32 path_count{}, mode_count{};
        if (GetDisplayConfigBufferSizes(flags, &path_count, &mode_count) != ERROR_SUCCESS) { break; }
        std::vector<DISPLAYCONFIG_PATH_INFO> paths(path_count);
        std::vector<DISPLAYCONFIG_MODE_INFO> modes(mode_count);
        const auto result = QueryDisplayConfig(flags, &path_count, paths.data(), &mode_count, modes.data(), nullptr);
        if (result == ERROR_INSUFFICIENT_BUFFER) { continue; }
        if (result != ERROR_SUCCESS) { break; }
        std::vector<Screen> screens;
        for (UINT32 i = 0; i < path_count; ++i) {
            const auto& path = paths[i];
            const UINT32 index = (path.flags & DISPLAYCONFIG_PATH_SUPPORT_VIRTUAL_MODE) != 0U
                ? path.sourceInfo.sourceModeInfoIdx : path.sourceInfo.modeInfoIdx;
            if (index >= mode_count || modes[index].infoType != DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE) { continue; }
            const auto& mode = modes[index].sourceMode;
            DISPLAYCONFIG_SOURCE_DEVICE_NAME source{};
            source.header = {DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME, sizeof(source),
                path.sourceInfo.adapterId, path.sourceInfo.id};
            DISPLAYCONFIG_TARGET_DEVICE_NAME target{};
            target.header = {DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME, sizeof(target),
                path.targetInfo.adapterId, path.targetInfo.id};
            if (DisplayConfigGetDeviceInfo(&source.header) != ERROR_SUCCESS ||
                DisplayConfigGetDeviceInfo(&target.header) != ERROR_SUCCESS || mode.width == 0U || mode.height == 0U) { continue; }
            // Use the display-path order, not the unrelated suffix of the GDI source name.
            screens.push_back({i + 1U, source.viewGdiDeviceName, target.monitorDevicePath,
                {mode.position.x, mode.position.y, mode.position.x + static_cast<LONG>(mode.width),
                 mode.position.y + static_cast<LONG>(mode.height)}, mode.position.x == 0 && mode.position.y == 0});
        }
        return screens;
    }
    return {};
}

inline const std::vector<Screen>& current(bool force = false) {
    static std::vector<Screen> screens;
    static ULONGLONG updated{};
    const auto now = GetTickCount64();
    // Reuse existing UI snapshots; no additional polling thread or timer.
    if (force || updated == 0U || now - updated >= 1000U) { screens = read(); updated = now; }
    return screens;
}

template<class Escape>
std::wstring json(Escape escape) {
    std::wstring value = L"[";
    for (const auto& screen : current()) {
        if (value.size() > 1U) { value += L','; }
        const auto& r = screen.bounds;
        value += L"{\"number\":" + std::to_wstring(screen.number) + L",\"source\":\"" + escape(screen.source) +
            L"\",\"path\":\"" + escape(screen.monitor) + L"\",\"x\":" + std::to_wstring(r.left) +
            L",\"y\":" + std::to_wstring(r.top) + L",\"width\":" + std::to_wstring(r.right - r.left) +
            L",\"height\":" + std::to_wstring(r.bottom - r.top) + L",\"primary\":" +
            (screen.primary ? L"true" : L"false") + L"}";
    }
    return value + L"]";
}

inline LRESULT CALLBACK identify_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_NCHITTEST) { return HTTRANSPARENT; }
    if (message == WM_TIMER) { DestroyWindow(window); return 0; }
    if (message == WM_ERASEBKGND) { return 1; }
    if (message == WM_PAINT) {
        PAINTSTRUCT paint{};
        const auto dc = BeginPaint(window, &paint);
        RECT rect{};
        GetClientRect(window, &rect);
        const auto background = CreateSolidBrush(RGB(32, 32, 32));
        FillRect(dc, &rect, background);
        wchar_t label[96]{};
        GetWindowTextW(window, label, static_cast<int>(std::size(label)));
        const auto text_height = std::max<LONG>(24, std::min(rect.bottom * 3 / 5,
            rect.right * 3 / (lstrlenW(label) + 2)));
        const auto font = CreateFontW(-text_height, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
            DEFAULT_PITCH, L"Segoe UI");
        const auto previous = SelectObject(dc, font);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(245, 248, 255));
        DrawTextW(dc, label, -1, &rect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        SelectObject(dc, previous);
        DeleteObject(font);
        DeleteObject(background);
        EndPaint(window, &paint);
        return 0;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

inline void identify() {
    constexpr wchar_t class_name[] = L"WireViewScreenIdentify";
    WNDCLASSW type{};
    type.lpfnWndProc = identify_proc;
    type.hInstance = GetModuleHandleW(nullptr);
    type.lpszClassName = class_name;
    RegisterClassW(&type);
    struct Group { RECT bounds; std::wstring label; };
    std::vector<Group> groups;
    for (const auto& screen : current(true)) {
        const auto found = std::find_if(groups.begin(), groups.end(), [&](const auto& group) {
            return EqualRect(&group.bounds, &screen.bounds) != FALSE;
        });
        if (found == groups.end()) { groups.push_back({screen.bounds, std::to_wstring(screen.number)}); }
        else { found->label += L" | " + std::to_wstring(screen.number); }
    }
    const auto previous_dpi = SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    for (const auto& group : groups) {
        const auto& r = group.bounds;
        MONITORINFO info{sizeof(info)};
        RECT work = r;
        if (GetMonitorInfoW(MonitorFromRect(&r, MONITOR_DEFAULTTONEAREST), &info)) { work = info.rcWork; }
        const auto window = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE |
            WS_EX_LAYERED | WS_EX_TRANSPARENT, class_name, group.label.c_str(), WS_POPUP,
            work.left + 20, work.top + 20, 200, 200, nullptr, nullptr, type.hInstance, nullptr);
        if (window != nullptr) {
            const auto dpi = GetDpiForWindow(window);
            const int limit = static_cast<int>(std::max<LONG>(96,
                std::min(work.right - work.left, work.bottom - work.top) / 3));
            const int side = std::clamp(MulDiv(200, static_cast<int>(dpi), 96), 96, limit);
            const int margin = MulDiv(24, static_cast<int>(dpi), 96);
            SetWindowPos(window, HWND_TOPMOST, work.left + margin, work.bottom - side - margin,
                side, side, SWP_NOACTIVATE);
            SetLayeredWindowAttributes(window, 0, 255, LWA_ALPHA);
            if (SetTimer(window, 1U, 3000U, nullptr) == 0U) { DestroyWindow(window); continue; }
            ShowWindow(window, SW_SHOWNOACTIVATE);
            UpdateWindow(window);
        }
    }
    if (previous_dpi != nullptr) { SetThreadDpiAwarenessContext(previous_dpi); }
}
} // namespace wireview::host::screen_layout
