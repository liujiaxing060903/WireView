/*++

Copyright (c) Microsoft Corporation

Abstract:

    This module contains a sample implementation of an indirect display driver. See the included README.md file and the
    various TODO blocks throughout this file and all accompanying files for information on building a production driver.

    MSDN documentation on indirect displays can be found at https://msdn.microsoft.com/en-us/library/windows/hardware/mt761968(v=vs.85).aspx.

Environment:

    User Mode, UMDF

--*/

#include "Driver.h"

#include <array>
#include <cstdio>
#include <cstring>
#include <string>

// WireView phase-0 spike: WPP generation belongs to the full WDK driver
// packaging pipeline. This sample currently has no Trace/TraceEvents calls, so
// keeping the generated Driver.tmh dependency would add no runtime value while
// preventing a package-local WDK compile on clean machines.

using namespace std;
using namespace Microsoft::IndirectDisp;
using namespace Microsoft::WRL;

namespace
{
void WireViewLogStatus(const char* stage, NTSTATUS status)
{
    HANDLE file = CreateFileW(
        L"C:\\Windows\\Temp\\WireViewIddDriver.log",
        FILE_APPEND_DATA,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (file == INVALID_HANDLE_VALUE)
    {
        return;
    }

    SYSTEMTIME now = {};
    GetSystemTime(&now);
    char line[256] = {};
    const int length = sprintf_s(
        line,
        "%04u-%02u-%02uT%02u:%02u:%02u.%03uZ %s status=0x%08lX\r\n",
        now.wYear,
        now.wMonth,
        now.wDay,
        now.wHour,
        now.wMinute,
        now.wSecond,
        now.wMilliseconds,
        stage,
        static_cast<unsigned long>(status));
    if (length > 0)
    {
        DWORD written = 0;
        WriteFile(file, line, static_cast<DWORD>(length), &written, nullptr);
    }
    CloseHandle(file);
}
}

namespace Microsoft
{
namespace IndirectDisp
{
class FrameRelayTextureRing;

bool OpenNamedRelayTextures(
    FrameRelayTextureRing* Ring,
    ID3D11Device* Device,
    const wireview::idd_relay::StreamDescription& Description,
    const std::array<std::wstring, wireview::idd_relay::kSlotCount>& Names);
bool TryDequeueRelayFrame(
    FrameRelayTextureRing* Ring,
    wireview::idd_relay::FrameReady* Ready);
HANDLE RelayFrameReadyEvent(FrameRelayTextureRing* Ring);

// The IddCx swap-chain loop must never wait for a user-mode process. This
// negotiator runs separately, retries at a low rate while the Host is absent,
// and only records the Host acknowledgement for the later shared-texture step.
class FrameRelayNegotiator
{
public:
    FrameRelayNegotiator(
        const wireview::idd_relay::StreamDescription& Description,
        ID3D11Device* Device,
        FrameRelayTextureRing* Ring)
        : m_Description(Description), m_Device(Device), m_Ring(Ring)
    {
        m_TerminateEvent.Attach(CreateEvent(nullptr, TRUE, FALSE, nullptr));
        if (!m_TerminateEvent.Get())
        {
            return;
        }
        m_Thread.Attach(CreateThread(nullptr, 0, RunThread, this, 0, nullptr));
    }

    ~FrameRelayNegotiator()
    {
        if (m_TerminateEvent.Get())
        {
            SetEvent(m_TerminateEvent.Get());
        }
        if (m_Thread.Get())
        {
            WaitForSingleObject(m_Thread.Get(), INFINITE);
        }
    }

private:
    static DWORD CALLBACK RunThread(LPVOID Argument)
    {
        reinterpret_cast<FrameRelayNegotiator*>(Argument)->Run();
        return 0;
    }

    bool IsTerminating() const
    {
        return WaitForSingleObject(m_TerminateEvent.Get(), 0U) == WAIT_OBJECT_0;
    }

    static bool WriteExact(HANDLE Pipe, const void* Data, DWORD Length)
    {
        const BYTE* bytes = static_cast<const BYTE*>(Data);
        DWORD offset = 0U;
        while (offset < Length)
        {
            DWORD written = 0U;
            if (!WriteFile(Pipe, bytes + offset, Length - offset, &written, nullptr) || written == 0U)
            {
                return false;
            }
            offset += written;
        }
        return true;
    }

    bool ReadExactWhenAvailable(HANDLE Pipe, void* Data, DWORD Length)
    {
        BYTE* bytes = static_cast<BYTE*>(Data);
        for (;;)
        {
            if (IsTerminating())
            {
                return false;
            }
            DWORD available = 0U;
            if (!PeekNamedPipe(Pipe, nullptr, 0U, nullptr, &available, nullptr))
            {
                return false;
            }
            if (available >= Length)
            {
                DWORD received = 0U;
                return ReadFile(Pipe, bytes, Length, &received, nullptr) != FALSE && received == Length;
            }
            WaitForSingleObject(m_TerminateEvent.Get(), 10U);
        }
    }

    bool AwaitConsumerReady(HANDLE Pipe)
    {
        constexpr DWORD kHeaderSize = static_cast<DWORD>(sizeof(wireview::idd_relay::MessageHeader));
        for (;;)
        {
            if (IsTerminating())
            {
                return false;
            }
            DWORD available = 0U;
            if (!PeekNamedPipe(Pipe, nullptr, 0U, nullptr, &available, nullptr))
            {
                return false;
            }
            if (available >= kHeaderSize)
            {
                wireview::idd_relay::MessageHeader header = {};
                DWORD received = 0U;
                if (!ReadFile(Pipe, &header, kHeaderSize, &received, nullptr) || received != kHeaderSize)
                {
                    return false;
                }
                return header.magic == wireview::idd_relay::kMagic &&
                    header.version == wireview::idd_relay::kProtocolVersion &&
                    header.type == wireview::idd_relay::MessageType::consumer_ready &&
                    header.payload_size == 0U;
            }
            WaitForSingleObject(m_TerminateEvent.Get(), 10U);
        }
    }

    bool ReadSharedTextureName(HANDLE Pipe, UINT* Slot, std::wstring* Name)
    {
        if (Slot == nullptr || Name == nullptr)
        {
            return false;
        }
        wireview::idd_relay::MessageHeader message = {};
        if (!ReadExactWhenAvailable(Pipe, &message, sizeof(message)) ||
            message.magic != wireview::idd_relay::kMagic ||
            message.version != wireview::idd_relay::kProtocolVersion ||
            message.type != wireview::idd_relay::MessageType::shared_texture_name ||
            message.payload_size < sizeof(wireview::idd_relay::SharedTextureNameHeader) ||
            message.payload_size > sizeof(wireview::idd_relay::SharedTextureNameHeader) + 256U * sizeof(wchar_t))
        {
            return false;
        }
        std::array<BYTE, sizeof(wireview::idd_relay::SharedTextureNameHeader) + 256U * sizeof(wchar_t)> payload = {};
        if (!ReadExactWhenAvailable(Pipe, payload.data(), message.payload_size))
        {
            return false;
        }
        wireview::idd_relay::SharedTextureNameHeader header = {};
        memcpy(&header, payload.data(), sizeof(header));
        if (header.slot >= wireview::idd_relay::kSlotCount || header.utf16_code_units == 0U ||
            message.payload_size != sizeof(header) + header.utf16_code_units * sizeof(wchar_t))
        {
            return false;
        }
        const wchar_t* characters = reinterpret_cast<const wchar_t*>(payload.data() + sizeof(header));
        Name->assign(characters, header.utf16_code_units);
        if (Name->find(L'\0') != std::wstring::npos ||
            Name->rfind(L"Global\\WireViewIdd-", 0U) != 0U)
        {
            return false;
        }
        *Slot = header.slot;
        return true;
    }

