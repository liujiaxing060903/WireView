#include "winusb_byte_stream.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winusb.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

namespace wireview::host {
namespace {

constexpr std::size_t kReadBufferSize = 64U * 1024U;

[[noreturn]] void throw_windows_error(const char* operation, DWORD error = GetLastError()) {
    throw std::runtime_error(
        std::string(operation) + " failed: Windows error " + std::to_string(error));
}

DWORD timeout_value(std::chrono::milliseconds timeout) {
    if (timeout.count() < 0) {
        return INFINITE;
    }
    const auto count = static_cast<unsigned long long>(timeout.count());
    return count >= static_cast<unsigned long long>(INFINITE)
        ? INFINITE - 1U
        : static_cast<DWORD>(count);
}

}  // namespace

BulkPipePair select_unique_bulk_pipes(std::span<const UsbPipeCandidate> pipes) {
    BulkPipePair selected{};
    std::size_t input_count = 0;
    std::size_t output_count = 0;
    for (const auto& pipe : pipes) {
        if (!pipe.is_bulk) {
            continue;
        }
        if ((pipe.id & 0x80U) != 0) {
            selected.input = pipe.id;
            ++input_count;
        } else {
            selected.output = pipe.id;
            ++output_count;
        }
    }
    if (input_count != 1U || output_count != 1U) {
        throw std::runtime_error(
            "WinUSB interface must expose exactly one bulk IN and one bulk OUT pipe");
    }
    return selected;
}

struct WinUsbByteStream::Impl final {
    HANDLE file{INVALID_HANDLE_VALUE};
    WINUSB_INTERFACE_HANDLE interface_handle{};
    UCHAR input_pipe{};
    UCHAR output_pipe{};
    HANDLE read_event{};
    HANDLE write_event{};
    OVERLAPPED read_overlapped{};
    OVERLAPPED write_overlapped{};
    std::array<std::uint8_t, kReadBufferSize> read_buffer{};
    std::size_t read_offset{};
    std::size_t read_size{};
    bool read_pending{};
    bool write_pending{};
    std::atomic_bool closing{};
    std::mutex read_mutex;
    std::mutex write_mutex;

