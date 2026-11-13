#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace wireview::host {

inline constexpr const wchar_t* kExtensionSlotNames[]{
    L"Local\\WireView.VddExtension.1", L"Local\\WireView.VddExtension.2"};

struct ParsecVddProbe {
    bool available{false};
    std::uint32_t version{};
    std::string detail;
};

// Queries the signed external Parsec VDD if it is already installed. This
// function never installs, enables, or changes a Windows driver.
ParsecVddProbe probe_parsec_vdd();

// Returns the currently attached Windows display-device names that can be
// identified as Parsec VDD. Recognition inspects the adapter and its monitor
// child, since different Windows/GPU stacks expose the Parsec marker in
// different fields.
std::vector<std::string> enumerate_active_parsec_vdd_displays();

// Lid-safe copy is offered for an active laptop panel and our own VDD only.
bool has_single_internal_display();
bool clone_internal_display_to_owned_vdd(const std::wstring& owned_device);

struct LidDisplaySize {
    std::uint32_t width{};
    std::uint32_t height{};
    std::vector<std::wstring> monitor_paths;
    std::uint32_t rotation{};
};

// Preserve this VDD's size across monitor attach/detach (including lid changes).
// Resolution changes with the same monitors remain controlled by Windows.
std::optional<long> restore_lid_display_size(std::wstring_view monitor_path, LidDisplaySize& open_size);

struct WindowsDisplaySource {
    std::wstring device_name;
    bool shared{};
    bool only_active{};
};

// A monitor path identifies the output even when Windows changes the GDI
// source name or merges it into another desktop.
std::wstring monitor_path_for_display(std::wstring_view device_name);
WindowsDisplaySource desktop_source_for_monitor(std::wstring_view monitor_path);

// Removes one explicitly identified Parsec VDD driver index. This is intended
// only for recovering a WireView-created display whose owner process ended
// before its normal RAII cleanup ran; callers must establish ownership first.
void remove_parsec_vdd_display(int display_index);

// Owns one WireView-created virtual display and its required keep-alive. The
// caller must retain this object for as long as Windows should keep that
// display attached. It deliberately has no driver-install capability.
class ParsecVddSession {
public:
    ParsecVddSession() = default;
    ~ParsecVddSession();

    ParsecVddSession(const ParsecVddSession&) = delete;
    ParsecVddSession& operator=(const ParsecVddSession&) = delete;

    void start(bool allow_additional = false);
    void stop() noexcept;

    [[nodiscard]] bool running() const noexcept;
    [[nodiscard]] int display_index() const noexcept;
    [[nodiscard]] std::string last_error() const;
    // Describes any topology repair WireView performed while waiting for this
    // newly-created VDD. Empty when no repair was necessary.
    [[nodiscard]] std::string creation_diagnostic() const;
    [[nodiscard]] std::string wait_for_display(std::chrono::milliseconds timeout) const;
    // Windows may recall an older copy/extend arrangement when a VDD is added.
    std::optional<long> restore_previous_vdd_desktop(std::wstring_view added_display) const;

private:
    void keep_alive_loop();

    void* device_handle_{};
    int display_index_{-1};
    std::atomic<bool> stop_requested_{false};
    std::atomic<bool> worker_failed_{false};
    std::string last_error_;
    mutable std::string creation_diagnostic_;
    // Stable display identities present before the driver's ADD request. They
    // allow the waiting path to adopt one uniquely new Windows output even on
    // systems that do not expose Parsec's marker in EnumDisplayDevices.
    std::vector<std::string> displays_before_start_;
    std::vector<std::wstring> monitor_paths_before_start_;
    class Impl;
    Impl* impl_{};
};

}  // namespace wireview::host