    bool AwaitFrameReleased(
        HANDLE Pipe,
        const wireview::idd_relay::FrameReady& Expected)
    {
        wireview::idd_relay::MessageHeader message = {};
        wireview::idd_relay::FrameReleased released = {};
        return ReadExactWhenAvailable(Pipe, &message, sizeof(message)) &&
            message.magic == wireview::idd_relay::kMagic &&
            message.version == wireview::idd_relay::kProtocolVersion &&
            message.type == wireview::idd_relay::MessageType::frame_released &&
            message.payload_size == sizeof(released) &&
            ReadExactWhenAvailable(Pipe, &released, sizeof(released)) &&
            released.slot == Expected.slot && released.sequence == Expected.sequence;
    }

    bool StreamFrames(HANDLE Pipe)
    {
        for (;;)
        {
            if (IsTerminating())
            {
                wireview::idd_relay::MessageHeader stop = {};
                stop.type = wireview::idd_relay::MessageType::stop_stream;
                (void)WriteExact(Pipe, &stop, sizeof(stop));
                return true;
            }
            wireview::idd_relay::FrameReady ready = {};
            if (TryDequeueRelayFrame(m_Ring, &ready))
            {
                wireview::idd_relay::MessageHeader header = {};
                header.type = wireview::idd_relay::MessageType::frame_ready;
                header.payload_size = sizeof(ready);
                if (!WriteExact(Pipe, &header, sizeof(header)) ||
                    !WriteExact(Pipe, &ready, sizeof(ready)) ||
                    !AwaitFrameReleased(Pipe, ready))
                {
                    return false;
                }
                continue;
            }
            HANDLE wait_handles[] = {m_TerminateEvent.Get(), RelayFrameReadyEvent(m_Ring)};
            if (wait_handles[1] == nullptr ||
                WaitForMultipleObjects(ARRAYSIZE(wait_handles), wait_handles, FALSE, 25U) == WAIT_FAILED)
            {
                return false;
            }
        }
    }

    bool NegotiateOnce()
    {
        HANDLE pipe = CreateFileW(
            wireview::idd_relay::kPipeName,
            GENERIC_READ | GENERIC_WRITE,
            0U,
            nullptr,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL,
            nullptr);
        if (pipe == INVALID_HANDLE_VALUE)
        {
            return false;
        }

        wireview::idd_relay::MessageHeader header = {};
        header.type = wireview::idd_relay::MessageType::stream_description;
        header.payload_size = sizeof(m_Description);
        bool completed = WriteExact(pipe, &header, sizeof(header)) &&
            WriteExact(pipe, &m_Description, sizeof(m_Description)) &&
            AwaitConsumerReady(pipe);
        std::array<std::wstring, wireview::idd_relay::kSlotCount> names;
        std::array<bool, wireview::idd_relay::kSlotCount> received = {};
        for (UINT count = 0U; completed && count < wireview::idd_relay::kSlotCount; ++count)
        {
            UINT slot = 0U;
            std::wstring name;
            completed = ReadSharedTextureName(pipe, &slot, &name);
            if (completed && received[slot])
            {
                completed = false;
            }
            if (completed)
            {
                received[slot] = true;
                names[slot] = std::move(name);
            }
        }
        if (completed)
        {
            completed = OpenNamedRelayTextures(m_Ring, m_Device.Get(), m_Description, names);
        }
        if (completed)
        {
            wireview::idd_relay::MessageHeader ready = {};
            ready.type = wireview::idd_relay::MessageType::textures_ready;
            completed = WriteExact(pipe, &ready, sizeof(ready));
        }
        if (completed)
        {
            m_OpenedSession = true;
            completed = StreamFrames(pipe);
        }
        CloseHandle(pipe);
        return completed;
    }

    void Run()
    {
        while (!IsTerminating())
        {
            if (NegotiateOnce())
            {
                return;
            }
            // A resource ring is intentionally single-session. If the Host
            // disconnects after opening it, a future swap-chain assignment
            // constructs a fresh ring; do not race the display thread by
            // replacing D3D resources under it.
            if (m_OpenedSession)
            {
                return;
            }
            WaitForSingleObject(m_TerminateEvent.Get(), 1'000U);
        }
    }

    wireview::idd_relay::StreamDescription m_Description;
    Microsoft::WRL::ComPtr<ID3D11Device> m_Device;
    FrameRelayTextureRing* m_Ring{};
    bool m_OpenedSession{ false };
    Microsoft::WRL::Wrappers::Thread m_Thread;
    Microsoft::WRL::Wrappers::Event m_TerminateEvent;
};

// Each slot starts owned by the driver (key 0). The future Host consumer will
// acquire key 1, copy into its encoder input, then release key 0. The driver
// never waits for that return path: an unavailable slot simply drops a frame.
class FrameRelayTextureRing
{
public:
    FrameRelayTextureRing()
    {
        m_ReadyEvent.Attach(CreateEvent(nullptr, FALSE, FALSE, nullptr));
    }

    bool OpenNamed(
        ID3D11Device* Device,
        const wireview::idd_relay::StreamDescription& Description,
        const std::array<std::wstring, wireview::idd_relay::kSlotCount>& Names)
    {
        if (Device == nullptr || Description.width == 0U || Description.height == 0U ||
            Description.dxgi_format != static_cast<UINT>(DXGI_FORMAT_B8G8R8A8_UNORM))
        {
            return false;
        }
        ComPtr<ID3D11Device1> device1;
        if (FAILED(Device->QueryInterface(IID_PPV_ARGS(&device1))))
        {
            return false;
        }
        std::array<Slot, wireview::idd_relay::kSlotCount> slots = {};
        for (UINT index = 0U; index < m_Slots.size(); ++index)
        {
            if (Names[index].empty() ||
                FAILED(device1->OpenSharedResourceByName(
                    Names[index].c_str(),
                    DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE,
                    IID_PPV_ARGS(&slots[index].Texture))) ||
                FAILED(slots[index].Texture.As(&slots[index].Mutex)))
            {
                return false;
            }
            D3D11_TEXTURE2D_DESC description = {};
            slots[index].Texture->GetDesc(&description);
            if (description.Width != Description.width || description.Height != Description.height ||
                static_cast<UINT>(description.Format) != Description.dxgi_format ||
                description.SampleDesc.Count != 1U)
            {
                return false;
            }
        }
        m_Slots = std::move(slots);
        m_Opened.store(true, std::memory_order_release);
        return true;
    }

