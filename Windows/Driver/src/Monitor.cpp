// ─── OpenDisplay IDD Virtual Display Driver ─────────────────────────────────
//
// Monitor.cpp — Monitor creation/destruction, DeviceIoControl IOCTL handlers,
// IddCx monitor callbacks, and handle-close cleanup for orphan prevention.
//
// Requirements implemented:
//   1.1  IDD-based virtual monitor creation
//   1.2  Resolution derived from receiver panel (even, within bounds)
//   1.3  Monitor maintained as extended desktop while session active
//   1.4  Monitor removed within 2 seconds on session end
//   1.5  Monitor reconfigured on orientation change (resize = destroy+recreate)
//   1.6  Resolution range 640×480 – 2732×2048 @ 60 Hz
//   1.7  Error on failure, no partial-init left behind
//   1.8  Zero/negative or oversized dimensions rejected
//   10.7 Orphaned displays removed on unexpected process termination

#include "Driver.h"
#include "Monitor.h"

#include <wdf.h>
#include <iddcx.h>
#include <ntstrsafe.h>

// ─── Module-Level State ──────────────────────────────────────────────────────

// Simple atomic counter for generating unique monitor IDs.
static volatile LONG g_nextMonitorId = 1;

// ─── Resolution Validation ───────────────────────────────────────────────────
//
// Requirements 1.2, 1.6, 1.8:
//  - Width must be even, in [640, 2732]
//  - Height must be even, in [480, 2048]
//  - Zero dimensions are rejected
//  - Dimensions exceeding maximum are rejected

NTSTATUS ValidateResolution(uint32_t width, uint32_t height)
{
    // Reject zero dimensions (Requirement 1.8)
    if (width == 0 || height == 0) {
        return STATUS_INVALID_PARAMETER;
    }

    // Reject dimensions exceeding maximum (Requirement 1.8)
    if (width > kMaxWidth || height > kMaxHeight) {
        return STATUS_INVALID_PARAMETER;
    }

    // Reject dimensions below minimum (Requirement 1.6)
    if (width < kMinWidth || height < kMinHeight) {
        return STATUS_INVALID_PARAMETER;
    }

    // Reject odd dimensions — must be even (Requirement 1.2)
    if ((width % 2) != 0 || (height % 2) != 0) {
        return STATUS_INVALID_PARAMETER;
    }

    return STATUS_SUCCESS;
}

// ─── Monitor Creation ────────────────────────────────────────────────────────
//
// Creates a new IddCx virtual monitor with the specified resolution.
// The monitor appears immediately in Windows Display Settings.
// (Requirement 1.1, 1.3)

