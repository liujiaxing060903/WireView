#pragma once

// mDNS discovery of WireView tablets via the Windows DNS Service API
// (DnsServiceBrowse/DnsServiceResolve, Win10 1703+). The tablet advertises
// _wireview._tcp with TXT v/id/name. Callbacks arrive on RPC thread-pool
// threads; results are collected under a mutex and snapshotted for the UI.

#include <cstdint>
#include <string>
#include <vector>

namespace wireview::host {

struct WifiDiscoveredDevice {
    std::string android_id;   // TXT "id" (may be empty if TXT parsing failed).
    std::string device_name;  // TXT "name", falling back to the instance name.
    std::string instance;     // Full mDNS instance name.
    std::string ip;           // IPv4 dotted quad (empty until resolved).
    std::uint16_t port{0};
};

class MdnsBrowser {
public:
    MdnsBrowser() = default;

    ~MdnsBrowser();

    MdnsBrowser(const MdnsBrowser&) = delete;
    MdnsBrowser& operator=(const MdnsBrowser&) = delete;

    // Starts browsing; idempotent. Returns false when the platform API is
    // unavailable (pre-1703 Windows) so the caller can fall back to manual
    // entry with a hint.
    bool start();

    // Stops browsing and cancels in-flight resolves. Cancelled resolve
    // contexts are deleted by their completion callbacks (the single
    // deletion point), not here.
    void stop();

    bool is_running() const noexcept;

    // Thread-safe snapshot of everything seen so far (resolves included).
    [[nodiscard]] std::vector<WifiDiscoveredDevice> snapshot() const;

    // The pimpl body lives in the .cpp; the type name must be public so the
    // anonymous-namespace callbacks can name it.
    struct Impl;

private:
    Impl* impl_{nullptr};
};

}  // namespace wireview::host