    bool TryCopy(ID3D11DeviceContext* Context, ID3D11Texture2D* Source)
    {
        if (Context == nullptr || Source == nullptr ||
            !m_Opened.load(std::memory_order_acquire) ||
            m_Queued.load(std::memory_order_acquire) >= m_Slots.size())
        {
            return false;
        }
        for (UINT attempt = 0U; attempt < m_Slots.size(); ++attempt)
        {
            const UINT index = (m_NextSlot + attempt) % static_cast<UINT>(m_Slots.size());
            if (!m_Slots[index].Mutex || !m_Slots[index].Texture)
            {
                continue;
            }
            const HRESULT acquired = m_Slots[index].Mutex->AcquireSync(0U, 0U);
            if (acquired == DXGI_ERROR_WAIT_TIMEOUT) {
                continue;
            }
            if (FAILED(acquired)) {
                return false;
            }
            Context->CopyResource(m_Slots[index].Texture.Get(), Source);
            if (FAILED(m_Slots[index].Mutex->ReleaseSync(1U))) {
                return false;
            }
            wireview::idd_relay::FrameReady ready = {};
            ready.slot = index;
            ready.sequence = m_NextSequence.fetch_add(1U, std::memory_order_relaxed);
            ready.capture_timestamp_micros = GetTickCount64() * 1'000ULL;
            m_QueuedFrames[m_WriteIndex] = ready;
            m_WriteIndex = (m_WriteIndex + 1U) % static_cast<UINT>(m_QueuedFrames.size());
            m_Queued.fetch_add(1U, std::memory_order_release);
            if (m_ReadyEvent.Get() != nullptr)
            {
                SetEvent(m_ReadyEvent.Get());
            }
            m_NextSlot = (index + 1U) % static_cast<UINT>(m_Slots.size());
            return true;
        }
        return false;
    }

    bool TryDequeue(wireview::idd_relay::FrameReady* Ready)
    {
        if (Ready == nullptr || m_Queued.load(std::memory_order_acquire) == 0U)
        {
            return false;
        }
        *Ready = m_QueuedFrames[m_ReadIndex];
        m_ReadIndex = (m_ReadIndex + 1U) % static_cast<UINT>(m_QueuedFrames.size());
        m_Queued.fetch_sub(1U, std::memory_order_release);
        return true;
    }

    HANDLE ReadyEvent() const
    {
        return m_ReadyEvent.Get();
    }

private:
    struct Slot
    {
        Microsoft::WRL::ComPtr<ID3D11Texture2D> Texture;
        Microsoft::WRL::ComPtr<IDXGIKeyedMutex> Mutex;
    };

    std::array<Slot, wireview::idd_relay::kSlotCount> m_Slots;
    std::array<wireview::idd_relay::FrameReady, wireview::idd_relay::kSlotCount> m_QueuedFrames{};
    std::atomic<bool> m_Opened{ false };
    std::atomic<UINT> m_Queued{ 0U };
    std::atomic<std::uint64_t> m_NextSequence{ 1U };
    Microsoft::WRL::Wrappers::Event m_ReadyEvent;
    UINT m_NextSlot{ 0U };
    UINT m_WriteIndex{ 0U };
    UINT m_ReadIndex{ 0U };
};

bool OpenNamedRelayTextures(
    FrameRelayTextureRing* Ring,
    ID3D11Device* Device,
    const wireview::idd_relay::StreamDescription& Description,
    const std::array<std::wstring, wireview::idd_relay::kSlotCount>& Names)
{
    return Ring != nullptr && Ring->OpenNamed(Device, Description, Names);
}

bool TryDequeueRelayFrame(
    FrameRelayTextureRing* Ring,
    wireview::idd_relay::FrameReady* Ready)
{
    return Ring != nullptr && Ring->TryDequeue(Ready);
}

HANDLE RelayFrameReadyEvent(FrameRelayTextureRing* Ring)
{
    return Ring == nullptr ? nullptr : Ring->ReadyEvent();
}
}
}

#pragma region SampleMonitors

static constexpr DWORD IDD_SAMPLE_MONITOR_COUNT = 1;
static const GUID WIREVIEW_MONITOR_CONTAINER_ID =
    { 0x8b3a4c90, 0x335b, 0x4f71, { 0xb6, 0xf3, 0x99, 0xca, 0x4f, 0x6f, 0x2d, 0x63 } };

// Default modes reported for the EDID-less WireView tablet display. The first
// mode is the clear, long-running reference profile; the two 1920x1200 modes
// match the verified high-refresh profile and its compatibility fallback.
static const struct IndirectSampleMonitor::SampleMonitorMode s_SampleDefaultModes[] = 
{
    { 2560, 1600,  60 },
    { 1920, 1200, 120 },
    { 1920, 1200,  60 },
};

// FOR SAMPLE PURPOSES ONLY, Static info about monitors that will be reported to OS
static const struct IndirectSampleMonitor s_SampleMonitors[] =
{
    // Modified EDID from Dell S2719DGF
    {
        {
            0x00,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0x00,0x10,0xAC,0xE6,0xD0,0x55,0x5A,0x4A,0x30,0x24,0x1D,0x01,
            0x04,0xA5,0x3C,0x22,0x78,0xFB,0x6C,0xE5,0xA5,0x55,0x50,0xA0,0x23,0x0B,0x50,0x54,0x00,0x02,0x00,
            0xD1,0xC0,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x58,0xE3,0x00,
            0xA0,0xA0,0xA0,0x29,0x50,0x30,0x20,0x35,0x00,0x55,0x50,0x21,0x00,0x00,0x1A,0x00,0x00,0x00,0xFF,
            0x00,0x37,0x4A,0x51,0x58,0x42,0x59,0x32,0x0A,0x20,0x20,0x20,0x20,0x20,0x00,0x00,0x00,0xFC,0x00,
            0x53,0x32,0x37,0x31,0x39,0x44,0x47,0x46,0x0A,0x20,0x20,0x20,0x20,0x00,0x00,0x00,0xFD,0x00,0x28,
            0x9B,0xFA,0xFA,0x40,0x01,0x0A,0x20,0x20,0x20,0x20,0x20,0x20,0x00,0x2C
        },
        {
            { 2560, 1440, 144 },
            { 1920, 1080,  60 },
            { 1024,  768,  60 },
        },
        0
    },
    // Modified EDID from Lenovo Y27fA
    {
        {
            0x00,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0x00,0x30,0xAE,0xBF,0x65,0x01,0x01,0x01,0x01,0x20,0x1A,0x01,
            0x04,0xA5,0x3C,0x22,0x78,0x3B,0xEE,0xD1,0xA5,0x55,0x48,0x9B,0x26,0x12,0x50,0x54,0x00,0x08,0x00,
            0xA9,0xC0,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x68,0xD8,0x00,
            0x18,0xF1,0x70,0x2D,0x80,0x58,0x2C,0x45,0x00,0x53,0x50,0x21,0x00,0x00,0x1E,0x00,0x00,0x00,0x10,
            0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xFD,0x00,
            0x30,0x92,0xB4,0xB4,0x22,0x01,0x0A,0x20,0x20,0x20,0x20,0x20,0x20,0x00,0x00,0x00,0xFC,0x00,0x4C,
            0x45,0x4E,0x20,0x59,0x32,0x37,0x66,0x41,0x0A,0x20,0x20,0x20,0x00,0x11
        },
        {
            { 3840, 2160,  60 },
            { 1600,  900,  60 },
            { 1024,  768,  60 },
        },
        0
    }
};

#pragma endregion

#pragma region helpers

static inline void FillSignalInfo(DISPLAYCONFIG_VIDEO_SIGNAL_INFO& Mode, DWORD Width, DWORD Height, DWORD VSync, bool bMonitorMode)
{
    Mode.totalSize.cx = Mode.activeSize.cx = Width;
    Mode.totalSize.cy = Mode.activeSize.cy = Height;

    // See https://docs.microsoft.com/en-us/windows/win32/api/wingdi/ns-wingdi-displayconfig_video_signal_info
    Mode.AdditionalSignalInfo.vSyncFreqDivider = bMonitorMode ? 0 : 1;
    Mode.AdditionalSignalInfo.videoStandard = 255;

    Mode.vSyncFreq.Numerator = VSync;
    Mode.vSyncFreq.Denominator = 1;
    Mode.hSyncFreq.Numerator = VSync * Height;
    Mode.hSyncFreq.Denominator = 1;

    Mode.scanLineOrdering = DISPLAYCONFIG_SCANLINE_ORDERING_PROGRESSIVE;

    Mode.pixelRate = ((UINT64) VSync) * ((UINT64) Width) * ((UINT64) Height);
}

static IDDCX_MONITOR_MODE CreateIddCxMonitorMode(DWORD Width, DWORD Height, DWORD VSync, IDDCX_MONITOR_MODE_ORIGIN Origin = IDDCX_MONITOR_MODE_ORIGIN_DRIVER)
{
    IDDCX_MONITOR_MODE Mode = {};

    Mode.Size = sizeof(Mode);
    Mode.Origin = Origin;
    FillSignalInfo(Mode.MonitorVideoSignalInfo, Width, Height, VSync, true);

    return Mode;
}

static IDDCX_TARGET_MODE CreateIddCxTargetMode(DWORD Width, DWORD Height, DWORD VSync)
{
    IDDCX_TARGET_MODE Mode = {};

    Mode.Size = sizeof(Mode);
    FillSignalInfo(Mode.TargetVideoSignalInfo.targetVideoSignalInfo, Width, Height, VSync, false);

    return Mode;
}

#pragma endregion

extern "C" DRIVER_INITIALIZE DriverEntry;

EVT_WDF_DRIVER_DEVICE_ADD IddSampleDeviceAdd;
EVT_WDF_DEVICE_D0_ENTRY IddSampleDeviceD0Entry;

EVT_IDD_CX_ADAPTER_INIT_FINISHED IddSampleAdapterInitFinished;
EVT_IDD_CX_ADAPTER_COMMIT_MODES IddSampleAdapterCommitModes;

EVT_IDD_CX_PARSE_MONITOR_DESCRIPTION IddSampleParseMonitorDescription;
EVT_IDD_CX_MONITOR_GET_DEFAULT_DESCRIPTION_MODES IddSampleMonitorGetDefaultModes;
EVT_IDD_CX_MONITOR_QUERY_TARGET_MODES IddSampleMonitorQueryModes;

EVT_IDD_CX_MONITOR_ASSIGN_SWAPCHAIN IddSampleMonitorAssignSwapChain;
EVT_IDD_CX_MONITOR_UNASSIGN_SWAPCHAIN IddSampleMonitorUnassignSwapChain;

struct IndirectDeviceContextWrapper
{
    IndirectDeviceContext* pContext;

