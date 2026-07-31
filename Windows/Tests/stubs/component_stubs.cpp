// ─── Stub implementations for test builds ───────────────────────────────────
// Provides minimal no-op implementations of component methods that
// SessionController.cpp references, so the test binary can link without
// requiring the actual DirectX/MFT/Winsock/Bonjour dependencies.

#include <mferror.h>

#include "DesktopDuplicationCapture.h"
#include "MFTEncoder.h"
#include "WireTransport.h"
#include "WindowsInputInjector.h"
#include "BonjourBrowser.h"

// ─── DesktopDuplicationCapture stubs ─────────────────────────────────────────

HRESULT DesktopDuplicationCapture::Initialize(IDXGIOutput* /*output*/, ID3D11Device* /*device*/)
{
    return S_OK;
}

HRESULT DesktopDuplicationCapture::AcquireFrame(uint32_t /*timeoutMs*/,
                                                 ID3D11Texture2D** outTexture,
                                                 DXGI_OUTDUPL_FRAME_INFO* /*outInfo*/)
{
    if (outTexture) *outTexture = nullptr;
    return DXGI_ERROR_WAIT_TIMEOUT; // No frame available
}

void DesktopDuplicationCapture::ReleaseFrame() {}

HRESULT DesktopDuplicationCapture::Reinitialize()
{
    return S_OK;
}

void DesktopDuplicationCapture::Shutdown() {}

// ─── MFTEncoder stubs ────────────────────────────────────────────────────────

HRESULT MFTEncoder::Initialize(const Config& /*config*/, ID3D11Device* /*device*/)
{
    return S_OK;
}

HRESULT MFTEncoder::SubmitFrame(ID3D11Texture2D* /*texture*/, int64_t /*captureTimestampMs*/)
{
    return S_OK;
}

void MFTEncoder::RequestKeyframe()
{
    m_forceKeyframe.store(true);
}

HRESULT MFTEncoder::GetOutput(std::vector<uint8_t>& annexBData, bool& isKeyframe)
{
    annexBData.clear();
    isKeyframe = false;
    return MF_E_TRANSFORM_NEED_MORE_INPUT;
}

void MFTEncoder::Shutdown() {}

bool MFTEncoder::IsHardwareEncoder() const { return false; }
bool MFTEncoder::HasError() const { return false; }

// ─── WireTransport stubs ─────────────────────────────────────────────────────

HRESULT WireTransport::ConnectUSB(const std::string& /*deviceUdid*/, uint16_t /*port*/)
{
    return E_FAIL; // Stub: no real connection
}

HRESULT WireTransport::ConnectWiFi(const std::string& /*host*/, uint16_t /*port*/)
{
    return E_FAIL; // Stub: no real connection
}

HRESULT WireTransport::ConnectWiFiWithRetry(const std::string& /*host*/, uint16_t /*port*/)
{
    return E_FAIL;
}

HRESULT WireTransport::FailoverToWiFi(const std::string& /*host*/, uint16_t /*port*/)
{
    return E_FAIL;
}

HRESULT WireTransport::SendVideoFrame(const std::vector<uint8_t>& /*annexB*/,
                                       int64_t /*captureMs*/, int64_t /*sendMs*/)
{
    return S_OK;
}

HRESULT WireTransport::SendControl(const std::string& /*json*/)
{
    return S_OK;
}

void WireTransport::SetControlHandler(ControlHandler /*handler*/) {}

bool WireTransport::IsConnected() const { return false; }

void WireTransport::Disconnect() {}

// ─── WindowsInputInjector stubs ──────────────────────────────────────────────

void WindowsInputInjector::SetDisplayBounds(RECT /*bounds*/) {}

void WindowsInputInjector::HandleTouch(const std::string& /*phase*/, double /*normX*/, double /*normY*/) {}

void WindowsInputInjector::HandleScroll(double /*dx*/, double /*dy*/) {}

// ─── BonjourBrowser stubs ────────────────────────────────────────────────────

HRESULT BonjourBrowser::StartBrowsing() { return S_OK; }

void BonjourBrowser::StopBrowsing() {}

std::vector<BonjourBrowser::DiscoveredService> BonjourBrowser::GetServices() const
{
    return {};
}

void BonjourBrowser::SetCallback(ServiceCallback /*callback*/) {}
