#include "idd_frame_relay_pipe.hpp"

#include "idd_frame_relay_codec.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <Sddl.h>
#include <d3d11_2.h>
#include <dxgi1_4.h>

#include <wrl/client.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace wireview::host {
namespace {

using Microsoft::WRL::ComPtr;

class Handle {
public:
    Handle() = default;
    explicit Handle(HANDLE value) : value_(value) {}

    ~Handle() {
        if (value_ != INVALID_HANDLE_VALUE && value_ != nullptr) {
            CloseHandle(value_);
        }
    }

    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;

    Handle(Handle&& other) noexcept : value_(other.value_) {
        other.value_ = INVALID_HANDLE_VALUE;
    }

    Handle& operator=(Handle&& other) noexcept {
        if (this != &other) {
            if (value_ != INVALID_HANDLE_VALUE && value_ != nullptr) {
                CloseHandle(value_);
            }
            value_ = other.value_;
            other.value_ = INVALID_HANDLE_VALUE;
        }
        return *this;
    }

    [[nodiscard]] HANDLE get() const noexcept {
        return value_;
    }

private:
    HANDLE value_{INVALID_HANDLE_VALUE};
};

[[nodiscard]] std::string win32_error(const char* operation) {
    return std::string(operation) + " failed with Win32 error " +
        std::to_string(GetLastError());
}

void read_exact(HANDLE handle, std::span<std::byte> destination) {
    std::size_t offset = 0U;
    while (offset < destination.size()) {
        DWORD received = 0U;
        if (!ReadFile(
                handle,
                destination.data() + offset,
                static_cast<DWORD>(destination.size() - offset),
                &received,
                nullptr)) {
            throw std::runtime_error(win32_error("ReadFile(Idd relay)"));
        }
        if (received == 0U) {
            throw std::runtime_error("Idd relay peer closed while sending a message");
        }
        offset += received;
    }
}

void write_all(HANDLE handle, std::span<const std::byte> source) {
    std::size_t offset = 0U;
    while (offset < source.size()) {
        DWORD written = 0U;
        if (!WriteFile(
                handle,
                source.data() + offset,
                static_cast<DWORD>(source.size() - offset),
                &written,
                nullptr)) {
            throw std::runtime_error(win32_error("WriteFile(Idd relay)"));
        }
        if (written == 0U) {
            throw std::runtime_error("Idd relay peer accepted no message bytes");
        }
        offset += written;
    }
}

[[nodiscard]] wireview::idd_relay::ParsedMessage read_message(HANDLE handle) {
    wireview::idd_relay::MessageHeader header{};
    read_exact(handle, std::as_writable_bytes(std::span{&header, std::size_t{1U}}));
    constexpr std::uint32_t kMaximumControlPayload = 4'096U;
    if (header.payload_size > kMaximumControlPayload) {
        throw std::runtime_error("Idd relay control payload exceeds the permitted size");
    }
    std::vector<std::byte> encoded(sizeof(header) + header.payload_size);
    std::memcpy(encoded.data(), &header, sizeof(header));
    if (header.payload_size != 0U) {
        read_exact(handle, std::span(encoded).subspan(sizeof(header)));
    }
    return wireview::idd_relay::parse_message(encoded);
}

void write_message(
    HANDLE handle,
    wireview::idd_relay::MessageType type,
    std::span<const std::byte> payload = {}) {
    const auto encoded = wireview::idd_relay::encode_message(type, payload);
    write_all(handle, encoded);
}

template <typename Payload>
void write_message(
    HANDLE handle,
    wireview::idd_relay::MessageType type,
    const Payload& payload) {
    const auto bytes = std::as_bytes(std::span{&payload, std::size_t{1U}});
    write_message(handle, type, bytes);
}

[[nodiscard]] Handle create_server_pipe() {
    PSECURITY_DESCRIPTOR security_descriptor = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            L"D:P(A;;GA;;;SY)(A;;GA;;;LS)(A;;GA;;;OW)",
            SDDL_REVISION_1,
            &security_descriptor,
            nullptr)) {
        throw std::runtime_error(win32_error("ConvertStringSecurityDescriptorToSecurityDescriptorW"));
    }
    SECURITY_ATTRIBUTES attributes{};
    attributes.nLength = sizeof(attributes);
    attributes.lpSecurityDescriptor = security_descriptor;
    attributes.bInheritHandle = FALSE;
    HANDLE pipe = CreateNamedPipeW(
        wireview::idd_relay::kPipeName,
        PIPE_ACCESS_DUPLEX,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
        1U,
        4'096U,
        4'096U,
        0U,
        &attributes);
    LocalFree(security_descriptor);
    if (pipe == INVALID_HANDLE_VALUE) {
        throw std::runtime_error(win32_error("CreateNamedPipeW(WireView Idd relay)"));
    }
    return Handle(pipe);
}