    void Cleanup()
    {
        delete pContext;
        pContext = nullptr;
    }
};

struct IndirectMonitorContextWrapper
{
    IndirectMonitorContext* pContext;

    void Cleanup()
    {
        delete pContext;
        pContext = nullptr;
    }
};

// This macro creates the methods for accessing an IndirectDeviceContextWrapper as a context for a WDF object
WDF_DECLARE_CONTEXT_TYPE(IndirectDeviceContextWrapper);

WDF_DECLARE_CONTEXT_TYPE(IndirectMonitorContextWrapper);

extern "C" BOOL WINAPI DllMain(
    _In_ HINSTANCE hInstance,
    _In_ UINT dwReason,
    _In_opt_ LPVOID lpReserved)
{
    UNREFERENCED_PARAMETER(hInstance);
    UNREFERENCED_PARAMETER(lpReserved);
    UNREFERENCED_PARAMETER(dwReason);

    return TRUE;
}

_Use_decl_annotations_
extern "C" NTSTATUS DriverEntry(
    PDRIVER_OBJECT  pDriverObject,
    PUNICODE_STRING pRegistryPath
)
{
    WireViewLogStatus("DriverEntry.begin", STATUS_SUCCESS);
    WDF_DRIVER_CONFIG Config;
    NTSTATUS Status;

    WDF_OBJECT_ATTRIBUTES Attributes;
    WDF_OBJECT_ATTRIBUTES_INIT(&Attributes);

    WDF_DRIVER_CONFIG_INIT(&Config,
        IddSampleDeviceAdd
    );

    Status = WdfDriverCreate(pDriverObject, pRegistryPath, &Attributes, &Config, WDF_NO_HANDLE);
    WireViewLogStatus("DriverEntry.WdfDriverCreate", Status);
    if (!NT_SUCCESS(Status))
    {
        return Status;
    }

    return Status;
}

_Use_decl_annotations_
NTSTATUS IddSampleDeviceAdd(WDFDRIVER Driver, PWDFDEVICE_INIT pDeviceInit)
{
    WireViewLogStatus("DeviceAdd.begin", STATUS_SUCCESS);
    NTSTATUS Status = STATUS_SUCCESS;
    WDF_PNPPOWER_EVENT_CALLBACKS PnpPowerCallbacks;

    UNREFERENCED_PARAMETER(Driver);

    // Register for power callbacks - in this sample only power-on is needed
    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&PnpPowerCallbacks);
    PnpPowerCallbacks.EvtDeviceD0Entry = IddSampleDeviceD0Entry;
    WdfDeviceInitSetPnpPowerEventCallbacks(pDeviceInit, &PnpPowerCallbacks);

    IDD_CX_CLIENT_CONFIG IddConfig;
    IDD_CX_CLIENT_CONFIG_INIT(&IddConfig);

    // If the driver wishes to handle custom IoDeviceControl requests, it's necessary to use this callback since IddCx
    // redirects IoDeviceControl requests to an internal queue. This sample does not need this.
    // IddConfig.EvtIddCxDeviceIoControl = IddSampleIoDeviceControl;