NTSTATUS MonitorCreate(
    _In_ IDDCX_ADAPTER adapter,
    _In_ WDFFILEOBJECT fileObject,
    _In_ const MonitorCreateParams* params,
    _Out_ MonitorCreateResult* result)
{
    NTSTATUS status;

    // Validate input parameters
    if (params == nullptr || result == nullptr) {
        return STATUS_INVALID_PARAMETER;
    }

    // Validate resolution constraints
    status = ValidateResolution(params->widthPixels, params->heightPixels);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    // Validate refresh rate — only 60 Hz is supported
    if (params->refreshHz != kRequiredRefreshHz) {
        return STATUS_INVALID_PARAMETER;
    }

    // Check that the file handle hasn't exceeded the maximum monitor count
    FileContext* fileCtx = GetFileContext(fileObject);
    if (fileCtx->MonitorCount >= kMaxMonitors) {
        return STATUS_QUOTA_EXCEEDED;
    }

    // Allocate a unique monitor ID
    uint32_t monitorId = (uint32_t)InterlockedIncrement(&g_nextMonitorId);

    // Configure the monitor object attributes to hold our per-monitor context
    WDF_OBJECT_ATTRIBUTES monitorAttributes;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&monitorAttributes, MonitorContext);

    // Set up the IddCx monitor info describing the physical connector
    IDDCX_MONITOR_INFO monitorInfo = {};
    monitorInfo.Size = sizeof(monitorInfo);
    monitorInfo.MonitorType = DISPLAYCONFIG_OUTPUT_TECHNOLOGY_OTHER;
    monitorInfo.ConnectorIndex = monitorId;

    // Provide a minimal EDID-like monitor description.
    // IddCx uses this to identify the monitor to the OS.
    monitorInfo.MonitorDescription.Size = sizeof(monitorInfo.MonitorDescription);
    monitorInfo.MonitorDescription.Type = IDDCX_MONITOR_DESCRIPTION_TYPE_EDID;
    // We supply no EDID blob — IddCx will use our default modes callback instead.
    monitorInfo.MonitorDescription.DataSize = 0;
    monitorInfo.MonitorDescription.pData = nullptr;

    // Build the monitor creation arguments
    IDARG_IN_MONITORCREATE monitorCreate = {};
    monitorCreate.ObjectAttributes = &monitorAttributes;
    monitorCreate.pMonitorInfo = &monitorInfo;

    IDARG_OUT_MONITORCREATE monitorCreateOut = {};

    // Create the IddCx monitor object (Requirement 1.1)
    status = IddCxMonitorCreate(adapter, &monitorCreate, &monitorCreateOut);
    if (!NT_SUCCESS(status)) {
        // Requirement 1.7: no partial-init left behind on failure
        return status;
    }

    IDDCX_MONITOR monitor = monitorCreateOut.MonitorObject;

    // Initialize the per-monitor context
    MonitorContext* monCtx = GetMonitorContext(monitor);
    monCtx->MonitorId = monitorId;
    monCtx->WidthPixels = params->widthPixels;
    monCtx->HeightPixels = params->heightPixels;
    monCtx->RefreshHz = params->refreshHz;
    monCtx->MonitorObject = monitor;
    monCtx->OwnerFileObject = fileObject;

    // Notify IddCx that the monitor has arrived (plugged in)
    IDARG_IN_MONITORARRIVAL arrivalArgs = {};
    status = IddCxMonitorArrival(monitor, &arrivalArgs);
    if (!NT_SUCCESS(status)) {
        // Requirement 1.7: clean up on failure — departure then destroy
        IddCxMonitorDeparture(monitor);
        return status;
    }

    // Register the monitor in the file context for handle-close cleanup
    fileCtx->Monitors[fileCtx->MonitorCount] = monitor;
    fileCtx->MonitorIds[fileCtx->MonitorCount] = monitorId;
    fileCtx->MonitorCount++;

    // Fill output result
    result->monitorId = monitorId;
    // The adapter LUID is retrieved from the adapter object. For simplicity,
    // we return the monitor ID — the app resolves the adapter LUID via
    // DXGI enumeration matching the device interface path.
    result->adapterLuid = 0;  // App resolves via DXGI

    return STATUS_SUCCESS;
}

// ─── Monitor Destruction ─────────────────────────────────────────────────────
//
// Destroys a previously created virtual monitor, removing it from Windows
// Display Settings. (Requirement 1.4)

NTSTATUS MonitorDestroy(
    _In_ WDFFILEOBJECT fileObject,
    _In_ uint32_t monitorId)
{
    FileContext* fileCtx = GetFileContext(fileObject);

    // Find the monitor by ID in the file context's array
    for (uint32_t i = 0; i < fileCtx->MonitorCount; i++) {
        if (fileCtx->MonitorIds[i] == monitorId) {
            IDDCX_MONITOR monitor = fileCtx->Monitors[i];

            // Signal departure (unplugged) then the OS will tear down
            NTSTATUS status = IddCxMonitorDeparture(monitor);
            if (!NT_SUCCESS(status)) {
                return status;
            }

            // Remove from tracking array (compact by shifting)
            fileCtx->MonitorCount--;
            if (i < fileCtx->MonitorCount) {
                fileCtx->Monitors[i] = fileCtx->Monitors[fileCtx->MonitorCount];
                fileCtx->MonitorIds[i] = fileCtx->MonitorIds[fileCtx->MonitorCount];
            }
            fileCtx->Monitors[fileCtx->MonitorCount] = nullptr;
            fileCtx->MonitorIds[fileCtx->MonitorCount] = 0;

            return STATUS_SUCCESS;
        }
    }

    // Monitor ID not found for this handle
    return STATUS_NOT_FOUND;
}

// ─── Monitor Resize ──────────────────────────────────────────────────────────
//
// Resizes an existing monitor by destroying it and recreating with new
// dimensions. (Requirement 1.5)
//
// IddCx does not support in-place resolution changes for an active monitor
// (IddCxMonitorUpdateModes requires the monitor to not have an active swap
// chain in some driver versions), so the safest approach is destroy + recreate.

