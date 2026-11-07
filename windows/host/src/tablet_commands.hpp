// Tablet-side control commands (protocol packet type 13 Android -> PC) and
// the window-list reply (packet type 14 PC -> Android) that back the Android
// edge sidebar: back (ESC), touch keyboard, orientation rotation request and
// the application switcher. Unknown types stay ignored on older hosts, so the
// additions are backward compatible (see docs/INPUT_PROTOCOL.md).
#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace wireview::host {

// Sub-commands carried by packet type 13.
enum class TabletCommand : std::uint8_t {
    escape = 1,
    touch_keyboard = 2,
    rotate_orientation = 3,
    request_window_list = 4,
    move_window = 5,
};

// Monitor classification shared by the list reply and the move request.
constexpr std::uint8_t kWindowAreaPrimary = 0;
constexpr std::uint8_t kWindowAreaVdd = 1;
constexpr std::uint8_t kWindowAreaOther = 2;

// Posted to the control center ("WireViewControlWindow") to ask for the
// stop-session -> flip orientation -> restart rotation flow.
inline constexpr UINT kControlCenterRotateMessage = WM_APP + 3U;
inline constexpr WPARAM kControlCenterRotateRequest = 0x5746524FU;  // 'WVRO'

struct TabletWindowEntry {
    HWND window{nullptr};
    std::uint8_t area{kWindowAreaOther};
    std::wstring title;
};

// type 13 payload: [u8 command] or [u8 command=5][u32 token][u32 index][u8 area].
bool decode_control_command(
    std::span<const std::uint8_t> payload, TabletCommand& command);
bool decode_window_move(
    std::span<const std::uint8_t> payload,
    std::uint32_t& token,
    std::uint32_t& index,
    std::uint8_t& target_area);

// type 14 payload: [u32 token][u16 count]
// then per entry: [u64 hwnd][u8 area][u16 title_bytes][utf-8 title].
std::vector<std::uint8_t> encode_window_list(
    std::uint32_t token,
    const std::vector<TabletWindowEntry>& entries);

// Send the ESC key through SendInput (INPUT_KEYBOARD).
bool inject_escape_key();

// Launch the Windows touch keyboard (TabTip, fallback osk.exe). Returns false
// when neither could be started.
bool launch_touch_keyboard();

void request_control_center_rotation();

// Visible, non-tool, non-cloaked top-level windows with a title, classified
// by the monitor they sit on. The WireView host windows are excluded.
std::vector<TabletWindowEntry> enumerate_streaming_windows(
    const std::wstring& vdd_device_name);

// Center the window on the monitor identified by target_area (restoring and
// re-maximizing maximized windows). Returns false when the window or the
// target monitor cannot be resolved.
bool move_window_to_area(
    HWND window,
    std::uint8_t target_area,
    const std::wstring& vdd_device_name);

}  // namespace wireview::host