void accept_pipe_client(HANDLE pipe) {
    if (ConnectNamedPipe(pipe, nullptr) != FALSE) {
        return;
    }
    const DWORD error = GetLastError();
    if (error != ERROR_PIPE_CONNECTED) {
        throw std::runtime_error(win32_error("ConnectNamedPipe(WireView Idd relay)"));
    }
}

void validate_stream_description(const wireview::idd_relay::StreamDescription& stream) {
    if (stream.width == 0U || stream.height == 0U ||
        (stream.width % 2U) != 0U || (stream.height % 2U) != 0U ||
        (stream.frame_rate != 60U && stream.frame_rate != 120U) ||
        stream.slot_count != wireview::idd_relay::kSlotCount || stream.dxgi_format == 0U) {
        throw std::runtime_error("Idd relay stream description is outside WireView limits");
    }
}

[[nodiscard]] ComPtr<ID3D11Device> create_device_for_adapter(
    std::uint32_t luid_low,
    std::int32_t luid_high) {
    LUID luid{};
    luid.LowPart = luid_low;
    luid.HighPart = luid_high;
    ComPtr<IDXGIFactory4> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
        throw std::runtime_error("CreateDXGIFactory1(Idd relay) failed");
    }
    ComPtr<IDXGIAdapter1> adapter;
    if (FAILED(factory->EnumAdapterByLuid(luid, IID_PPV_ARGS(&adapter)))) {
        throw std::runtime_error("Idd relay adapter LUID is unavailable to the Host");
    }
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    if (FAILED(D3D11CreateDevice(
            adapter.Get(),
            D3D_DRIVER_TYPE_UNKNOWN,
            nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT,
            nullptr,
            0U,
            D3D11_SDK_VERSION,
            &device,
            nullptr,
            &context))) {
        throw std::runtime_error("D3D11CreateDevice(Idd relay adapter) failed");
    }
    return device;
}

[[nodiscard]] ComPtr<ID3D11Texture2D> open_shared_texture(
    ID3D11Device* device,
    const wireview::idd_relay::SharedTexture& descriptor,
    const wireview::idd_relay::StreamDescription& stream) {
    if (descriptor.host_handle == 0U) {
        throw std::runtime_error("Idd relay shared texture handle is null");
    }
    ComPtr<ID3D11Device1> device1;
    if (FAILED(device->QueryInterface(IID_PPV_ARGS(&device1)))) {
        throw std::runtime_error("Idd relay Host device does not support NT shared handles");
    }
    ComPtr<ID3D11Texture2D> texture;
    const HANDLE handle = reinterpret_cast<HANDLE>(
        static_cast<std::uintptr_t>(descriptor.host_handle));
    if (FAILED(device1->OpenSharedResource1(handle, IID_PPV_ARGS(&texture)))) {
        throw std::runtime_error("Idd relay Host could not open a shared texture");
    }
    D3D11_TEXTURE2D_DESC description{};
    texture->GetDesc(&description);
    if (description.Width != stream.width || description.Height != stream.height ||
        static_cast<std::uint32_t>(description.Format) != stream.dxgi_format) {
        throw std::runtime_error("Idd relay shared texture does not match the negotiated stream");
    }
    return texture;
}

struct HostSharedTextureRing {
    std::array<ComPtr<ID3D11Texture2D>, wireview::idd_relay::kSlotCount> textures;
    std::array<ComPtr<IDXGIKeyedMutex>, wireview::idd_relay::kSlotCount> mutexes;
    // The named resource remains published only while this owning handle is
    // alive. Keep it for the negotiated session instead of closing it after
    // creation.
    std::array<Handle, wireview::idd_relay::kSlotCount> handles;
    std::array<std::wstring, wireview::idd_relay::kSlotCount> names;
};

[[nodiscard]] std::wstring make_shared_texture_name(std::uint32_t slot, bool global_namespace) {
    GUID guid{};
    if (FAILED(CoCreateGuid(&guid))) {
        throw std::runtime_error("CoCreateGuid(Idd relay texture) failed");
    }
    wchar_t guid_text[40]{};
    if (StringFromGUID2(guid, guid_text, static_cast<int>(std::size(guid_text))) == 0) {
        throw std::runtime_error("StringFromGUID2(Idd relay texture) failed");
    }
    return std::wstring(global_namespace ? L"Global\\WireViewIdd-" : L"Local\\WireViewIdd-") +
        guid_text + L"-" + std::to_wstring(slot);
}

