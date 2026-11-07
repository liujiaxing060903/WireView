#include "edge_mapping.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <algorithm>
#include <chrono>
#include <string>
#include <vector>

namespace wireview::host {
namespace {

// 与 rust/crates/lbm-ipc/src/endpoint.rs 逐字对齐：每个登录会话一条管道。
std::wstring pipe_path_for_current_session() {
    DWORD session_id = 0U;
    if (ProcessIdToSessionId(GetCurrentProcessId(), &session_id) == FALSE) {
        session_id = 1U;
    }
    return L"\\\\.\\pipe\\LittleBigMouse-v1-session-" + std::to_wstring(session_id);
}

// 会话期间保持的守护进程管道连接（apply 成功后持有，shutdown 消费）。
HANDLE g_edge_mapping_pipe = nullptr;

std::string narrow_utf8(const std::wstring& text) {
    if (text.empty()) {
        return {};
    }
    const int size = WideCharToMultiByte(
        CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
        nullptr, 0, nullptr, nullptr);
    std::string result(static_cast<std::size_t>(size > 0 ? size : 0), '\0');
    if (size > 0) {
        WideCharToMultiByte(
            CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
            result.data(), size, nullptr, nullptr);
    }
    return result;
}

std::wstring widen_utf8(const std::string& text) {
    if (text.empty()) {
        return {};
    }
    const int size = MultiByteToWideChar(
        CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring result(static_cast<std::size_t>(size > 0 ? size : 0), L'\0');
    if (size > 0) {
        MultiByteToWideChar(
            CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), size);
    }
    return result;
}

struct MonitorRect {
    std::wstring device_name;
    RECT rect{};
};

// 当前桌面全部显示器的设备名与矩形（虚拟桌面坐标）。
std::vector<MonitorRect> enumerate_monitor_rects() {
    std::vector<MonitorRect> result;
    EnumDisplayMonitors(
        nullptr, nullptr,
        [](HMONITOR monitor, HDC, LPRECT, LPARAM lparam) -> BOOL {
            MONITORINFOEXW info{};
            info.cbSize = sizeof(info);
            if (GetMonitorInfoW(monitor, &info) != FALSE) {
                auto* monitors = reinterpret_cast<std::vector<MonitorRect>*>(lparam);
                monitors->push_back({info.szDevice, info.rcMonitor});
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&result));
    return result;
}

// 打开会话管道；不存在时返回 nullptr。
HANDLE try_open_pipe() {
    const std::wstring path = pipe_path_for_current_session();
    HANDLE pipe = CreateFileW(
        path.c_str(), GENERIC_READ | GENERIC_WRITE, 0U, nullptr,
        OPEN_EXISTING, 0U, nullptr);
    if (pipe == INVALID_HANDLE_VALUE) {
        return nullptr;
    }
    return pipe;
}

bool write_frame(HANDLE pipe, const std::string& utf8_payload) {
    const std::uint32_t length = static_cast<std::uint32_t>(utf8_payload.size());
    DWORD written = 0U;
    if (WriteFile(pipe, &length, sizeof(length), &written, nullptr) == FALSE ||
        written != sizeof(length)) {
        return false;
    }
    if (!utf8_payload.empty()) {
        if (WriteFile(pipe, utf8_payload.data(), length, &written, nullptr) == FALSE ||
            written != length) {
            return false;
        }
    }
    return FlushFileBuffers(pipe) != FALSE;
}

// 读取一帧（u32 小端长度 + UTF-8）；deadline 毫秒内拿不到返回 false。
bool read_frame(HANDLE pipe, std::string& utf8_payload, DWORD deadline_millis) {
    std::uint32_t length = 0U;
    DWORD read_bytes = 0U;
    const auto start = GetTickCount64();
    while (read_bytes < sizeof(length)) {
        if (!PeekNamedPipe(pipe, reinterpret_cast<LPBYTE>(&length) + read_bytes,
                sizeof(length) - read_bytes, &read_bytes, nullptr, nullptr)) {
            return false;
        }
        if (GetTickCount64() - start > deadline_millis) {
            return false;
        }
        if (read_bytes < sizeof(length)) {
            Sleep(25U);
        }
    }
    if (length == 0U || length > 1024U * 1024U) {
        return false;
    }
    utf8_payload.assign(length, '\0');
    read_bytes = 0U;
    while (read_bytes < length) {
        DWORD chunk = 0U;
        if (!PeekNamedPipe(pipe, utf8_payload.data() + read_bytes,
                length - read_bytes, &chunk, nullptr, nullptr)) {
            return false;
        }
        read_bytes += chunk;
        if (read_bytes < length) {
            if (GetTickCount64() - start > deadline_millis) {
                return false;
            }
            Sleep(25U);
        }
    }
    return true;
}

// 启动 Hook.exe（standalone + LBM_HOOK_UI=1：不回放 Current.xml，全权 IPC 控制）。
void spawn_hook(const std::string& hook_path_narrow) {
    std::wstring command = L"\"" + widen_utf8(hook_path_narrow) + L"\"";
    // 环境块：每项以 \0 结尾，整块以双 \0 结束。注意 operator+=(const
    // wchar_t*) 会在内嵌 \0 处截断，必须用 push_back 追加 null。
    std::wstring environment;
    environment += L"LBM_HOOK_UI=1";
    environment.push_back(L'\0');
    environment += L"LBM_HOOK_DEBUG=";
    environment.push_back(L'\0');
    environment.push_back(L'\0');

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION process{};
    // CREATE_UNICODE_ENVIRONMENT + 分离的只读环境块。
    std::vector<wchar_t> environment_block(environment.begin(), environment.end());
    const BOOL created = CreateProcessW(
        nullptr, command.data(), nullptr, nullptr, FALSE,
        CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT,
        environment_block.data(), nullptr, &startup, &process);
    if (created == FALSE) {
        return;
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
}

std::wstring rect_to_mm_attributes(const RECT& rect) {
    // golden 基准：1920x1080 -> 480x270（4 px/mm）。虚拟桌面坐标按同一
    // 比例缩放，保证两屏边界的 mm 对齐关系与像素一致。
    const auto to_mm = [](LONG value) -> std::wstring {
        return std::to_wstring(static_cast<long long>(value) * 1000LL / 4000LL);
    };
    return L"Left=\"" + to_mm(rect.left) + L"\" Top=\"" + to_mm(rect.top) +
        L"\" Width=\"" + to_mm(rect.right - rect.left) + L"\" Height=\"" +
        to_mm(rect.bottom - rect.top) + L"\"";
}

std::wstring disconnected_link_xml() {
    // 断开边：无限大范围（golden 中 TargetId=-1 的样式）。
    return L"<ZoneLink From=\"-1.7976931348623157E+308\""
        L" To=\"1.7976931348623157E+308\""
        L" SourceFromPixel=\"-2147483648\" SourceToPixel=\"2147483647\""
        L" TargetFromPixel=\"-2147483648\" TargetToPixel=\"2147483647\""
        L" BorderResistance=\"0\" MoveBlock=\"False\" DragResistance=\"0\""
        L" DragBlock=\"False\" TargetId=\"-1\"></ZoneLink>";
}

// 连通边：显式范围（golden 连通段样式）。两端比例区间决定跨屏时的
// 位置映射；长度不同（如 1600 vs 1920）按全边对全边线性映射。
std::wstring link_xml(LONG target_id, LONG source_length_px, LONG target_length_px) {
    const auto to_mm = [](LONG px) -> std::wstring {
        return std::to_wstring(static_cast<long long>(px) * 1000LL / 4000LL);
    };
    std::wstring xml = L"<ZoneLink From=\"0\" To=\"" + to_mm(source_length_px) + L"\""
        L" SourceFromPixel=\"0\" SourceToPixel=\"" + std::to_wstring(source_length_px) + L"\""
        L" TargetFromPixel=\"0\" TargetToPixel=\"" + std::to_wstring(target_length_px) + L"\""
        L" BorderResistance=\"0\" MoveBlock=\"False\" DragResistance=\"0\""
        L" DragBlock=\"False\" TargetId=\"";
    xml += std::to_wstring(target_id) + L"\"></ZoneLink>";
    return xml;
}

std::wstring zone_xml(int id, const std::wstring& gdi_device_name, const RECT& rect,
    int left_target, int top_target, int right_target, int bottom_target,
    LONG peer_length_px) {
    // golden 里 DeviceId 无 "\\.\\" 前缀（"DISPLAY1" 样式）。
    std::wstring device_name = gdi_device_name;
    const std::wstring prefix = L"\\\\.\\";
    if (device_name.rfind(prefix, 0U) == 0U) {
        device_name = device_name.substr(prefix.size());
    }
    const LONG width = rect.right - rect.left;
    const LONG height = rect.bottom - rect.top;
    // 相邻边：垂直邻接时共享边是高度，水平邻接时是宽度；对端长度用于
    // Target 像素区间（高度不等时线性映射）。-1 表示该边不连通。
    const auto edge = [&](int target, bool horizontal_edge) -> std::wstring {
        if (target < 0) {
            return disconnected_link_xml();
        }
        const LONG own_length = horizontal_edge ? width : height;
        return link_xml(target, own_length, peer_length_px);
    };
    std::wstring xml = L"<Zone Id=\"" + std::to_wstring(id) + L"\" Name=\"Zone" +
        std::to_wstring(id) + L"\" DeviceId=\"" + device_name + L"\">";
    xml += L"<PixelsBounds><Rect Left=\"" + std::to_wstring(rect.left) +
        L"\" Top=\"" + std::to_wstring(rect.top) + L"\" Width=\"" +
        std::to_wstring(width) + L"\" Height=\"" + std::to_wstring(height) +
        L"\"></Rect></PixelsBounds>";
    xml += L"<PhysicalBounds><Rect " + rect_to_mm_attributes(rect) +
        L"></Rect></PhysicalBounds>";
    xml += L"<LeftLinks>" + edge(left_target, false) + L"</LeftLinks>";
    xml += L"<TopLinks>" + edge(top_target, true) + L"</TopLinks>";
    xml += L"<RightLinks>" + edge(right_target, false) + L"</RightLinks>";
    xml += L"<BottomLinks>" + edge(bottom_target, true) + L"</BottomLinks>";
    xml += L"</Zone>";
    return xml;
}

}  // namespace

std::string edge_mapping_hook_path() {
    char module_path[MAX_PATH]{};
    const DWORD length = GetModuleFileNameA(nullptr, module_path, MAX_PATH);
    if (length != 0U) {
        std::string local(module_path, length);
        const std::size_t slash = local.find_last_of("\\/");
        if (slash != std::string::npos) {
            local = local.substr(0U, slash + 1U);
        } else {
            local.clear();
        }
        local += "LittleBigMouse.Hook.exe";
        if (GetFileAttributesA(local.c_str()) != INVALID_FILE_ATTRIBUTES) {
            return local;
        }
    }
    // 官方安装目录（本机已安装时直接复用）。
    char program_files[MAX_PATH]{};
    if (GetEnvironmentVariableA("ProgramFiles", program_files, MAX_PATH) != 0U) {
        std::string installed = std::string(program_files) + "\\LittleBigMouse\\LittleBigMouse.Hook.exe";
        if (GetFileAttributesA(installed.c_str()) != INVALID_FILE_ATTRIBUTES) {
            return installed;
        }
    }
    return {};
}

bool edge_mapping_available() {
    return !edge_mapping_hook_path().empty();
}

bool edge_mapping_apply(
    const wchar_t* primary_device_name,
    const wchar_t* vdd_device_name,
    std::wstring& error) {
    if (primary_device_name == nullptr || vdd_device_name == nullptr) {
        error = L"显示器参数为空";
        return false;
    }
    const std::string hook_path = edge_mapping_hook_path();
    if (hook_path.empty()) {
        error = L"未找到 LittleBigMouse.Hook.exe（请安装 LittleBigMouse，或将其复制到 WireView 目录）";
        return false;
    }

    const auto monitors = enumerate_monitor_rects();
    const RECT* primary_rect = nullptr;
    const RECT* vdd_rect = nullptr;
    for (const auto& monitor : monitors) {
        if (_wcsicmp(monitor.device_name.c_str(), primary_device_name) == 0) {
            primary_rect = &monitor.rect;
        }
        if (_wcsicmp(monitor.device_name.c_str(), vdd_device_name) == 0) {
            vdd_rect = &monitor.rect;
        }
    }
    if (primary_rect == nullptr || vdd_rect == nullptr) {
        error = L"主屏或副屏当前不在桌面上，无法生成指针映射布局";
        return false;
    }

    // 拼布局：Zone0=主屏，Zone1=副屏，两屏相邻边互写连通 ZoneLink。
    // 根据两屏相对位置判定相邻边（与副屏摆放选择一致）。
    int horizontal = 0;  // -1 副屏在左，+1 副屏在右
    int vertical = 0;    // -1 副屏在上，+1 副屏在下
    if (vdd_rect->left >= primary_rect->right) {
        horizontal = 1;
    } else if (vdd_rect->right <= primary_rect->left) {
        horizontal = -1;
    } else if (vdd_rect->top >= primary_rect->bottom) {
        vertical = 1;
    } else if (vdd_rect->bottom <= primary_rect->top) {
        vertical = -1;
    } else {
        error = L"副屏与主屏边界不邻接，无法建立指针映射";
        return false;
    }
    int primary_left = -1, primary_top = -1, primary_right = -1, primary_bottom = -1;
    int vdd_left = -1, vdd_top = -1, vdd_right = -1, vdd_bottom = -1;
    if (horizontal == 1) {
        primary_right = 1;
        vdd_left = 0;
    } else if (horizontal == -1) {
        primary_left = 1;
        vdd_right = 0;
    } else if (vertical == 1) {
        primary_bottom = 1;
        vdd_top = 0;
    } else {
        primary_top = 1;
        vdd_bottom = 0;
    }

    std::wstring layout = L"<ZonesLayout AdjustPointer=\"True\" AdjustSpeed=\"False\""
        L" LoopX=\"False\" LoopY=\"False\" Virtual=\"False\""
        L" RescueShortcut=\"Ctrl+Alt+Shift+M\" Priority=\"High\""
        L" PriorityUnhooked=\"Idle\" Algorithm=\"Cross\" MaxTravelDistance=\"150\""
        L" FreelookCheckInterval=\"100\" FreelookEnabled=\"True\"><MainZones>";
    const LONG primary_height = primary_rect->bottom - primary_rect->top;
    const LONG primary_width = primary_rect->right - primary_rect->left;
    const LONG vdd_height = vdd_rect->bottom - vdd_rect->top;
    const LONG vdd_width = vdd_rect->right - vdd_rect->left;
    // 垂直邻接共享高度（对端=对侧屏高度），水平邻接共享宽度。
    const bool vertical_adjacency = primary_left != -1 || primary_right != -1;
    const LONG main_peer = vertical_adjacency ? vdd_height : vdd_width;
    const LONG vdd_peer = vertical_adjacency ? primary_height : primary_width;
    layout += zone_xml(0, primary_device_name, *primary_rect,
        primary_left, primary_top, primary_right, primary_bottom, main_peer);
    layout += zone_xml(1, vdd_device_name, *vdd_rect,
        vdd_left, vdd_top, vdd_right, vdd_bottom, vdd_peer);
    layout += L"</MainZones></ZonesLayout>";

    const std::wstring commands = L"<Messages>"
        L"<CommandMessage Command=\"Load\"><Payload>" + layout + L"</Payload></CommandMessage>"
        L"<CommandMessage Command=\"Run\" Payload=\"\"/>"
        L"</Messages>";

    // 优先复用已运行的守护进程（单实例：第二个实例会 panic 退出）。
    HANDLE pipe = try_open_pipe();
    if (pipe == nullptr) {
        spawn_hook(hook_path);
        for (int attempt = 0; attempt < 30 && pipe == nullptr; ++attempt) {
            Sleep(100U);
            pipe = try_open_pipe();
        }
    }
    if (pipe == nullptr) {
        error = L"无法连接 LittleBigMouse 守护进程管道";
        return false;
    }

    // Listen 放在 Load/Run 之前：本连接成为事件接收者，LoadFailed/
    // RunRefused/Running 都会推回同一连接——State 只反映"是否挂钩"，
    // 无法发现 Load 被拒后 Run 继续跑旧布局的情况（验证必须听事件）。
    const std::string batched = narrow_utf8(
        L"<Messages>"
        L"<CommandMessage Command=\"Listen\" Payload=\"\"/>"
        L"</Messages>");
    if (!write_frame(pipe, batched)) {
        CloseHandle(pipe);
        error = L"向守护进程注册事件监听失败";
        return false;
    }
    if (!write_frame(pipe, narrow_utf8(commands))) {
        CloseHandle(pipe);
        error = L"向守护进程发送布局失败";
        return false;
    }
    bool running = false;
    std::wstring failure;
    for (int received = 0; received < 6 && !running && failure.empty(); ++received) {
        std::string reply;
        if (!read_frame(pipe, reply, 2000U)) {
            break;
        }
        const std::wstring reply_text = widen_utf8(reply);
        if (reply_text.find(L"Running") != std::wstring::npos) {
            running = true;
        } else if (reply_text.find(L"LoadFailed") != std::wstring::npos ||
                   reply_text.find(L"RunRefused") != std::wstring::npos) {
            failure = reply_text;
        }
    }
    if (!running) {
        CloseHandle(pipe);
        error = failure.empty()
            ? L"守护进程未在超时内确认布局"
            : L"守护进程拒绝布局：" + failure;
        return false;
    }
    // 成功：连接保留（作为事件监听者），会话结束时 Quit。
    g_edge_mapping_pipe = pipe;
    return true;
}

void edge_mapping_shutdown() {
    HANDLE pipe = g_edge_mapping_pipe;
    g_edge_mapping_pipe = nullptr;
    if (pipe == nullptr) {
        pipe = try_open_pipe();
        if (pipe == nullptr) {
            return;
        }
    }
    (void)write_frame(pipe, narrow_utf8(L"<CommandMessage Command=\"Quit\" Payload=\"\"/>"));
    // Hook 收到 Quit 后自行退出；不等待也不强杀（钩子随进程消失）。
    Sleep(150U);
    CancelIo(pipe);
    CloseHandle(pipe);
}

}  // namespace wireview::host