    IddConfig.EvtIddCxAdapterInitFinished = IddSampleAdapterInitFinished;

    IddConfig.EvtIddCxParseMonitorDescription = IddSampleParseMonitorDescription;
    IddConfig.EvtIddCxMonitorGetDefaultDescriptionModes = IddSampleMonitorGetDefaultModes;
    IddConfig.EvtIddCxMonitorQueryTargetModes = IddSampleMonitorQueryModes;
    IddConfig.EvtIddCxAdapterCommitModes = IddSampleAdapterCommitModes;
    IddConfig.EvtIddCxMonitorAssignSwapChain = IddSampleMonitorAssignSwapChain;
    IddConfig.EvtIddCxMonitorUnassignSwapChain = IddSampleMonitorUnassignSwapChain;

    Status = IddCxDeviceInitConfig(pDeviceInit, &IddConfig);
    WireViewLogStatus("DeviceAdd.IddCxDeviceInitConfig", Status);
    if (!NT_SUCCESS(Status))
    {
        return Status;
    }

    WDF_OBJECT_ATTRIBUTES Attr;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&Attr, IndirectDeviceContextWrapper);
    Attr.EvtCleanupCallback = [](WDFOBJECT Object)
    {
        // Automatically cleanup the context when the WDF object is about to be deleted
        auto* pContext = WdfObjectGet_IndirectDeviceContextWrapper(Object);
        if (pContext)
        {
            pContext->Cleanup();
        }
    };

    WDFDEVICE Device = nullptr;
    Status = WdfDeviceCreate(&pDeviceInit, &Attr, &Device);
    WireViewLogStatus("DeviceAdd.WdfDeviceCreate", Status);
    if (!NT_SUCCESS(Status))
    {
        return Status;
    }

    Status = IddCxDeviceInitialize(Device);
    WireViewLogStatus("DeviceAdd.IddCxDeviceInitialize", Status);

    // Create a new device context object and attach it to the WDF device object
    auto* pContext = WdfObjectGet_IndirectDeviceContextWrapper(Device);
    pContext->pContext = new IndirectDeviceContext(Device);

    return Status;
}

_Use_decl_annotations_
NTSTATUS IddSampleDeviceD0Entry(WDFDEVICE Device, WDF_POWER_DEVICE_STATE PreviousState)
{
    WireViewLogStatus("DeviceD0Entry.begin", STATUS_SUCCESS);
    UNREFERENCED_PARAMETER(PreviousState);

    // This function is called by WDF to start the device in the fully-on power state.

    auto* pContext = WdfObjectGet_IndirectDeviceContextWrapper(Device);
    pContext->pContext->InitAdapter();

    return STATUS_SUCCESS;
}

#pragma region Direct3DDevice

Direct3DDevice::Direct3DDevice(LUID AdapterLuid) : AdapterLuid(AdapterLuid)
{

}

Direct3DDevice::Direct3DDevice()
{
    AdapterLuid = LUID{};
}

HRESULT Direct3DDevice::Init()
{
    // The DXGI factory could be cached, but if a new render adapter appears on the system, a new factory needs to be
    // created. If caching is desired, check DxgiFactory->IsCurrent() each time and recreate the factory if !IsCurrent.
    HRESULT hr = CreateDXGIFactory2(0, IID_PPV_ARGS(&DxgiFactory));
    if (FAILED(hr))
    {
        return hr;
    }

    // Find the specified render adapter
    hr = DxgiFactory->EnumAdapterByLuid(AdapterLuid, IID_PPV_ARGS(&Adapter));
    if (FAILED(hr))
    {
        return hr;
    }

    // Create a D3D device using the render adapter. BGRA support is required by the WHQL test suite.
    hr = D3D11CreateDevice(Adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, &Device, nullptr, &DeviceContext);
    if (FAILED(hr))
    {
        // If creating the D3D device failed, it's possible the render GPU was lost (e.g. detachable GPU) or else the
        // system is in a transient state.
        return hr;
    }

    return S_OK;
}

#pragma endregion

#pragma region SwapChainProcessor

SwapChainProcessor::SwapChainProcessor(
    IDDCX_SWAPCHAIN hSwapChain,
    shared_ptr<Direct3DDevice> Device,
    HANDLE NewFrameEvent,
    UINT frameRate)
    : m_hSwapChain(hSwapChain),
      m_Device(Device),
      m_hAvailableBufferEvent(NewFrameEvent),
      m_frameRate(frameRate)
{
    m_hTerminateEvent.Attach(CreateEvent(nullptr, FALSE, FALSE, nullptr));

    // Immediately create and run the swap-chain processing thread, passing 'this' as the thread parameter
    m_hThread.Attach(CreateThread(nullptr, 0, RunThread, this, 0, nullptr));
}

SwapChainProcessor::~SwapChainProcessor()
{
    // Alert the swap-chain processing thread to terminate
    SetEvent(m_hTerminateEvent.Get());

    if (m_hThread.Get())
    {
        // Wait for the thread to terminate
        WaitForSingleObject(m_hThread.Get(), INFINITE);
    }
}

DWORD CALLBACK SwapChainProcessor::RunThread(LPVOID Argument)
{
    reinterpret_cast<SwapChainProcessor*>(Argument)->Run();
    return 0;
}

void SwapChainProcessor::Run()
{
    // For improved performance, make use of the Multimedia Class Scheduler Service, which will intelligently
    // prioritize this thread for improved throughput in high CPU-load scenarios.
    DWORD AvTask = 0;
    HANDLE AvTaskHandle = AvSetMmThreadCharacteristicsW(L"Distribution", &AvTask);

    RunCore();

    // Always delete the swap-chain object when swap-chain processing loop terminates in order to kick the system to
    // provide a new swap-chain if necessary.
    WdfObjectDelete((WDFOBJECT)m_hSwapChain);
    m_hSwapChain = nullptr;

    if (AvTaskHandle != nullptr)
    {
        AvRevertMmThreadCharacteristics(AvTaskHandle);
    }
}

