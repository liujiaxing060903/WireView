#pragma once

// R3 WiFi mode orchestration for the control center, extracted from
// control_main.cpp. Owns the identity file, the whitelist, the mDNS browser,
// the pairing thread, and the session-target bookkeeping; control_main keeps
// only the UI call sites.

#include <Windows.h>

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace wireview::host::wifi_control {

struct DiscoveredDevice {
    std::wstring android_id;
    std::wstring device_name;
    std::wstring ip;
};

struct WhitelistItem {
    std::wstring android_id;
    std::wstring device_name;
    std::wstring paired_at;
    std::wstring last_ip;
};

// Shared message/timer identifiers (WndProc and this module both use them).
constexpr UINT kWifiPairingDoneMessageId = WM_APP + 5U;
constexpr UINT_PTR kWifiDiscoverTimerId = 5;

// UI hooks so this module never depends on control_main internals.
struct UiHooks {
    void (*set_status)(const std::wstring& text) = nullptr;
    void (*publish_web_state)() = nullptr;
    HWND (*window)() = nullptr;
    std::filesystem::path (*session_launcher)() = nullptr;
};

void attach_hooks(const UiHooks& hooks);

// Called once the settings directory is known (control window creation).
void load_identity(const std::filesystem::path& settings_directory);

bool enabled();

// True while a pairing attempt is in flight (for the web UI).
bool pairing_active();

// Toggles the mode: starts/stops the mDNS browser and the discovery timer.
void set_enabled(HWND window, bool on);

// Restores the persisted switch state on startup without re-writing it:
// starts mDNS and the discovery timer when the switch was on.
void resume_enabled(HWND window);

std::wstring pc_uuid();

// Computer name for the WiFi handshake display identity.
std::wstring host_display_name();

// Snapshots for the web UI.
std::vector<DiscoveredDevice> devices();
std::vector<WhitelistItem> whitelist_entries();

// Session target selection and credential resolution for start_session.
void select_target(const std::wstring& android_id);
bool has_session_target();
// Fills the DPAPI-unsealed credential for the selected target; returns false
// (with a user-facing message) when the target is unknown, offline, or the
// credential cannot be unsealed.
bool resolve_session_credential(std::wstring* endpoint, std::vector<std::uint8_t>* key);

void begin_pairing(const std::wstring& ip);
bool remove_from_whitelist(const std::wstring& android_id);

// Timer tick (kWifiDiscoverTimer): refresh the discovery snapshot.
void on_discover_tick();

// kWifiPairingDoneMessage receiver; takes ownership of the lparam bundle.
void on_pairing_done(HWND window, WPARAM wparam, LPARAM lparam);

constexpr std::uint16_t kTabletPort = 39873;

}  // namespace wireview::host::wifi_control
