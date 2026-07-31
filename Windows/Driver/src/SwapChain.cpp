// ─── OpenDisplay IDD Virtual Display Driver ─────────────────────────────────
//
// SwapChain.cpp — SwapChainProcessor implementation and IddCx swap chain
// callbacks (EvtIddCxMonitorAssignSwapChain, EvtIddCxMonitorUnassignSwapChain).
//
// The SwapChainProcessor runs in a dedicated thread. It waits for the OS to
// signal that a new buffer is available in the swap chain, acquires the frame
// surface, and immediately releases it back. This "no-op presentation" allows
// the DWM compositor to include the virtual display's content, which the
// user-mode application then captures via DXGI Desktop Duplication.
//
// Requirements: 1.3 (maintain virtual display as extended desktop target),
//               2.1 (enable DXGI Desktop Duplication capture of virtual display)

#include "SwapChain.h"
#include "Driver.h"

// ─── SwapChainProcessor ──────────────────────────────────────────────────────

SwapChainProcessor::SwapChainProcessor(IDDCX_SWAPCHAIN hSwapChain,
                                       HANDLE hAvailableBufferEvent)
    : m_hSwapChain(hSwapChain)
    , m_hAvailableBufferEvent(hAvailableBufferEvent)
    , m_hTerminateEvent(nullptr)
    , m_hThread(nullptr)
{
    // Create the termination event (manual-reset, initially non-signaled)
    m_hTerminateEvent = CreateEvent(nullptr, TRUE, FALSE, nullptr);

    // Create the D3D device and assign it to the swap chain
    if (SUCCEEDED(CreateD3DDevice())) {
        // Tell IddCx which D3D device to use for this swap chain.
        // The device must be set before frames can be acquired.
        IDDCX_SWAPCHAIN_SETDEVICE_IN setDeviceIn = {};
        setDeviceIn.Size = sizeof(setDeviceIn);
        setDeviceIn.pDevice = m_d3dDevice.Get();
        IddCxSwapChainSetDevice(m_hSwapChain, &setDeviceIn);

        // Start the processing thread
        m_hThread = CreateThread(nullptr, 0, RunThread, this, 0, nullptr);
    }
}

SwapChainProcessor::~SwapChainProcessor()
{
    // Signal the thread to stop
    if (m_hTerminateEvent) {
        SetEvent(m_hTerminateEvent);
    }

    // Wait for the thread to finish (with a reasonable timeout)
    if (m_hThread) {
        WaitForSingleObject(m_hThread, 5000);
        CloseHandle(m_hThread);
        m_hThread = nullptr;
    }

    if (m_hTerminateEvent) {
        CloseHandle(m_hTerminateEvent);
        m_hTerminateEvent = nullptr;
    }
}

HRESULT SwapChainProcessor::CreateD3DDevice()
{
    // Create a D3D11 device for the swap chain. The driver needs a device
    // so IddCx can hand off surfaces for "presentation". We create a basic
    // hardware device; feature level doesn't matter much since we're not
    // doing any rendering — just accepting and releasing frames.
    D3D_FEATURE_LEVEL featureLevels[] = {
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1,
        D3D_FEATURE_LEVEL_10_0,
    };

    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;

    HRESULT hr = D3D11CreateDevice(
        nullptr,                      // Default adapter
        D3D_DRIVER_TYPE_HARDWARE,     // Hardware device
        nullptr,                      // No software rasterizer
        flags,
        featureLevels,
        ARRAYSIZE(featureLevels),
        D3D11_SDK_VERSION,
        m_d3dDevice.GetAddressOf(),
        nullptr,                      // Actual feature level (unused)
        nullptr);                     // Immediate context (unused)

    // If hardware device creation fails, fall back to WARP (software)
    if (FAILED(hr)) {
        hr = D3D11CreateDevice(
            nullptr,
            D3D_DRIVER_TYPE_WARP,
            nullptr,
            flags,
            featureLevels,
            ARRAYSIZE(featureLevels),
            D3D11_SDK_VERSION,
            m_d3dDevice.GetAddressOf(),
            nullptr,
            nullptr);
    }

    return hr;
}

DWORD WINAPI SwapChainProcessor::RunThread(LPVOID pContext)
{
    auto* pProcessor = static_cast<SwapChainProcessor*>(pContext);
    pProcessor->Run();
    return 0;
}