void SwapChainProcessor::RunCore()
{
    // Get the DXGI device interface
    ComPtr<IDXGIDevice> DxgiDevice;
    HRESULT hr = m_Device->Device.As(&DxgiDevice);
    if (FAILED(hr))
    {
        return;
    }

    IDARG_IN_SWAPCHAINSETDEVICE SetDevice = {};
    SetDevice.pDevice = DxgiDevice.Get();

    hr = IddCxSwapChainSetDevice(m_hSwapChain, &SetDevice);
    if (FAILED(hr))
    {
        return;
    }

    // Acquire and release buffers in a loop
    for (;;)
    {
        ComPtr<IDXGIResource> AcquiredBuffer;

        // Ask for the next buffer from the producer
        IDARG_OUT_RELEASEANDACQUIREBUFFER Buffer = {};
        hr = IddCxSwapChainReleaseAndAcquireBuffer(m_hSwapChain, &Buffer);

        // AcquireBuffer immediately returns STATUS_PENDING if no buffer is yet available
        if (hr == E_PENDING)
        {
            // We must wait for a new buffer
            HANDLE WaitHandles [] =
            {
                m_hAvailableBufferEvent,
                m_hTerminateEvent.Get()
            };
            DWORD WaitResult = WaitForMultipleObjects(ARRAYSIZE(WaitHandles), WaitHandles, FALSE, 16);
            if (WaitResult == WAIT_OBJECT_0 || WaitResult == WAIT_TIMEOUT)
            {
                // We have a new buffer, so try the AcquireBuffer again
                continue;
            }
            else if (WaitResult == WAIT_OBJECT_0 + 1)
            {
                // We need to terminate
                break;
            }
            else
            {
                // The wait was cancelled or something unexpected happened
                hr = HRESULT_FROM_WIN32(WaitResult);
                break;
            }
        }
        else if (SUCCEEDED(hr))
        {
            // We have new frame to process, the surface has a reference on it that the driver has to release
            AcquiredBuffer.Attach(Buffer.MetaData.pSurface);

            ComPtr<ID3D11Texture2D> texture;
            (void)AcquiredBuffer.As(&texture);
            if (!m_sharedTextures)
            {
                D3D11_TEXTURE2D_DESC description = {};
                DXGI_ADAPTER_DESC1 adapterDescription = {};
                if (texture)
                {
                    texture->GetDesc(&description);
                }
                if (texture && SUCCEEDED(m_Device->Adapter->GetDesc1(&adapterDescription)) &&
                    description.Width > 0U && description.Width <= 65535U &&
                    description.Height > 0U && description.Height <= 65535U &&
                    (description.Width % 2U) == 0U && (description.Height % 2U) == 0U &&
                    (m_frameRate == 60U || m_frameRate == 120U))
                {
                    wireview::idd_relay::StreamDescription stream = {};
                    stream.adapter_luid_low = adapterDescription.AdapterLuid.LowPart;
                    stream.adapter_luid_high = adapterDescription.AdapterLuid.HighPart;
                    stream.dxgi_format = static_cast<UINT>(description.Format);
                    stream.width = static_cast<std::uint16_t>(description.Width);
                    stream.height = static_cast<std::uint16_t>(description.Height);
                    stream.frame_rate = static_cast<std::uint16_t>(m_frameRate);
                    stream.slot_count = static_cast<std::uint16_t>(wireview::idd_relay::kSlotCount);
                    auto sharedTextures = std::unique_ptr<FrameRelayTextureRing>(
                        new FrameRelayTextureRing());
                    m_sharedTextures = std::move(sharedTextures);
                    m_frameRelay.reset(new FrameRelayNegotiator(
                        stream, m_Device->Device.Get(), m_sharedTextures.get()));
                }
            }

            // The IddCx thread publishes only when a previously negotiated
            // slot is immediately available. It neither waits for the Host
            // nor performs pipe I/O; the relay thread sends the frame record.
            if (m_sharedTextures && texture)
            {
                (void)m_sharedTextures->TryCopy(m_Device->DeviceContext.Get(), texture.Get());
            }

            // ==============================
            // TODO: Process the frame here
            //
            // This is the most performance-critical section of code in an IddCx driver. It's important that whatever
            // is done with the acquired surface be finished as quickly as possible. This operation could be:
            //  * a GPU copy to another buffer surface for later processing (such as a staging surface for mapping to CPU memory)
            //  * a GPU encode operation
            //  * a GPU VPBlt to another surface
            //  * a GPU custom compute shader encode operation
            // ==============================

            // We have finished processing this frame hence we release the reference on it.
            // If the driver forgets to release the reference to the surface, it will be leaked which results in the
            // surfaces being left around after swapchain is destroyed.
            // NOTE: Although in this sample we release reference to the surface here; the driver still
            // owns the Buffer.MetaData.pSurface surface until IddCxSwapChainReleaseAndAcquireBuffer returns
            // S_OK and gives us a new frame, a driver may want to use the surface in future to re-encode the desktop 
            // for better quality if there is no new frame for a while
            AcquiredBuffer.Reset();
            
            // Indicate to OS that we have finished inital processing of the frame, it is a hint that
            // OS could start preparing another frame
            hr = IddCxSwapChainFinishedProcessingFrame(m_hSwapChain);
            if (FAILED(hr))
            {
                break;
            }

            // ==============================
            // TODO: Report frame statistics once the asynchronous encode/send work is completed
            //
            // Drivers should report information about sub-frame timings, like encode time, send time, etc.
            // ==============================
            // IddCxSwapChainReportFrameStatistics(m_hSwapChain, ...);
        }
        else
        {
            // The swap-chain was likely abandoned (e.g. DXGI_ERROR_ACCESS_LOST), so exit the processing loop
            break;
        }
    }
}

#pragma endregion

#pragma region IndirectDeviceContext

IndirectDeviceContext::IndirectDeviceContext(_In_ WDFDEVICE WdfDevice) :
    m_WdfDevice(WdfDevice)
{
    m_Adapter = {};
}

IndirectDeviceContext::~IndirectDeviceContext()
{
}

void IndirectDeviceContext::InitAdapter()
{
    // ==============================
    // TODO: Update the below diagnostic information in accordance with the target hardware. The strings and version
    // numbers are used for telemetry and may be displayed to the user in some situations.
    //
    // This is also where static per-adapter capabilities are determined.
    // ==============================

    IDDCX_ADAPTER_CAPS AdapterCaps = {};
    AdapterCaps.Size = sizeof(AdapterCaps);

    // Declare basic feature support for the adapter (required)
    AdapterCaps.MaxMonitorsSupported = IDD_SAMPLE_MONITOR_COUNT;
    AdapterCaps.EndPointDiagnostics.Size = sizeof(AdapterCaps.EndPointDiagnostics);
    AdapterCaps.EndPointDiagnostics.GammaSupport = IDDCX_FEATURE_IMPLEMENTATION_NONE;
    AdapterCaps.EndPointDiagnostics.TransmissionType = IDDCX_TRANSMISSION_TYPE_WIRED_OTHER;

    // Declare your device strings for telemetry (required)
    AdapterCaps.EndPointDiagnostics.pEndPointFriendlyName = L"WireView Virtual Display";
    AdapterCaps.EndPointDiagnostics.pEndPointManufacturerName = L"WireView Project";
    AdapterCaps.EndPointDiagnostics.pEndPointModelName = L"WireView Phase-0 Display";

    // Declare your hardware and firmware versions (required)
    IDDCX_ENDPOINT_VERSION Version = {};
    Version.Size = sizeof(Version);
    Version.MajorVer = 1;
    AdapterCaps.EndPointDiagnostics.pFirmwareVersion = &Version;
    AdapterCaps.EndPointDiagnostics.pHardwareVersion = &Version;

    // Initialize a WDF context that can store a pointer to the device context object
    WDF_OBJECT_ATTRIBUTES Attr;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&Attr, IndirectDeviceContextWrapper);

    IDARG_IN_ADAPTER_INIT AdapterInit = {};
    AdapterInit.WdfDevice = m_WdfDevice;
    AdapterInit.pCaps = &AdapterCaps;
    AdapterInit.ObjectAttributes = &Attr;

    // Start the initialization of the adapter, which will trigger the AdapterFinishInit callback later
    IDARG_OUT_ADAPTER_INIT AdapterInitOut;
    NTSTATUS Status = IddCxAdapterInitAsync(&AdapterInit, &AdapterInitOut);
    WireViewLogStatus("InitAdapter.IddCxAdapterInitAsync", Status);

    if (NT_SUCCESS(Status))
    {
        // Store a reference to the WDF adapter handle
        m_Adapter = AdapterInitOut.AdapterObject;

        // Store the device context object into the WDF object context
        auto* pContext = WdfObjectGet_IndirectDeviceContextWrapper(AdapterInitOut.AdapterObject);
        pContext->pContext = this;
    }
}