NTSTATUS MonitorResize(
    _In_ IDDCX_ADAPTER adapter,
    _In_ WDFFILEOBJECT fileObject,
    _In_ const MonitorResizeParams* params)
{
    if (params == nullptr) {
        return STATUS_INVALID_PARAMETER;
    }

    // Validate new resolution
    NTSTATUS status = ValidateResolution(params->newWidthPixels, params->newHeightPixels);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    // Destroy the existing monitor
    status = MonitorDestroy(fileObject, params->monitorId);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    // Recreate with new dimensions
    MonitorCreateParams createParams = {};
    createParams.widthPixels = params->newWidthPixels;
    createParams.heightPixels = params->newHeightPixels;
    createParams.refreshHz = kRequiredRefreshHz;

    MonitorCreateResult createResult = {};
    status = MonitorCreate(adapter, fileObject, &createParams, &createResult);
    if (!NT_SUCCESS(status)) {
        // Requirement 1.7: resize failed, old monitor already gone,
        // report error — no partial state remains.
        return status;
    }

    return STATUS_SUCCESS;
}

// ─── Destroy All Monitors (Handle-Close Cleanup) ─────────────────────────────
//
// Called when a file handle is closed (normal or crash). Removes all monitors
// owned by that handle to prevent orphaned displays. (Requirement 10.7)

void MonitorDestroyAll(_In_ WDFFILEOBJECT fileObject)
{
    FileContext* fileCtx = GetFileContext(fileObject);

    // Destroy monitors in reverse order
    while (fileCtx->MonitorCount > 0) {
        uint32_t idx = fileCtx->MonitorCount - 1;
        IDDCX_MONITOR monitor = fileCtx->Monitors[idx];

        if (monitor != nullptr) {
            // Best-effort departure — ignore errors during cleanup
            IddCxMonitorDeparture(monitor);
        }

        fileCtx->Monitors[idx] = nullptr;
        fileCtx->MonitorIds[idx] = 0;
        fileCtx->MonitorCount--;
    }
}

// ─── IddCx Monitor Callbacks ─────────────────────────────────────────────────

// EvtIddCxMonitorGetDefaultModes
//
// Called by IddCx to query the supported display modes for a monitor.
// We provide the current resolution as the only target mode.
// (Supports Requirement 1.6 — resolution within [640×480, 2732×2048] @ 60 Hz)

NTSTATUS EvtIddCxMonitorGetDefaultModes(
    _In_ IDDCX_MONITOR MonitorObject,
    _In_ const IDARG_IN_GETDEFAULTDESCRIPTIONMODES* pInArgs,
    _Out_ IDARG_OUT_GETDEFAULTDESCRIPTIONMODES* pOutArgs)
{
    MonitorContext* monCtx = GetMonitorContext(MonitorObject);

    // We provide exactly one mode: the current resolution at the configured Hz.
    pOutArgs->DefaultMonitorModeBufferOutputCount = 0;

    if (pInArgs->DefaultMonitorModeBufferInputCount == 0) {
        // First call: OS is querying how many modes we have
        pOutArgs->DefaultMonitorModeBufferOutputCount = 1;
        return STATUS_SUCCESS;
    }

    // Second call: fill in the mode(s)
    if (pInArgs->DefaultMonitorModeBufferInputCount >= 1 &&
        pInArgs->pDefaultMonitorModes != nullptr) {

        IDDCX_MONITOR_MODE* mode = &pInArgs->pDefaultMonitorModes[0];
        RtlZeroMemory(mode, sizeof(*mode));
        mode->Size = sizeof(*mode);
        mode->Origin = IDDCX_MONITOR_MODE_ORIGIN_DRIVER;

        // Fill DISPLAYCONFIG_VIDEO_SIGNAL_INFO
        mode->MonitorVideoSignalInfo.totalSize.cx = monCtx->WidthPixels;
        mode->MonitorVideoSignalInfo.totalSize.cy = monCtx->HeightPixels;
        mode->MonitorVideoSignalInfo.activeSize.cx = monCtx->WidthPixels;
        mode->MonitorVideoSignalInfo.activeSize.cy = monCtx->HeightPixels;
        mode->MonitorVideoSignalInfo.AdditionalSignalInfo.vSyncFreqDivider = 1;
        mode->MonitorVideoSignalInfo.AdditionalSignalInfo.videoStandard =
            D3DKMDT_VSS_OTHER;
        // Pixel clock and sync rates for 60 Hz
        // vSyncFreq = pixelRate / (totalSize.cx * totalSize.cy)
        // We set vSyncFreq directly as a rational.
        mode->MonitorVideoSignalInfo.vSyncFreq.Numerator = monCtx->RefreshHz;
        mode->MonitorVideoSignalInfo.vSyncFreq.Denominator = 1;
        mode->MonitorVideoSignalInfo.hSyncFreq.Numerator =
            monCtx->RefreshHz * monCtx->HeightPixels;
        mode->MonitorVideoSignalInfo.hSyncFreq.Denominator = 1;
        mode->MonitorVideoSignalInfo.pixelRate =
            (UINT64)monCtx->WidthPixels * monCtx->HeightPixels * monCtx->RefreshHz;
        mode->MonitorVideoSignalInfo.scanLineOrdering =
            DISPLAYCONFIG_SCANLINE_ORDERING_PROGRESSIVE;

        pOutArgs->DefaultMonitorModeBufferOutputCount = 1;
        pOutArgs->PreferredMonitorModeIdx = 0;
    }

    return STATUS_SUCCESS;
}

