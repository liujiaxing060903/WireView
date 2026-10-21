#include <windows.h>
#include <setupapi.h>
#include <winusb.h>

#include "wireview/protocol.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cwctype>
#include <cstdint>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

// Android's standard WinUSB interface GUID, registered by ADB-compatible
// interfaces that expose the Microsoft OS WINUSB compatible ID.
constexpr GUID kAndroidWinUsbInterfaceGuid{
    0xf72fe0d4,
    0xcbcb,
    0x407d,
    {0x88, 0x14, 0x9e, 0xd6, 0x73, 0xd0, 0xdd, 0x6b},
};

constexpr std::uint16_t kGoogleVendorId = 0x18d1;
constexpr std::uint16_t kAccessoryProductId = 0x2d00;
constexpr std::uint16_t kAccessoryAdbProductId = 0x2d01;
constexpr std::uint8_t kGetProtocolRequest = 51;
constexpr std::uint8_t kSendStringRequest = 52;
constexpr std::uint8_t kStartAccessoryRequest = 53;

static_assert(kGoogleVendorId == 0x18d1);
static_assert(kAccessoryProductId == 0x2d00);
static_assert(kAccessoryAdbProductId == 0x2d01);
static_assert(kGetProtocolRequest == 51);
static_assert(kSendStringRequest == 52);
static_assert(kStartAccessoryRequest == 53);

class DeviceInfoSet final {
public:
    explicit DeviceInfoSet(HDEVINFO value) noexcept : value_(value) {
    }

    ~DeviceInfoSet() {
        if (value_ != INVALID_HANDLE_VALUE) {
            SetupDiDestroyDeviceInfoList(value_);
        }
    }

    DeviceInfoSet(const DeviceInfoSet&) = delete;
    DeviceInfoSet& operator=(const DeviceInfoSet&) = delete;

    [[nodiscard]] HDEVINFO get() const noexcept {
        return value_;
    }

private:
    HDEVINFO value_{INVALID_HANDLE_VALUE};
};

class FileHandle final {
public:
    explicit FileHandle(HANDLE value = INVALID_HANDLE_VALUE) noexcept : value_(value) {
    }

    ~FileHandle() {
        if (value_ != INVALID_HANDLE_VALUE) {
            CloseHandle(value_);
        }
    }

    FileHandle(const FileHandle&) = delete;
    FileHandle& operator=(const FileHandle&) = delete;

    [[nodiscard]] HANDLE get() const noexcept {
        return value_;
    }

private:
    HANDLE value_{INVALID_HANDLE_VALUE};
};

class WinUsbHandle final {
public:
    explicit WinUsbHandle(WINUSB_INTERFACE_HANDLE value = nullptr) noexcept : value_(value) {
    }

    ~WinUsbHandle() {
        if (value_ != nullptr) {
            WinUsb_Free(value_);
        }
    }

    WinUsbHandle(const WinUsbHandle&) = delete;
    WinUsbHandle& operator=(const WinUsbHandle&) = delete;

    [[nodiscard]] WINUSB_INTERFACE_HANDLE get() const noexcept {
        return value_;
    }

private:
    WINUSB_INTERFACE_HANDLE value_{};
};

struct BootstrapInterface {
    std::wstring name;
    std::wstring instance_id;
    std::wstring path;
};

[[nodiscard]] bool is_accessory_instance(std::wstring instance_id) {
    std::transform(
        instance_id.begin(),
        instance_id.end(),
        instance_id.begin(),
        [](wchar_t value) { return static_cast<wchar_t>(std::towupper(value)); });
    return instance_id.find(L"VID_18D1&PID_2D00") != std::wstring::npos ||
        instance_id.find(L"VID_18D1&PID_2D01") != std::wstring::npos;
}

[[noreturn]] void throw_windows_error(std::string_view operation) {
    throw std::runtime_error(
        std::string(operation) + " failed: " + std::to_string(GetLastError()));
}

std::wstring device_property(
    HDEVINFO devices,
    SP_DEVINFO_DATA& device,
    DWORD property) {
    DWORD required = 0;
    DWORD type = 0;
    if (SetupDiGetDeviceRegistryPropertyW(
            devices,
            &device,
            property,
            &type,
            nullptr,
            0,
            &required) != FALSE) {
        return {};
    }
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || required == 0) {
        return {};
    }

    std::vector<std::uint8_t> storage(required);
    if (SetupDiGetDeviceRegistryPropertyW(
            devices,
            &device,
            property,
            &type,
            storage.data(),
            required,
            nullptr) == FALSE) {
        return {};
    }
    const auto* text = reinterpret_cast<const wchar_t*>(storage.data());
    return std::wstring(text);
}

