#pragma once

#include <cstdint>
#include <type_traits>

namespace wireview {
namespace idd_relay {

// This pipe is local to the Windows machine. It carries only control metadata;
// video pixels stay in D3D11 shared textures.
static constexpr wchar_t kPipeName[] = L"\\\\.\\pipe\\WireViewIddFrameRelay.v1";
static constexpr std::uint32_t kMagic = 0x5756524CU;  // "WVRL" in little endian.
static constexpr std::uint16_t kProtocolVersion = 1U;
static constexpr std::uint32_t kSlotCount = 3U;

enum class MessageType : std::uint16_t {
    stream_description = 1U,
    shared_texture = 2U,
    consumer_ready = 3U,
    frame_ready = 4U,
    frame_released = 5U,
    stop_stream = 6U,
    textures_ready = 7U,
    // A UTF-16 shared-resource name. The Host creates the resource with a
    // restrictive ACL; the UMDF driver opens it by name, so no process-handle
    // duplication is needed across their different identities.
    shared_texture_name = 8U,
};

#pragma pack(push, 1)
struct MessageHeader {
    std::uint32_t magic{kMagic};
    std::uint16_t version{kProtocolVersion};
    MessageType type{};
    std::uint32_t payload_size{};
};

// DXGI_FORMAT is serialized as its integer value to keep this header free of
// DirectX headers and usable by both the UMDF driver and the user-mode Host.
struct StreamDescription {
    std::uint32_t adapter_luid_low{};
    std::int32_t adapter_luid_high{};
    std::uint32_t dxgi_format{};
    std::uint16_t width{};
    std::uint16_t height{};
    std::uint16_t frame_rate{};
    std::uint16_t slot_count{};
};

// The handle has already been duplicated into the Host process. Its numeric
// value is meaningful only to that process and is never persisted.
struct SharedTexture {
    std::uint32_t slot{};
    std::uint32_t reserved{};
    std::uint64_t host_handle{};
};

// Followed by exactly utf16_code_units UTF-16 code units (without a trailing
// NUL). Keeping the string outside the fixed header makes all wire structures
// architecture independent while allowing unpredictable per-session names.
struct SharedTextureNameHeader {
    std::uint32_t slot{};
    std::uint32_t utf16_code_units{};
};

struct FrameReady {
    std::uint32_t slot{};
    std::uint32_t reserved{};
    std::uint64_t sequence{};
    std::uint64_t capture_timestamp_micros{};
};

struct FrameReleased {
    std::uint32_t slot{};
    std::uint32_t reserved{};
    std::uint64_t sequence{};
};
#pragma pack(pop)

static_assert(std::is_trivially_copyable_v<MessageHeader>);
static_assert(std::is_trivially_copyable_v<StreamDescription>);
static_assert(std::is_trivially_copyable_v<SharedTexture>);
static_assert(std::is_trivially_copyable_v<SharedTextureNameHeader>);
static_assert(std::is_trivially_copyable_v<FrameReady>);
static_assert(std::is_trivially_copyable_v<FrameReleased>);
static_assert(sizeof(MessageHeader) == 12U);
static_assert(sizeof(StreamDescription) == 20U);
static_assert(sizeof(SharedTexture) == 16U);
static_assert(sizeof(SharedTextureNameHeader) == 8U);
static_assert(sizeof(FrameReady) == 24U);
static_assert(sizeof(FrameReleased) == 16U);

}  // namespace idd_relay
}  // namespace wireview
