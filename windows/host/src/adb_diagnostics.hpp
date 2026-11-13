#pragma once

#include <filesystem>
#include <algorithm>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace wireview::host {

// Shared by the session and both control-center UIs. Existing exit codes stay
// compatible; new codes distinguish failures previously reported as no phone.
enum class AdbStatus : unsigned long {
    ready = 0, missing = 2, no_device = 3, unauthorized = 4,
    multiple = 5, app_not_running = 6, incomplete = 7, offline = 8,
    command_failed = 9, unavailable = 10, app_missing = 11,
};

struct AdbDevice {
    std::wstring serial;
    std::wstring state;
    std::wstring model;
};

inline bool valid_adb_serial(std::wstring_view serial) {
    return !serial.empty() && serial.size() <= 256U &&
        std::all_of(serial.begin(), serial.end(), [](wchar_t c) {
            return c > L' ' && c < 127 && c != L'"' && c != L'\\' && c != L'|';
        });
}

inline AdbStatus adb_device_status(std::wstring_view state) {
    if (state == L"device") { return AdbStatus::ready; }
    if (state == L"unauthorized") { return AdbStatus::unauthorized; }
    if (state == L"offline") { return AdbStatus::offline; }
    return AdbStatus::unavailable;
}

struct AdbDeviceList {
    AdbStatus status{AdbStatus::no_device};
    std::vector<std::wstring> ready;
    std::vector<AdbDevice> devices;
};

inline AdbDeviceList parse_adb_devices(const std::wstring& output,
    std::wstring_view selected_serial = {}) {
    AdbDeviceList result;
    std::wistringstream lines(output);
    std::wstring line;
    bool list_started = false;
    while (std::getline(lines, line)) {
        // Plain `devices` uses tabs, while `devices -l` pads the serial with
        // spaces. Both start with this header, after any daemon diagnostics.
        if (line.starts_with(L"List of devices attached")) { list_started = true; continue; }
        if (!list_started) { continue; }
        std::wistringstream fields(line);
        std::wstring serial, state;
        if (!(fields >> serial >> state)) { continue; }
        if (!valid_adb_serial(serial) || state.empty()) { continue; }
        AdbDevice device{serial, state, {}};
        std::wstring field;
        while (fields >> field) {
            if (field.starts_with(L"model:")) { device.model = field.substr(6U); }
        }
        result.devices.push_back(std::move(device));
        if (!selected_serial.empty() && serial != selected_serial) { continue; }
        result.status = adb_device_status(state);
        if (state == L"device") { result.ready.push_back(serial); }
    }
    if (selected_serial.empty() && result.devices.size() > 1U) { result.status = AdbStatus::multiple; }
    return result;
}

inline bool adb_dependencies_complete(const std::filesystem::path& adb) {
    for (const auto* name : {L"AdbWinApi.dll", L"AdbWinUsbApi.dll"}) {
        std::error_code error;
        if (!std::filesystem::is_regular_file(adb.parent_path() / name, error)) {
            return false;
        }
    }
    return true;
}

inline std::wstring adb_status_message(AdbStatus status) {
    switch (status) {
    case AdbStatus::missing:
        return L"未找到 adb.exe。请将 Google 官方完整 platform-tools 文件夹解压到 WireView 同级目录。";
    case AdbStatus::no_device:
        return L"ADB 未枚举到设备。请开启 USB 调试、解锁设备，并检查 Windows 设备管理器中的 Android ADB 接口驱动；再检查 USB 数据连接和端口。部分机型需选择文件传输或开启额外的 USB 调试开关。";
    case AdbStatus::unauthorized:
        return L"ADB 已发现设备，但 USB 调试未授权。请解锁 Android 并允许此电脑的 RSA 指纹；未弹出时可在开发者选项撤销 USB 调试授权后重新插线。";
    case AdbStatus::offline:
        return L"ADB 已发现设备，但状态为 offline（离线）。请解锁设备并重新插线；检查是否有手机助手占用或旧版 ADB 服务冲突。必要时退出其他调试工具后手动重启 ADB 服务，再重新授权。";
    case AdbStatus::multiple:
        return L"ADB 发现多个设备，请在“投屏设备”中选择一台；命令行可用 --serial 指定目标。";
    case AdbStatus::incomplete:
        return L"所选 ADB 缺少 AdbWinApi.dll 或 AdbWinUsbApi.dll。请重新完整解压官方 platform-tools，不能只复制 adb.exe；如文件随后消失，请检查安全软件隔离记录。";
    case AdbStatus::command_failed:
        return L"ADB 命令启动失败、超时或返回错误，尚不能判断 USB 设备状态。请检查 platform-tools 完整性、ADB 服务冲突和系统错误；原始诊断已保留。";
    case AdbStatus::unavailable:
        return L"ADB 设备当前不处于可调试状态（如 recovery、sideload 或驱动权限异常）。请正常启动 Android 并检查设备管理器与 USB 调试设置。";
    case AdbStatus::app_not_running:
        return L"请在手机上打开 WireView，再开始投屏。";
    case AdbStatus::app_missing:
        return L"USB 已连接，手机上未安装 WireView。请先安装 Android APK。";
    default: return L"ADB 设备已连接。";
    }
}
} // namespace wireview::host