// EvtIddCxMonitorAssignSwapChain
//
// Called by IddCx when the DWM assigns a swap chain to our monitor for
// frame presentation. The swap chain delivers composed desktop frames.
// For the capture path, we accept the swap chain and begin processing frames
// in the SwapChainProcessor (implemented in SwapChain.cpp).

NTSTATUS EvtIddCxMonitorAssignSwapChain(
    _In_ IDDCX_MONITOR MonitorObject,
    _In_ const IDARG_IN_SETSWAPCHAIN* pInArgs)
{
    UNREFERENCED_PARAMETER(MonitorObject);
    UNREFERENCED_PARAMETER(pInArgs);

    // The swap chain is assigned. In a full implementation, we would start
    // the SwapChainProcessor thread here to present frames. For the capture
    // path, DXGI Desktop Duplication captures from the compositor directly,
    // so the swap chain processor simply acquires and releases frames to keep
    // the DWM rendering pipeline active.
    //
    // SwapChain.cpp handles the actual frame processing.

    return STATUS_SUCCESS;
}

// EvtIddCxMonitorUnassignSwapChain
//
// Called by IddCx when the DWM removes the swap chain from our monitor
// (e.g., monitor departure, mode change). Clean up any swap chain resources.

NTSTATUS EvtIddCxMonitorUnassignSwapChain(
    _In_ IDDCX_MONITOR MonitorObject)
{
    UNREFERENCED_PARAMETER(MonitorObject);

    // Stop swap chain processing. In the full implementation, signal the
    // SwapChainProcessor thread to stop and wait for it to exit.

    return STATUS_SUCCESS;
}

// ─── DeviceIoControl Dispatch ────────────────────────────────────────────────
//
// EvtIoDeviceControl handles IOCTL requests from the user-mode application
// for creating, destroying, and resizing virtual monitors.

