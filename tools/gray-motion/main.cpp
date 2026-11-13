#define NOMINMAX
#include <Windows.h>
#include <d3d11_1.h>
#include <dxgi1_3.h>
#include <wrl/client.h>
#include "frame_pacer.hpp"
#include <algorithm>
#include <cwctype>
#include <stdexcept>
#include <string>
#include <vector>

// Uses the same D3D11 ClearView drawing as the original motion.cpp fixture.
namespace {
using Microsoft::WRL::ComPtr;
bool running = true, paused = false, fullscreen = false;
WINDOWPLACEMENT placement{sizeof(placement)};
DWORD window_style = 0;

void check(HRESULT hr, const char* operation) {
    if (FAILED(hr)) throw std::runtime_error(std::string(operation) + ": " + std::to_string(hr));
}

BOOL CALLBACK choose_monitor(HMONITOR monitor, HDC, LPRECT, LPARAM data) {
    auto& chosen = *reinterpret_cast<MONITORINFOEXW*>(data);
    MONITORINFOEXW current{};
    current.cbSize = sizeof(current);
    if (GetMonitorInfoW(monitor, &current)) {
        const bool secondary = !(current.dwFlags & MONITORINFOF_PRIMARY);
        if (!chosen.cbSize || ((chosen.dwFlags & MONITORINFOF_PRIMARY) && secondary)) chosen = current;
        if (secondary) {
            for (DWORD index=0;; ++index) {
                DISPLAY_DEVICEW child{};
                child.cb = sizeof(child);
                if (!EnumDisplayDevicesW(current.szDevice, index, &child, 0)) break;
                std::wstring identity = std::wstring(child.DeviceString) + child.DeviceID + child.DeviceKey;
                std::transform(identity.begin(), identity.end(), identity.begin(),
                    [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
                if (identity.find(L"parsec") != std::wstring::npos || identity.find(L"psccdd0") != std::wstring::npos) {
                    chosen = current; return FALSE;
                }
            }
        }
    }
    return TRUE;
}

void toggle_fullscreen(HWND window) {
    if (!fullscreen) {
        placement.length = sizeof(placement);
        GetWindowPlacement(window, &placement);
        window_style = GetWindowLongW(window, GWL_STYLE);
        MONITORINFO monitor{sizeof(monitor)};
        GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &monitor);
        SetWindowLongW(window, GWL_STYLE, window_style & ~WS_OVERLAPPEDWINDOW);
        const RECT r = monitor.rcMonitor;
        SetWindowPos(window, nullptr, r.left, r.top, r.right-r.left, r.bottom-r.top,
            SWP_NOZORDER | SWP_FRAMECHANGED);
    } else {
        SetWindowLongW(window, GWL_STYLE, window_style);
        SetWindowPlacement(window, &placement);
        SetWindowPos(window, nullptr, 0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
    }
    fullscreen = !fullscreen;
}

BOOL CALLBACK list_monitors(HMONITOR monitor, HDC, LPRECT, LPARAM data) {
    MONITORINFOEXW info{};
    info.cbSize = sizeof(info);
    if (GetMonitorInfoW(monitor, &info))
        reinterpret_cast<std::vector<MONITORINFOEXW>*>(data)->push_back(info);
    return TRUE;
}

void switch_monitor(HWND window, int direction) {
    std::vector<MONITORINFOEXW> screens;
    EnumDisplayMonitors(nullptr, nullptr, list_monitors, reinterpret_cast<LPARAM>(&screens));
    if (screens.empty()) return;
    MONITORINFO current{sizeof(current)};
    GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &current);
    int index = 0, count = static_cast<int>(screens.size());
    for (int i=0; i<count; ++i)
        if (EqualRect(&current.rcMonitor, &screens[i].rcMonitor)) index = i;
    index = (index+direction+count)%count;
    if (direction) {
        const bool was_fullscreen = fullscreen;
        if (was_fullscreen) toggle_fullscreen(window);
        ShowWindow(window, SW_RESTORE);
        const RECT r = screens[index].rcWork;
        SetWindowPos(window, nullptr, r.left+40, r.top+40, (r.right-r.left)*3/4,
            (r.bottom-r.top)*3/4, SWP_NOZORDER);
        if (was_fullscreen) toggle_fullscreen(window);
    }
    wchar_t label[80]{};
    swprintf_s(label, L"下一屏 ▶  %d/%d", index+1, count);
    SetWindowTextW(GetDlgItem(window, 2), label);
    SetFocus(window);
}

LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
    case WM_CLOSE: DestroyWindow(window); return 0;
    case WM_DESTROY: running = false; PostQuitMessage(0); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_COMMAND:
        if (LOWORD(wparam) == 1 || LOWORD(wparam) == 2) {
            switch_monitor(window, LOWORD(wparam) == 1 ? -1 : 1); return 0;
        }
        break;
    case WM_KEYDOWN:
        if (!(lparam & (1LL << 30))) {
            if (wparam == VK_LEFT || wparam == VK_RIGHT) {
                switch_monitor(window, wparam == VK_LEFT ? -1 : 1); return 0;
            }
            if (wparam == VK_ESCAPE) { PostMessageW(window, WM_CLOSE, 0, 0); return 0; }
            if (wparam == VK_F11) { toggle_fullscreen(window); return 0; }
            if (wparam == VK_SPACE) {
                paused = !paused;
                SetWindowTextW(window, paused ? L"WireView 灰块测试 | 已暂停 | 空格 继续 | F11 全屏 | Esc 退出"
                    : L"WireView 灰块拍照测试 | 120 格依次闪亮 | ←/→ 切屏 | F11 全屏 | 空格 暂停 | Esc 退出");
                return 0;
            }
        }
        break;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

void draw_grid(ID3D11DeviceContext1* context, ID3D11RenderTargetView* view,
               unsigned frame, LONG width, LONG height) {
    constexpr float guide[]{0.24F,0.24F,0.24F,1.F};
    constexpr float gray[]{0.90F,0.90F,0.90F,1.F};
    height = std::max<LONG>(1, height-48);
    const LONG margin = std::max<LONG>(2, std::min(width,height)/50);
    const LONG cell = std::max<LONG>(3, std::min((width-2*margin)/12, (height-2*margin)/10));
    const LONG gap = std::max<LONG>(1, cell/12);
    const LONG stroke = std::max<LONG>(1, cell/64);
    const LONG left = (width-cell*12)/2, top = 48+(height-cell*10)/2;
    D3D11_RECT outlines[480]{}, active{};
    for (unsigned i=0; i<120; ++i) {
        const LONG x = left + static_cast<LONG>(i%12)*cell + gap;
        const LONG y = top + static_cast<LONG>(i/12)*cell + gap;
        const LONG right = x+cell-2*gap, bottom = y+cell-2*gap;
        outlines[4*i] = {x,y,right,y+stroke};
        outlines[4*i+1] = {x,bottom-stroke,right,bottom};
        outlines[4*i+2] = {x,y,x+stroke,bottom};
        outlines[4*i+3] = {right-stroke,y,right,bottom};
        if (i == frame%120) active = {x,y,right,bottom};
    }
    context->ClearView(view, guide, outlines, 480);
    // The previous square is erased every frame: no trail or accumulating marks.
    context->ClearView(view, gray, &active, 1);
}
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    HWND window = nullptr;
    HANDLE ready = nullptr;
    try {
        SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        MONITORINFOEXW monitor{};
        EnumDisplayMonitors(nullptr, nullptr, choose_monitor, reinterpret_cast<LPARAM>(&monitor));
        if (!monitor.cbSize) throw std::runtime_error("No display available");
        WNDCLASSW wc{};
        wc.hInstance = instance;
        wc.lpszClassName = L"WireViewGrayMotion";
        wc.lpfnWndProc = window_proc;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        if (!RegisterClassW(&wc)) throw std::runtime_error("RegisterClassW failed");
        const RECT area = monitor.rcWork;
        window = CreateWindowExW(0, wc.lpszClassName,
            L"WireView 灰块拍照测试 | 120 格依次闪亮 | ←/→ 切屏 | F11 全屏 | 空格 暂停 | Esc 退出",
            WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, area.left+40, area.top+40,
            std::max<LONG>(320, (area.right-area.left)*3/4),
            std::max<LONG>(240, (area.bottom-area.top)*3/4),
            nullptr, nullptr, instance, nullptr);
        if (!window) throw std::runtime_error("CreateWindowExW failed");
        ShowWindow(window, SW_SHOWMAXIMIZED);
        toggle_fullscreen(window);
        const HWND previous = CreateWindowW(L"BUTTON", L"◀ 上一屏", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            12, 8, 120, 32, window, reinterpret_cast<HMENU>(1), instance, nullptr);
        const HWND next = CreateWindowW(L"BUTTON", L"下一屏 ▶", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            144, 8, 170, 32, window, reinterpret_cast<HMENU>(2), instance, nullptr);
        if (!previous || !next) throw std::runtime_error("Monitor buttons could not be created");
        switch_monitor(window, 0);

        ComPtr<ID3D11Device> device;
        ComPtr<ID3D11DeviceContext> context;
        check(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
            D3D11_SDK_VERSION, &device, nullptr, &context), "D3D11CreateDevice");
        ComPtr<ID3D11DeviceContext1> context1;
        check(context.As(&context1), "ID3D11DeviceContext1");
        ComPtr<IDXGIDevice> dxgi;
        check(device.As(&dxgi), "IDXGIDevice");
        ComPtr<IDXGIAdapter> adapter;
        check(dxgi->GetAdapter(&adapter), "GetAdapter");
        ComPtr<IDXGIFactory2> factory;
        check(adapter->GetParent(IID_PPV_ARGS(&factory)), "IDXGIFactory2");
        DXGI_SWAP_CHAIN_DESC1 desc{};
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.BufferCount = 2;
        desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        desc.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
        ComPtr<IDXGISwapChain1> swap;
        check(factory->CreateSwapChainForHwnd(device.Get(), window, &desc, nullptr, nullptr, &swap), "CreateSwapChainForHwnd");
        factory->MakeWindowAssociation(window, DXGI_MWA_NO_ALT_ENTER);
        ComPtr<IDXGISwapChain2> swap2;
        check(swap.As(&swap2), "IDXGISwapChain2");
        check(swap2->SetMaximumFrameLatency(2), "SetMaximumFrameLatency");
        ready = swap2->GetFrameLatencyWaitableObject();
        if (!ready) throw std::runtime_error("No frame-ready handle");
        ComPtr<ID3D11RenderTargetView> view;
        LONG width = 0, height = 0;
        using Pacer = wireview::host::FramePacer;
        Pacer pacer;
        const auto interval = std::chrono::nanoseconds(1'000'000'000LL/120);
        auto next_frame = Pacer::Clock::now();
        unsigned frame = 0;
        constexpr float background[]{0.F,0.F,0.F,1.F};
        while (running) {
            MSG message{};
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&message); DispatchMessageW(&message);
            }
            if (!running) break;
            if (IsIconic(window) || paused) {
                MsgWaitForMultipleObjects(0, nullptr, FALSE, 50, QS_ALLINPUT);
                next_frame = Pacer::Clock::now();
                continue;
            }
            RECT client{};
            GetClientRect(window, &client);
            if (client.right <= 0 || client.bottom <= 0) continue;
            if (width != client.right || height != client.bottom) {
                view.Reset();
                check(swap->ResizeBuffers(0, client.right, client.bottom, DXGI_FORMAT_UNKNOWN, desc.Flags), "ResizeBuffers");
                ComPtr<ID3D11Texture2D> buffer;
                check(swap->GetBuffer(0, IID_PPV_ARGS(&buffer)), "GetBuffer");
                check(device->CreateRenderTargetView(buffer.Get(), nullptr, &view), "CreateRenderTargetView");
                width = client.right; height = client.bottom;
            }
            const DWORD wait = MsgWaitForMultipleObjects(1, &ready, FALSE, 1000, QS_ALLINPUT);
            if (wait == WAIT_OBJECT_0+1 || wait == WAIT_TIMEOUT) continue;
            if (wait != WAIT_OBJECT_0) throw std::runtime_error("Frame wait failed");
            pacer.wait_until(next_frame);
            const auto now = Pacer::Clock::now();
            // Keep an absolute cadence; skip expired slots rather than burst after a stall.
            next_frame += interval;
            if (next_frame <= now) next_frame = now + interval;
            context->ClearRenderTargetView(view.Get(), background);
            draw_grid(context1.Get(), view.Get(), frame, width, height);
            check(swap->Present(0, 0), "Present");
            ++frame;
        }
        CloseHandle(ready);
        return 0;
    } catch (const std::exception& error) {
        if (ready) CloseHandle(ready);
        if (window && IsWindow(window)) DestroyWindow(window);
        MessageBoxA(nullptr, error.what(), "WireView gray test could not start", MB_OK | MB_ICONERROR);
        return 1;
    }
}