    explicit Impl(std::wstring_view device_path) {
        try {
            const std::wstring path(device_path);
            file = CreateFileW(
                path.c_str(),
                GENERIC_READ | GENERIC_WRITE,
                0,
                nullptr,
                OPEN_EXISTING,
                FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED,
                nullptr);
            if (file == INVALID_HANDLE_VALUE) {
                throw_windows_error("CreateFileW(WinUSB accessory)");
            }
            if (WinUsb_Initialize(file, &interface_handle) == FALSE) {
                throw_windows_error("WinUsb_Initialize(accessory)");
            }

            USB_INTERFACE_DESCRIPTOR descriptor{};
            if (WinUsb_QueryInterfaceSettings(interface_handle, 0, &descriptor) == FALSE) {
                throw_windows_error("WinUsb_QueryInterfaceSettings(accessory)");
            }
            std::vector<UsbPipeCandidate> candidates;
            candidates.reserve(descriptor.bNumEndpoints);
            for (UCHAR index = 0; index < descriptor.bNumEndpoints; ++index) {
                WINUSB_PIPE_INFORMATION pipe{};
                if (WinUsb_QueryPipe(interface_handle, 0, index, &pipe) == FALSE) {
                    throw_windows_error("WinUsb_QueryPipe(accessory)");
                }
                candidates.push_back(UsbPipeCandidate{
                    .id = pipe.PipeId,
                    .is_bulk = pipe.PipeType == UsbdPipeTypeBulk,
                });
            }
            const auto pipes = select_unique_bulk_pipes(candidates);
            input_pipe = pipes.input;
            output_pipe = pipes.output;

            UCHAR enabled = TRUE;
            if (WinUsb_SetPipePolicy(
                    interface_handle,
                    input_pipe,
                    ALLOW_PARTIAL_READS,
                    sizeof(enabled),
                    &enabled) == FALSE) {
                throw_windows_error("WinUsb_SetPipePolicy(ALLOW_PARTIAL_READS)");
            }
            if (WinUsb_SetPipePolicy(
                    interface_handle,
                    input_pipe,
                    AUTO_CLEAR_STALL,
                    sizeof(enabled),
                    &enabled) == FALSE) {
                throw_windows_error("WinUsb_SetPipePolicy(input AUTO_CLEAR_STALL)");
            }
            if (WinUsb_SetPipePolicy(
                    interface_handle,
                    output_pipe,
                    AUTO_CLEAR_STALL,
                    sizeof(enabled),
                    &enabled) == FALSE) {
                throw_windows_error("WinUsb_SetPipePolicy(output AUTO_CLEAR_STALL)");
            }
            ULONG transfer_timeout_millis = 10'000U;
            if (WinUsb_SetPipePolicy(
                    interface_handle,
                    input_pipe,
                    PIPE_TRANSFER_TIMEOUT,
                    sizeof(transfer_timeout_millis),
                    &transfer_timeout_millis) == FALSE) {
                throw_windows_error("WinUsb_SetPipePolicy(input timeout)");
            }
            if (WinUsb_SetPipePolicy(
                    interface_handle,
                    output_pipe,
                    PIPE_TRANSFER_TIMEOUT,
                    sizeof(transfer_timeout_millis),
                    &transfer_timeout_millis) == FALSE) {
                throw_windows_error("WinUsb_SetPipePolicy(output timeout)");
            }

            read_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            if (read_event == nullptr) {
                throw_windows_error("CreateEventW(WinUSB read)");
            }
            write_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            if (write_event == nullptr) {
                throw_windows_error("CreateEventW(WinUSB write)");
            }
            read_overlapped.hEvent = read_event;
            write_overlapped.hEvent = write_event;
        } catch (...) {
            close_handles();
            throw;
        }
    }

    ~Impl() {
        closing.store(true, std::memory_order_release);
        if (file != INVALID_HANDLE_VALUE) {
            CancelIoEx(file, nullptr);
        }
        if (read_pending && read_event != nullptr) {
            WaitForSingleObject(read_event, INFINITE);
        }
        if (write_pending && write_event != nullptr) {
            WaitForSingleObject(write_event, INFINITE);
        }
        close_handles();
    }

    void close_handles() noexcept {
        if (read_event != nullptr) {
            CloseHandle(read_event);
            read_event = nullptr;
        }
        if (write_event != nullptr) {
            CloseHandle(write_event);
            write_event = nullptr;
        }
        if (interface_handle != nullptr) {
            WinUsb_Free(interface_handle);
            interface_handle = nullptr;
        }
        if (file != INVALID_HANDLE_VALUE) {
            CloseHandle(file);
            file = INVALID_HANDLE_VALUE;
        }
    }

    void begin_read() {
        if (read_pending || read_offset < read_size) {
            return;
        }
        if (closing.load(std::memory_order_acquire)) {
            throw std::runtime_error("WinUSB byte stream is closing");
        }
        ResetEvent(read_event);
        read_overlapped = {};
        read_overlapped.hEvent = read_event;
        read_pending = true;
        if (WinUsb_ReadPipe(
                interface_handle,
                input_pipe,
                read_buffer.data(),
                static_cast<ULONG>(read_buffer.size()),
                nullptr,
                &read_overlapped) == FALSE) {
            const DWORD error = GetLastError();
            if (error != ERROR_IO_PENDING) {
                read_pending = false;
                throw_windows_error("WinUsb_ReadPipe", error);
            }
        }
    }

