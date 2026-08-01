// OpenDisplay Windows Sender — Application entry point
//
// Initializes COM, Media Foundation, and Winsock, then creates the main window
// and wires UI events to the SessionController. On exit, invokes graceful
// shutdown on all sessions within a 3-second deadline.
//
// Validates: Requirements 9.2, 10.5

#include <winsock2.h>
#include <windows.h>
#include <shellscalingapi.h>
#include <mfapi.h>
#include <objbase.h>

#include <iostream>
#include <memory>
#include <string>
#include <cstdio>

#include "BonjourBrowser.h"
#include "SessionController.h"
#include "WireTransport.h"
#include "UI/MainWindow.h"

// ─── Forward declarations ────────────────────────────────────────────────────

static bool InitializeCOM();
static bool InitializeMediaFoundation();
static bool InitializeWinsock();
static void ShutdownMediaFoundation();
static void ShutdownWinsock();
static void RunMessageLoop();

// ─── Globals ─────────────────────────────────────────────────────────────────

static WSADATA g_wsaData = {};
static bool g_mfInitialized = false;
static bool g_comInitialized = false;
static bool g_winsockInitialized = false;

// ─── WinMain Entry Point ─────────────────────────────────────────────────────

int WINAPI WinMain(
    _In_ HINSTANCE hInstance,
    _In_opt_ HINSTANCE hPrevInstance,
    _In_ LPSTR lpCmdLine,
    _In_ int nCmdShow)
{
    (void)hPrevInstance;
    (void)lpCmdLine;
    (void)nCmdShow;

    // Allocate a console for debug output (stderr logging)
    AllocConsole();
    FILE* dummy;
    freopen_s(&dummy, "CONOUT$", "w", stderr);
    std::cerr << "[OpenDisplay] Starting...\n";

    // Set DPI awareness BEFORE any DXGI/window creation.
    // Desktop Duplication requires the process to be DPI-aware.
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    // ── Step 1: Initialize COM (apartment-threaded for WinUI/media) ──────────
    if (!InitializeCOM()) {
        MessageBoxW(nullptr, L"Failed to initialize COM runtime.",
                    L"OpenDisplay", MB_OK | MB_ICONERROR);
        return 1;
    }

    // ── Step 2: Initialize Media Foundation ──────────────────────────────────
    if (!InitializeMediaFoundation()) {
        MessageBoxW(nullptr, L"Failed to initialize Media Foundation.",
                    L"OpenDisplay", MB_OK | MB_ICONERROR);
        CoUninitialize();
        return 1;
    }

    // ── Step 3: Initialize Winsock ───────────────────────────────────────────
    if (!InitializeWinsock()) {
        MessageBoxW(nullptr, L"Failed to initialize Winsock.",
                    L"OpenDisplay", MB_OK | MB_ICONERROR);
        ShutdownMediaFoundation();
        CoUninitialize();
        return 1;
    }

    // ── Step 4: Create core components ───────────────────────────────────────

    // Bonjour discovery browser for WiFi receivers
    auto bonjourBrowser = std::make_shared<BonjourBrowser>();

    // AMDS client for USB device communication
    auto amdsClient = std::make_shared<AmdsClient>();

    // Session controller (manages the streaming pipeline)
    auto sessionController = std::make_shared<SessionController>();
    sessionController->SetBonjourBrowser(bonjourBrowser);

    // ── Step 5: Create and initialize the main window ────────────────────────

    OpenDisplay::MainWindow mainWindow;
    HRESULT hr = mainWindow.Initialize(hInstance);
    if (FAILED(hr)) {
        MessageBoxW(nullptr, L"Failed to create application window.",
                    L"OpenDisplay", MB_OK | MB_ICONERROR);
        ShutdownWinsock();
        ShutdownMediaFoundation();
        CoUninitialize();
        return 1;
    }

    // ── Step 6: Wire components together ─────────────────────────────────────

    // Give the main window access to the session controller.
    // SetSessionController internally wires the state callback to the UI.
    mainWindow.SetSessionController(sessionController);

    // Wire Bonjour discovery results to the connection picker UI
    bonjourBrowser->SetCallback(
        [&mainWindow](const std::vector<BonjourBrowser::DiscoveredService>& services) {
            // Update the connection picker with WiFi-discovered devices
            auto& picker = mainWindow.GetConnectionPicker();
            std::vector<DeviceInfo> wifiDevices;
            wifiDevices.reserve(services.size());
            for (const auto& svc : services) {
                DeviceInfo info;
                info.name = svc.name;
                info.udid = svc.installId;
                info.host = svc.host;
                info.port = svc.port;
                info.isUSB = false;
                wifiDevices.push_back(std::move(info));
            }
            picker.UpdateWiFiDevices(wifiDevices);
            mainWindow.NotifyDevicesChanged();
        });

    // Wire AMDS device attach/detach events to the connection picker and session
    HRESULT amdsHr = amdsClient->Connect();
    if (SUCCEEDED(amdsHr)) {
        // Enumerate initially-attached USB devices BEFORE Subscribe(),
        // because Subscribe() takes over the connection for async event
        // reading and ListDevices() cannot share the same pipe/socket.
        std::vector<AmdsClient::Device> existingDevices;
        if (SUCCEEDED(amdsClient->ListDevices(existingDevices))) {
            auto& picker = mainWindow.GetConnectionPicker();
            for (const auto& device : existingDevices) {
                DeviceInfo info;
                info.name = device.name;
                info.udid = device.udid;
                info.isUSB = true;
                info.port = 9000;
                picker.AddUSBDevice(info);
            }
        }

        // Now subscribe for future attach/detach events.
        // This takes over the AMDS connection — no further ListDevices
        // calls should be made on this client instance.
        amdsClient->Subscribe(
            [&mainWindow, &sessionController](const AmdsClient::Device& device, bool attached) {
                DeviceInfo info;
                info.name = device.name;
                info.udid = device.udid;
                info.isUSB = true;
                info.port = 9000;

                auto& picker = mainWindow.GetConnectionPicker();
                if (attached) {
                    picker.AddUSBDevice(info);
                    sessionController->OnUSBDeviceAttached(info);
                } else {
                    picker.RemoveUSBDevice(info.udid);
                    sessionController->OnUSBDeviceDetached(device.udid);
                }
                mainWindow.NotifyDevicesChanged();
            });
    } else {
        // AMDS unavailable — show message to install iTunes/Apple Devices
        mainWindow.ShowError("USB Not Available",
            "Apple Mobile Device Service not found. "
            "Install iTunes or Apple Devices from the Microsoft Store "
            "to connect iPad via USB.");
    }

    // ── Step 7: Start discovery services ─────────────────────────────────────

    bonjourBrowser->StartBrowsing();

    // ── Step 8: Show window and run message loop ─────────────────────────────

    mainWindow.Show();
    RunMessageLoop();

    // ── Step 9: App exit — graceful shutdown ─────────────────────────────────
    // Shut down all active sessions within 3 seconds (Req 10.5).
    // After the deadline, the IDD driver's handle-close callback removes
    // any orphaned virtual displays on process exit (Req 10.7).

    SessionController::ShutdownAllSessions();

    // ── Step 10: Stop discovery and cleanup ──────────────────────────────────

    bonjourBrowser->StopBrowsing();

    // Release shared pointers before system teardown
    sessionController.reset();
    amdsClient.reset();
    bonjourBrowser.reset();

    // ── Step 11: Shutdown subsystems in reverse order ────────────────────────

    ShutdownWinsock();
    ShutdownMediaFoundation();
    CoUninitialize();

    return 0;
}