void SwapChainProcessor::Run()
{
    // The processing loop waits on two events:
    //   1. m_hAvailableBufferEvent — signaled by IddCx when a frame is ready
    //   2. m_hTerminateEvent — signaled when the swap chain is being torn down
    HANDLE waitHandles[] = { m_hTerminateEvent, m_hAvailableBufferEvent };
    constexpr DWORD handleCount = ARRAYSIZE(waitHandles);

    while (true) {
        DWORD waitResult = WaitForMultipleObjects(handleCount, waitHandles,
                                                   FALSE, INFINITE);

        if (waitResult == WAIT_OBJECT_0) {
            // Terminate event signaled — exit the thread
            break;
        }

        if (waitResult == WAIT_OBJECT_0 + 1) {
            // A buffer is available — process all pending frames.
            // IddCx may batch multiple frames before signaling, so we loop
            // until there are no more frames to acquire.
            ProcessFrame();
        }
    }
}

void SwapChainProcessor::ProcessFrame()
{
    // Acquire frames in a loop until there are no more available.
    // IddCxSwapChainReleaseAndAcquireBuffer may return multiple frames
    // if the compositor was producing faster than we consumed.
    while (true) {
        IDARG_OUT_RELEASEANDACQUIREBUFFER outBuffer = {};

        // Release the previous frame (if any) and acquire the next one.
        // On the first call, there's no frame to release — IddCx handles this.
        NTSTATUS status = IddCxSwapChainReleaseAndAcquireBuffer(
            m_hSwapChain, &outBuffer);

        if (!NT_SUCCESS(status)) {
            // No more frames available or an error occurred.
            // STATUS_PENDING means no buffer is available right now.
            break;
        }

        // We have acquired a frame surface (outBuffer.MetaData.pSurface).
        // In a full indirect display driver, you would copy or transmit
        // this surface to a physical display. For OpenDisplay, we simply
        // release it back immediately — the DWM compositor has already
        // rendered the virtual display content, and DXGI Desktop Duplication
        // in the user-mode app captures from the compositor output.

        // Signal that we're done with this frame so the compositor can
        // recycle the buffer.
        NTSTATUS finishStatus = IddCxSwapChainFinishedProcessingFrame(
            m_hSwapChain);

        if (!NT_SUCCESS(finishStatus)) {
            // If finishing fails, the swap chain may be in a bad state.
            // Break out and let the next signal re-attempt.
            break;
        }
    }
}

// ─── IddCx Monitor Swap Chain Callbacks ──────────────────────────────────────
//
// These callbacks are registered in Driver.cpp via the IDD_CX_CLIENT_CONFIG.
// They are called by the OS when a swap chain is assigned to or removed from
// one of our virtual monitors.

// MonitorContext stores per-monitor state including the swap chain processor.
struct MonitorContext {
    SwapChainProcessor* pSwapChainProcessor;
};
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(MonitorContext, GetMonitorContext);

// ─── EvtIddCxMonitorAssignSwapChain ─────────────────────────────────────────
//
// Called when the OS assigns a swap chain to a monitor. This happens after
// the monitor is created and the display pipeline is established. We create
// a SwapChainProcessor to handle frames on this monitor.

NTSTATUS EvtIddCxMonitorAssignSwapChain(
    _In_ IDDCX_MONITOR MonitorObject,
    _In_ const IDARG_IN_SETSWAPCHAIN* pInArgs)
{
    MonitorContext* pMonitorContext = GetMonitorContext(MonitorObject);

    // Create the swap chain processor. It will create a D3D device, set it
    // on the swap chain, and start processing frames in a background thread.
    pMonitorContext->pSwapChainProcessor = new SwapChainProcessor(
        pInArgs->hSwapChain,
        pInArgs->hNextSurfaceAvailable);

    return STATUS_SUCCESS;
}

// ─── EvtIddCxMonitorUnassignSwapChain ────────────────────────────────────────
//
// Called when the OS removes the swap chain from a monitor. This happens
// during monitor teardown or display mode changes. We destroy the
// SwapChainProcessor, which signals its thread to stop and waits for it.

NTSTATUS EvtIddCxMonitorUnassignSwapChain(
    _In_ IDDCX_MONITOR MonitorObject)
{
    MonitorContext* pMonitorContext = GetMonitorContext(MonitorObject);

    if (pMonitorContext->pSwapChainProcessor) {
        delete pMonitorContext->pSwapChainProcessor;
        pMonitorContext->pSwapChainProcessor = nullptr;
    }

    return STATUS_SUCCESS;
}