    bool finish_read(DWORD timeout) {
        const DWORD wait_result = WaitForSingleObject(read_event, timeout);
        if (wait_result == WAIT_TIMEOUT) {
            return false;
        }
        if (wait_result != WAIT_OBJECT_0) {
            throw_windows_error("WaitForSingleObject(WinUSB read)");
        }
        ULONG transferred = 0;
        if (WinUsb_GetOverlappedResult(
                interface_handle,
                &read_overlapped,
                &transferred,
                FALSE) == FALSE) {
            const DWORD error = GetLastError();
            read_pending = false;
            if (error == ERROR_OPERATION_ABORTED && closing.load(std::memory_order_acquire)) {
                return true;
            }
            throw_windows_error("WinUsb_GetOverlappedResult(read)", error);
        }
        read_pending = false;
        read_offset = 0;
        read_size = transferred;
        return true;
    }
};

std::unique_ptr<WinUsbByteStream> WinUsbByteStream::open(std::wstring_view device_path) {
    if (device_path.empty()) {
        throw std::invalid_argument("WinUSB device path must not be empty");
    }
    return std::unique_ptr<WinUsbByteStream>(
        new WinUsbByteStream(std::make_unique<Impl>(device_path)));
}

WinUsbByteStream::WinUsbByteStream(std::unique_ptr<Impl> implementation) noexcept
    : implementation_(std::move(implementation)) {
}

WinUsbByteStream::~WinUsbByteStream() = default;

std::size_t WinUsbByteStream::read(std::span<std::uint8_t> destination) {
    if (destination.empty()) {
        return 0;
    }
    std::scoped_lock lock(implementation_->read_mutex);
    if (implementation_->read_offset == implementation_->read_size) {
        implementation_->begin_read();
        (void)implementation_->finish_read(INFINITE);
    }
    const std::size_t available = implementation_->read_size - implementation_->read_offset;
    const std::size_t count = std::min(destination.size(), available);
    std::copy_n(
        implementation_->read_buffer.begin() +
            static_cast<std::ptrdiff_t>(implementation_->read_offset),
        count,
        destination.begin());
    implementation_->read_offset += count;
    return count;
}

std::size_t WinUsbByteStream::write(std::span<const std::uint8_t> source) {
    if (source.empty()) {
        return 0;
    }
    if (source.size() > std::numeric_limits<ULONG>::max()) {
        throw std::invalid_argument("WinUSB write exceeds ULONG transfer size");
    }
    std::scoped_lock lock(implementation_->write_mutex);
    if (implementation_->closing.load(std::memory_order_acquire)) {
        throw std::runtime_error("WinUSB byte stream is closing");
    }

    ResetEvent(implementation_->write_event);
    implementation_->write_overlapped = {};
    implementation_->write_overlapped.hEvent = implementation_->write_event;
    implementation_->write_pending = true;
    if (WinUsb_WritePipe(
            implementation_->interface_handle,
            implementation_->output_pipe,
            const_cast<PUCHAR>(source.data()),
            static_cast<ULONG>(source.size()),
            nullptr,
            &implementation_->write_overlapped) == FALSE) {
        const DWORD error = GetLastError();
        if (error != ERROR_IO_PENDING) {
            implementation_->write_pending = false;
            throw_windows_error("WinUsb_WritePipe", error);
        }
    }
    const DWORD wait_result = WaitForSingleObject(implementation_->write_event, INFINITE);
    if (wait_result != WAIT_OBJECT_0) {
        implementation_->write_pending = false;
        throw_windows_error("WaitForSingleObject(WinUSB write)");
    }
    ULONG transferred = 0;
    if (WinUsb_GetOverlappedResult(
            implementation_->interface_handle,
            &implementation_->write_overlapped,
            &transferred,
            FALSE) == FALSE) {
        const DWORD error = GetLastError();
        implementation_->write_pending = false;
        throw_windows_error("WinUsb_GetOverlappedResult(write)", error);
    }
    implementation_->write_pending = false;
    return transferred;
}

bool WinUsbByteStream::wait_readable(std::chrono::milliseconds timeout) {
    std::scoped_lock lock(implementation_->read_mutex);
    if (implementation_->read_offset < implementation_->read_size) {
        return true;
    }
    implementation_->begin_read();
    return implementation_->finish_read(timeout_value(timeout));
}

}  // namespace wireview::host
