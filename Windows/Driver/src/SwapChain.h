#pragma once
// ─── OpenDisplay IDD Virtual Display Driver ─────────────────────────────────
//
// SwapChain.h — SwapChainProcessor declarations for frame acquisition and
// presentation. The processor runs in a dedicated thread, accepting frames
// from the IddCx swap chain and releasing them so the compositor can include
// the virtual display output for DXGI Desktop Duplication capture.
//
// Requirements: 1.3 (maintain virtual display as extended desktop),
//               2.1 (enable DXGI Desktop Duplication capture)

#include <windows.h>
#include <wdf.h>
#include <iddcx.h>
#include <dxgi1_2.h>
#include <d3d11.h>
#include <wrl/client.h>

// ─── SwapChainProcessor ──────────────────────────────────────────────────────
//
// Manages a dedicated thread that processes frames from an IDDCX_SWAPCHAIN.
// When the OS composites content onto the virtual display, the swap chain
// receives buffers. This processor acquires each frame and immediately releases
// it back, allowing the DWM compositor to include the virtual display in its
// composition. DXGI Desktop Duplication in the user-mode app then captures
// the composed output.

class SwapChainProcessor {
public:
    SwapChainProcessor(IDDCX_SWAPCHAIN hSwapChain,
                       HANDLE hAvailableBufferEvent);
    ~SwapChainProcessor();

    // Non-copyable, non-movable
    SwapChainProcessor(const SwapChainProcessor&) = delete;
    SwapChainProcessor& operator=(const SwapChainProcessor&) = delete;

private:
    // Thread entry point
    static DWORD WINAPI RunThread(LPVOID pContext);

    // Main processing loop — waits for buffers and processes them
    void Run();

    // Acquire a single frame from the swap chain and release it
    void ProcessFrame();

    // Create the D3D11 device used by the swap chain
    HRESULT CreateD3DDevice();

private:
    IDDCX_SWAPCHAIN m_hSwapChain;

    // Event signaled by IddCx when a new buffer is available
    HANDLE m_hAvailableBufferEvent;

    // Event signaled to tell the processing thread to terminate
    HANDLE m_hTerminateEvent;

    // Processing thread handle
    HANDLE m_hThread;

    // D3D11 device set on the swap chain for frame presentation
    Microsoft::WRL::ComPtr<ID3D11Device> m_d3dDevice;
};

