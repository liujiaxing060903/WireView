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
    // 2 was the Windows touch keyboard; retired in favor of the Android IME
    // bridge (text arrives via ime_text / ime_key).
    rotate_orientation = 3,
    request_window_list = 4,
    move_window = 5,
    // Text committed by the Android IME: [6][u16 utf8 bytes][utf-8 text].
    ime_text = 6,
    // Special key from the Android IME bar: [7][u8 vk]. VK codes only
    // (VK_BACK, VK_RETURN, VK_TAB, arrows); printable text uses ime_text.
    ime_key = 7,
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

// type 13 ime_text payload: [6][u16 utf8 bytes][utf-8 text].
bool decode_ime_text(std::span<const std::uint8_t> payload, std::wstring& text);

// type 13 ime_key payload: [7][u8 vk]; only editing keys are accepted.
bool decode_ime_key(std::span<const std::uint8_t> payload, UINT& virtual_key);

// type 14 payload: [u32 token][u16 count]
// then per entry: [u64 hwnd][u8 area][u16 title_bytes][utf-8 title].
std::vector<std::uint8_t> encode_window_list(
    std::uint32_t token,
    const std::vector<TabletWindowEntry>& entries);

// Send the ESC key through SendInput (INPUT_KEYBOARD).
bool inject_escape_key();

// Inject UTF-16 text through VK_PACKET / KEYEVENTF_UNICODE, bypassing the
// PC-side IME entirely (the Android IME already did the composition).
bool inject_unicode_text(const std::wstring& text);

// Press and release a virtual key (VK_BACK, VK_RETURN, ...).
bool inject_virtual_key(UINT virtual_key);

void request_control_center_rotation();

// Visible, non-tool, non-cloaked top-level windows with a title, classified
// by the monitor they sit on. The WireView host windows are excluded.
std::vector<TabletWindowEntry> enumerate_streaming_windows(
    const std::wstring& vdd_device_name);

// Center the window on the monitor identified by target_area (restoring and
// re-maximizing maximized windows). When the window already sits on the
// requested monitor, it is sent to the other main monitor instead; the
// tablet list may be stale, and the user's intent is always "switch sides".
// Returns false when the window or the target monitor cannot be resolved.
bool move_window_to_area(
    HWND window,
    std::uint8_t target_area,
    const std::wstring& vdd_device_name);

}  // namespace wireview::host