[[nodiscard]] HostSharedTextureRing create_host_shared_texture_ring(
    ID3D11Device* device,
    const wireview::idd_relay::StreamDescription& stream,
    bool global_namespace) {
    if (device == nullptr || stream.dxgi_format != static_cast<std::uint32_t>(DXGI_FORMAT_B8G8R8A8_UNORM)) {
        throw std::runtime_error("Idd relay only supports BGRA shared textures");
    }
    PSECURITY_DESCRIPTOR security_descriptor = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            L"D:P(A;;GA;;;SY)(A;;GA;;;LS)(A;;GA;;;OW)",
            SDDL_REVISION_1,
            &security_descriptor,
            nullptr)) {
        throw std::runtime_error(win32_error("ConvertStringSecurityDescriptorToSecurityDescriptorW(texture)"));
    }
    SECURITY_ATTRIBUTES attributes{};
    attributes.nLength = sizeof(attributes);
    attributes.lpSecurityDescriptor = security_descriptor;
    attributes.bInheritHandle = FALSE;

    HostSharedTextureRing result{};
    D3D11_TEXTURE2D_DESC description{};
    description.Width = stream.width;
    description.Height = stream.height;
    description.MipLevels = 1U;
    description.ArraySize = 1U;
    description.Format = static_cast<DXGI_FORMAT>(stream.dxgi_format);
    description.SampleDesc.Count = 1U;
    description.Usage = D3D11_USAGE_DEFAULT;
    description.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    description.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE |
        D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
    try {
        for (std::uint32_t slot = 0U; slot < wireview::idd_relay::kSlotCount; ++slot) {
            if (FAILED(device->CreateTexture2D(&description, nullptr, &result.textures[slot]))) {
                throw std::runtime_error("CreateTexture2D(Idd relay Host texture) failed");
            }
            ComPtr<IDXGIResource1> resource;
            if (FAILED(result.textures[slot].As(&resource))) {
                throw std::runtime_error("Idd relay Host texture lacks IDXGIResource1");
            }
            if (FAILED(result.textures[slot].As(&result.mutexes[slot]))) {
                throw std::runtime_error("Idd relay Host texture lacks IDXGIKeyedMutex");
            }
            result.names[slot] = make_shared_texture_name(slot, global_namespace);
            HANDLE handle = nullptr;
            if (FAILED(resource->CreateSharedHandle(
                    &attributes,
                    DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE,
                    result.names[slot].c_str(),
                    &handle))) {
                throw std::runtime_error("CreateSharedHandle(Idd relay named texture) failed");
            }
            result.handles[slot] = Handle(handle);
        }
    } catch (...) {
        LocalFree(security_descriptor);
        throw;
    }
    LocalFree(security_descriptor);
    return result;
}

[[nodiscard]] ComPtr<ID3D11Texture2D> open_shared_texture_by_name(
    ID3D11Device* device,
    std::wstring_view name,
    const wireview::idd_relay::StreamDescription& stream) {
    ComPtr<ID3D11Device1> device1;
    if (device == nullptr || name.empty() ||
        FAILED(device->QueryInterface(IID_PPV_ARGS(&device1)))) {
        throw std::runtime_error("Idd relay device cannot open a named shared texture");
    }
    ComPtr<ID3D11Texture2D> texture;
    if (FAILED(device1->OpenSharedResourceByName(
            std::wstring(name).c_str(),
            DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE,
            IID_PPV_ARGS(&texture)))) {
        throw std::runtime_error("Idd relay could not open a named shared texture");
    }
    D3D11_TEXTURE2D_DESC description{};
    texture->GetDesc(&description);
    if (description.Width != stream.width || description.Height != stream.height ||
        static_cast<std::uint32_t>(description.Format) != stream.dxgi_format) {
        throw std::runtime_error("Idd relay named shared texture does not match the negotiated stream");
    }
    return texture;
}

using RelayFrameHandler = std::function<void(
    ID3D11Texture2D*,
    const wireview::idd_relay::FrameReady&)>;