void IndirectDeviceContext::FinishInit(UINT ConnectorIndex)
{
    // ==============================
    // TODO: In a real driver, the EDID should be retrieved dynamically from a connected physical monitor. The EDIDs
    // provided here are purely for demonstration.
    // Monitor manufacturers are required to correctly fill in physical monitor attributes in order to allow the OS
    // to optimize settings like viewing distance and scale factor. Manufacturers should also use a unique serial
    // number every single device to ensure the OS can tell the monitors apart.
    // ==============================

    WDF_OBJECT_ATTRIBUTES Attr;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&Attr, IndirectMonitorContextWrapper);

    // In the sample driver, we report a monitor right away but a real driver would do this when a monitor connection event occurs
    IDDCX_MONITOR_INFO MonitorInfo = {};
    MonitorInfo.Size = sizeof(MonitorInfo);
    MonitorInfo.MonitorType = DISPLAYCONFIG_OUTPUT_TECHNOLOGY_INDIRECT_WIRED;
    MonitorInfo.ConnectorIndex = ConnectorIndex;

    MonitorInfo.MonitorDescription.Size = sizeof(MonitorInfo.MonitorDescription);
    MonitorInfo.MonitorDescription.Type = IDDCX_MONITOR_DESCRIPTION_TYPE_EDID;
    // Phase 0 intentionally reports an EDID-less monitor. The default-mode callback
    // makes 1920x1080@60 the preferred mode until WireView supplies its own EDID.
    MonitorInfo.MonitorDescription.DataSize = 0;
    MonitorInfo.MonitorDescription.pData = nullptr;

    // ==============================
    // TODO: The monitor's container ID should be distinct from "this" device's container ID if the monitor is not
    // permanently attached to the display adapter device object. The container ID is typically made unique for each
    // monitor and can be used to associate the monitor with other devices, like audio or input devices. In this
    // sample we generate a random container ID GUID, but it's best practice to choose a stable container ID for a
    // unique monitor or to use "this" device's container ID for a permanent/integrated monitor.
    // ==============================

    // Keep the same monitor identity across launcher restarts so Windows can retain
    // its display placement and scaling settings.
    MonitorInfo.MonitorContainerId = WIREVIEW_MONITOR_CONTAINER_ID;

    IDARG_IN_MONITORCREATE MonitorCreate = {};
    MonitorCreate.ObjectAttributes = &Attr;
    MonitorCreate.pMonitorInfo = &MonitorInfo;

    // Create a monitor object with the specified monitor descriptor
    IDARG_OUT_MONITORCREATE MonitorCreateOut;
    NTSTATUS Status = IddCxMonitorCreate(m_Adapter, &MonitorCreate, &MonitorCreateOut);
    WireViewLogStatus("FinishInit.IddCxMonitorCreate", Status);
    if (NT_SUCCESS(Status))
    {
        // Create a new monitor context object and attach it to the Idd monitor object
        auto* pMonitorContextWrapper = WdfObjectGet_IndirectMonitorContextWrapper(MonitorCreateOut.MonitorObject);
        pMonitorContextWrapper->pContext = new IndirectMonitorContext(MonitorCreateOut.MonitorObject);

        // Tell the OS that the monitor has been plugged in
        IDARG_OUT_MONITORARRIVAL ArrivalOut;
        Status = IddCxMonitorArrival(MonitorCreateOut.MonitorObject, &ArrivalOut);
        WireViewLogStatus("FinishInit.IddCxMonitorArrival", Status);
    }
}

IndirectMonitorContext::IndirectMonitorContext(_In_ IDDCX_MONITOR Monitor) :
    m_Monitor(Monitor)
{
}

IndirectMonitorContext::~IndirectMonitorContext()
{
    m_ProcessingThread.reset();
}

void IndirectMonitorContext::AssignSwapChain(IDDCX_SWAPCHAIN SwapChain, LUID RenderAdapter, HANDLE NewFrameEvent)
{
    m_ProcessingThread.reset();

    auto Device = make_shared<Direct3DDevice>(RenderAdapter);
    if (FAILED(Device->Init()))
    {
        // It's important to delete the swap-chain if D3D initialization fails, so that the OS knows to generate a new
        // swap-chain and try again.
        WdfObjectDelete(SwapChain);
    }
    else
    {
        // Create a new swap-chain processing thread
        m_ProcessingThread.reset(new SwapChainProcessor(
            SwapChain, Device, NewFrameEvent, m_FrameRate.load()));
    }
}

void IndirectMonitorContext::UnassignSwapChain()
{
    // Stop processing the last swap-chain
    m_ProcessingThread.reset();
}

void IndirectMonitorContext::CommitMode(const IDDCX_PATH& Path)
{
    if ((Path.Flags & IDDCX_PATH_FLAGS_ACTIVE) == 0U)
    {
        m_FrameRate.store(60U);
        return;
    }

    const DISPLAYCONFIG_RATIONAL frequency = Path.TargetVideoSignalInfo.vSyncFreq;
    const UINT divider = Path.TargetVideoSignalInfo.AdditionalSignalInfo.vSyncFreqDivider;
    if (frequency.Denominator == 0U || divider == 0U)
    {
        m_FrameRate.store(60U);
        return;
    }
    const UINT64 rate = static_cast<UINT64>(frequency.Numerator) /
        (static_cast<UINT64>(frequency.Denominator) * divider);
    m_FrameRate.store(rate >= 100U ? 120U : 60U);
}

#pragma endregion

#pragma region DDI Callbacks

