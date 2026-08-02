#include "DesktopDuplicationCapture.h"

#include <dxgi1_5.h>
#include <thread>
#include <chrono>

// ----------------------------------------------------------------------------
// IsAccessLostError: Determines if the HRESULT indicates a loss of access to
// the desktop duplication — typically caused by a mode change, secure desktop
// transition (e.g., UAC prompt, Ctrl+Alt+Del), or display reconfiguration.
// These errors require full reinitialization of the duplication session.
// ----------------------------------------------------------------------------
bool DesktopDuplicationCapture::IsAccessLostError(HRESULT hr)
{
    return hr == DXGI_ERROR_ACCESS_LOST ||
           hr == DXGI_ERROR_ACCESS_DENIED ||
           hr == E_ACCESSDENIED;
}

// ----------------------------------------------------------------------------
// IsTransientError: Determines if the HRESULT indicates a transient condition
// that may resolve itself after a brief delay (e.g., device temporarily busy).
// These errors are retried within kTransientRetryDelayMs before counting as
// a consecutive failure.
// ----------------------------------------------------------------------------
bool DesktopDuplicationCapture::IsTransientError(HRESULT hr)
{
    // DXGI_ERROR_DEVICE_REMOVED and DXGI_ERROR_DEVICE_RESET are not transient
    // (they require full device recreation). We treat other generic failures
    // as potentially transient.
    return hr != DXGI_ERROR_ACCESS_LOST &&
           hr != DXGI_ERROR_ACCESS_DENIED &&
           hr != DXGI_ERROR_DEVICE_REMOVED &&
           hr != DXGI_ERROR_DEVICE_RESET &&
           hr != E_INVALIDARG &&
           hr != E_NOT_VALID_STATE;
}

// ----------------------------------------------------------------------------
// Initialize: Open an IDXGIOutputDuplication on the specified virtual display
// output using the provided D3D11 device.
// ----------------------------------------------------------------------------
HRESULT DesktopDuplicationCapture::Initialize(IDXGIOutput* output, ID3D11Device* device)
{
    if (!output || !device)
    {
        return E_INVALIDARG;
    }

    // Store the device and output, retrieve the immediate context
    m_device = device;
    m_output = output;
    m_device->GetImmediateContext(m_context.ReleaseAndGetAddressOf());

    m_consecutiveFailures = 0;

    // Try DuplicateOutput1 first (DXGI 1.5) for better format control,
    // falling back to DuplicateOutput (DXGI 1.2) if unavailable.
    ComPtr<IDXGIOutput5> output5;
    HRESULT hr = output->QueryInterface(IID_PPV_ARGS(&output5));
    if (SUCCEEDED(hr))
    {
        // DuplicateOutput1 allows specifying supported formats explicitly.
        // We prefer BGRA as it's the standard desktop composition format.
        DXGI_FORMAT supportedFormats[] = {
            DXGI_FORMAT_B8G8R8A8_UNORM,
            DXGI_FORMAT_R8G8B8A8_UNORM
        };

        hr = output5->DuplicateOutput1(
            device,
            0, // flags
            _countof(supportedFormats),
            supportedFormats,
            m_duplication.ReleaseAndGetAddressOf());
    }
    else
    {
        // Fallback to DuplicateOutput (DXGI 1.2)
        ComPtr<IDXGIOutput1> output1;
        hr = output->QueryInterface(IID_PPV_ARGS(&output1));
        if (FAILED(hr))
        {
            return hr;
        }

        hr = output1->DuplicateOutput(device, m_duplication.ReleaseAndGetAddressOf());
    }

    return hr;
}

