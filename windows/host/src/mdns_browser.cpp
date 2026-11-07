#include "mdns_browser.hpp"

// WinSock2 must precede any header that pulls in <Windows.h>.
#include <WinSock2.h>
#include <Windows.h>
#include <windns.h>

#include <algorithm>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#pragma comment(lib, "dnsapi.lib")

namespace wireview::host {

struct MdnsBrowser::Impl {
    // One per in-flight DnsServiceResolve; the completion callback or a
    // failed start request is its sole owner.
    struct ResolveContext {
        Impl* owner;
        std::string instance_name;
        std::wstring query_name;
        DNS_SERVICE_CANCEL cancel{};
    };

    std::mutex mutex;
    std::vector<WifiDiscoveredDevice> devices;
    std::set<std::string> resolving;
    std::vector<ResolveContext*> active_resolves;
    DNS_SERVICE_CANCEL browse_cancel{};
    bool browse_active{false};

    void upsert(const WifiDiscoveredDevice& device) {
        std::lock_guard lock(mutex);
        resolving.erase(device.instance);
        const auto match = std::find_if(
            devices.begin(), devices.end(),
            [&device](const WifiDiscoveredDevice& existing) {
                return existing.instance == device.instance;
            });
        if (match == devices.end()) {
            devices.push_back(device);
            return;
        }
        if (!device.ip.empty()) {
            match->ip = device.ip;
            match->port = device.port;
        }
        if (!device.android_id.empty()) {
            match->android_id = device.android_id;
        }
        if (!device.device_name.empty()) {
            match->device_name = device.device_name;
        }
    }