VOID EvtIoDeviceControl(
    _In_ WDFQUEUE Queue,
    _In_ WDFREQUEST Request,
    _In_ size_t OutputBufferLength,
    _In_ size_t InputBufferLength,
    _In_ ULONG IoControlCode)
{
    UNREFERENCED_PARAMETER(OutputBufferLength);
    UNREFERENCED_PARAMETER(InputBufferLength);

    NTSTATUS status = STATUS_INVALID_DEVICE_REQUEST;
    size_t bytesReturned = 0;

    // Get the device and adapter from the queue's parent device
    WDFDEVICE device = WdfIoQueueGetDevice(Queue);
    DeviceContext* devCtx = GetDeviceContext(device);
    IDDCX_ADAPTER adapter = devCtx->Adapter;

    // Get the file object associated with this request (identifies the app handle)
    WDFFILEOBJECT fileObject = WdfRequestGetFileObject(Request);

    switch (IoControlCode) {

    case IOCTL_CREATE_MONITOR: {
        // Retrieve input buffer (MonitorCreateParams)
        MonitorCreateParams* params = nullptr;
        status = WdfRequestRetrieveInputBuffer(
            Request,
            sizeof(MonitorCreateParams),
            reinterpret_cast<PVOID*>(&params),
            nullptr);
        if (!NT_SUCCESS(status)) {
            break;
        }

        // Retrieve output buffer (MonitorCreateResult)
        MonitorCreateResult* result = nullptr;
        status = WdfRequestRetrieveOutputBuffer(
            Request,
            sizeof(MonitorCreateResult),
            reinterpret_cast<PVOID*>(&result),
            nullptr);
        if (!NT_SUCCESS(status)) {
            break;
        }

        // Create the monitor
        status = MonitorCreate(adapter, fileObject, params, result);
        if (NT_SUCCESS(status)) {
            bytesReturned = sizeof(MonitorCreateResult);
        }
        break;
    }

    case IOCTL_DESTROY_MONITOR: {
        // Retrieve input buffer (MonitorDestroyParams)
        MonitorDestroyParams* params = nullptr;
        status = WdfRequestRetrieveInputBuffer(
            Request,
            sizeof(MonitorDestroyParams),
            reinterpret_cast<PVOID*>(&params),
            nullptr);
        if (!NT_SUCCESS(status)) {
            break;
        }

        // Destroy the monitor
        status = MonitorDestroy(fileObject, params->monitorId);
        break;
    }

    case IOCTL_RESIZE_MONITOR: {
        // Retrieve input buffer (MonitorResizeParams)
        MonitorResizeParams* params = nullptr;
        status = WdfRequestRetrieveInputBuffer(
            Request,
            sizeof(MonitorResizeParams),
            reinterpret_cast<PVOID*>(&params),
            nullptr);
        if (!NT_SUCCESS(status)) {
            break;
        }

        // Resize the monitor (validates resolution internally)
        status = MonitorResize(adapter, fileObject, params);
        break;
    }

    default:
        status = STATUS_INVALID_DEVICE_REQUEST;
        break;
    }

    // Complete the request with the status and bytes returned
    WdfRequestCompleteWithInformation(Request, status, bytesReturned);
}

// ─── File Object Configuration ───────────────────────────────────────────────
//
// Configures file object settings on the WDFDEVICE_INIT structure. This MUST
// be called during EvtDriverDeviceAdd BEFORE WdfDeviceCreate so that WDF
// allocates per-file-object context and dispatches close/cleanup callbacks.
//
// This enables tracking which monitors belong to which app handle, and
// cleanup on unexpected process termination. (Requirement 10.7)

void MonitorConfigureFileObject(_Inout_ PWDFDEVICE_INIT DeviceInit)
{
    WDF_FILEOBJECT_CONFIG fileConfig;
    WDF_FILEOBJECT_CONFIG_INIT(
        &fileConfig,
        WDF_NO_EVENT_CALLBACK,   // EvtDeviceFileCreate — not needed
        EvtFileClose,            // EvtFileClose — cleanup on handle close
        EvtFileCleanup           // EvtFileCleanup — early cleanup
    );

    WDF_OBJECT_ATTRIBUTES fileAttributes;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&fileAttributes, FileContext);

    WdfDeviceInitSetFileObjectConfig(DeviceInit, &fileConfig, &fileAttributes);
}

// ─── I/O Queue Initialization ────────────────────────────────────────────────
//
// Creates the default I/O queue for the device to handle DeviceIoControl
// requests. Must be called AFTER WdfDeviceCreate.

NTSTATUS MonitorQueueInitialize(_In_ WDFDEVICE device)
{
    WDF_IO_QUEUE_CONFIG queueConfig;
    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&queueConfig, WdfIoQueueDispatchParallel);
    queueConfig.EvtIoDeviceControl = EvtIoDeviceControl;

    WDFQUEUE queue = nullptr;
    NTSTATUS status = WdfIoQueueCreate(
        device, &queueConfig, WDF_NO_OBJECT_ATTRIBUTES, &queue);

    return status;
}

// ─── File Handle Close Callback ──────────────────────────────────────────────
//
// Called when the application's handle to the driver device is closed. This
// includes unexpected process termination (crash, kill). We destroy all monitors
// owned by this handle to prevent orphaned displays. (Requirement 10.7)

VOID EvtFileClose(
    _In_ WDFFILEOBJECT FileObject)
{
    MonitorDestroyAll(FileObject);
}

// EvtFileCleanup — Called during IRP_MJ_CLEANUP (handle being closed but file
// object not yet destroyed). We perform the same cleanup here for immediate
// orphan removal.

VOID EvtFileCleanup(
    _In_ WDFFILEOBJECT FileObject)
{
    MonitorDestroyAll(FileObject);
}
