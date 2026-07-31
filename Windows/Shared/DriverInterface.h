#pragma once
// ─── OpenDisplay Driver Interface ────────────────────────────────────────────
//
// Shared header between the user-mode application and the IDD virtual display
// driver. Defines IOCTL codes and structures used for DeviceIoControl
// communication.
//
// Both the App and Driver targets include this file via the Shared/ directory.

#include <cstdint>

#ifdef _WIN32
#include <windows.h>
#include <winioctl.h>
#endif

// ─── Device Interface GUID ───────────────────────────────────────────────────
// {E4F3C5A1-7B2D-4F8A-9C1E-6D3A5B8F2E70}
// Used by the app to open a handle to the driver via SetupDiGetClassDevs +
// CreateFile.
// clang-format off
static constexpr GUID GUID_DEVINTERFACE_OPENDISPLAY_IDD = {
    0xE4F3C5A1, 0x7B2D, 0x4F8A,
    { 0x9C, 0x1E, 0x6D, 0x3A, 0x5B, 0x8F, 0x2E, 0x70 }
};
// clang-format on

// ─── IOCTL Codes ─────────────────────────────────────────────────────────────
// Device type for OpenDisplay driver (vendor-defined range: 0x8000+)
#define OPENDISPLAY_DEVICE_TYPE 0x8000

// Function codes (vendor-defined range: 0x800+)
#define IOCTL_CREATE_MONITOR \
    CTL_CODE(OPENDISPLAY_DEVICE_TYPE, 0x800, METHOD_BUFFERED, FILE_ANY_ACCESS)

#define IOCTL_DESTROY_MONITOR \
    CTL_CODE(OPENDISPLAY_DEVICE_TYPE, 0x801, METHOD_BUFFERED, FILE_ANY_ACCESS)

#define IOCTL_RESIZE_MONITOR \
    CTL_CODE(OPENDISPLAY_DEVICE_TYPE, 0x802, METHOD_BUFFERED, FILE_ANY_ACCESS)

// ─── Shared Structures ───────────────────────────────────────────────────────

// Input for IOCTL_CREATE_MONITOR
// Creates a new virtual monitor with the specified resolution.
struct MonitorCreateParams {
    uint32_t widthPixels;   // Must be even, range [640, 2732]
    uint32_t heightPixels;  // Must be even, range [480, 2048]
    uint32_t refreshHz;     // Refresh rate in Hz (typically 60)
};

// Output for IOCTL_CREATE_MONITOR
// Returns the created monitor's identifier and adapter LUID for DXGI
// enumeration.
struct MonitorCreateResult {
    uint32_t monitorId;     // Handle for subsequent operations (destroy, resize)
    uint32_t adapterLuid;   // LUID of the adapter hosting the virtual display
};

// Input for IOCTL_DESTROY_MONITOR
// Destroys a previously created virtual monitor.
struct MonitorDestroyParams {
    uint32_t monitorId;     // Monitor handle from MonitorCreateResult
};

// Input for IOCTL_RESIZE_MONITOR
// Resizes an existing virtual monitor to new dimensions.
struct MonitorResizeParams {
    uint32_t monitorId;       // Monitor handle from MonitorCreateResult
    uint32_t newWidthPixels;  // Must be even, range [640, 2732]
    uint32_t newHeightPixels; // Must be even, range [480, 2048]
};