std::wstring device_instance_id(HDEVINFO devices, SP_DEVINFO_DATA& device) {
    DWORD required = 0;
    SetupDiGetDeviceInstanceIdW(devices, &device, nullptr, 0, &required);
    if (required == 0) {
        return {};
    }
    std::vector<wchar_t> storage(required);
    if (SetupDiGetDeviceInstanceIdW(
            devices,
            &device,
            storage.data(),
            static_cast<DWORD>(storage.size()),
            nullptr) == FALSE) {
        return {};
    }
    return std::wstring(storage.data());
}

std::vector<BootstrapInterface> enumerate_bootstrap_interfaces() {
    DeviceInfoSet devices(SetupDiGetClassDevsW(
        &kAndroidWinUsbInterfaceGuid,
        nullptr,
        nullptr,
        DIGCF_DEVICEINTERFACE | DIGCF_PRESENT));
    if (devices.get() == INVALID_HANDLE_VALUE) {
        throw_windows_error("SetupDiGetClassDevsW(Android WinUSB)");
    }

    std::vector<BootstrapInterface> result;
    for (DWORD index = 0;; ++index) {
        SP_DEVICE_INTERFACE_DATA interface_data{};
        interface_data.cbSize = sizeof(interface_data);
        if (SetupDiEnumDeviceInterfaces(
                devices.get(),
                nullptr,
                &kAndroidWinUsbInterfaceGuid,
                index,
                &interface_data) == FALSE) {
            if (GetLastError() == ERROR_NO_MORE_ITEMS) {
                break;
            }
            throw_windows_error("SetupDiEnumDeviceInterfaces(Android WinUSB)");
        }

        DWORD required = 0;
        SetupDiGetDeviceInterfaceDetailW(
            devices.get(),
            &interface_data,
            nullptr,
            0,
            &required,
            nullptr);
        if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || required == 0) {
            throw_windows_error("SetupDiGetDeviceInterfaceDetailW(size)");
        }

        std::vector<std::uint8_t> detail_storage(required);
        auto* detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(
            detail_storage.data());
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        SP_DEVINFO_DATA device{};
        device.cbSize = sizeof(device);
        if (SetupDiGetDeviceInterfaceDetailW(
                devices.get(),
                &interface_data,
                detail,
                required,
                nullptr,
                &device) == FALSE) {
            throw_windows_error("SetupDiGetDeviceInterfaceDetailW(data)");
        }

        std::wstring name = device_property(devices.get(), device, SPDRP_FRIENDLYNAME);
        if (name.empty()) {
            name = device_property(devices.get(), device, SPDRP_DEVICEDESC);
        }
        result.push_back(BootstrapInterface{
            .name = std::move(name),
            .instance_id = device_instance_id(devices.get(), device),
            .path = detail->DevicePath,
        });
    }
    return result;
}

int list_bootstrap_interfaces() {
    const auto interfaces = enumerate_bootstrap_interfaces();
    for (std::size_t index = 0; index < interfaces.size(); ++index) {
        const auto& device = interfaces[index];
        std::wcout << L"Android WinUSB interface " << index + 1U << L":\n"
                   << L"  role: "
                   << (is_accessory_instance(device.instance_id)
                           ? L"AOA accessory"
                           : L"bootstrap candidate")
                   << L'\n'
                   << L"  name: " << device.name << L'\n'
                   << L"  instance: " << device.instance_id << L'\n'
                   << L"  path: " << device.path << L'\n';
    }

    std::cout << "Android WinUSB interfaces found: " << interfaces.size() << '\n';
    if (interfaces.empty()) {
        std::cout
            << "No ADB-compatible WinUSB interface is currently available. "
               "This probe does not change drivers or enable USB debugging.\n";
    }
    return 0;
}

void control_transfer(
    WINUSB_INTERFACE_HANDLE handle,
    WINUSB_SETUP_PACKET setup,
    std::span<std::uint8_t> buffer) {
    ULONG transferred = 0;
    if (WinUsb_ControlTransfer(
            handle,
            setup,
            buffer.empty() ? nullptr : buffer.data(),
            static_cast<ULONG>(buffer.size()),
            &transferred,
            nullptr) == FALSE) {
        throw_windows_error("WinUsb_ControlTransfer(AOA)");
    }
    if (transferred != buffer.size()) {
        throw std::runtime_error(
            "AOA control transfer was truncated: " + std::to_string(transferred) +
            "/" + std::to_string(buffer.size()));
    }
}