// ----------------------------------------------------------------------------
// AcquireFrame: Non-blocking frame acquisition with failure recovery.
//
// Recovery strategy (per Requirement 2.5):
// 1. On success: reset consecutive failure counter immediately.
// 2. On access-lost errors (DXGI_ERROR_ACCESS_LOST, DXGI_ERROR_ACCESS_DENIED):
//    immediately call Reinitialize() — these indicate mode change or secure
//    desktop transitions that require full session recreation.
// 3. On transient errors: retry acquisition after a brief delay (100ms).
//    If still failing, increment consecutive failure counter.
// 4. After kMaxConsecutiveFailures (3) consecutive failures: call
//    Reinitialize() to fully recreate the duplication session.
// ----------------------------------------------------------------------------
HRESULT DesktopDuplicationCapture::AcquireFrame(uint32_t timeoutMs,
                                                 ID3D11Texture2D** outTexture,
                                                 DXGI_OUTDUPL_FRAME_INFO* outInfo)
{
    if (!m_duplication)
    {
        return E_NOT_VALID_STATE;
    }

    if (!outTexture || !outInfo)
    {
        return E_INVALIDARG;
    }

    *outTexture = nullptr;

    ComPtr<IDXGIResource> desktopResource;
    HRESULT hr = m_duplication->AcquireNextFrame(timeoutMs, outInfo, desktopResource.GetAddressOf());

    if (hr == DXGI_ERROR_WAIT_TIMEOUT)
    {
        // No new content — this is expected behavior, not a failure.
        // The caller should skip encoding for this interval.
        return DXGI_ERROR_WAIT_TIMEOUT;
    }

    if (FAILED(hr))
    {
        // --- Access-lost errors: immediate reinitialize ---
        if (IsAccessLostError(hr))
        {
            m_consecutiveFailures++;
            // Access lost means the duplication is invalidated.
            // Reinitialize immediately rather than waiting for the threshold.
            HRESULT reinitHr = Reinitialize();
            if (FAILED(reinitHr))
            {
                return reinitHr;
            }
            // After reinit, the caller should retry on the next frame interval
            return hr;
        }

        // --- Transient errors: retry within 100ms ---
        if (IsTransientError(hr))
        {
            // Brief delay before retry (Requirement 2.5: retry within 100ms)
            std::this_thread::sleep_for(std::chrono::milliseconds(kTransientRetryDelayMs));

            // Retry acquisition once
            desktopResource.Reset();
            HRESULT retryHr = m_duplication->AcquireNextFrame(
                timeoutMs, outInfo, desktopResource.GetAddressOf());

            if (retryHr == DXGI_ERROR_WAIT_TIMEOUT)
            {
                return DXGI_ERROR_WAIT_TIMEOUT;
            }

            if (SUCCEEDED(retryHr))
            {
                // Retry succeeded — reset counter and continue to texture extraction
                m_consecutiveFailures = 0;
                hr = desktopResource->QueryInterface(IID_PPV_ARGS(outTexture));
                if (FAILED(hr))
                {
                    m_duplication->ReleaseFrame();
                    m_consecutiveFailures++;
                    if (m_consecutiveFailures >= kMaxConsecutiveFailures)
                    {
                        Reinitialize();
                    }
                    return hr;
                }
                return S_OK;
            }

            // Retry also failed — fall through to consecutive failure logic
            hr = retryHr;
        }

        // --- Consecutive failure tracking ---
        m_consecutiveFailures++;

        // After kMaxConsecutiveFailures (3) consecutive failures, reinitialize
        if (m_consecutiveFailures >= kMaxConsecutiveFailures)
        {
            Reinitialize();
        }

        return hr;
    }

    // --- Success: reset consecutive failure counter ---
    m_consecutiveFailures = 0;

    // Query the ID3D11Texture2D from the DXGI resource.
    // This texture is GPU-resident and can be passed directly to the encoder
    // for a zero-copy path (both share the same D3D11 device).
    hr = desktopResource->QueryInterface(IID_PPV_ARGS(outTexture));
    if (FAILED(hr))
    {
        // If we can't get the texture, release the frame so DXGI isn't stuck
        m_duplication->ReleaseFrame();
        m_consecutiveFailures++;
        if (m_consecutiveFailures >= kMaxConsecutiveFailures)
        {
            Reinitialize();
        }
        return hr;
    }

    return S_OK;
}

// ----------------------------------------------------------------------------
// ReleaseFrame: Return the acquired frame back to DXGI so the next frame
// can be acquired. Must be called after each successful AcquireFrame.
// ----------------------------------------------------------------------------
void DesktopDuplicationCapture::ReleaseFrame()
{
    if (m_duplication)
    {
        m_duplication->ReleaseFrame();
    }
}

// ----------------------------------------------------------------------------
// Reinitialize: Recreate the duplication session after access is lost.
// Called automatically when:
//   - An access-lost error is detected (mode change, secure desktop)
//   - kMaxConsecutiveFailures (3) consecutive acquisition failures occur
//
// The method releases the current duplication interface, re-enumerates the
// display output, and creates a new duplication session. The consecutive
// failure counter is reset on successful reinitialize.
// ----------------------------------------------------------------------------
HRESULT DesktopDuplicationCapture::Reinitialize()
{
    if (!m_device)
    {
        return E_NOT_VALID_STATE;
    }

    // Release existing duplication interface
    m_duplication.Reset();
    m_consecutiveFailures = 0;

    // Determine the output to reinitialize on.
    // Prefer the stored m_output if available; otherwise re-enumerate from adapter.
    ComPtr<IDXGIOutput> output;
    if (m_output)
    {
        output = m_output;
    }
    else
    {
        // Re-acquire the output from the device's adapter.
        // Walk the DXGI hierarchy: Device → Adapter → Output
        ComPtr<IDXGIDevice> dxgiDevice;
        HRESULT hr = m_device->QueryInterface(IID_PPV_ARGS(&dxgiDevice));
        if (FAILED(hr))
        {
            return hr;
        }

        ComPtr<IDXGIAdapter> adapter;
        hr = dxgiDevice->GetAdapter(adapter.GetAddressOf());
        if (FAILED(hr))
        {
            return hr;
        }

        // Enumerate outputs to find the virtual display
        hr = adapter->EnumOutputs(0, output.GetAddressOf());
        if (FAILED(hr))
        {
            return hr;
        }
    }

    // Try DuplicateOutput1 (DXGI 1.5) first, then fall back to DuplicateOutput
    ComPtr<IDXGIOutput5> output5;
    HRESULT hr = output->QueryInterface(IID_PPV_ARGS(&output5));
    if (SUCCEEDED(hr))
    {
        DXGI_FORMAT supportedFormats[] = {
            DXGI_FORMAT_B8G8R8A8_UNORM,
            DXGI_FORMAT_R8G8B8A8_UNORM
        };

        hr = output5->DuplicateOutput1(
            m_device.Get(),
            0,
            _countof(supportedFormats),
            supportedFormats,
            m_duplication.ReleaseAndGetAddressOf());
    }
    else
    {
        ComPtr<IDXGIOutput1> output1;
        hr = output->QueryInterface(IID_PPV_ARGS(&output1));
        if (FAILED(hr))
        {
            return hr;
        }

        hr = output1->DuplicateOutput(m_device.Get(), m_duplication.ReleaseAndGetAddressOf());
    }

    return hr;
}

// ----------------------------------------------------------------------------
// Shutdown: Release all COM resources and reset state.
// ----------------------------------------------------------------------------
void DesktopDuplicationCapture::Shutdown()
{
    m_duplication.Reset();
    m_context.Reset();
    m_output.Reset();
    m_device.Reset();
    m_consecutiveFailures = 0;
}
