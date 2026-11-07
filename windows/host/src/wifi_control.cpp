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
bool g_enabled = false;
std::wstring g_pc_uuid;
std::filesystem::path g_settings_directory;
UiHooks g_hooks;
MdnsBrowser* g_browser = nullptr;

WifiWhitelist& whitelist() {
    static WifiWhitelist instance(g_settings_directory / L"wifi-whitelist.ini");
    static const bool loaded = [] {
        if (!g_settings_directory.empty()) {
            instance.load();
        }
        return true;
    }();
    (void)loaded;
    return instance;
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
    std::error_code error;
    std::filesystem::create_directories(g_settings_directory, error);
    std::ofstream output(identity_file(), std::ios::trunc);
    if (!output) {
        return;
    }
    output << "wifi_pc_uuid=";
    for (const wchar_t character : g_pc_uuid) {
        output << static_cast<char>(character);
    }
    output << "\nwifi_enabled=" << (g_enabled ? 1 : 0) << '\n';
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

std::vector<WhitelistItem> whitelist_entries() {
    std::vector<WhitelistItem> result;
    for (const auto& entry : whitelist().entries()) {
        result.push_back(WhitelistItem{
            ascii_to_wide(entry.android_id),
            ascii_to_wide(entry.device_name),
            ascii_to_wide(entry.paired_at)});
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
}

bool has_session_target() {
    std::lock_guard lock(g_mutex);
    return !g_selected_id.empty() && !g_selected_ip.empty();
}

bool resolve_session_credential(
    std::wstring* endpoint,
    std::vector<std::uint8_t>* key) {
    std::wstring android_id;
    {
        std::lock_guard lock(g_mutex);
        android_id = g_selected_id;
    }
    const auto* entry = whitelist().find(utf8_string(android_id));
    if (entry == nullptr) {
        notify_status(L"所选 WiFi 设备不在白名单中，请先配对。");
        return false;
    }
    try {
        *key = WifiWhitelist::unseal(*entry);
    } catch (const std::runtime_error&) {
        notify_status(L"读取已配对凭据失败（DPAPI）：请删除该白名单条目后重新配对。");
        return false;
    }
    std::lock_guard lock(g_mutex);
    *endpoint = g_selected_ip + L":" + std::to_wstring(kTabletPort);
    return true;
}

void begin_pairing(const std::wstring& ip) {
    bool expected = false;
    if (!g_pairing_active.compare_exchange_strong(expected, true)) {
        notify_status(L"已有一次配对正在进行。");
        return;
    }
    const std::string uuid = uuid_hex();
    std::vector<std::uint8_t> pc_uuid_bytes;
    try {
        pc_uuid_bytes = from_hex(uuid);
    } catch (const std::runtime_error&) {
        g_pairing_active.store(false);
        notify_status(L"WiFi 身份标识损坏，请删除 wifi-identity.ini 后重试。");
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
            whitelist().upsert(peer.android_id, peer.device_name, credential);
            paired = true;
            result->first = utf8_to_wide(peer.device_name);
        } catch (const std::exception& error) {
            result->second = utf8_to_wide(error.what());
        }
        g_pairing_active.store(false);
        PostMessageW(
            g_hooks.window != nullptr ? g_hooks.window() : nullptr,
            kWifiPairingDoneMessageId,
            paired ? 1U : 0U, reinterpret_cast<LPARAM>(result));
    }).detach();
}

bool remove_from_whitelist(const std::wstring& android_id) {
    if (!whitelist().remove(utf8_string(android_id))) {
        return false;
    }
    std::wstring error;
    if (!whitelist().save(&error)) {
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
    if (browser().start()) {
        SetTimer(window, kWifiDiscoverTimerId, 1000U, nullptr);
    }
}

void on_discover_tick() {
    if (!enabled()) {
        return;
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
        if (!whitelist().save(&error)) {
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