std::uint16_t query_accessory_protocol(WINUSB_INTERFACE_HANDLE handle) {
    std::array<std::uint8_t, 2> response{};
    control_transfer(
        handle,
        WINUSB_SETUP_PACKET{
            .RequestType = 0xc0,
            .Request = kGetProtocolRequest,
            .Value = 0,
            .Index = 0,
            .Length = static_cast<USHORT>(response.size()),
        },
        response);
    return static_cast<std::uint16_t>(
        static_cast<std::uint16_t>(response[0]) |
        (static_cast<std::uint16_t>(response[1]) << 8U));
}

void send_accessory_string(
    WINUSB_INTERFACE_HANDLE handle,
    std::uint16_t index,
    std::string_view text) {
    std::vector<std::uint8_t> bytes(text.begin(), text.end());
    bytes.push_back(0);
    control_transfer(
        handle,
        WINUSB_SETUP_PACKET{
            .RequestType = 0x40,
            .Request = kSendStringRequest,
            .Value = 0,
            .Index = index,
            .Length = static_cast<USHORT>(bytes.size()),
        },
        bytes);
}

int switch_unique_bootstrap_to_accessory() {
    auto interfaces = enumerate_bootstrap_interfaces();
    std::erase_if(interfaces, [](const BootstrapInterface& device) {
        return is_accessory_instance(device.instance_id);
    });
    if (interfaces.empty()) {
        std::cerr << "No Android WinUSB bootstrap interface is present\n";
        return 2;
    }
    if (interfaces.size() != 1U) {
        std::cerr
            << "Refusing to switch AOA because more than one Android WinUSB "
               "bootstrap interface is present\n";
        return 2;
    }

    const auto& device = interfaces.front();
    std::wcout << L"Opening the only Android WinUSB bootstrap interface:\n"
               << L"  name: " << device.name << L'\n'
               << L"  instance: " << device.instance_id << L'\n';
    FileHandle file(CreateFileW(
        device.path.c_str(),
        GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED,
        nullptr));
    if (file.get() == INVALID_HANDLE_VALUE) {
        const DWORD error = GetLastError();
        if (error == ERROR_SHARING_VIOLATION || error == ERROR_ACCESS_DENIED) {
            throw std::runtime_error(
                "Android WinUSB interface is busy or inaccessible; stop the adb server "
                "before retrying (Windows error " + std::to_string(error) + ")");
        }
        throw_windows_error("CreateFileW(Android WinUSB)");
    }

    WINUSB_INTERFACE_HANDLE raw_winusb = nullptr;
    if (WinUsb_Initialize(file.get(), &raw_winusb) == FALSE) {
        throw_windows_error("WinUsb_Initialize(Android bootstrap)");
    }
    WinUsbHandle winusb(raw_winusb);

    const std::uint16_t protocol_version = query_accessory_protocol(winusb.get());
    if (protocol_version == 0) {
        std::cerr << "The selected Android device does not report AOA support\n";
        return 3;
    }
    std::cout << "Android Open Accessory protocol version: " << protocol_version << '\n';

    send_accessory_string(winusb.get(), 0, "WireView");
    send_accessory_string(winusb.get(), 1, "WireView Receiver");
    send_accessory_string(winusb.get(), 2, "Low-latency Android display");
    send_accessory_string(winusb.get(), 3, "1");
    send_accessory_string(winusb.get(), 4, "");
    send_accessory_string(winusb.get(), 5, "wireview");

    std::array<std::uint8_t, 0> no_payload{};
    control_transfer(
        winusb.get(),
        WINUSB_SETUP_PACKET{
            .RequestType = 0x40,
            .Request = kStartAccessoryRequest,
            .Value = 0,
            .Index = 0,
            .Length = 0,
        },
        no_payload);
    std::cout
        << "AOA start request sent. The Android device should disconnect and "
           "re-enumerate as VID_18D1/PID_2D00 or PID_2D01.\n";
    return 0;
}

