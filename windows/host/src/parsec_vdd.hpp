#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

namespace wireview::host {

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

    void start();
    void stop() noexcept;

    [[nodiscard]] bool running() const noexcept;
    [[nodiscard]] int display_index() const noexcept;
    [[nodiscard]] std::string last_error() const;
    // Describes any topology repair WireView performed while waiting for this
    // newly-created VDD. Empty when no repair was necessary.
    [[nodiscard]] std::string creation_diagnostic() const;
    [[nodiscard]] std::string wait_for_display(std::chrono::milliseconds timeout) const;

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
    class Impl;
    Impl* impl_{};
};

}  // namespace wireview::host