[[nodiscard]] std::uint64_t handle_one_connection(
    HANDLE pipe,
    bool exchange_named_textures,
    const RelayFrameHandler& on_frame = {}) {
    const auto message = read_message(pipe);
    const auto stream = wireview::idd_relay::parse_payload<wireview::idd_relay::StreamDescription>(
        message, wireview::idd_relay::MessageType::stream_description);
    validate_stream_description(stream);
    write_message(pipe, wireview::idd_relay::MessageType::consumer_ready);
    if (!exchange_named_textures) {
        return 0U;
    }
    const ComPtr<ID3D11Device> device = create_device_for_adapter(
        stream.adapter_luid_low, stream.adapter_luid_high);
    const auto textures = create_host_shared_texture_ring(device.Get(), stream, true);
    for (std::uint32_t slot = 0U; slot < wireview::idd_relay::kSlotCount; ++slot) {
        const auto encoded = wireview::idd_relay::encode_shared_texture_name(slot, textures.names[slot]);
        write_all(pipe, encoded);
    }
    const auto textures_ready = read_message(pipe);
    if (textures_ready.type != wireview::idd_relay::MessageType::textures_ready ||
        !textures_ready.payload.empty()) {
        throw std::runtime_error("Idd relay driver did not confirm its named textures");
    }
    std::cout << "WireView Idd relay prepared " << stream.width << 'x' << stream.height
              << '@' << stream.frame_rate << " with " << stream.slot_count
              << " named GPU texture slots\n";

    std::uint64_t frame_count = 0U;
    std::uint64_t last_sequence = 0U;
    for (;;) {
        const auto incoming = read_message(pipe);
        if (incoming.type == wireview::idd_relay::MessageType::stop_stream) {
            if (!incoming.payload.empty()) {
                throw std::runtime_error("Idd relay stop message unexpectedly has a payload");
            }
            return frame_count;
        }
        const auto ready = wireview::idd_relay::parse_payload<wireview::idd_relay::FrameReady>(
            incoming, wireview::idd_relay::MessageType::frame_ready);
        if (ready.slot >= wireview::idd_relay::kSlotCount || ready.sequence == 0U ||
            ready.sequence <= last_sequence) {
            throw std::runtime_error("Idd relay frame identity is invalid or not monotonic");
        }
        const HRESULT acquired = textures.mutexes[ready.slot]->AcquireSync(1U, 50U);
        if (FAILED(acquired)) {
            throw std::runtime_error("Idd relay Host could not acquire a published texture slot");
        }
        try {
            if (on_frame) {
                on_frame(textures.textures[ready.slot].Get(), ready);
            }
        } catch (...) {
            (void)textures.mutexes[ready.slot]->ReleaseSync(0U);
            throw;
        }
        if (FAILED(textures.mutexes[ready.slot]->ReleaseSync(0U))) {
            throw std::runtime_error("Idd relay Host could not return a texture slot");
        }
        const wireview::idd_relay::FrameReleased released{
            .slot = ready.slot,
            .sequence = ready.sequence,
        };
        write_message(pipe, wireview::idd_relay::MessageType::frame_released, released);
        last_sequence = ready.sequence;
        ++frame_count;
    }
}