    void finish_resolve(ResolveContext* context) {
        {
            std::lock_guard lock(mutex);
            resolving.erase(context->instance_name);
            const auto match = std::find(
                active_resolves.begin(), active_resolves.end(), context);
            if (match != active_resolves.end()) {
                active_resolves.erase(match);
            }
        }
        delete context;
    }
};

namespace {

std::string wide_to_utf8(std::wstring_view value) {
    if (value.empty()) {
        return {};
    }
    const int length = WideCharToMultiByte(
        CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
        nullptr, 0, nullptr, nullptr);
    if (length <= 0) {
        return {};
    }
    std::string result(static_cast<std::size_t>(length), '\0');
    WideCharToMultiByte(
        CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
        result.data(), length, nullptr, nullptr);
    return result;
}

// IP4_ADDRESS is a plain DWORD in network byte order.
std::string ipv4_text(IP4_ADDRESS address) {
    char buffer[16]{};
    (void)snprintf(
        buffer, sizeof(buffer), "%u.%u.%u.%u",
        (address >> 24) & 0xffU,
        (address >> 16) & 0xffU,
        (address >> 8) & 0xffU,
        address & 0xffU);
    return buffer;
}

std::string txt_value(const DNS_SERVICE_INSTANCE& instance, std::string_view key) {
    if (instance.keys == nullptr || instance.values == nullptr) {
        return {};
    }
    const std::wstring key_wide(key.begin(), key.end());
    for (DWORD index = 0U; index < instance.dwPropertyCount; ++index) {
        if (instance.keys[index] == nullptr || instance.values[index] == nullptr) {
            continue;
        }
        if (key_wide != instance.keys[index]) {
            continue;
        }
        return wide_to_utf8(instance.values[index]);
    }
    return {};
}

VOID WINAPI resolve_complete_callback(
    DWORD status, PVOID raw_context, PDNS_SERVICE_INSTANCE instance) {
    auto* resolve_context = static_cast<MdnsBrowser::Impl::ResolveContext*>(raw_context);
    auto* impl = resolve_context->owner;
    if (status == ERROR_SUCCESS && instance != nullptr) {
        WifiDiscoveredDevice device;
        device.instance = resolve_context->instance_name;
        device.android_id = txt_value(*instance, "id");
        device.device_name = txt_value(*instance, "name");
        if (device.device_name.empty()) {
            const auto marker = device.instance.find("._wireview");
            device.device_name = marker == std::string::npos
                ? device.instance
                : device.instance.substr(0U, marker);
        }
        if (instance->ip4Address != nullptr && instance->ip4Address[0] != 0U) {
            // IP4 arrays are 0-terminated; WireView tablets advertise one.
            device.ip = ipv4_text(instance->ip4Address[0]);
            device.port = instance->wPort;
        }
        impl->upsert(device);
    }
    impl->finish_resolve(resolve_context);
}

VOID WINAPI browse_callback(PVOID raw_context, PDNS_QUERY_RESULT result) {
    // Runs on an RPC thread-pool thread.
    auto* impl = static_cast<MdnsBrowser::Impl*>(raw_context);
    if (result == nullptr || result->pQueryRecords == nullptr) {
        return;
    }
    for (PDNS_RECORD record = result->pQueryRecords;
         record != nullptr; record = record->pNext) {
        if (record->wType != DNS_TYPE_PTR || record->Data.Ptr.pNameHost == nullptr) {
            continue;
        }
        // DNS_RECORD names are ANSI (PSTR); the wire format is UTF-8 already.
        const std::string instance = record->Data.Ptr.pNameHost;
        bool should_resolve = false;
        {
            std::lock_guard lock(impl->mutex);
            const bool known = std::any_of(
                impl->devices.begin(), impl->devices.end(),
                [&instance](const WifiDiscoveredDevice& device) {
                    return device.instance == instance;
                });
            should_resolve = !known && !impl->resolving.contains(instance);
            if (should_resolve) {
                impl->resolving.insert(instance);
            }
        }
        if (!should_resolve) {
            continue;
        }
        auto* resolve_context = new MdnsBrowser::Impl::ResolveContext();
        resolve_context->owner = impl;
        resolve_context->instance_name = instance;
        resolve_context->query_name = std::wstring(instance.begin(), instance.end());
        DNS_SERVICE_RESOLVE_REQUEST request{};
        request.Version = DNS_QUERY_REQUEST_VERSION1;
        request.InterfaceIndex = 0U;
        request.QueryName = resolve_context->query_name.data();
        request.pResolveCompletionCallback = resolve_complete_callback;
        request.pQueryContext = resolve_context;
        const DNS_STATUS status = DnsServiceResolve(&request, &resolve_context->cancel);
        if (status == ERROR_SUCCESS) {
            std::lock_guard lock(impl->mutex);
            impl->active_resolves.push_back(resolve_context);
        } else {
            // The callback will never fire, so clean up here.
            std::lock_guard lock(impl->mutex);
            impl->resolving.erase(instance);
            delete resolve_context;
        }
    }
}

}  // namespace

MdnsBrowser::~MdnsBrowser() {
    stop();
}

bool MdnsBrowser::start() {
    if (impl_ != nullptr && is_running()) {
        return true;
    }
    if (impl_ == nullptr) {
        impl_ = new Impl();
    }
    DNS_SERVICE_BROWSE_REQUEST request{};
    request.Version = DNS_QUERY_REQUEST_VERSION1;
    request.InterfaceIndex = 0U;
    // QueryName is not modified by DnsServiceBrowse, but the API predates
    // const-correctness: the cast matches the SDK header contract.
    request.QueryName = const_cast<PWSTR>(L"_wireview._tcp.local");
    request.pBrowseCallbackV2 = browse_callback;
    request.pQueryContext = impl_;

    const DNS_STATUS status = DnsServiceBrowse(&request, &impl_->browse_cancel);
    if (status != ERROR_SUCCESS) {
        return false;
    }
    impl_->browse_active = true;
    return true;
}

void MdnsBrowser::stop() {
    if (impl_ == nullptr) {
        return;
    }
    if (impl_->browse_active) {
        (void)DnsServiceBrowseCancel(&impl_->browse_cancel);
        impl_->browse_cancel = DNS_SERVICE_CANCEL{};
        impl_->browse_active = false;
    }
    std::vector<Impl::ResolveContext*> pending;
    {
        std::lock_guard lock(impl_->mutex);
        impl_->resolving.clear();
        impl_->devices.clear();
        pending.swap(impl_->active_resolves);
    }
    for (auto* resolve_context : pending) {
        (void)DnsServiceResolveCancel(&resolve_context->cancel);
        delete resolve_context;
    }
}

bool MdnsBrowser::is_running() const noexcept {
    return impl_ != nullptr && impl_->browse_active;
}

std::vector<WifiDiscoveredDevice> MdnsBrowser::snapshot() const {
    if (impl_ == nullptr) {
        return {};
    }
    std::lock_guard lock(impl_->mutex);
    return impl_->devices;
}

}  // namespace wireview::host
