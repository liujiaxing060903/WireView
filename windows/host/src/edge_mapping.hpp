// R2：指针跨屏边缘比例映射（集成 LittleBigMouse 守护进程）。
// 通过命名管道 IPC（协议见 mgth/LittleBigMouse wire-contract）向
// LittleBigMouse.Hook.exe 下发 ZonesLayout 并启停，实现"鼠标从主屏
// 任意位置进入副屏时按物理比例重映射"。进程隔离调用，无 GPL 传染。
#pragma once

#include <string>

namespace wireview::host {

// Hook.exe 的候选路径：优先 WireView 程序目录，其次官方安装目录。
std::string edge_mapping_hook_path();

// 功能总开关对应的运行前置：Hook.exe 存在即视为可用。
bool edge_mapping_available();

// 按当前真实显示器拓扑生成布局并应用到守护进程：
// 找不到 Hook、连不上管道、启动超时或被 RunRefused 都返回 false，
// error 填中文说明；成功返回 true（守护进程保持运行，会话结束时
// 由 edge_mapping_shutdown() 回收）。
// vdd_device_name：WireView 副屏的 "\\.\DISPLAYn"；primary_device_name：
// 主屏设备名。两者都必须当前附加到桌面。
bool edge_mapping_apply(
    const wchar_t* primary_device_name,
    const wchar_t* vdd_device_name,
    std::wstring& error);

// 会话结束：向守护进程发 Quit（钩子随进程退出，鼠标恢复系统默认）；
// 超时则 TerminateProcess 兜底。未运行时为无害空操作。
void edge_mapping_shutdown();

}  // namespace wireview::host