[[nodiscard]] Handle connect_loopback_client() {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    for (;;) {
        HANDLE pipe = CreateFileW(
            wireview::idd_relay::kPipeName,
            GENERIC_READ | GENERIC_WRITE,
            0U,
            nullptr,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL,
            nullptr);
        if (pipe != INVALID_HANDLE_VALUE) {
            return Handle(pipe);
        }
        if (GetLastError() != ERROR_FILE_NOT_FOUND && GetLastError() != ERROR_PIPE_BUSY) {
            throw std::runtime_error(win32_error("CreateFileW(WireView Idd relay)"));
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            throw std::runtime_error("WireView Idd relay listener did not become available");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

struct LoopbackSharedTextures {
    wireview::idd_relay::StreamDescription stream{};
    std::array<ComPtr<ID3D11Texture2D>, wireview::idd_relay::kSlotCount> textures;
    std::array<Handle, wireview::idd_relay::kSlotCount> duplicated_handles;
};

[[nodiscard]] LoopbackSharedTextures create_loopback_shared_textures() {
    LoopbackSharedTextures result;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    if (FAILED(D3D11CreateDevice(
            nullptr,
            D3D_DRIVER_TYPE_HARDWARE,
            nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT,
            nullptr,
            0U,
            D3D11_SDK_VERSION,
            &device,
            nullptr,
            &context))) {
        throw std::runtime_error("D3D11CreateDevice(loopback shared textures) failed");
    }
    ComPtr<IDXGIDevice> dxgi_device;
    ComPtr<IDXGIAdapter> adapter;
    DXGI_ADAPTER_DESC adapter_description{};
    if (FAILED(device.As(&dxgi_device)) || FAILED(dxgi_device->GetAdapter(&adapter)) ||
        FAILED(adapter->GetDesc(&adapter_description))) {
        throw std::runtime_error("Unable to obtain the loopback D3D adapter LUID");
    }

    result.stream.adapter_luid_low = adapter_description.AdapterLuid.LowPart;
    result.stream.adapter_luid_high = adapter_description.AdapterLuid.HighPart;
    result.stream.dxgi_format = static_cast<std::uint32_t>(DXGI_FORMAT_B8G8R8A8_UNORM);
    result.stream.width = 64U;
    result.stream.height = 64U;
    result.stream.frame_rate = 60U;
    result.stream.slot_count = static_cast<std::uint16_t>(wireview::idd_relay::kSlotCount);

    D3D11_TEXTURE2D_DESC texture_description{};
    texture_description.Width = result.stream.width;
    texture_description.Height = result.stream.height;
    texture_description.MipLevels = 1U;
    texture_description.ArraySize = 1U;
    texture_description.Format = static_cast<DXGI_FORMAT>(result.stream.dxgi_format);
    texture_description.SampleDesc.Count = 1U;
    texture_description.Usage = D3D11_USAGE_DEFAULT;
    texture_description.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    texture_description.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE |
        D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
    for (std::uint32_t slot = 0U; slot < wireview::idd_relay::kSlotCount; ++slot) {
        if (FAILED(device->CreateTexture2D(
                &texture_description, nullptr, &result.textures[slot]))) {
            throw std::runtime_error("CreateTexture2D(loopback shared texture) failed");
        }
        ComPtr<IDXGIResource1> resource;
        HANDLE local_handle = nullptr;
        if (FAILED(result.textures[slot].As(&resource)) ||
            FAILED(resource->CreateSharedHandle(
                nullptr,
                DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE,
                nullptr,
                &local_handle))) {
            throw std::runtime_error("CreateSharedHandle(loopback texture) failed");
        }
        HANDLE duplicated_handle = nullptr;
        const BOOL duplicated = DuplicateHandle(
            GetCurrentProcess(),
            local_handle,
            GetCurrentProcess(),
            &duplicated_handle,
            0U,
            FALSE,
            DUPLICATE_SAME_ACCESS);
        CloseHandle(local_handle);
        if (duplicated == FALSE) {
            throw std::runtime_error(win32_error("DuplicateHandle(loopback texture)"));
        }
        result.duplicated_handles[slot] = Handle(duplicated_handle);
    }
    return result;
}

}  // namespace

int run_idd_frame_relay_listener() {
    const Handle pipe = create_server_pipe();
    std::cout << "WireView Idd relay listening for the virtual-display driver\n";
    accept_pipe_client(pipe.get());
    const std::uint64_t frames = handle_one_connection(pipe.get(), true);
    if (!FlushFileBuffers(pipe.get())) {
        throw std::runtime_error(win32_error("FlushFileBuffers(Idd relay)"));
    }
    DisconnectNamedPipe(pipe.get());
    std::cout << "WireView Idd relay session ended after " << frames << " frames\n";
    return 0;
}

int run_idd_frame_relay_pipe_loopback_test() {
    const LoopbackSharedTextures shared_textures = create_loopback_shared_textures();
    std::exception_ptr server_error;
    std::exception_ptr client_error;
    std::uint64_t consumed_frames = 0U;
    std::thread server([&] {
        try {
            const Handle pipe = create_server_pipe();
            accept_pipe_client(pipe.get());
            consumed_frames = handle_one_connection(
                pipe.get(),
                true,
                [](ID3D11Texture2D* texture, const wireview::idd_relay::FrameReady&) {
                    if (texture == nullptr) {
                        throw std::runtime_error("Idd relay Host received a null frame texture");
                    }
                });
            if (!FlushFileBuffers(pipe.get())) {
                throw std::runtime_error(win32_error("FlushFileBuffers(Idd relay loopback)"));
            }
            DisconnectNamedPipe(pipe.get());
        } catch (...) {
            server_error = std::current_exception();
        }
    });
    try {
        const Handle pipe = connect_loopback_client();
        const auto encoded = wireview::idd_relay::encode_message(
            wireview::idd_relay::MessageType::stream_description, shared_textures.stream);
        write_all(pipe.get(), encoded);
        const auto response = read_message(pipe.get());
        if (response.type != wireview::idd_relay::MessageType::consumer_ready ||
            !response.payload.empty()) {
            throw std::runtime_error("Idd relay listener returned an invalid ready acknowledgement");
        }
        const ComPtr<ID3D11Device> driver_device = create_device_for_adapter(
            shared_textures.stream.adapter_luid_low, shared_textures.stream.adapter_luid_high);
        std::array<bool, wireview::idd_relay::kSlotCount> slot_received{};
        std::array<ComPtr<ID3D11Texture2D>, wireview::idd_relay::kSlotCount> driver_textures;
        for (std::uint32_t count = 0U; count < wireview::idd_relay::kSlotCount; ++count) {
            const auto texture_message = read_message(pipe.get());
            const auto texture_name = wireview::idd_relay::parse_shared_texture_name(texture_message);
            if (texture_name.slot >= wireview::idd_relay::kSlotCount ||
                slot_received[texture_name.slot]) {
                throw std::runtime_error("Idd relay listener returned an invalid named texture slot");
            }
            driver_textures[texture_name.slot] = open_shared_texture_by_name(
                driver_device.Get(), texture_name.name, shared_textures.stream);
            slot_received[texture_name.slot] = true;
        }
        write_message(pipe.get(), wireview::idd_relay::MessageType::textures_ready);
        ComPtr<ID3D11DeviceContext> driver_context;
        driver_device->GetImmediateContext(&driver_context);
        for (std::uint64_t sequence = 1U; sequence <= 6U; ++sequence) {
            const std::uint32_t slot = static_cast<std::uint32_t>(
                (sequence - 1U) % wireview::idd_relay::kSlotCount);
            ComPtr<IDXGIKeyedMutex> mutex;
            if (FAILED(driver_textures[slot].As(&mutex)) ||
                FAILED(mutex->AcquireSync(0U, 50U))) {
                throw std::runtime_error("Idd relay simulated driver could not acquire its texture slot");
            }
            ComPtr<ID3D11RenderTargetView> target;
            if (FAILED(driver_device->CreateRenderTargetView(
                    driver_textures[slot].Get(), nullptr, &target))) {
                (void)mutex->ReleaseSync(0U);
                throw std::runtime_error("Idd relay simulated driver could not create a render target");
            }
            const float color[] = {
                static_cast<float>(sequence) / 6.0F, 0.25F, 0.75F, 1.0F};
            driver_context->ClearRenderTargetView(target.Get(), color);
            driver_context->Flush();
            if (FAILED(mutex->ReleaseSync(1U))) {
                throw std::runtime_error("Idd relay simulated driver could not publish a texture slot");
            }
            const wireview::idd_relay::FrameReady ready{
                .slot = slot,
                .sequence = sequence,
                .capture_timestamp_micros = sequence * 1'000U,
            };
            write_message(pipe.get(), wireview::idd_relay::MessageType::frame_ready, ready);
            const auto released_message = read_message(pipe.get());
            const auto released = wireview::idd_relay::parse_payload<wireview::idd_relay::FrameReleased>(
                released_message, wireview::idd_relay::MessageType::frame_released);
            if (released.slot != slot || released.sequence != sequence) {
                throw std::runtime_error("Idd relay Host released the wrong texture slot");
            }
        }
        write_message(pipe.get(), wireview::idd_relay::MessageType::stop_stream);
    } catch (...) {
        client_error = std::current_exception();
    }
    server.join();
    if (server_error) {
        std::rethrow_exception(server_error);
    }
    if (client_error) {
        std::rethrow_exception(client_error);
    }
    if (consumed_frames != 6U) {
        throw std::runtime_error("Idd relay Host did not consume every simulated frame");
    }
    const ComPtr<ID3D11Device> host_device = create_device_for_adapter(
        shared_textures.stream.adapter_luid_low, shared_textures.stream.adapter_luid_high);
    for (std::uint32_t slot = 0U; slot < wireview::idd_relay::kSlotCount; ++slot) {
        const wireview::idd_relay::SharedTexture texture{
            .slot = slot,
            .host_handle = static_cast<std::uint64_t>(
                reinterpret_cast<std::uintptr_t>(shared_textures.duplicated_handles[slot].get())),
        };
        (void)open_shared_texture(host_device.Get(), texture, shared_textures.stream);
    }
    return 0;
}

}  // namespace wireview::host
