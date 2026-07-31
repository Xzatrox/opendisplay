#pragma once
// ─── OpenDisplay IDD Virtual Display Driver ─────────────────────────────────
//
// Driver.h — Declarations for DriverEntry, device callbacks, and IddCx adapter
// callbacks. This is the top-level header for the OpenDisplayIdd UMDF driver.

#include <windows.h>
#include <wdf.h>
#include <iddcx.h>

#include "DriverInterface.h"

// ─── Driver Context ──────────────────────────────────────────────────────────

// Per-adapter context stored with the IDDCX_ADAPTER object.
struct AdapterContext {
    WDFDEVICE WdfDevice;
    IDDCX_ADAPTER Adapter;
};
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(AdapterContext, GetAdapterContext);

// Per-device context stored with the WDFDEVICE object.
struct DeviceContext {
    IDDCX_ADAPTER Adapter;
};
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(DeviceContext, GetDeviceContext);

// ─── Driver Entry Points ─────────────────────────────────────────────────────

extern "C" DRIVER_INITIALIZE DriverEntry;

EVT_WDF_DRIVER_DEVICE_ADD EvtDriverDeviceAdd;

// ─── IddCx Adapter Callbacks ─────────────────────────────────────────────────

EVT_IDD_CX_ADAPTER_INIT_FINISHED EvtIddCxAdapterInitFinished;
EVT_IDD_CX_ADAPTER_COMMIT_MODES EvtIddCxAdapterCommitModes;

// ─── IddCx Monitor Callbacks (declared here, implemented in Monitor.cpp) ────

EVT_IDD_CX_MONITOR_GET_DEFAULT_DESCRIPTION_MODES EvtIddCxMonitorGetDefaultModes;
EVT_IDD_CX_MONITOR_ASSIGN_SWAPCHAIN EvtIddCxMonitorAssignSwapChain;
EVT_IDD_CX_MONITOR_UNASSIGN_SWAPCHAIN EvtIddCxMonitorUnassignSwapChain;
