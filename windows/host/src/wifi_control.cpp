// WinSock2 must precede any header that pulls in <Windows.h> (the project
// hpp files include it for HWND).
#include <WinSock2.h>
#include <WS2tcpip.h>

#include "wifi_control.hpp"

#include "mdns_browser.hpp"
#include "socket_stream.hpp"
#include "wifi_crypto.hpp"
#include "wifi_link.hpp"
#include "wifi_whitelist.hpp"
#include "wireview/auth_messages.hpp"
#include "wireview/protocol.hpp"

#include <atomic>
#include <cassert>
#include <chrono>
#include <fstream>
#include <mutex>
#include <thread>
#include <utility>

namespace wireview::host::wifi_control {
namespace {

// WinSock2 must be initialized before any socket work; the pairing thread
// creates its own session per attempt.
std::mutex g_mutex;
std::atomic_bool g_pairing_active{false};
// Cooperative cancel request for the in-flight pairing worker (see
// cancel_pairing): the worker checks it only after the handshake returns
// or throws, so no socket or thread lifetime is disturbed.
std::atomic_bool g_pairing_cancel{false};

std::mutex& impl_mutex() {
    return g_mutex;
}
bool g_enabled = false;
std::wstring g_pc_uuid;
std::filesystem::path g_settings_directory;
UiHooks g_hooks;
MdnsBrowser* g_browser = nullptr;

// Whitelist singleton, constructed explicitly by load_identity() under
// g_mutex. It must NOT be a function-local static initialised from
// g_settings_directory: any call before load_identity() would latch an
// empty path forever (audit A6 fix design 2) and make the first save()
// write a stray file into the working directory.
WifiWhitelist* g_whitelist = nullptr;

WifiWhitelist& whitelist() {
    if (g_whitelist == nullptr) {
        // Before load_identity(): hand out an empty-path object. Its load()
        // is a no-op and its save() fails with an error instead of writing
        // a dirty relative path.
        assert(false && "whitelist() used before load_identity()");
        static WifiWhitelist fallback{std::wstring{}};
        return fallback;
    }
    return *g_whitelist;
}

MdnsBrowser& browser() {
    if (g_browser == nullptr) {
        g_browser = new MdnsBrowser();
    }
    return *g_browser;
}

std::filesystem::path identity_file() {
    return g_settings_directory / L"wifi-identity.ini";
}

void save_identity_locked() {
    if (g_settings_directory.empty()) {
        // No settings directory yet: never write a relative path into the
        // working directory.
        return;
    }
    std::error_code error;
    std::filesystem::create_directories(g_settings_directory, error);
    // Atomic replace (audit A6 claim 5): write a sibling temp file, then
    // rename it over the real one. Truncating the identity file in place
    // could lose the pc_uuid on a crash mid-write, which silently
    // invalidates every tablet-side credential.
    const std::filesystem::path target = identity_file();
    std::filesystem::path temp_path = target;
    temp_path += L".tmp";
    bool written = false;
    {
        std::ofstream output(temp_path, std::ios::trunc);
        if (output) {
            output << "wifi_pc_uuid=";
            for (const wchar_t character : g_pc_uuid) {
                output << static_cast<char>(character);
            }
            output << "\nwifi_enabled=" << (g_enabled ? 1 : 0) << '\n';
            output.flush();
            written = output.good();
        }
    }
    if (!written) {
        (void)DeleteFileW(temp_path.c_str());
        return;
    }
    if (!MoveFileExW(
            temp_path.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        (void)DeleteFileW(temp_path.c_str());
    }
}

std::string utf8_string(const std::wstring& value) {
    std::string text;
    text.reserve(value.size());
    for (const wchar_t character : value) {
        text.push_back(static_cast<char>(character));
    }
    return text;
}

std::string uuid_hex() {
    std::string text;
    for (const wchar_t character : g_pc_uuid) {
        text.push_back(static_cast<char>(character));
    }
    return text;
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
    MultiByteToWideChar(
        CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
        result.data(), length);
    return result;
}

std::wstring ascii_to_wide(std::string_view text) {
    return std::wstring(text.begin(), text.end());
}

// Session target (selected tablet): android id + last known IP.
std::wstring g_selected_id;
std::wstring g_selected_ip;
std::wstring g_selected_name;

void notify_status(const std::wstring& text) {
    if (g_hooks.set_status != nullptr) {
        g_hooks.set_status(text);
    }
}

void notify_publish() {
    if (g_hooks.publish_web_state != nullptr) {
        g_hooks.publish_web_state();
    }
}

// mDNS responses arrive as inbound UDP; Windows blocks them for unsigned
// apps unless a rule exists. Idempotent: query first, add only when
// missing. Without admin rights the add fails and discovery silently
// falls back to manual IP entry.
void ensure_mdns_firewall_rule() {
    const wchar_t* rule_name = L"WireView mDNS discovery";
    std::wstring query = L"netsh advfirewall firewall show rule name=\"" +
        std::wstring(rule_name) + L"\">nul 2>&1";
    std::vector<wchar_t> query_buffer(query.begin(), query.end());
    query_buffer.push_back(wchar_t(0));
    if (_wsystem(query_buffer.data()) == 0) {
        return;
    }
    std::wstring add =
        L"netsh advfirewall firewall add rule name=\"" + std::wstring(rule_name) +
        L"\" dir=in action=allow protocol=UDP localport=5353 profile=any>nul 2>&1";
    std::vector<wchar_t> add_buffer(add.begin(), add.end());
    add_buffer.push_back(wchar_t(0));
    (void)_wsystem(add_buffer.data());
}

// PIN over USB: the cable is the out-of-band channel, so first-time pairing
// needs no manual typing when the tablet is attached. Fire-and-forget.
void notify_pairing_approved_over_usb(const std::string& pin) {
    if (g_hooks.session_launcher == nullptr) {
        return;
    }
    const auto launcher = g_hooks.session_launcher();
    if (launcher.empty()) {
        return;
    }
    std::wstring command = L"\"" + launcher.wstring() +
        L"\" --notify-pairing-approved \"" + ascii_to_wide(pin) + L"\"";
    std::vector<wchar_t> buffer(command.begin(), command.end());
    buffer.push_back(wchar_t(0));
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (CreateProcessW(
            nullptr,
            buffer.data(),
            nullptr,
            nullptr,
            FALSE,
            CREATE_NO_WINDOW,
            nullptr,
            nullptr,
            &startup,
            &process)) {
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
    }
}

}  // namespace

void attach_hooks(const UiHooks& hooks) {
    g_hooks = hooks;
}

void load_identity(const std::filesystem::path& settings_directory) {
    std::wstring auto_selected;
    {
        std::lock_guard lock(g_mutex);
        g_settings_directory = settings_directory;
        std::ifstream input(identity_file());
        if (input) {
            std::string line;
            while (std::getline(input, line)) {
                if (line.rfind("wifi_pc_uuid=", 0U) == 0U) {
                    const std::string value = line.substr(13U);
                    g_pc_uuid.assign(value.begin(), value.end());
                } else if (line == "wifi_enabled=1") {
                    g_enabled = true;
                }
            }
        }
        if (g_pc_uuid.empty()) {
            const std::vector<std::uint8_t> bytes = random_bytes(16U);
            g_pc_uuid = ascii_to_wide(to_hex(bytes));
            save_identity_locked();
        }
        // Explicit path-driven whitelist construction (audit A6 fix design
        // 2): the path always comes from this initialisation instead of a
        // first-call static capture of g_settings_directory. Load
        // unconditionally; a missing file just yields an empty list.
        g_whitelist = new WifiWhitelist(
            (g_settings_directory / L"wifi-whitelist.ini").wstring());
        g_whitelist->load();
        // One whitelisted tablet means one obvious session target: selecting
        // it here spares the user a trip to the full-page whitelist card.
        // Multiple devices keep the explicit selection. The entries read
        // stays inside g_mutex (audit A6 claim 4); select_target takes the
        // same mutex, so it runs after this scope ends.
        if (g_whitelist->entries().size() == 1U) {
            auto_selected = ascii_to_wide(
                g_whitelist->entries().front().android_id);
        }
    }
    if (!auto_selected.empty()) {
        select_target(auto_selected);
    }
}

bool enabled() {
    std::lock_guard lock(g_mutex);
    return g_enabled;
}

bool pairing_active() {
    return g_pairing_active.load();
}

void set_enabled(HWND window, bool on) {
    {
        std::lock_guard lock(g_mutex);
        if (g_enabled == on) {
            return;
        }
        g_enabled = on;
        save_identity_locked();
    }
    if (on) {
        ensure_mdns_firewall_rule();
        if (!browser().start()) {
            notify_status(L"WiFi 投屏已开启；mDNS 自动发现不可用，请改用手动输入平板 IP。");
        } else {
            notify_status(L"WiFi 投屏已开启，正在搜索同一局域网内的平板…");
        }
        SetTimer(window, kWifiDiscoverTimerId, 1000U, nullptr);
    } else {
        browser().stop();
        KillTimer(window, kWifiDiscoverTimerId);
        notify_status(L"WiFi 投屏已关闭。");
    }
    notify_publish();
}

std::wstring pc_uuid() {
    std::lock_guard lock(g_mutex);
    return g_pc_uuid;
}

std::wstring host_display_name() {
    wchar_t computer[MAX_COMPUTERNAME_LENGTH + 1U]{};
    DWORD length = MAX_COMPUTERNAME_LENGTH + 1U;
    if (GetComputerNameW(computer, &length) == FALSE) {
        return L"WireView PC";
    }
    return std::wstring(computer, length);
}

std::vector<DiscoveredDevice> devices() {
    std::vector<DiscoveredDevice> result;
    if (!enabled()) {
        return result;
    }
    for (const auto& device : browser().snapshot()) {
        result.push_back(DiscoveredDevice{
            ascii_to_wide(device.android_id),
            ascii_to_wide(device.device_name),
            ascii_to_wide(device.ip)});
    }
    return result;
}

TargetInfo target_info() {
    std::lock_guard lock(g_mutex);
    TargetInfo info;
    info.android_id = g_selected_id;
    info.ip = g_selected_ip;
    info.device_name = g_selected_name;
    if (info.device_name.empty() && info.android_id.empty() == false) {
        for (const auto& entry : whitelist().entries()) {
            if (ascii_to_wide(entry.android_id) == info.android_id) {
                info.device_name = ascii_to_wide(entry.device_name);
                break;
            }
        }
    }
    return info;
}

std::vector<WhitelistItem> whitelist_entries() {
    std::vector<WhitelistItem> result;
    std::lock_guard lock(g_mutex);
    for (const auto& entry : whitelist().entries()) {
        result.push_back(WhitelistItem{
            ascii_to_wide(entry.android_id),
            ascii_to_wide(entry.device_name),
            ascii_to_wide(entry.paired_at),
            ascii_to_wide(entry.last_ip)});
    }
    return result;
}

void select_target(const std::wstring& android_id) {
    std::lock_guard lock(g_mutex);
    g_selected_id = android_id;
    g_selected_ip.clear();
    g_selected_name.clear();
    for (const auto& entry : whitelist().entries()) {
        if (ascii_to_wide(entry.android_id) == android_id) {
            g_selected_name = ascii_to_wide(entry.device_name);
            break;
        }
    }
    for (const auto& device : browser().snapshot()) {
        if (ascii_to_wide(device.android_id) == android_id && !device.ip.empty()) {
            g_selected_ip = ascii_to_wide(device.ip);
            break;
        }
    }
    if (g_selected_ip.empty()) {
        // mDNS may be unavailable on this network; fall back to the last
        // address the tablet was actually reached on.
        for (const auto& entry : whitelist().entries()) {
            if (ascii_to_wide(entry.android_id) == android_id &&
                !entry.last_ip.empty()) {
                g_selected_ip = ascii_to_wide(entry.last_ip);
                break;
            }
        }
    }
}

bool has_session_target() {
    std::lock_guard lock(g_mutex);
    // ID only: the address is recovered at session start (mDNS snapshot,
    // then the whitelist's last-known IP). Requiring the IP here made the
    // extend flow silently fall back to ADB whenever discovery had not
    // produced an address yet (audit P0-1).
    return !g_selected_id.empty();
}

bool resolve_session_credential(
    std::wstring* endpoint,
    std::vector<std::uint8_t>* key) {
    std::wstring android_id;
    std::wstring ip;
    std::vector<std::uint8_t> sealed_key;
    {
        std::lock_guard lock(g_mutex);
        android_id = g_selected_id;
        ip = g_selected_ip;
    }
    std::string android_id_utf8 = utf8_string(android_id);
    // Whitelist access is serialised with g_mutex: the pairing thread
    // upserts under the same lock (audit P1 data race).
    std::vector<std::uint8_t> plain_key;
    {
        std::lock_guard lock(g_mutex);
        const auto* entry = whitelist().find(android_id_utf8);
        if (entry == nullptr) {
            notify_status(L"所选 WiFi 设备不在白名单中，请先配对。");
            return false;
        }
        try {
            plain_key = WifiWhitelist::unseal(*entry);
        } catch (const std::runtime_error&) {
            notify_status(L"读取已配对凭据失败（DPAPI）：请删除该白名单条目后重新配对。");
            return false;
        }
        // Address recovery order: this round's selection, the mDNS
        // snapshot, then the last IP a session actually dialled.
        if (ip.empty()) {
            for (const auto& device : browser().snapshot()) {
                if (ascii_to_wide(device.android_id) == android_id &&
                    !device.ip.empty()) {
                    ip = ascii_to_wide(device.ip);
                    break;
                }
            }
        }
        if (ip.empty() && entry->last_ip.empty() == false) {
            ip = ascii_to_wide(entry->last_ip);
        }
        if (ip.empty()) {
            notify_status(L"所选 WiFi 设备当前不在线：请确认平板的 WiFi 投屏开关已打开，"
                L"或使用完整模式 WiFi 卡片的手动 IP 配对。");
            return false;
        }
        // Remember the dial target so a session started without a fresh
        // discovery (hotspot networks without usable mDNS) still works.
        whitelist().update_last_ip(android_id_utf8, utf8_string(ip));
        std::wstring save_error;
        whitelist().save(&save_error);
        key->swap(plain_key);
        *endpoint = ip + L":" + std::to_wstring(kTabletPort);
        return true;
    }
}

void begin_pairing(const std::wstring& ip) {
    bool expected = false;
    if (!g_pairing_active.compare_exchange_strong(expected, true)) {
        notify_status(L"已有一次配对正在进行。");
        return;
    }
    // Fresh attempt: clear any stale cancel request from a previous run.
    g_pairing_cancel.store(false);
    const std::string uuid = uuid_hex();
    std::vector<std::uint8_t> pc_uuid_bytes;
    try {
        pc_uuid_bytes = from_hex(uuid);
    } catch (const std::runtime_error&) {
        g_pairing_active.store(false);
        notify_status(L"WiFi 身份标识损坏，请删除 wifi-identity.ini 后重试。");
        return;
    }
    // A hand-edited or corrupted identity file may hold a uuid of the wrong
    // length: a longer one would overflow the fixed 16-byte challenge
    // field in the handshake, a shorter one silently breaks the MAC
    // (audit A6 claim 6). The session path (development_transport.cpp)
    // applies the same check.
    if (pc_uuid_bytes.size() != wireview::protocol::kAuthUuidSize) {
        g_pairing_active.store(false);
        notify_status(L"WiFi 身份标识长度异常，请删除 wifi-identity.ini 后重试。");
        return;
    }
    const std::string pin = random_pairing_pin();
    notify_pairing_approved_over_usb(pin);
    notify_status(L"正在与 " + ip + L" 配对（PIN " + ascii_to_wide(pin) + L"）…");
    notify_publish();

    auto bundle = std::make_shared<
        std::pair<std::string, std::vector<std::uint8_t>>>(pin, pc_uuid_bytes);
    auto shared_ip = std::make_shared<std::wstring>(ip);
    auto shared_name = std::make_shared<std::string>(utf8_string(host_display_name()));
    std::thread([bundle, shared_ip, shared_name]() {
        auto* result = new std::pair<std::wstring, std::wstring>();
        bool paired = false;
        try {
            WinsockSession winsock;
            sockaddr_in address{};
            address.sin_family = AF_INET;
            address.sin_port = htons(kTabletPort);
            if (inet_pton(AF_INET, utf8_string(*shared_ip).c_str(), &address.sin_addr) != 1) {
                throw std::runtime_error("IP address is not valid IPv4");
            }
            auto socket = connect_tcp_with_timeout(address, std::chrono::seconds(2));
            SocketByteStream stream(socket.get());
            WifiLinkIdentity identity{bundle->second, *shared_name};
            std::vector<std::uint8_t> credential;
            const WifiPeerInfo peer = perform_wifi_handshake(
                stream, identity, wireview::protocol::AuthMode::pairing, {},
                bundle->first, true, &credential);
            if (g_pairing_cancel.load()) {
                // Cancelled while the handshake was in flight: discard the
                // result so nothing lands in the whitelist.
                result->second = L"已取消（设备未加入白名单）。";
            } else {
                {
                    std::lock_guard lock(impl_mutex());
                    whitelist().upsert(peer.android_id, peer.device_name, credential);
                    std::wstring save_error;
                    whitelist().save(&save_error);
                }
                paired = true;
                result->first = utf8_to_wide(peer.device_name);
            }
        } catch (const std::exception& error) {
            result->second = g_pairing_cancel.load()
                ? std::wstring(L"已取消。")
                : utf8_to_wide(error.what());
        }
        g_pairing_active.store(false);
        PostMessageW(
            g_hooks.window != nullptr ? g_hooks.window() : nullptr,
            kWifiPairingDoneMessageId,
            paired ? 1U : 0U, reinterpret_cast<LPARAM>(result));
    }).detach();
}

void cancel_pairing() {
    if (!g_pairing_active.load()) {
        notify_status(L"当前没有正在进行的 WiFi 配对。");
        return;
    }
    g_pairing_cancel.store(true);
    notify_status(L"已请求取消配对，等待当前尝试结束（最长约 130 秒）…");
}

bool remove_from_whitelist(const std::wstring& android_id) {
    bool removed = false;
    bool saved = false;
    std::wstring error;
    {
        // remove + save under the same lock (audit A6 claim 4): an
        // unsynchronised save here races the pairing worker's
        // upsert/save on the same entries vector and file.
        std::lock_guard lock(g_mutex);
        removed = whitelist().remove(utf8_string(android_id));
        if (removed) {
            saved = whitelist().save(&error);
        }
    }
    if (!removed) {
        return false;
    }
    if (!saved) {
        notify_status(L"白名单删除成功但保存失败：" + error);
    } else {
        notify_status(L"已从 WiFi 白名单移除该设备；重新配对即可再次使用。");
    }
    return true;
}

void resume_enabled(HWND window) {
    bool on = false;
    {
        std::lock_guard lock(g_mutex);
        on = g_enabled;
    }
    if (!on) {
        return;
    }
    ensure_mdns_firewall_rule();
    if (browser().start()) {
        SetTimer(window, kWifiDiscoverTimerId, 1000U, nullptr);
    } else {
        notify_status(L"WiFi 投屏已恢复开启；mDNS 自动发现不可用，会话将使用上次成功的 IP 直连。");
    }
}

void on_discover_tick() {
    if (!enabled()) {
        return;
    }
    // Keep the session target's address fresh: the dial always uses the
    // latest discovery result for the selected device.
    {
        std::lock_guard lock(g_mutex);
        if (!g_selected_id.empty()) {
            g_selected_ip.clear();
            for (const auto& device : browser().snapshot()) {
                if (ascii_to_wide(device.android_id) == g_selected_id &&
                    !device.ip.empty()) {
                    g_selected_ip = ascii_to_wide(device.ip);
                    break;
                }
            }
            if (g_selected_ip.empty()) {
                for (const auto& entry : whitelist().entries()) {
                    if (ascii_to_wide(entry.android_id) == g_selected_id &&
                        !entry.last_ip.empty()) {
                        g_selected_ip = ascii_to_wide(entry.last_ip);
                        break;
                    }
                }
            }
        }
    }
    notify_publish();
}

void on_pairing_done(HWND window, WPARAM wparam, LPARAM lparam) {
    (void)window;
    auto* result = reinterpret_cast<std::pair<std::wstring, std::wstring>*>(lparam);
    if (result == nullptr) {
        return;
    }
    if (wparam != 0U) {
        std::wstring error;
        bool saved = false;
        {
            // The pairing worker already saved under the lock; this save is
            // the reported retry and must take the same lock (audit A6
            // claim 4).
            std::lock_guard lock(g_mutex);
            saved = whitelist().save(&error);
        }
        if (!saved) {
            notify_status(L"与 " + result->first + L" 配对成功，但白名单保存失败：" + error);
        } else {
            notify_status(L"WiFi 配对成功：" + result->first + L" 已加入白名单。");
        }
    } else {
        notify_status(L"WiFi 配对失败：" + result->second);
    }
    delete result;
    notify_publish();
}

}  // namespace wireview::host::wifi_control
