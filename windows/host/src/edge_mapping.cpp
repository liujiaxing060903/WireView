#include "edge_mapping.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cwchar>
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

// 事件连接的 drain 线程：Listen 之后守护进程会持续向该连接推送事件帧，
// 会话期无人读会打满命名管道内核缓冲、拖住守护进程的 IPC 线程。线程持有
// 句柄的 DuplicateHandle 副本（不能直接复用 g_edge_mapping_pipe 的数值：
// apply 关闭旧连接后紧接着 CreateFileW，新句柄大概率复用刚释放的数值，
// 旧线程的 ReadFile 会读上新命令连接、把握手回复偷走）。副本只由线程
// 自己关闭，UI 线程经 CancelIoEx 唤醒、join 后再清空全局值，无 double-close。
HANDLE g_drain_pipe = nullptr;
HANDLE g_drain_thread = nullptr;

DWORD WINAPI drain_pipe_thread(LPVOID parameter) {
    HANDLE pipe = static_cast<HANDLE>(parameter);
    for (;;) {
        char discard[512];
        DWORD bytes_read = 0U;
        // 丢弃一切事件字节。管道断开（守护进程退出）、原始句柄生命周期
        // 变化或 stop_event_drain 的 CancelIoEx 都会让 ReadFile 失败退出。
        if (ReadFile(pipe, discard, sizeof(discard), &bytes_read, nullptr) == FALSE ||
            bytes_read == 0U) {
            break;
        }
    }
    CloseHandle(pipe);  // 只关自己持有的副本。
    return 0U;
}

void start_event_drain(HANDLE pipe) {
    HANDLE drain_pipe = nullptr;
    if (DuplicateHandle(
            GetCurrentProcess(), pipe, GetCurrentProcess(), &drain_pipe,
            0U, FALSE, DUPLICATE_SAME_ACCESS) == FALSE) {
        return;  // 尽力而为：没有 drain 线程时退回旧行为（只是无人读事件）。
    }
    HANDLE thread = CreateThread(
        nullptr, 0U, drain_pipe_thread, drain_pipe, 0U, nullptr);
    if (thread == nullptr) {
        CloseHandle(drain_pipe);
        return;
    }
    g_drain_pipe = drain_pipe;
    g_drain_thread = thread;
}

void stop_event_drain() {
    HANDLE drain_pipe = g_drain_pipe;
    HANDLE thread = g_drain_thread;
    g_drain_pipe = nullptr;
    g_drain_thread = nullptr;
    if (drain_pipe != nullptr) {
        CancelIoEx(drain_pipe, nullptr);  // 唤醒阻塞中的 ReadFile。
    }
    if (thread != nullptr) {
        // 等线程退出再返回：此刻线程已 CloseHandle 自己的副本，全局里的
        // 旧数值从此不再被引用。超时（理论不可达）时只放弃线程句柄，
        // 不关副本——由线程退出时自行关闭，避免 double-close。
        WaitForSingleObject(thread, 2000U);
        CloseHandle(thread);
    }
}

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
// 注意 PeekNamedPipe 的 lpBytesRead 是"本次调用读到的字节数"而不是累计
// 值，长度前缀与载荷一律用 ReadFile 循环补齐，否则分片到达时会读乱。
bool read_frame(HANDLE pipe, std::string& utf8_payload, DWORD deadline_millis) {
    std::uint32_t length = 0U;
    std::uint32_t have = 0U;
    const auto start = GetTickCount64();
    auto* length_bytes = reinterpret_cast<char*>(&length);
    while (have < sizeof(length)) {
        DWORD available = 0U;
        if (!PeekNamedPipe(pipe, nullptr, 0U, nullptr, &available, nullptr)) {
            return false;
        }
        if (available == 0U) {
            if (GetTickCount64() - start > deadline_millis) {
                return false;
            }
            Sleep(20U);
            continue;
        }
        const DWORD want = sizeof(length) - have;
        DWORD chunk = 0U;
        if (!ReadFile(pipe, length_bytes + have, want, &chunk, nullptr) ||
            chunk == 0U) {
            return false;
        }
        have += chunk;
    }
    if (length == 0U || length > 1024U * 1024U) {
        return false;
    }
    utf8_payload.assign(length, '\0');
    DWORD total = 0U;
    while (total < length) {
        DWORD available = 0U;
        if (!PeekNamedPipe(pipe, nullptr, 0U, nullptr, &available, nullptr)) {
            return false;
        }
        if (available == 0U) {
            if (GetTickCount64() - start > deadline_millis) {
                return false;
            }
            Sleep(20U);
            continue;
        }
        DWORD chunk = 0U;
        if (!ReadFile(pipe, utf8_payload.data() + total, length - total, &chunk,
                nullptr) ||
            chunk == 0U) {
            return false;
        }
        total += chunk;
    }
    return true;
}