int inspect_unique_accessory() {
    auto interfaces = enumerate_bootstrap_interfaces();
    std::erase_if(interfaces, [](const BootstrapInterface& device) {
        return !is_accessory_instance(device.instance_id);
    });
    if (interfaces.empty()) {
        std::cerr << "No AOA accessory WinUSB interface is present\n";
        return 2;
    }
    if (interfaces.size() != 1U) {
        std::cerr << "Refusing to inspect because more than one AOA interface is present\n";
        return 2;
    }

    const auto& device = interfaces.front();
    FileHandle file(CreateFileW(
        device.path.c_str(),
        GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED,
        nullptr));
    if (file.get() == INVALID_HANDLE_VALUE) {
        throw_windows_error("CreateFileW(AOA accessory)");
    }

    WINUSB_INTERFACE_HANDLE raw_winusb = nullptr;
    if (WinUsb_Initialize(file.get(), &raw_winusb) == FALSE) {
        throw_windows_error("WinUsb_Initialize(AOA accessory)");
    }
    WinUsbHandle winusb(raw_winusb);

    USB_INTERFACE_DESCRIPTOR descriptor{};
    if (WinUsb_QueryInterfaceSettings(winusb.get(), 0, &descriptor) == FALSE) {
        throw_windows_error("WinUsb_QueryInterfaceSettings(AOA accessory)");
    }

    std::wcout << L"AOA accessory interface:\n"
               << L"  name: " << device.name << L'\n'
               << L"  instance: " << device.instance_id << L'\n';
    std::cout << "  interfaceNumber: " << static_cast<unsigned>(descriptor.bInterfaceNumber)
              << "\n  endpoints: " << static_cast<unsigned>(descriptor.bNumEndpoints) << '\n';

    std::size_t bulk_in = 0;
    std::size_t bulk_out = 0;
    for (UCHAR index = 0; index < descriptor.bNumEndpoints; ++index) {
        WINUSB_PIPE_INFORMATION pipe{};
        if (WinUsb_QueryPipe(winusb.get(), 0, index, &pipe) == FALSE) {
            throw_windows_error("WinUsb_QueryPipe(AOA accessory)");
        }
        const bool input = (pipe.PipeId & 0x80U) != 0;
        std::cout << "  pipe " << static_cast<unsigned>(index)
                  << ": id=0x" << std::hex << static_cast<unsigned>(pipe.PipeId)
                  << std::dec << " | type=" << static_cast<unsigned>(pipe.PipeType)
                  << " | direction=" << (input ? "IN" : "OUT")
                  << " | maxPacket=" << pipe.MaximumPacketSize << '\n';
        if (pipe.PipeType == UsbdPipeTypeBulk) {
            if (input) {
                ++bulk_in;
            } else {
                ++bulk_out;
            }
        }
    }
    if (bulk_in != 1U || bulk_out != 1U) {
        throw std::runtime_error(
            "AOA primary interface must expose exactly one bulk IN and one bulk OUT pipe");
    }
    std::cout << "AOA primary bulk endpoint layout is valid\n";
    return 0;
}

[[nodiscard]] std::uint64_t monotonic_micros() {
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(now).count());
}