_Use_decl_annotations_
NTSTATUS IddSampleAdapterInitFinished(IDDCX_ADAPTER AdapterObject, const IDARG_IN_ADAPTER_INIT_FINISHED* pInArgs)
{
    WireViewLogStatus("AdapterInitFinished", pInArgs->AdapterInitStatus);
    // This is called when the OS has finished setting up the adapter for use by the IddCx driver. It's now possible
    // to report attached monitors.

    auto* pDeviceContextWrapper = WdfObjectGet_IndirectDeviceContextWrapper(AdapterObject);
    if (NT_SUCCESS(pInArgs->AdapterInitStatus))
    {
        for (DWORD i = 0; i < IDD_SAMPLE_MONITOR_COUNT; i++)
        {
            pDeviceContextWrapper->pContext->FinishInit(i);
        }
    }

    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS IddSampleAdapterCommitModes(IDDCX_ADAPTER AdapterObject, const IDARG_IN_COMMITMODES* pInArgs)
{
    UNREFERENCED_PARAMETER(AdapterObject);
    for (UINT index = 0U; index < pInArgs->PathCount; ++index)
    {
        const IDDCX_PATH& path = pInArgs->pPaths[index];
        auto* pMonitorContextWrapper = WdfObjectGet_IndirectMonitorContextWrapper(path.MonitorObject);
        if (pMonitorContextWrapper != nullptr && pMonitorContextWrapper->pContext != nullptr)
        {
            pMonitorContextWrapper->pContext->CommitMode(path);
        }
    }

    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS IddSampleParseMonitorDescription(const IDARG_IN_PARSEMONITORDESCRIPTION* pInArgs, IDARG_OUT_PARSEMONITORDESCRIPTION* pOutArgs)
{
    // ==============================
    // TODO: In a real driver, this function would be called to generate monitor modes for an EDID by parsing it. In
    // this sample driver, we hard-code the EDID, so this function can generate known modes.
    // ==============================

    pOutArgs->MonitorModeBufferOutputCount = IndirectSampleMonitor::szModeList;

    if (pInArgs->MonitorModeBufferInputCount < IndirectSampleMonitor::szModeList)
    {
        // Return success if there was no buffer, since the caller was only asking for a count of modes
        return (pInArgs->MonitorModeBufferInputCount > 0) ? STATUS_BUFFER_TOO_SMALL : STATUS_SUCCESS;
    }
    else
    {
        // In the sample driver, we have reported some static information about connected monitors
        // Check which of the reported monitors this call is for by comparing it to the pointer of
        // our known EDID blocks.

        if (pInArgs->MonitorDescription.DataSize != IndirectSampleMonitor::szEdidBlock)
            return STATUS_INVALID_PARAMETER;

        DWORD SampleMonitorIdx = 0;
        for(; SampleMonitorIdx < ARRAYSIZE(s_SampleMonitors); SampleMonitorIdx++)
        {
            if (memcmp(pInArgs->MonitorDescription.pData, s_SampleMonitors[SampleMonitorIdx].pEdidBlock, IndirectSampleMonitor::szEdidBlock) == 0)
            {
                // Copy the known modes to the output buffer
                for (DWORD ModeIndex = 0; ModeIndex < IndirectSampleMonitor::szModeList; ModeIndex++)
                {
                    pInArgs->pMonitorModes[ModeIndex] = CreateIddCxMonitorMode(
                        s_SampleMonitors[SampleMonitorIdx].pModeList[ModeIndex].Width,
                        s_SampleMonitors[SampleMonitorIdx].pModeList[ModeIndex].Height,
                        s_SampleMonitors[SampleMonitorIdx].pModeList[ModeIndex].VSync,
                        IDDCX_MONITOR_MODE_ORIGIN_MONITORDESCRIPTOR
                    );
                }

                // Set the preferred mode as represented in the EDID
                pOutArgs->PreferredMonitorModeIdx = s_SampleMonitors[SampleMonitorIdx].ulPreferredModeIdx;
        
                return STATUS_SUCCESS;
            }
        }

        // This EDID block does not belong to the monitors we reported earlier
        return STATUS_INVALID_PARAMETER;
    }
}

_Use_decl_annotations_
NTSTATUS IddSampleMonitorGetDefaultModes(IDDCX_MONITOR MonitorObject, const IDARG_IN_GETDEFAULTDESCRIPTIONMODES* pInArgs, IDARG_OUT_GETDEFAULTDESCRIPTIONMODES* pOutArgs)
{
    UNREFERENCED_PARAMETER(MonitorObject);

    // ==============================
    // TODO: In a real driver, this function would be called to generate monitor modes for a monitor with no EDID.
    // Drivers should report modes that are guaranteed to be supported by the transport protocol and by nearly all
    // monitors (such 640x480, 800x600, or 1024x768). If the driver has access to monitor modes from a descriptor other
    // than an EDID, those modes would also be reported here.
    // ==============================

    if (pInArgs->DefaultMonitorModeBufferInputCount == 0)
    {
        pOutArgs->DefaultMonitorModeBufferOutputCount = ARRAYSIZE(s_SampleDefaultModes); 
    }
    else
    {
        for (DWORD ModeIndex = 0; ModeIndex < ARRAYSIZE(s_SampleDefaultModes); ModeIndex++)
        {
            pInArgs->pDefaultMonitorModes[ModeIndex] = CreateIddCxMonitorMode(
                s_SampleDefaultModes[ModeIndex].Width,
                s_SampleDefaultModes[ModeIndex].Height,
                s_SampleDefaultModes[ModeIndex].VSync,
                IDDCX_MONITOR_MODE_ORIGIN_DRIVER
            );
        }

        pOutArgs->DefaultMonitorModeBufferOutputCount = ARRAYSIZE(s_SampleDefaultModes); 
        pOutArgs->PreferredMonitorModeIdx = 0;
    }

    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS IddSampleMonitorQueryModes(IDDCX_MONITOR MonitorObject, const IDARG_IN_QUERYTARGETMODES* pInArgs, IDARG_OUT_QUERYTARGETMODES* pOutArgs)
{
    UNREFERENCED_PARAMETER(MonitorObject);

    vector<IDDCX_TARGET_MODE> TargetModes;

    // Create a set of modes supported for frame processing and scan-out. These are typically not based on the
    // monitor's descriptor and instead are based on the static processing capability of the device. The OS will
    // report the available set of modes for a given output as the intersection of monitor modes with target modes.

    TargetModes.push_back(CreateIddCxTargetMode(2560, 1600,  60));
    TargetModes.push_back(CreateIddCxTargetMode(1920, 1200, 120));
    TargetModes.push_back(CreateIddCxTargetMode(1920, 1200,  60));

    pOutArgs->TargetModeBufferOutputCount = (UINT) TargetModes.size();

    if (pInArgs->TargetModeBufferInputCount >= TargetModes.size())
    {
        copy(TargetModes.begin(), TargetModes.end(), pInArgs->pTargetModes);
    }

    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS IddSampleMonitorAssignSwapChain(IDDCX_MONITOR MonitorObject, const IDARG_IN_SETSWAPCHAIN* pInArgs)
{
    auto* pMonitorContextWrapper = WdfObjectGet_IndirectMonitorContextWrapper(MonitorObject);
    pMonitorContextWrapper->pContext->AssignSwapChain(pInArgs->hSwapChain, pInArgs->RenderAdapterLuid, pInArgs->hNextSurfaceAvailable);
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS IddSampleMonitorUnassignSwapChain(IDDCX_MONITOR MonitorObject)
{
    auto* pMonitorContextWrapper = WdfObjectGet_IndirectMonitorContextWrapper(MonitorObject);
    pMonitorContextWrapper->pContext->UnassignSwapChain();
    return STATUS_SUCCESS;
}

#pragma endregion
