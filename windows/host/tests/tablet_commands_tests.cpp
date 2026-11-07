#include "tablet_commands.hpp"

#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace wireview::host;

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

std::vector<std::uint8_t> bytes(std::initializer_list<std::uint8_t> values) {
    return std::vector<std::uint8_t>(values);
}

void test_decode_control_command() {
    TabletCommand command{};
    require(!decode_control_command({}, command), "empty payload must be rejected");
    require(decode_control_command(bytes({0x01}), command) &&
        command == TabletCommand::escape, "escape was not decoded");
    require(decode_control_command(bytes({0x04}), command) &&
        command == TabletCommand::request_window_list, "list request was not decoded");
}

void test_decode_ime_text() {
    std::wstring text;
    // [6][len lo][len hi]...utf-8 "AB"
    require(decode_ime_text(bytes({0x06, 0x02, 0x00, 0x41, 0x42}), text) &&
        text == L"AB", "ascii ime text was not decoded");
    // Chinese: U+6C49 (UTF-8: E6 B1 89)
    require(decode_ime_text(bytes({0x06, 0x03, 0x00, 0xE6, 0xB1, 0x89}), text) &&
        text == L"\x6C49", "chinese ime text was not decoded");
    require(!decode_ime_text(bytes({0x06, 0x02, 0x00, 0x41}), text),
        "truncated ime text must be rejected");
    require(!decode_ime_text(bytes({0x06, 0x00, 0x00}), text),
        "empty ime text must be rejected");
    require(!decode_ime_text(bytes({0x07, 0x01, 0x00, 0x41}), text),
        "wrong command byte must be rejected");
}

void test_decode_ime_key_whitelist() {
    UINT virtual_key = 0U;
    require(decode_ime_key(bytes({0x07, 0x08}), virtual_key) && virtual_key == VK_BACK,
        "backspace must pass the whitelist");
    require(decode_ime_key(bytes({0x07, 0x0D}), virtual_key) && virtual_key == VK_RETURN,
        "return must pass the whitelist");
    require(decode_ime_key(bytes({0x07, 0x28}), virtual_key) && virtual_key == VK_DOWN,
        "arrow keys must pass the whitelist");
    require(!decode_ime_key(bytes({0x07, 0x41}), virtual_key),
        "printable keys must not pass the whitelist");
    require(!decode_ime_key(bytes({0x07}), virtual_key),
        "truncated ime key must be rejected");
}

void test_decode_window_move() {
    std::uint32_t token = 0U;
    std::uint32_t index = 0U;
    std::uint8_t area = 0U;
    require(decode_window_move(
        bytes({0x05, 0x21, 0x43, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x01}),
        token, index, area), "window move was not decoded");
    require(token == 0x4321 && index == 2U && area == 1U, "window move fields mismatch");
    require(!decode_window_move(
        bytes({0x05, 0x21, 0x43, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x03}),
        token, index, area), "unknown target area must be rejected");
    require(!decode_window_move(bytes({0x05, 0x01, 0x02, 0x03}), token, index, area),
        "truncated window move must be rejected");
}

void test_encode_window_list_roundtrip() {
    std::vector<TabletWindowEntry> entries;
    TabletWindowEntry primary{nullptr, kWindowAreaPrimary, L"Visual Studio"};
    TabletWindowEntry vdd{reinterpret_cast<HWND>(0x12345), kWindowAreaVdd, L"\x6D4B\x8BD5"};
    entries.push_back(primary);
    entries.push_back(vdd);
    const auto payload = encode_window_list(0x01020304U, entries);

    require(payload.size() >= 6U, "window list payload is too short");
    const std::uint32_t token = payload[0] | (payload[1] << 8U) |
        (payload[2] << 16U) | (static_cast<std::uint32_t>(payload[3]) << 24U);
    const std::uint16_t count = static_cast<std::uint16_t>(payload[4] | (payload[5] << 8U));
    require(token == 0x01020304U, "window list token mismatch");
    require(count == 2U, "window list count mismatch");

    std::size_t offset = 6U;
    for (std::uint16_t entry_index = 0; entry_index < count; ++entry_index) {
        require(offset + 11U <= payload.size(), "window list entry header overflow");
        std::uint64_t handle = 0U;
        for (int shift = 0; shift < 64; shift += 8) {
            handle |= static_cast<std::uint64_t>(payload[offset + (shift / 8)]) << shift;
        }
        offset += 8U;
        const std::uint8_t area = payload[offset++];
        const std::uint16_t title_bytes = static_cast<std::uint16_t>(
            payload[offset] | (payload[offset + 1] << 8U));
        offset += 2U;
        require(offset + title_bytes <= payload.size(), "window list title overflow");
        const std::string title(
            reinterpret_cast<const char*>(payload.data() + offset), title_bytes);
        offset += title_bytes;
        if (entry_index == 0) {
            require(handle == 0U && area == kWindowAreaPrimary && title == "Visual Studio",
                "primary entry round trip mismatch");
        } else {
            require(handle == 0x12345U && area == kWindowAreaVdd,
                "vdd entry handle/area mismatch");
            // \x6D4B\x8BD5 -> UTF-8 E6 B5 8B E8 AF 95
            require(title == "\xE6\xB5\x8B\xE8\xAF\x95", "vdd entry title round trip mismatch");
        }
    }
    require(offset == payload.size(), "window list payload has trailing bytes");
}

}  // namespace

int main() {
    try {
        test_decode_control_command();
        test_decode_ime_text();
        test_decode_ime_key_whitelist();
        test_decode_window_move();
        test_encode_window_list_roundtrip();
    } catch (const std::exception& error) {
        std::cerr << "tablet commands test failed: " << error.what() << '\n';
        return 1;
    }
    std::cout << "tablet commands tests passed\n";
    return 0;
}
