#pragma once
// ─── OpenDisplay IDD Virtual Display Driver ─────────────────────────────────
//
// Monitor.h — Declarations for monitor management, IOCTL dispatch, and IddCx
// monitor callbacks. Implements virtual display creation, destruction, resize,
// and handle-close cleanup for unexpected process termination.
//
// Requirements: 1.1, 1.2, 1.3, 1.4, 1.5, 1.6, 1.7, 1.8, 10.7

#include <windows.h>
#include <wdf.h>
#include <iddcx.h>

#include "DriverInterface.h"

// ─── Constants ───────────────────────────────────────────────────────────────

// Resolution constraints matching design spec and DriverInterface.h comments.
static constexpr uint32_t kMinWidth = 640;
static constexpr uint32_t kMaxWidth = 2732;
static constexpr uint32_t kMinHeight = 480;
static constexpr uint32_t kMaxHeight = 2048;
static constexpr uint32_t kRequiredRefreshHz = 60;
static constexpr uint32_t kMaxMonitors = 4;

// ─── Per-Monitor Context ─────────────────────────────────────────────────────

// Stored with each IDDCX_MONITOR object to track current resolution.
struct MonitorContext {
    uint32_t MonitorId;
    uint32_t WidthPixels;
    uint32_t HeightPixels;
    uint32_t RefreshHz;
    IDDCX_MONITOR MonitorObject;
    WDFFILEOBJECT OwnerFileObject;  // File handle that created this monitor
};
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(MonitorContext, GetMonitorContext);

// ─── Per-FileObject Context ──────────────────────────────────────────────────

// Tracks monitors created by a particular file handle (app connection).
// When the handle is closed (normal or crash), all associated monitors
// are destroyed to prevent orphaned displays. (Requirement 10.7)
struct FileContext {
    IDDCX_MONITOR Monitors[kMaxMonitors];
    uint32_t MonitorIds[kMaxMonitors];
    uint32_t MonitorCount;
};
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(FileContext, GetFileContext);

// ─── Validation ──────────────────────────────────────────────────────────────

// Validates resolution constraints per Requirements 1.2, 1.6, 1.8:
//  - Width must be even, in [640, 2732]
//  - Height must be even, in [480, 2048]
//  - Zero or negative dimensions are rejected
//  - Dimensions exceeding max are rejected
NTSTATUS ValidateResolution(uint32_t width, uint32_t height);

// ─── Monitor Lifecycle ───────────────────────────────────────────────────────

// Create a new virtual monitor with the given resolution.
NTSTATUS MonitorCreate(
    _In_ IDDCX_ADAPTER adapter,
    _In_ WDFFILEOBJECT fileObject,
    _In_ const MonitorCreateParams* params,
    _Out_ MonitorCreateResult* result);

// Destroy a previously created virtual monitor.
NTSTATUS MonitorDestroy(
    _In_ WDFFILEOBJECT fileObject,
    _In_ uint32_t monitorId);

// Resize an existing monitor (destroy + recreate with new dimensions).
NTSTATUS MonitorResize(
    _In_ IDDCX_ADAPTER adapter,
    _In_ WDFFILEOBJECT fileObject,
    _In_ const MonitorResizeParams* params);

// Remove all monitors owned by a file handle (handle-close cleanup).
void MonitorDestroyAll(_In_ WDFFILEOBJECT fileObject);

// ─── WDF Queue / IOCTL Dispatch ──────────────────────────────────────────────

// Configures file object settings on the WDFDEVICE_INIT before device creation.
// Must be called during EvtDriverDeviceAdd BEFORE WdfDeviceCreate.
void MonitorConfigureFileObject(_Inout_ PWDFDEVICE_INIT DeviceInit);

// Creates the default I/O queue for DeviceIoControl dispatch.
// Must be called AFTER WdfDeviceCreate.
NTSTATUS MonitorQueueInitialize(_In_ WDFDEVICE device);

// EvtIoDeviceControl callback — dispatches IOCTL_CREATE/DESTROY/RESIZE_MONITOR.
EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL EvtIoDeviceControl;

// EvtFileClose callback — called when the app's handle to the driver is closed
// (including on unexpected process termination). Removes all monitors owned by
// that handle. (Requirement 10.7)
EVT_WDF_FILE_CLOSE EvtFileClose;

// EvtFileCleanup callback — called during handle cleanup.
EVT_WDF_FILE_CLEANUP EvtFileCleanup;


