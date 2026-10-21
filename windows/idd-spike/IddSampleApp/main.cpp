#include <iostream>
#include <chrono>
#include <string>
#include <thread>

#include <windows.h>
#include <swdevice.h>
#include <conio.h>
#include <wrl.h>

struct CreationContext
{
    HANDLE Event;
    HRESULT Result;
};

VOID WINAPI
CreationCallback(
    _In_ HSWDEVICE hSwDevice,
    _In_ HRESULT hrCreateResult,
    _In_opt_ PVOID pContext,
    _In_opt_ PCWSTR pszDeviceInstanceId
    )
{
    auto* context = static_cast<CreationContext*>(pContext);
    context->Result = hrCreateResult;

    if (FAILED(hrCreateResult))
    {
        std::wcerr << L"WireView software device creation failed with 0x"
                   << std::hex << static_cast<unsigned long>(hrCreateResult) << L"\n";
    }
    SetEvent(context->Event);
    UNREFERENCED_PARAMETER(hSwDevice);
    UNREFERENCED_PARAMETER(pszDeviceInstanceId);
}

int __cdecl wmain(int argc, wchar_t* argv[])
{
    unsigned long durationSeconds = 0;
    if (argc == 3 && wcscmp(argv[1], L"--duration-seconds") == 0)
    {
        try
        {
            durationSeconds = std::stoul(argv[2]);
        }
        catch (...)
        {
            durationSeconds = 0;
        }
        if (durationSeconds == 0 || durationSeconds > 3600)
        {
            printf("Duration must be between 1 and 3600 seconds\n");
            return 64;
        }
    }
    else if (argc != 1)
    {
        printf("Usage: WireViewIddLauncher [--duration-seconds <1-3600>]\n");
        return 64;
    }

    HANDLE hEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    if (hEvent == nullptr)
    {
        printf("CreateEvent failed with %lu\n", GetLastError());
        return 1;
    }

    CreationContext creationContext = { hEvent, E_PENDING };
    HSWDEVICE hSwDevice = nullptr;
    SW_DEVICE_CREATE_INFO createInfo = { 0 };
    PCWSTR description = L"WireView Virtual Display Adapter";

    // These match the Pnp id's in the inf file so OS will load the driver when the device is created    
    PCWSTR instanceId = L"WireViewIdd";
    PCWSTR hardwareIds = L"WireViewIdd\0\0";
    PCWSTR compatibleIds = L"WireViewIdd\0\0";

    createInfo.cbSize = sizeof(createInfo);
    createInfo.pszzCompatibleIds = compatibleIds;
    createInfo.pszInstanceId = instanceId;
    createInfo.pszzHardwareIds = hardwareIds;
    createInfo.pszDeviceDescription = description;

    createInfo.CapabilityFlags = SWDeviceCapabilitiesRemovable |
                                 SWDeviceCapabilitiesSilentInstall |
                                 SWDeviceCapabilitiesDriverRequired;

    // Create the device
    HRESULT hr = SwDeviceCreate(L"WireViewIdd",
                                L"HTREE\\ROOT\\0",
                                &createInfo,
                                0,
                                nullptr,
                                CreationCallback,
                                &creationContext,
                                &hSwDevice);
    if (FAILED(hr))
    {
        printf("WireView SwDeviceCreate failed with 0x%lx\n", hr);
        CloseHandle(hEvent);
        return 1;
    }

    // Wait for callback to signal that the device has been created
    printf("Waiting for the WireView virtual display adapter...\n");
    DWORD waitResult = WaitForSingleObject(hEvent, 10*1000);
    if (waitResult != WAIT_OBJECT_0)
    {
        printf("Wait for device creation failed\n");
        SwDeviceClose(hSwDevice);
        CloseHandle(hEvent);
        return 1;
    }
    if (FAILED(creationContext.Result))
    {
        printf("WireView device creation callback failed with 0x%lx\n", creationContext.Result);
        SwDeviceClose(hSwDevice);
        CloseHandle(hEvent);
        return 1;
    }
    printf("WireView virtual display adapter created\n\n");
    
    if (durationSeconds > 0)
    {
        printf("The test display will remain for %lu seconds\n", durationSeconds);
        std::this_thread::sleep_for(std::chrono::seconds(durationSeconds));
    }
    else
    {
        // Now wait for user to indicate the device should be stopped
        printf("Press 'x' to remove the test display and exit\n");
        bool bExit = false;
        do
        {
            int key = _getch();
            if (key == 'x' || key == 'X')
            {
                bExit = true;
            }
        } while (!bExit);
    }
    
    // Stop the device, this will cause the sample to be unloaded
    SwDeviceClose(hSwDevice);
    CloseHandle(hEvent);

    return 0;
}