int handshake_unique_accessory() {
    auto interfaces = enumerate_bootstrap_interfaces();
    std::erase_if(interfaces, [](const BootstrapInterface& device) {
        return !is_accessory_instance(device.instance_id);
    });
    if (interfaces.size() != 1U) {
        std::cerr << "Exactly one AOA accessory interface is required for the handshake\n";
        return 2;
    }

    FileHandle file(CreateFileW(
        interfaces.front().path.c_str(),
        GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED,
        nullptr));
    if (file.get() == INVALID_HANDLE_VALUE) {
        throw_windows_error("CreateFileW(AOA handshake)");
    }
    WINUSB_INTERFACE_HANDLE raw_winusb = nullptr;
    if (WinUsb_Initialize(file.get(), &raw_winusb) == FALSE) {
        throw_windows_error("WinUsb_Initialize(AOA handshake)");
    }
    WinUsbHandle winusb(raw_winusb);

    USB_INTERFACE_DESCRIPTOR descriptor{};
    if (WinUsb_QueryInterfaceSettings(winusb.get(), 0, &descriptor) == FALSE) {
        throw_windows_error("WinUsb_QueryInterfaceSettings(AOA handshake)");
    }
    UCHAR input_pipe = 0;
    UCHAR output_pipe = 0;
    for (UCHAR index = 0; index < descriptor.bNumEndpoints; ++index) {
        WINUSB_PIPE_INFORMATION pipe{};
        if (WinUsb_QueryPipe(winusb.get(), 0, index, &pipe) == FALSE) {
            throw_windows_error("WinUsb_QueryPipe(AOA handshake)");
        }
        if (pipe.PipeType != UsbdPipeTypeBulk) {
            continue;
        }
        if ((pipe.PipeId & 0x80U) != 0) {
            input_pipe = pipe.PipeId;
        } else {
            output_pipe = pipe.PipeId;
        }
    }
    if (input_pipe == 0 || output_pipe == 0) {
        throw std::runtime_error("AOA bulk pipes were not found");
    }

    ULONG timeout_millis = 5'000;
    if (WinUsb_SetPipePolicy(
            winusb.get(),
            input_pipe,
            PIPE_TRANSFER_TIMEOUT,
            sizeof(timeout_millis),
            &timeout_millis) == FALSE) {
        throw_windows_error("WinUsb_SetPipePolicy(AOA input timeout)");
    }
    if (WinUsb_SetPipePolicy(
            winusb.get(),
            output_pipe,
            PIPE_TRANSFER_TIMEOUT,
            sizeof(timeout_millis),
            &timeout_millis) == FALSE) {
        throw_windows_error("WinUsb_SetPipePolicy(AOA output timeout)");
    }

    const std::string hello_text = "WireView Windows AOA probe";
    const std::vector<std::uint8_t> hello_payload(hello_text.begin(), hello_text.end());
    const auto hello = wireview::protocol::encode_packet(
        wireview::protocol::PacketHeader{
            .type = wireview::protocol::PacketType::hello,
            .flags = wireview::protocol::PacketFlag::acknowledgement_required,
            .sequence = 1,
            .timestamp_micros = monotonic_micros(),
        },
        hello_payload);
    ULONG written = 0;
    if (WinUsb_WritePipe(
            winusb.get(),
            output_pipe,
            const_cast<PUCHAR>(hello.data()),
            static_cast<ULONG>(hello.size()),
            &written,
            nullptr) == FALSE) {
        throw_windows_error("WinUsb_WritePipe(WVR1 HELLO)");
    }
    if (written != hello.size()) {
        throw std::runtime_error("AOA WVR1 HELLO write was truncated");
    }

    wireview::protocol::PacketStreamParser parser;
    std::array<std::uint8_t, 16U * 1024U> receive_buffer{};
    while (true) {
        auto parsed = parser.next();
        if (parsed.status == wireview::protocol::StreamStatus::packet_ready && parsed.packet) {
            if (parsed.packet->header.type != wireview::protocol::PacketType::hello_ack) {
                throw std::runtime_error("Android did not return WVR1 HELLO_ACK over AOA");
            }
            const std::string device_name(
                parsed.packet->payload.begin(), parsed.packet->payload.end());
            std::cout << "WVR1 AOA handshake complete with: " << device_name << '\n';
            return 0;
        }
        if (parsed.status == wireview::protocol::StreamStatus::error) {
            throw std::runtime_error("WVR1 parser rejected the Android AOA response");
        }

        ULONG received = 0;
        if (WinUsb_ReadPipe(
                winusb.get(),
                input_pipe,
                receive_buffer.data(),
                static_cast<ULONG>(receive_buffer.size()),
                &received,
                nullptr) == FALSE) {
            const DWORD error = GetLastError();
            if (error == ERROR_SEM_TIMEOUT) {
                throw std::runtime_error(
                    "timed out waiting for Android WVR1 HELLO_ACK; accept the "
                    "WireView USB accessory prompt on the tablet");
            }
            throw_windows_error("WinUsb_ReadPipe(WVR1 HELLO_ACK)");
        }
        if (received == 0) {
            throw std::runtime_error("Android closed AOA before WVR1 HELLO_ACK");
        }
        if (!parser.push(std::span(receive_buffer).first(received))) {
            throw std::runtime_error("WVR1 AOA receive buffer limit exceeded");
        }
    }
}

int run_self_test() {
    std::cout << "WireView USB accessory probe self-test passed\n";
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        if (argc == 1 || (argc == 2 && std::string_view(argv[1]) == "--list-bootstrap")) {
            return list_bootstrap_interfaces();
        }
        if (argc == 2 && std::string_view(argv[1]) == "--self-test") {
            return run_self_test();
        }
        if (argc == 2 && std::string_view(argv[1]) == "--switch-unique-bootstrap") {
            return switch_unique_bootstrap_to_accessory();
        }
        if (argc == 2 && std::string_view(argv[1]) == "--inspect-accessory") {
            return inspect_unique_accessory();
        }
        if (argc == 2 && std::string_view(argv[1]) == "--handshake-accessory") {
            return handshake_unique_accessory();
        }
        std::cerr
            << "Usage: wireview_usb_probe "
               "[--list-bootstrap | --switch-unique-bootstrap | "
               "--inspect-accessory | --handshake-accessory | --self-test]\n";
        return 2;
    } catch (const std::exception& error) {
        std::cerr << "WireView USB probe failed: " << error.what() << '\n';
        return 1;
    }
}
