// ─── OpenDisplay IDD Virtual Display Driver ─────────────────────────────────
//
// Driver.cpp — DriverEntry, EvtDriverDeviceAdd, and IddCx adapter callbacks.
//
// This is a UMDF Indirect Display Driver (IDD) using the IddCx 1.4+ framework.
// It creates a virtual display visible in Windows Display Settings, enabling
// DXGI Desktop Duplication capture by the OpenDisplay sender application.
//
// Requirements: 1.1 (IDD-based virtual monitor), 9.3 (UMDF driver with IddCx)

#include "Driver.h"
#include "Monitor.h"

// ─── DriverEntry ─────────────────────────────────────────────────────────────
//
// Entry point for the UMDF driver. Initializes WDF driver object and registers
// the EvtDriverDeviceAdd callback so PnP can create device instances.

extern "C" NTSTATUS DriverEntry(
    _In_ PDRIVER_OBJECT  DriverObject,
    _In_ PUNICODE_STRING RegistryPath)
{
    WDF_DRIVER_CONFIG config;
    WDF_DRIVER_CONFIG_INIT(&config, EvtDriverDeviceAdd);

    NTSTATUS status = WdfDriverCreate(
        DriverObject,
        RegistryPath,
        WDF_NO_OBJECT_ATTRIBUTES,
        &config,
        WDF_NO_HANDLE);

    return status;
}

// ─── EvtDriverDeviceAdd ──────────────────────────────────────────────────────
//
// Called by the framework when PnP enumerates the device. Creates the WDF
// device, configures it with a device interface GUID for user-mode communication,
// and initiates asynchronous IddCx adapter initialization.

NTSTATUS EvtDriverDeviceAdd(
    _In_ WDFDRIVER       Driver,
    _Inout_ PWDFDEVICE_INIT DeviceInit)
{
    UNREFERENCED_PARAMETER(Driver);

    NTSTATUS status;

    // Mark this device as the IddCx adapter device
    IddCxDeviceInitConfig(DeviceInit, nullptr);

    // Configure file object handling for per-handle monitor tracking
    // and handle-close cleanup (Requirement 10.7).
    // This MUST be called before WdfDeviceCreate.
    MonitorConfigureFileObject(DeviceInit);

    // Configure device context
    WDF_OBJECT_ATTRIBUTES deviceAttributes;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&deviceAttributes, DeviceContext);

    WDFDEVICE device = nullptr;
    status = WdfDeviceCreate(&DeviceInit, &deviceAttributes, &device);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    // Create a device interface so the user-mode app can open a handle
    status = WdfDeviceCreateDeviceInterface(
        device,
        &GUID_DEVINTERFACE_OPENDISPLAY_IDD,
        nullptr);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    // Create the I/O queue for DeviceIoControl dispatch (IOCTL handlers)
    status = MonitorQueueInitialize(device);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    // Set up IddCx adapter initialization
    IDDCX_ADAPTER_CAPS adapterCaps = {};
    adapterCaps.Size = sizeof(adapterCaps);
    adapterCaps.MaxMonitorsSupported = 4;  // Support up to 4 concurrent sessions

    IDD_CX_CLIENT_CONFIG iddConfig = {};
    iddConfig.Size = sizeof(iddConfig);
    iddConfig.EvtIddCxAdapterInitFinished = EvtIddCxAdapterInitFinished;
    iddConfig.EvtIddCxAdapterCommitModes = EvtIddCxAdapterCommitModes;
    iddConfig.EvtIddCxMonitorGetDefaultDescriptionModes = EvtIddCxMonitorGetDefaultModes;
    iddConfig.EvtIddCxMonitorAssignSwapChain = EvtIddCxMonitorAssignSwapChain;
    iddConfig.EvtIddCxMonitorUnassignSwapChain = EvtIddCxMonitorUnassignSwapChain;

    status = IddCxDeviceInitialize(device);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    // Begin asynchronous adapter initialization
    // IddCx will call EvtIddCxAdapterInitFinished when ready.
    IDARG_IN_ADAPTER_INIT adapterInit = {};
    adapterInit.WdfDevice = device;
    adapterInit.pCaps = &adapterCaps;
    adapterInit.ObjectAttributes = WDF_NO_OBJECT_ATTRIBUTES;

    IDARG_OUT_ADAPTER_INIT adapterInitOut = {};

    status = IddCxAdapterInitAsync(&adapterInit, &adapterInitOut);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    // Store the adapter handle in device context for later use
    DeviceContext* deviceContext = GetDeviceContext(device);
    deviceContext->Adapter = adapterInitOut.AdapterObject;

    return STATUS_SUCCESS;
}

// ─── EvtIddCxAdapterInitFinished ────────────────────────────────────────────
//
// Called by IddCx when the adapter has been fully initialized and is ready
// for monitor creation. At this point, the driver can begin responding to
// IOCTL requests from the user-mode application to create virtual monitors.

NTSTATUS EvtIddCxAdapterInitFinished(
    _In_ IDDCX_ADAPTER AdapterObject,
    _In_ const IDARG_IN_ADAPTER_INIT_FINISHED* pInArgs)
{
    UNREFERENCED_PARAMETER(AdapterObject);

    // If initialization failed, the driver cannot proceed.
    // The adapter will not be usable and no monitors can be created.
    if (!NT_SUCCESS(pInArgs->AdapterInitStatus)) {
        return pInArgs->AdapterInitStatus;
    }

    // Adapter is ready — monitors can now be created via IOCTL from user mode.
    // No additional setup needed here; the DeviceIoControl handler in
    // Monitor.cpp will use the adapter handle to create monitors on demand.

    return STATUS_SUCCESS;
}

// ─── EvtIddCxAdapterCommitModes ─────────────────────────────────────────────
//
// Called by the OS display subsystem when it wants to commit a set of display
// modes to our virtual monitors. The OS provides the target mode for each
// active path. We accept the proposed modes since our virtual display can
// support any resolution within bounds.

NTSTATUS EvtIddCxAdapterCommitModes(
    _In_ IDDCX_ADAPTER AdapterObject,
    _In_ const IDARG_IN_COMMITMODES* pInArgs)
{
    UNREFERENCED_PARAMETER(AdapterObject);

    // Iterate through the paths the OS wants to commit.
    // For each monitor path, the OS tells us what mode it selected.
    // Since our virtual display is flexible (software-defined), we accept
    // any mode the OS proposes — the resolution is ultimately determined
    // by the receiver's panel dimensions communicated via the app.
    for (UINT i = 0; i < pInArgs->PathCount; i++) {
        // Each path contains the monitor and the selected target mode.
        // We could store the committed mode here if needed for validation,
        // but since Desktop Duplication captures whatever the compositor
        // renders, we simply acknowledge the commit.
        UNREFERENCED_PARAMETER(pInArgs->pPaths[i]);
    }

    return STATUS_SUCCESS;
}