// ─── Initialization Helpers ──────────────────────────────────────────────────

/// Initialize COM with apartment-threaded model.
/// Required for WinUI 3 / XAML framework and Media Foundation.
static bool InitializeCOM()
{
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(hr)) {
        std::cerr << "[main] CoInitializeEx failed: 0x"
                  << std::hex << hr << std::dec << "\n";
        return false;
    }
    g_comInitialized = true;
    return true;
}

/// Initialize Media Foundation (lightweight startup for encoder support).
static bool InitializeMediaFoundation()
{
    HRESULT hr = MFStartup(MF_VERSION, MFSTARTUP_LITE);
    if (FAILED(hr)) {
        std::cerr << "[main] MFStartup failed: 0x"
                  << std::hex << hr << std::dec << "\n";
        return false;
    }
    g_mfInitialized = true;
    return true;
}

/// Initialize Winsock 2.2 for TCP transport.
static bool InitializeWinsock()
{
    int result = WSAStartup(MAKEWORD(2, 2), &g_wsaData);
    if (result != 0) {
        std::cerr << "[main] WSAStartup failed: " << result << "\n";
        return false;
    }
    g_winsockInitialized = true;
    return true;
}

/// Shutdown Media Foundation.
static void ShutdownMediaFoundation()
{
    if (g_mfInitialized) {
        MFShutdown();
        g_mfInitialized = false;
    }
}

/// Shutdown Winsock.
static void ShutdownWinsock()
{
    if (g_winsockInitialized) {
        WSACleanup();
        g_winsockInitialized = false;
    }
}

/// Standard Win32 message loop.
static void RunMessageLoop()
{
    MSG msg = {};
    while (GetMessage(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
}
