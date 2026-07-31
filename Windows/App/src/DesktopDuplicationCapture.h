#pragma once

#include <cstdint>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

/// Captures the virtual display's framebuffer using DXGI Desktop Duplication API,
/// providing GPU-resident textures to the encoder with zero-copy when supported.
///
/// Validates: Requirements 1.1, 2.1, 2.5
class DesktopDuplicationCapture {
public:
    /// Initialize capture on a specific display output.
    /// @param output The DXGI output corresponding to the virtual display.
    /// @param device The D3D11 device used for texture operations.
    /// @return S_OK on success, or an appropriate error HRESULT.
    HRESULT Initialize(IDXGIOutput* output, ID3D11Device* device);

    /// Acquire next frame (non-blocking with timeout).
    /// Implements failure recovery: retries transient errors within 100ms,
    /// triggers Reinitialize() after kMaxConsecutiveFailures consecutive failures,
    /// and immediately reinitializes on access-lost errors.
    /// @param timeoutMs Maximum time in milliseconds to wait for a new frame.
    /// @param outTexture Receives the GPU-resident texture for the captured frame.
    /// @param outInfo Receives frame metadata from DXGI.
    /// @return S_OK with texture, DXGI_ERROR_WAIT_TIMEOUT if no change,
    ///         or error HRESULT if recovery also fails.
    HRESULT AcquireFrame(uint32_t timeoutMs,
                         ID3D11Texture2D** outTexture,
                         DXGI_OUTDUPL_FRAME_INFO* outInfo);

    /// Release the acquired frame back to DXGI.
    void ReleaseFrame();

    /// Reinitialize after access lost (e.g., mode change, secure desktop).
    /// Called automatically after kMaxConsecutiveFailures consecutive failures,
    /// or immediately on access-lost error codes.
    /// @return S_OK on success, or an appropriate error HRESULT.
    HRESULT Reinitialize();

    /// Shutdown and release all resources.
    void Shutdown();

    /// Returns the current consecutive failure count (for testing/diagnostics).
    int GetConsecutiveFailures() const { return m_consecutiveFailures; }

private:
    /// Checks whether the given HRESULT indicates an access-lost condition
    /// (secure desktop transition, display mode change, etc.)
    static bool IsAccessLostError(HRESULT hr);

    /// Checks whether the given HRESULT indicates a transient error
    /// that may resolve with a brief retry.
    static bool IsTransientError(HRESULT hr);

    ComPtr<IDXGIOutputDuplication> m_duplication;
    ComPtr<ID3D11Device> m_device;
    ComPtr<ID3D11DeviceContext> m_context;
    ComPtr<IDXGIOutput> m_output; ///< Stored output for reinitialize
    int m_consecutiveFailures = 0;
    static constexpr int kMaxConsecutiveFailures = 3;
    static constexpr uint32_t kTransientRetryDelayMs = 100;
};