// 启动 Hook.exe（standalone + LBM_HOOK_UI=1：不回放 Current.xml，全权 IPC 控制）。
void spawn_hook(const std::string& hook_path_narrow) {
    std::wstring command = L"\"" + widen_utf8(hook_path_narrow) + L"\"";
    // 环境块：以父进程环境为基础（GetEnvironmentStringsW 复制），复制时剔除
    // 旧的 LBM_HOOK_* 项（同名项留下会与末尾追加的新值形成重复定义），再在
    // 块尾追加本次设置。整体替换会丢 SystemRoot/TEMP/PATH 等，.NET 运行时
    // 初始化将直接失败。每项以 \0 结尾，整块以双 \0 结束；块内含内嵌 \0，
    // 只能按长度逐项遍历，不能当普通宽字符串拼接。
    std::vector<wchar_t> environment;
    LPWCH parent_block = GetEnvironmentStringsW();
    if (parent_block != nullptr) {
        const wchar_t* cursor = parent_block;
        while (*cursor != L'\0') {
            const std::size_t entry_length = wcslen(cursor);
            if (wcsncmp(cursor, L"LBM_HOOK_", 9U) != 0) {
                environment.insert(
                    environment.end(), cursor, cursor + entry_length + 1U);
            }
            cursor += entry_length + 1U;
        }
        FreeEnvironmentStringsW(parent_block);
    }
    // 字面量自带两条完整条目与块尾双 \0（含字符串本身的结尾 \0）。
    const wchar_t kOverrides[] = L"LBM_HOOK_UI=1\0LBM_HOOK_DEBUG=\0";
    environment.insert(
        environment.end(), kOverrides,
        kOverrides + sizeof(kOverrides) / sizeof(kOverrides[0]));

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION process{};
    // CREATE_UNICODE_ENVIRONMENT + 继承父环境并覆盖 LBM_HOOK_* 的环境块。
    const BOOL created = CreateProcessW(
        nullptr, command.data(), nullptr, nullptr, FALSE,
        CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT,
        environment.data(), nullptr, &startup, &process);
    if (created == FALSE) {
        return;
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
}

// --- 毫米几何 --------------------------------------------------------------
// Cross 算法（本布局 Algorithm="Cross"）的跨界映射是纯几何的：指针轨迹
// 换算到全局毫米坐标系，落点 = 轨迹与目标屏毫米矩形的交点；ZoneLink 只
// 提供阻力与越界门卫。因此"比例正确"的关键在于两屏共享边在毫米空间里
// 等长对齐：主屏按 golden 4 px/mm，副屏共享轴毫米区间直接取主屏的值。

constexpr double kMillimetersPerPixel = 0.25;  // golden 基准：4 px/mm。

struct MmRect {
    double left;
    double top;
    double right;
    double bottom;
};

MmRect to_mm_uniform(const EdgeMappingRect& rect) {
    return {rect.left * kMillimetersPerPixel, rect.top * kMillimetersPerPixel,
            rect.right * kMillimetersPerPixel, rect.bottom * kMillimetersPerPixel};
}

// 双精度毫米 → XML 数字（守护进程按 invariant culture 解析，去尾零）。
std::wstring mm_text(double value) {
    wchar_t buffer[32]{};
    std::swprintf(buffer, 32U, L"%.3f", value);
    std::wstring text(buffer);
    const std::size_t dot = text.find(L'.');
    if (dot != std::wstring::npos) {
        std::size_t last = text.find_last_not_of(L"0");
        if (last != std::wstring::npos && last > dot) {
            text.erase(last + 1U);
        } else if (last == dot) {
            text.erase(dot);
        }
    }
    if (text == L"-0") {
        text = L"0";
    }
    return text;
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

// 连通边：显式范围（golden 连通段样式）。From/To 为共享边的全局毫米区间；
// 源/目标像素区间各取自己那条边的全长（Strait 算法按此做全边线性映射，
// Cross 算法只用毫米区间）。
std::wstring link_xml(LONG target_id, double from_mm, double to_mm,
    LONG source_length_px, LONG target_length_px) {
    std::wstring xml = L"<ZoneLink From=\"" + mm_text(from_mm) + L"\" To=\"" +
        mm_text(to_mm) + L"\""
        L" SourceFromPixel=\"0\" SourceToPixel=\"" + std::to_wstring(source_length_px) + L"\""
        L" TargetFromPixel=\"0\" TargetToPixel=\"" + std::to_wstring(target_length_px) + L"\""
        L" BorderResistance=\"0\" MoveBlock=\"False\" DragResistance=\"0\""
        L" DragBlock=\"False\" TargetId=\"";
    xml += std::to_wstring(target_id) + L"\"></ZoneLink>";
    return xml;
}

std::wstring zone_xml(int id, const std::wstring& gdi_device_name,
    const EdgeMappingRect& pixels, const MmRect& mm,
    int left_target, int top_target, int right_target, int bottom_target,
    bool shared_axis_vertical, LONG peer_shared_px,
    double shared_from_mm, double shared_to_mm) {
    // golden 里 DeviceId 无 "\\.\\" 前缀（"DISPLAY1" 样式）。
    std::wstring device_name = gdi_device_name;
    const std::wstring prefix = L"\\\\.\\";
    if (device_name.rfind(prefix, 0U) == 0U) {
        device_name = device_name.substr(prefix.size());
    }
    const auto edge = [&](int target) -> std::wstring {
        if (target < 0) {
            return disconnected_link_xml();
        }
        // 连通边即共享边：像素区间取自己这条边的全长。
        const LONG own_shared_px = shared_axis_vertical
            ? pixels.bottom - pixels.top
            : pixels.right - pixels.left;
        return link_xml(target, shared_from_mm, shared_to_mm, own_shared_px, peer_shared_px);
    };
    std::wstring xml = L"<Zone Id=\"" + std::to_wstring(id) + L"\" Name=\"Zone" +
        std::to_wstring(id) + L"\" DeviceId=\"" + device_name + L"\">";
    xml += L"<PixelsBounds><Rect Left=\"" + std::to_wstring(pixels.left) +
        L"\" Top=\"" + std::to_wstring(pixels.top) + L"\" Width=\"" +
        std::to_wstring(pixels.right - pixels.left) + L"\" Height=\"" +
        std::to_wstring(pixels.bottom - pixels.top) +
        L"\"></Rect></PixelsBounds>";
    xml += L"<PhysicalBounds><Rect Left=\"" + mm_text(mm.left) +
        L"\" Top=\"" + mm_text(mm.top) + L"\" Width=\"" +
        mm_text(mm.right - mm.left) + L"\" Height=\"" +
        mm_text(mm.bottom - mm.top) + L"\"></Rect></PhysicalBounds>";
    xml += L"<LeftLinks>" + edge(left_target) + L"</LeftLinks>";
    xml += L"<TopLinks>" + edge(top_target) + L"</TopLinks>";
    xml += L"<RightLinks>" + edge(right_target) + L"</RightLinks>";
    xml += L"<BottomLinks>" + edge(bottom_target) + L"</BottomLinks>";
    xml += L"</Zone>";
    return xml;
}

}  // namespace

std::wstring build_zones_layout_xml(EdgeMappingRect primary, EdgeMappingRect vdd,
    const std::wstring& primary_device_name, const std::wstring& vdd_device_name) {
    // 拼布局：Zone0=主屏，Zone1=副屏，两屏相邻边互写连通 ZoneLink。
    // 根据两屏相对位置判定相邻边（与副屏摆放选择一致）。
    // 相邻 = 一轴分离（含恰好相接）且另一轴区间重叠 > 0：单轴短路会把
    // 对角摆放/仅角接触/完全分离的布局也连起来，指针在物理不相邻的两
    // 屏间"穿越空隙"跳变，零重叠一律返回空串交给调用方报"不邻接"。
    int horizontal = 0;  // -1 副屏在左，+1 副屏在右
    int vertical = 0;    // -1 副屏在上，+1 副屏在下
    const LONG overlap_x =
        std::min(primary.right, vdd.right) - std::max(primary.left, vdd.left);
    const LONG overlap_y =
        std::min(primary.bottom, vdd.bottom) - std::max(primary.top, vdd.top);
    if (vdd.left >= primary.right && overlap_y > 0) {
        horizontal = 1;
    } else if (vdd.right <= primary.left && overlap_y > 0) {
        horizontal = -1;
    } else if (vdd.top >= primary.bottom && overlap_x > 0) {
        vertical = 1;
    } else if (vdd.bottom <= primary.top && overlap_x > 0) {
        vertical = -1;
    } else {
        return {};  // 边界不邻接（含仅角接触与完全分离）
    }

    const MmRect primary_mm = to_mm_uniform(primary);
    MmRect vdd_mm = to_mm_uniform(vdd);
    int primary_left = -1, primary_top = -1, primary_right = -1, primary_bottom = -1;
    int vdd_left = -1, vdd_top = -1, vdd_right = -1, vdd_bottom = -1;
    bool shared_axis_vertical = false;
    double shared_from_mm = 0.0;
    double shared_to_mm = 0.0;
    if (horizontal != 0) {
        // 共享边为竖直边：长度 = 各自像素高度；毫米区间对齐主屏毫米高度。
        if (horizontal > 0) {
            primary_right = 1;
            vdd_left = 0;
        } else {
            primary_left = 1;
            vdd_right = 0;
        }
        shared_axis_vertical = true;
        vdd_mm.top = primary_mm.top;
        vdd_mm.bottom = primary_mm.bottom;
        shared_from_mm = primary_mm.top;
        shared_to_mm = primary_mm.bottom;
    } else {
        // 共享边为水平边：长度 = 各自像素宽度；毫米区间对齐主屏毫米宽度。
        if (vertical > 0) {
            primary_bottom = 1;
            vdd_top = 0;
        } else {
            primary_top = 1;
            vdd_bottom = 0;
        }
        vdd_mm.left = primary_mm.left;
        vdd_mm.right = primary_mm.right;
        shared_from_mm = primary_mm.left;
        shared_to_mm = primary_mm.right;
    }

    std::wstring layout = L"<ZonesLayout AdjustPointer=\"True\" AdjustSpeed=\"False\""
        L" LoopX=\"False\" LoopY=\"False\" Virtual=\"False\""
        L" RescueShortcut=\"Ctrl+Alt+Shift+M\" Priority=\"High\""
        L" PriorityUnhooked=\"Idle\" Algorithm=\"Cross\" MaxTravelDistance=\"150\""
        L" FreelookCheckInterval=\"100\" FreelookEnabled=\"True\"><MainZones>";
    const LONG primary_shared_px = shared_axis_vertical
        ? primary.bottom - primary.top
        : primary.right - primary.left;
    const LONG vdd_shared_px = shared_axis_vertical
        ? vdd.bottom - vdd.top
        : vdd.right - vdd.left;
    layout += zone_xml(0, primary_device_name, primary, primary_mm,
        primary_left, primary_top, primary_right, primary_bottom,
        shared_axis_vertical, vdd_shared_px, shared_from_mm, shared_to_mm);
    layout += zone_xml(1, vdd_device_name, vdd, vdd_mm,
        vdd_left, vdd_top, vdd_right, vdd_bottom,
        shared_axis_vertical, primary_shared_px, shared_from_mm, shared_to_mm);
    layout += L"</MainZones></ZonesLayout>";
    return layout;
}

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

    const EdgeMappingRect primary_pixels{
        primary_rect->left, primary_rect->top, primary_rect->right, primary_rect->bottom};
    const EdgeMappingRect vdd_pixels{
        vdd_rect->left, vdd_rect->top, vdd_rect->right, vdd_rect->bottom};
    std::wstring layout = build_zones_layout_xml(
        primary_pixels, vdd_pixels, primary_device_name, vdd_device_name);
    if (layout.empty()) {
        error = L"副屏与主屏边界不邻接，无法建立指针映射";
        return false;
    }

    // 重复 apply（拓扑变化重挂）时释放上一条事件连接（先停 drain 线程，
    // 再关原始句柄），避免句柄泄漏与句柄数值复用竞态。
    stop_event_drain();
    if (g_edge_mapping_pipe != nullptr) {
        CancelIo(g_edge_mapping_pipe);
        CloseHandle(g_edge_mapping_pipe);
        g_edge_mapping_pipe = nullptr;
    }

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

    // 致命协议细节（探针实测 v5.6.0 官方 Hook.exe）：客户端发出 Listen 后，
    // 守护进程立即把该连接切到"纯事件接收"模式——同一连接上后续任何命令
    // 帧会被读进即丢弃，连接随即被拆除，布局从未被加载（apply 永远超时）。
    // 因此 Listen 必须与 Load/Run 打包在同一个帧里；成功后本连接只读事件。
    const std::string batched = narrow_utf8(
        L"<Messages>"
        L"<CommandMessage Command=\"Listen\" Payload=\"\"/>"
        L"<CommandMessage Command=\"Load\"><Payload>" + layout + L"</Payload></CommandMessage>"
        L"<CommandMessage Command=\"Run\" Payload=\"\"/>"
        L"</Messages>");
    if (!write_frame(pipe, batched)) {
        CloseHandle(pipe);
        error = L"向守护进程发送布局失败";
        return false;
    }
    bool running = false;
    std::wstring failure;
    for (int received = 0; received < 8 && !running && failure.empty(); ++received) {
        std::string reply;
        if (!read_frame(pipe, reply, 1500U)) {
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
    // 成功：连接保留（作为事件监听者；drain 线程持续丢弃事件帧，防止
    // 打满管道内核缓冲），会话结束时 Quit。
    g_edge_mapping_pipe = pipe;
    start_event_drain(pipe);
    return true;
}

void edge_mapping_shutdown() {
    // 事件模式连接上的命令会被守护进程丢弃，Quit 必须走一条全新的
    // 命令模式连接（探针实测）。旧的事件连接先显式关闭：控制中心长期
    // 存活，置空不关会让每次停止会话都积一个内核句柄。先停 drain 线程
    // 再关原始句柄，两步顺序与 apply 的重挂路径一致。
    stop_event_drain();
    HANDLE stale = g_edge_mapping_pipe;
    g_edge_mapping_pipe = nullptr;
    if (stale != nullptr) {
        CancelIo(stale);
        CloseHandle(stale);
    }
    HANDLE pipe = try_open_pipe();
    if (pipe == nullptr) {
        return;
    }
    (void)write_frame(pipe, narrow_utf8(L"<CommandMessage Command=\"Quit\" Payload=\"\"/>"));
    // Hook 收到 Quit 后自行退出；不等待也不强杀（钩子随进程消失）。
    Sleep(150U);
    CancelIo(pipe);
    CloseHandle(pipe);
}

}  // namespace wireview::host
