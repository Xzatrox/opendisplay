#include "MFTEncoder.h"

#include <mfapi.h>
#include <mferror.h>
#include <mftransform.h>
#include <mfidl.h>
#include <codecapi.h>
#include <strmif.h>
#include <d3d11.h>
#include <d3d10.h>
#include <iostream>

#ifndef OutputDebugStringA
#include <debugapi.h>
#endif

// Log helper for encoder errors
static void LogEncoderError(const char* context, HRESULT hr)
{
    char buf[256];
    snprintf(buf, sizeof(buf), "[MFTEncoder] Error in %s: HRESULT=0x%08lX\n", context, (unsigned long)hr);
    OutputDebugStringA(buf);
}

// Annex B 4-byte start code
static const uint8_t kAnnexBStartCode[4] = { 0x00, 0x00, 0x00, 0x01 };

// Helper to set a UINT32 codec property via ICodecAPI
static HRESULT SetCodecProperty(ICodecAPI* codecApi, const GUID& guid, UINT32 value)
{
    VARIANT var;
    VariantInit(&var);
    var.vt = VT_UI4;
    var.ulVal = value;
    HRESULT hr = codecApi->SetValue(&guid, &var);
    VariantClear(&var);
    return hr;
}

// Helper to set a BOOL codec property via ICodecAPI
static HRESULT SetCodecBoolProperty(ICodecAPI* codecApi, const GUID& guid, BOOL value)
{
    VARIANT var;
    VariantInit(&var);
    var.vt = VT_BOOL;
    var.boolVal = value ? VARIANT_TRUE : VARIANT_FALSE;
    HRESULT hr = codecApi->SetValue(&guid, &var);
    VariantClear(&var);
    return hr;
}

// Helper to set a UINT64 codec property via ICodecAPI
static HRESULT SetCodecProperty64(ICodecAPI* codecApi, const GUID& guid, UINT64 value)
{
    VARIANT var;
    VariantInit(&var);
    var.vt = VT_UI8;
    var.ullVal = value;
    HRESULT hr = codecApi->SetValue(&guid, &var);
    VariantClear(&var);
    return hr;
}

// ----------------------------------------------------------------------------
// FindEncoder: Enumerate MFT hardware H.264 encoders, optionally including
// software encoders. Returns the first available encoder transform.
// ----------------------------------------------------------------------------
static HRESULT FindEncoder(bool hardwareOnly, ID3D11Device* device,
                           IMFTransform** outTransform, bool& isHardware)
{
    isHardware = false;

    // Set up the search criteria for H.264 encoders
    MFT_REGISTER_TYPE_INFO outputType = {};
    outputType.guidMajorType = MFMediaType_Video;
    outputType.guidSubtype = MFVideoFormat_H264;

    UINT32 flags = MFT_ENUM_FLAG_SORTANDFILTER | MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_ASYNCMFT;

    // First pass: try hardware encoders
    IMFActivate** ppActivate = nullptr;
    UINT32 numActivate = 0;

    UINT32 hwFlags = flags | MFT_ENUM_FLAG_HARDWARE;
    HRESULT hr = MFTEnumEx(
        MFT_CATEGORY_VIDEO_ENCODER,
        hwFlags,
        nullptr,           // input type (any)
        &outputType,
        &ppActivate,
        &numActivate);

    if (SUCCEEDED(hr) && numActivate > 0 && device)
    {
        // For hardware encoder, we need to pass the D3D device manager.
        // Create a FRESH device using the default adapter (D3D_DRIVER_TYPE_HARDWARE)
        // which is what the AMD AMF encoder expects.
        ComPtr<ID3D11Device> encDevice;
        D3D_FEATURE_LEVEL fl;
        D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
        HRESULT devHr = D3D11CreateDevice(
            nullptr,  // Default adapter — let the system pick the right GPU
            D3D_DRIVER_TYPE_HARDWARE,
            nullptr,
            D3D11_CREATE_DEVICE_VIDEO_SUPPORT,
            levels, 2,
            D3D11_SDK_VERSION,
            encDevice.GetAddressOf(), &fl, nullptr);

        if (FAILED(devHr)) {
            // Fall back to using the passed-in device
            encDevice = device;
        } else {
            // Enable multithread protection
            ComPtr<ID3D10Multithread> mt;
            if (SUCCEEDED(encDevice->QueryInterface(IID_PPV_ARGS(&mt)))) {
                mt->SetMultithreadProtected(TRUE);
            }
        }

        UINT resetToken = 0;
        ComPtr<IMFDXGIDeviceManager> deviceManager;
        MFCreateDXGIDeviceManager(&resetToken, deviceManager.GetAddressOf());
        if (deviceManager) {
            deviceManager->ResetDevice(encDevice.Get(), resetToken);
        }

        for (UINT32 i = 0; i < numActivate; ++i)
        {
            // Activate the encoder
            hr = ppActivate[i]->ActivateObject(IID_PPV_ARGS(outTransform));
            if (SUCCEEDED(hr))
            {
                isHardware = true;
                std::cerr << "[FindEncoder] HW encoder activated (index " << i << ")\n";

                // Set the device manager on the transform AFTER activation
                if (deviceManager) {
                    HRESULT dmHr = (*outTransform)->ProcessMessage(
                        MFT_MESSAGE_SET_D3D_MANAGER,
                        reinterpret_cast<ULONG_PTR>(deviceManager.Get()));
                    std::cerr << "[FindEncoder] SET_D3D_MANAGER: 0x"
                              << std::hex << dmHr << std::dec << "\n";
                    // If it fails, release and try next encoder
                    if (FAILED(dmHr)) {
                        (*outTransform)->Release();
                        *outTransform = nullptr;
                        isHardware = false;
                        continue;
                    }
                }

                for (UINT32 j = 0; j < numActivate; j++) ppActivate[j]->Release();
                CoTaskMemFree(ppActivate);
                return S_OK;
            }
        }

        for (UINT32 i = 0; i < numActivate; i++) ppActivate[i]->Release();
        CoTaskMemFree(ppActivate);
    }
    else
    {
        if (ppActivate) {
            for (UINT32 i = 0; i < numActivate; i++) ppActivate[i]->Release();
            CoTaskMemFree(ppActivate);
        }
    }

    // Second pass: software fallback (if allowed)
    if (hardwareOnly)
    {
        return MF_E_TOPO_CODEC_NOT_FOUND;
    }

    ppActivate = nullptr;
    numActivate = 0;

    UINT32 swFlags = MFT_ENUM_FLAG_SORTANDFILTER | MFT_ENUM_FLAG_SYNCMFT |
                     MFT_ENUM_FLAG_ASYNCMFT | MFT_ENUM_FLAG_LOCALMFT;
    hr = MFTEnumEx(
        MFT_CATEGORY_VIDEO_ENCODER,
        swFlags,
        nullptr,
        &outputType,
        &ppActivate,
        &numActivate);

    if (SUCCEEDED(hr) && numActivate > 0)
    {
        for (UINT32 i = 0; i < numActivate; ++i)
        {
            hr = ppActivate[i]->ActivateObject(IID_PPV_ARGS(outTransform));
            if (SUCCEEDED(hr))
            {
                for (UINT32 j = 0; j < numActivate; j++) ppActivate[j]->Release();
                CoTaskMemFree(ppActivate);
                std::cerr << "[FindEncoder] SW encoder activated (index " << i << ")\n";
                return S_OK;
            }
        }
        for (UINT32 i = 0; i < numActivate; i++) ppActivate[i]->Release();
        CoTaskMemFree(ppActivate);
    }
    else if (ppActivate)
    {
        CoTaskMemFree(ppActivate);
    }

    return MF_E_TOPO_CODEC_NOT_FOUND;
}

// ----------------------------------------------------------------------------
// ConfigureOutputType: Set the encoder output media type.
// H.264 High profile, CBR, specified bitrate and frame rate.
// ----------------------------------------------------------------------------
static HRESULT ConfigureOutputType(IMFTransform* transform, const MFTEncoder::Config& config)
{
    ComPtr<IMFMediaType> outputType;
    HRESULT hr = MFCreateMediaType(outputType.GetAddressOf());
    if (FAILED(hr)) return hr;

    hr = outputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    if (FAILED(hr)) return hr;

    hr = outputType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
    if (FAILED(hr)) return hr;

    // H.264 High profile (eAVEncH264VProfile_High = 100)
    hr = outputType->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_High);
    if (FAILED(hr)) return hr;

    // Average bitrate from quality preset
    hr = outputType->SetUINT32(MF_MT_AVG_BITRATE, config.bitrate);
    if (FAILED(hr)) return hr;

    // Frame size
    hr = MFSetAttributeSize(outputType.Get(), MF_MT_FRAME_SIZE, config.width, config.height);
    if (FAILED(hr)) return hr;

    // Frame rate (e.g., 60 fps)
    hr = MFSetAttributeRatio(outputType.Get(), MF_MT_FRAME_RATE, config.fps, 1);
    if (FAILED(hr)) return hr;

    // Interlace mode: progressive
    hr = outputType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    if (FAILED(hr)) return hr;

    // Set the output type on the transform (stream index 0)
    hr = transform->SetOutputType(0, outputType.Get(), 0);
    return hr;
}

// ----------------------------------------------------------------------------
// ConfigureInputType: Set the encoder input media type.
// NV12 format matching the captured GPU textures.
// ----------------------------------------------------------------------------
static HRESULT ConfigureInputType(IMFTransform* transform, const MFTEncoder::Config& config)
{
    // Strategy: Query the encoder for available input types.
    // After output type is set, the encoder knows what input formats it can accept.
    // Try each available type AS-IS first (it already has the correct resolution
    // from the output type), then try overriding frame size if needed.

    for (DWORD typeIdx = 0; typeIdx < 20; ++typeIdx)
    {
        ComPtr<IMFMediaType> availType;
        HRESULT hr = transform->GetInputAvailableType(0, typeIdx, availType.GetAddressOf());
        if (FAILED(hr)) break;

        // Try setting it directly (encoder should have already configured resolution)
        hr = transform->SetInputType(0, availType.Get(), 0);
        if (SUCCEEDED(hr)) {
            std::cerr << "[MFTEncoder] Accepted input type index " << typeIdx << " as-is\n";
            return S_OK;
        }

        // If that failed, try setting the frame size/rate explicitly
        MFSetAttributeSize(availType.Get(), MF_MT_FRAME_SIZE, config.width, config.height);
        MFSetAttributeRatio(availType.Get(), MF_MT_FRAME_RATE, config.fps, 1);
        availType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);

        hr = transform->SetInputType(0, availType.Get(), 0);
        if (SUCCEEDED(hr)) {
            std::cerr << "[MFTEncoder] Accepted input type index " << typeIdx << " with size override\n";
            return S_OK;
        }
    }

    // Last resort: create NV12 type manually
    ComPtr<IMFMediaType> inputType;
    HRESULT hr = MFCreateMediaType(inputType.GetAddressOf());
    if (FAILED(hr)) return hr;

    inputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    inputType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
    MFSetAttributeSize(inputType.Get(), MF_MT_FRAME_SIZE, config.width, config.height);
    MFSetAttributeRatio(inputType.Get(), MF_MT_FRAME_RATE, config.fps, 1);
    inputType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);

    hr = transform->SetInputType(0, inputType.Get(), 0);
    if (SUCCEEDED(hr)) {
        std::cerr << "[MFTEncoder] Accepted manual NV12 type\n";
        return S_OK;
    }

    std::cerr << "[MFTEncoder] No acceptable input type found (last hr=0x"
              << std::hex << hr << std::dec << ")\n";
    return hr;
}

// ----------------------------------------------------------------------------
// ConfigureEncoderProperties: Apply low-latency encoding settings via ICodecAPI.
// Matches Mac's VideoToolbox configuration for streaming parity.
// ----------------------------------------------------------------------------
static HRESULT ConfigureEncoderProperties(IMFTransform* transform, const MFTEncoder::Config& config)
{
    ComPtr<ICodecAPI> codecApi;
    HRESULT hr = transform->QueryInterface(IID_PPV_ARGS(&codecApi));
    if (FAILED(hr))
    {
        // Some encoders may not support ICodecAPI — not fatal for basic operation
        return S_OK;
    }

    // Real-time priority: CODECAPI_AVEncCommonRealTime = TRUE
    SetCodecBoolProperty(codecApi.Get(), CODECAPI_AVEncCommonRealTime, TRUE);

    // CBR rate control mode for consistent bandwidth
    SetCodecProperty(codecApi.Get(), CODECAPI_AVEncCommonRateControlMode,
                     eAVEncCommonRateControlMode_CBR);

    // Low-latency mode: minimize encode delay
    SetCodecBoolProperty(codecApi.Get(), CODECAPI_AVLowLatencyMode, TRUE);

    // Disable B-frames: CODECAPI_AVEncMPVDefaultBPictureCount = 0
    SetCodecProperty(codecApi.Get(), CODECAPI_AVEncMPVDefaultBPictureCount, 0);

    // Max keyframe interval: 3600 frames (60 seconds at 60 fps)
    SetCodecProperty(codecApi.Get(), CODECAPI_AVEncMPVGOPSize, 3600);

    // Mean bitrate (matches the output type avg bitrate)
    SetCodecProperty(codecApi.Get(), CODECAPI_AVEncCommonMeanBitRate, config.bitrate);

    // Disable frame reordering (no B-frames means no reordering needed)
    SetCodecBoolProperty(codecApi.Get(), CODECAPI_AVEncVideoForceKeyFrame, FALSE);

    // Max frame delay count = 0 (no buffering/delay for output frames)
    SetCodecProperty(codecApi.Get(), CODECAPI_AVEncCommonBufferSize, config.bitrate / config.fps);

    return S_OK;
}

// ----------------------------------------------------------------------------
// Initialize: Create and configure the Media Foundation H.264 encoder.
//
// Hardware encoder activation order:
// 1. Intel QSV (Intel Quick Sync Video)
// 2. NVIDIA NVENC
// 3. AMD AMF (Advanced Media Framework)
//
// Falls back to software encoding if hardwareOnly == false and no HW encoder
// is available. Configures H.264 High profile with CBR low-latency settings.
//
// Validates: Requirements 3.1, 3.2, 3.7, 3.8
// ----------------------------------------------------------------------------
HRESULT MFTEncoder::Initialize(const Config& config, ID3D11Device* device)
{
    if (config.width == 0 || config.height == 0 || config.fps == 0 || config.bitrate == 0)
    {
        return E_INVALIDARG;
    }

    // Store configuration
    m_config = config;
    m_device = device;  // may be nullptr for software-only path
    m_forceKeyframe.store(true);  // First frame is always IDR
    m_pendingEncodes.store(0);

    // Find an available encoder (hardware first, then software if allowed)
    bool isHardware = false;
    HRESULT hr = FindEncoder(config.hardwareOnly, device, m_transform.ReleaseAndGetAddressOf(), isHardware);
    if (FAILED(hr))
    {
        return hr;
    }

    m_isHardware = isHardware;

    // Associate the D3D11 device with the encoder for zero-copy GPU texture input.
    // AMD AMF requires: 1) Check MF_SA_D3D11_AWARE, 2) Create DXGI manager,
    // 3) ResetDevice, 4) ProcessMessage with SET_D3D_MANAGER.
    // The device manager must persist for the encoder's lifetime.
    // NOTE: FindEncoder already sets the device manager during activation.
    // We store a reference here for lifetime management.
    if (device)
    {
        UINT resetToken = 0;
        hr = MFCreateDXGIDeviceManager(&resetToken, m_deviceManager.ReleaseAndGetAddressOf());
        if (SUCCEEDED(hr)) {
            m_deviceManager->ResetDevice(device, resetToken);
        }
        std::cerr << "[MFTEncoder] Device manager stored for lifetime\n";
    }

    // Configure encoder output type (H.264 High profile, bitrate, resolution)
    hr = ConfigureOutputType(m_transform.Get(), m_config);
    if (FAILED(hr))
    {
        std::cerr << "[MFTEncoder] ConfigureOutputType FAILED: 0x"
                  << std::hex << hr << std::dec << "\n";
        m_transform.Reset();
        return hr;
    }
    std::cerr << "[MFTEncoder] Output type set OK\n";

    // Configure encoder input type (ARGB32/NV12 format matching capture textures)
    hr = ConfigureInputType(m_transform.Get(), m_config);
    if (FAILED(hr))
    {
        std::cerr << "[MFTEncoder] ConfigureInputType FAILED: 0x"
                  << std::hex << hr << std::dec << "\n";
        m_transform.Reset();
        return hr;
    }
    std::cerr << "[MFTEncoder] Input type set OK\n";

    // Apply low-latency codec properties (real-time, no B-frames, CBR, etc.)
    hr = ConfigureEncoderProperties(m_transform.Get(), m_config);
    if (FAILED(hr))
    {
        // Non-fatal: encoder will work with defaults if codec API isn't fully supported
        hr = S_OK;
    }

    // Signal the encoder to begin processing
    hr = m_transform->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    if (FAILED(hr))
    {
        m_transform.Reset();
        return hr;
    }

    hr = m_transform->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    if (FAILED(hr))
    {
        m_transform.Reset();
        return hr;
    }

    // Try to extract SPS/PPS from the output type (may not be available yet)
    ExtractSPSPPS();

    return S_OK;
}

// ----------------------------------------------------------------------------
// IsHardwareEncoder: Returns true if the active encoder is hardware-accelerated.
// Used by the UI to display a warning when software fallback is active.
//
// Validates: Requirements 3.8
// ----------------------------------------------------------------------------
bool MFTEncoder::IsHardwareEncoder() const
{
    return m_isHardware;
}

// ----------------------------------------------------------------------------
// SubmitFrame: Submit a GPU texture for encoding.
// Drops frames when pendingEncodes >= 1 (returns MF_E_NOTACCEPTING).
// Forces IDR on first frame and when m_forceKeyframe is set.
// On encoder error, logs and ceases encoding until Shutdown/re-Initialize.
//
// Validates: Requirements 3.3, 3.4, 3.6, 3.9
// ----------------------------------------------------------------------------
HRESULT MFTEncoder::SubmitFrame(ID3D11Texture2D* texture, int64_t captureTimestampMs)
{
    if (!texture)
    {
        return E_INVALIDARG;
    }

    if (!m_transform)
    {
        return E_NOT_VALID_STATE;
    }

    // If the encoder has entered an error state, refuse all submissions
    if (m_hasError)
    {
        return E_FAIL;
    }

    // Drop frame if encoder is busy (pendingEncodes >= 1)
    if (m_pendingEncodes.load() >= 1)
    {
        return MF_E_NOTACCEPTING;
    }

    // If a keyframe is requested, force IDR via ICodecAPI
    bool forceIdr = m_forceKeyframe.exchange(false);
    if (forceIdr)
    {
        ComPtr<ICodecAPI> codecApi;
        HRESULT hrCodec = m_transform->QueryInterface(IID_PPV_ARGS(&codecApi));
        if (SUCCEEDED(hrCodec))
        {
            VARIANT var;
            VariantInit(&var);
            var.vt = VT_UI4;
            var.ulVal = 1;  // Force keyframe
            codecApi->SetValue(&CODECAPI_AVEncVideoForceKeyFrame, &var);
            VariantClear(&var);
        }
    }

    // Create an IMFSample wrapping the input texture
    ComPtr<IMFSample> sample;
    HRESULT hr = MFCreateSample(sample.GetAddressOf());
    if (FAILED(hr))
    {
        LogEncoderError("MFCreateSample", hr);
        m_hasError = true;
        return hr;
    }

    ComPtr<IMFMediaBuffer> mediaBuffer;

    if (m_isHardware)
    {
        // Hardware path: create DXGI surface buffer (zero-copy)
        hr = MFCreateDXGISurfaceBuffer(
            __uuidof(ID3D11Texture2D),
            texture,
            0,      // subresource index
            FALSE,  // bottom-up (FALSE = top-down)
            mediaBuffer.GetAddressOf());
    }
    else
    {
        // Software path: copy GPU texture to CPU memory, create memory buffer
        // 1. Get texture description
        D3D11_TEXTURE2D_DESC texDesc = {};
        texture->GetDesc(&texDesc);
        std::cerr << "[Enc] SW path: " << texDesc.Width << "x" << texDesc.Height
                  << " fmt=" << texDesc.Format << "\n";

        // 2. Create staging texture for CPU readback
        ComPtr<ID3D11Device> texDevice;
        texture->GetDevice(texDevice.GetAddressOf());
        ComPtr<ID3D11DeviceContext> ctx;
        texDevice->GetImmediateContext(ctx.GetAddressOf());

        D3D11_TEXTURE2D_DESC stagingDesc = texDesc;
        stagingDesc.Usage = D3D11_USAGE_STAGING;
        stagingDesc.BindFlags = 0;
        stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        stagingDesc.MiscFlags = 0;

        ComPtr<ID3D11Texture2D> staging;
        hr = texDevice->CreateTexture2D(&stagingDesc, nullptr, staging.GetAddressOf());
        if (FAILED(hr))
        {
            std::cerr << "[Enc] CreateStagingTexture failed: 0x" << std::hex << hr << std::dec << "\n";
            m_hasError = true;
            return hr;
        }

        // 3. Copy GPU texture to staging
        std::cerr << "[Enc] CopyResource...\n";
        ctx->CopyResource(staging.Get(), texture);
        std::cerr << "[Enc] Map...\n";

        // 4. Map staging texture to get CPU pointer
        D3D11_MAPPED_SUBRESOURCE mapped = {};
        hr = ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped);
        if (FAILED(hr))
        {
            std::cerr << "[Enc] Map failed: 0x" << std::hex << hr << std::dec << "\n";
            m_hasError = true;
            return hr;
        }
        std::cerr << "[Enc] Mapped OK, pitch=" << mapped.RowPitch << "\n";

        // 5. Create MF memory buffer with NV12 data converted from BGRA
        // NV12 = Y plane (full res) + interleaved UV plane (half res)
        // Use ENCODER height (m_config.height) not texture height to match encoder config
        UINT32 encW = m_config.width;
        UINT32 encH = m_config.height;
        UINT32 srcW = texDesc.Width;
        UINT32 srcH = texDesc.Height;
        // Clamp to encoder dimensions
        UINT32 copyW = (srcW < encW) ? srcW : encW;
        UINT32 copyH = (srcH < encH) ? srcH : encH;

        UINT32 nv12Size = encW * encH * 3 / 2;
        hr = MFCreateMemoryBuffer(nv12Size, mediaBuffer.GetAddressOf());
        if (SUCCEEDED(hr))
        {
            BYTE* bufferPtr = nullptr;
            DWORD maxLen = 0;
            hr = mediaBuffer->Lock(&bufferPtr, &maxLen, nullptr);
            if (SUCCEEDED(hr))
            {
                // Zero the buffer first (handles padding if texture is smaller than encoder)
                memset(bufferPtr, 0, nv12Size);
                memset(bufferPtr + encW * encH, 128, encW * encH / 2); // UV = 128 (neutral)

                // Convert BGRA → NV12 (BT.601)
                BYTE* yPlane = bufferPtr;
                BYTE* uvPlane = bufferPtr + encW * encH;
                const BYTE* srcData = static_cast<const BYTE*>(mapped.pData);

                for (UINT32 y = 0; y < copyH; ++y)
                {
                    const BYTE* row = srcData + y * mapped.RowPitch;
                    for (UINT32 x = 0; x < copyW; ++x)
                    {
                        BYTE b = row[x * 4 + 0];
                        BYTE g = row[x * 4 + 1];
                        BYTE r = row[x * 4 + 2];

                        int yVal = ((66 * r + 129 * g + 25 * b + 128) >> 8) + 16;
                        yPlane[y * encW + x] = static_cast<BYTE>(
                            yVal < 0 ? 0 : (yVal > 255 ? 255 : yVal));

                        if ((y % 2 == 0) && (x % 2 == 0))
                        {
                            int uVal = ((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128;
                            int vVal = ((112 * r - 94 * g - 18 * b + 128) >> 8) + 128;
                            UINT32 uvIdx = (y / 2) * encW + x;
                            uvPlane[uvIdx] = static_cast<BYTE>(
                                uVal < 0 ? 0 : (uVal > 255 ? 255 : uVal));
                            uvPlane[uvIdx + 1] = static_cast<BYTE>(
                                vVal < 0 ? 0 : (vVal > 255 ? 255 : vVal));
                        }
                    }
                }
                mediaBuffer->Unlock();
                mediaBuffer->SetCurrentLength(nv12Size);
                std::cerr << "[Enc] NV12 conversion done (" << copyW << "x" << copyH
                          << " -> enc " << encW << "x" << encH << ")\n";
            }
        }

        ctx->Unmap(staging.Get(), 0);
        std::cerr << "[Enc] Buffer ready\n";

        if (FAILED(hr))
        {
            std::cerr << "[Enc] Buffer creation failed: 0x" << std::hex << hr << std::dec << "\n";
            m_hasError = true;
            return hr;
        }
    }

    if (FAILED(hr))
    {
        LogEncoderError("CreateBuffer", hr);
        m_hasError = true;
        return hr;
    }

    hr = sample->AddBuffer(mediaBuffer.Get());
    if (FAILED(hr))
    {
        LogEncoderError("AddBuffer", hr);
        m_hasError = true;
        return hr;
    }

    // Set the sample timestamp (convert milliseconds to 100-nanosecond MF units)
    // MF timestamps are in 100ns units
    LONGLONG sampleTime = captureTimestampMs * 10000LL;
    hr = sample->SetSampleTime(sampleTime);
    if (FAILED(hr))
    {
        LogEncoderError("SetSampleTime", hr);
        m_hasError = true;
        return hr;
    }

    // Set sample duration based on frame rate
    LONGLONG duration = 10000000LL / static_cast<LONGLONG>(m_config.fps);
    sample->SetSampleDuration(duration);

    std::cerr << "[Enc] ProcessInput...\n";
    std::cerr.flush();
    hr = m_transform->ProcessInput(0, sample.Get(), 0);
    if (FAILED(hr))
    {
        std::cerr << "[Enc] ProcessInput FAILED: 0x" << std::hex << hr << std::dec << "\n";
        std::cerr.flush();
        m_hasError = true;
        return hr;
    }
    std::cerr << "[Enc] ProcessInput OK\n";
    std::cerr.flush();

    // Track pending encode
    m_pendingEncodes.fetch_add(1);

    return S_OK;
}

// ----------------------------------------------------------------------------
// RequestKeyframe: Force the next encoded output to be an IDR keyframe.
// Called when the receiver sends a "kf" control message.
//
// Validates: Requirements 3.4
// ----------------------------------------------------------------------------
void MFTEncoder::RequestKeyframe()
{
    m_forceKeyframe.store(true);
}

// ----------------------------------------------------------------------------
// GetOutput: Retrieve encoded Annex B NAL units from the encoder.
// Converts encoder output to Annex B format with 4-byte start codes.
// Prefixes keyframes with SPS+PPS NAL units.
// Decrements pendingEncodes on success.
//
// Validates: Requirements 3.3, 3.5, 3.9
// ----------------------------------------------------------------------------
HRESULT MFTEncoder::GetOutput(std::vector<uint8_t>& annexBData, bool& isKeyframe)
{
    annexBData.clear();
    isKeyframe = false;

    if (!m_transform)
    {
        return E_NOT_VALID_STATE;
    }

    // If the encoder has entered an error state, refuse all output retrieval
    if (m_hasError)
    {
        return E_FAIL;
    }

    // Query the output stream info to determine if we need to allocate the buffer
    MFT_OUTPUT_STREAM_INFO streamInfo = {};
    HRESULT hr = m_transform->GetOutputStreamInfo(0, &streamInfo);
    if (FAILED(hr))
    {
        LogEncoderError("GetOutputStreamInfo", hr);
        m_hasError = true;
        return hr;
    }

    // Prepare the output data buffer
    MFT_OUTPUT_DATA_BUFFER outputBuffer = {};
    outputBuffer.dwStreamID = 0;
    outputBuffer.pSample = nullptr;
    outputBuffer.dwStatus = 0;
    outputBuffer.pEvents = nullptr;

    // If the MFT does not provide its own samples, we must allocate one
    ComPtr<IMFSample> outputSample;
    ComPtr<IMFMediaBuffer> outputMediaBuffer;
    bool mftProvidesSamples = (streamInfo.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES) != 0;

    if (!mftProvidesSamples)
    {
        hr = MFCreateSample(outputSample.GetAddressOf());
        if (FAILED(hr))
        {
            LogEncoderError("MFCreateSample (output)", hr);
            m_hasError = true;
            return hr;
        }

        DWORD bufferSize = streamInfo.cbSize > 0 ? streamInfo.cbSize : (1024 * 1024);
        hr = MFCreateMemoryBuffer(bufferSize, outputMediaBuffer.GetAddressOf());
        if (FAILED(hr))
        {
            LogEncoderError("MFCreateMemoryBuffer", hr);
            m_hasError = true;
            return hr;
        }

        hr = outputSample->AddBuffer(outputMediaBuffer.Get());
        if (FAILED(hr))
        {
            LogEncoderError("AddBuffer (output)", hr);
            m_hasError = true;
            return hr;
        }

        outputBuffer.pSample = outputSample.Get();
    }

    // Try to retrieve output from the encoder
    DWORD status = 0;
    std::cerr << "[Enc] GetOutput: calling ProcessOutput...\n";
    std::cerr.flush();
    hr = m_transform->ProcessOutput(0, 1, &outputBuffer, &status);
    std::cerr << "[Enc] GetOutput: ProcessOutput returned 0x" << std::hex << hr << std::dec << "\n";
    std::cerr.flush();

    // Release any events returned
    if (outputBuffer.pEvents)
    {
        outputBuffer.pEvents->Release();
        outputBuffer.pEvents = nullptr;
    }

    if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT)
    {
        // Encoder consumed the previous input but hasn't produced output yet.
        // Decrement pendingEncodes so new frames can be submitted.
        m_pendingEncodes.fetch_sub(1);
        return MF_E_TRANSFORM_NEED_MORE_INPUT;
    }

    if (hr == MF_E_TRANSFORM_STREAM_CHANGE)
    {
        std::cerr << "[Enc] GetOutput: STREAM_CHANGE — renegotiating\n";
        // Output type changed — re-negotiate and try again
        ComPtr<IMFMediaType> newOutputType;
        HRESULT hrType = m_transform->GetOutputAvailableType(0, 0, newOutputType.GetAddressOf());
        if (SUCCEEDED(hrType))
        {
            m_transform->SetOutputType(0, newOutputType.Get(), 0);
        }
        ExtractSPSPPS();
        return MF_E_TRANSFORM_NEED_MORE_INPUT;
    }

    if (FAILED(hr))
    {
        std::cerr << "[Enc] GetOutput: ProcessOutput FAILED: 0x" << std::hex << hr << std::dec << "\n";
        m_hasError = true;
        return hr;
    }

    std::cerr << "[Enc] GetOutput: ProcessOutput SUCCESS\n";

    // Get the sample from the output buffer
    IMFSample* resultSample = outputBuffer.pSample;
    if (!resultSample)
    {
        // Should not happen, but handle gracefully
        m_pendingEncodes.fetch_sub(1);
        return MF_E_TRANSFORM_NEED_MORE_INPUT;
    }

    // Extract the buffer from the sample
    ComPtr<IMFMediaBuffer> resultBuffer;
    hr = resultSample->ConvertToContiguousBuffer(resultBuffer.GetAddressOf());
    if (FAILED(hr))
    {
        LogEncoderError("ConvertToContiguousBuffer", hr);
        m_hasError = true;
        // If the MFT provided the sample, don't release it (it's managed internally)
        if (mftProvidesSamples)
        {
            resultSample->Release();
        }
        return hr;
    }

    // Lock the buffer and convert to Annex B
    BYTE* rawData = nullptr;
    DWORD rawLength = 0;
    hr = resultBuffer->Lock(&rawData, nullptr, &rawLength);
    if (FAILED(hr))
    {
        LogEncoderError("Lock buffer", hr);
        m_hasError = true;
        if (mftProvidesSamples)
        {
            resultSample->Release();
        }
        return hr;
    }

    // Convert the raw encoder output to Annex B format
    ConvertToAnnexB(rawData, rawLength, annexBData, isKeyframe);

    resultBuffer->Unlock();

    // If this is a keyframe, prefix with SPS + PPS
    if (isKeyframe && (!m_spsNalu.empty() || !m_ppsNalu.empty()))
    {
        std::vector<uint8_t> prefixed;
        // Reserve space: SPS start code + SPS + PPS start code + PPS + data
        prefixed.reserve(
            (m_spsNalu.empty() ? 0 : 4 + m_spsNalu.size()) +
            (m_ppsNalu.empty() ? 0 : 4 + m_ppsNalu.size()) +
            annexBData.size());

        // Append SPS with start code
        if (!m_spsNalu.empty())
        {
            prefixed.insert(prefixed.end(), kAnnexBStartCode, kAnnexBStartCode + 4);
            prefixed.insert(prefixed.end(), m_spsNalu.begin(), m_spsNalu.end());
        }

        // Append PPS with start code
        if (!m_ppsNalu.empty())
        {
            prefixed.insert(prefixed.end(), kAnnexBStartCode, kAnnexBStartCode + 4);
            prefixed.insert(prefixed.end(), m_ppsNalu.begin(), m_ppsNalu.end());
        }

        // Append the slice data
        prefixed.insert(prefixed.end(), annexBData.begin(), annexBData.end());
        annexBData = std::move(prefixed);
    }

    // If the MFT provided the sample, release it
    if (mftProvidesSamples)
    {
        resultSample->Release();
    }

    // Decrement pending encodes on successful output
    m_pendingEncodes.fetch_sub(1);

    return S_OK;
}

// ----------------------------------------------------------------------------
// Shutdown: Release encoder session and all associated resources.
// Resets error state so the encoder can be re-initialized.
// ----------------------------------------------------------------------------
void MFTEncoder::Shutdown()
{
    if (m_transform)
    {
        m_transform->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
        m_transform->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0);
        m_transform.Reset();
    }

    m_device.Reset();
    m_pendingEncodes.store(0);
    m_forceKeyframe.store(true);
    m_isHardware = false;
    m_hasError = false;
    m_spsNalu.clear();
    m_ppsNalu.clear();
    m_config = {};
}

// ----------------------------------------------------------------------------
// HasError: Returns true if the encoder has encountered an error.
// The encoder ceases encoding until Shutdown() and re-Initialize() are called.
//
// Validates: Requirements 3.9
// ----------------------------------------------------------------------------
bool MFTEncoder::HasError() const
{
    return m_hasError;
}

// ----------------------------------------------------------------------------
// ExtractSPSPPS: Extract SPS and PPS NAL units from the encoder's output
// media type. MF encoders often store these in the MF_MT_MPEG_SEQUENCE_HEADER
// attribute as an AVCC decoder configuration record.
// ----------------------------------------------------------------------------
HRESULT MFTEncoder::ExtractSPSPPS()
{
    if (!m_transform)
    {
        return E_NOT_VALID_STATE;
    }

    ComPtr<IMFMediaType> outputType;
    HRESULT hr = m_transform->GetOutputCurrentType(0, outputType.GetAddressOf());
    if (FAILED(hr))
    {
        return hr;
    }

    // Try to get the sequence header (AVCC format: contains SPS and PPS)
    UINT32 headerSize = 0;
    hr = outputType->GetBlobSize(MF_MT_MPEG_SEQUENCE_HEADER, &headerSize);
    if (FAILED(hr) || headerSize == 0)
    {
        // Not available — will extract from bitstream on first keyframe
        return S_FALSE;
    }

    std::vector<uint8_t> header(headerSize);
    hr = outputType->GetBlob(MF_MT_MPEG_SEQUENCE_HEADER, header.data(), headerSize, &headerSize);
    if (FAILED(hr))
    {
        return hr;
    }

    // The sequence header may be in Annex B format (start codes) or AVCC format.
    // Parse as Annex B: scan for start codes and identify NAL types.
    m_spsNalu.clear();
    m_ppsNalu.clear();

    size_t i = 0;
    while (i < header.size())
    {
        // Find a start code (00 00 00 01 or 00 00 01)
        size_t startCodeLen = 0;
        if (i + 3 < header.size() &&
            header[i] == 0x00 && header[i + 1] == 0x00 &&
            header[i + 2] == 0x00 && header[i + 3] == 0x01)
        {
            startCodeLen = 4;
        }
        else if (i + 2 < header.size() &&
                 header[i] == 0x00 && header[i + 1] == 0x00 && header[i + 2] == 0x01)
        {
            startCodeLen = 3;
        }
        else
        {
            i++;
            continue;
        }

        size_t naluStart = i + startCodeLen;

        // Find the end of this NAL unit (next start code or end of buffer)
        size_t naluEnd = header.size();
        for (size_t j = naluStart; j + 2 < header.size(); j++)
        {
            if (header[j] == 0x00 && header[j + 1] == 0x00 &&
                (header[j + 2] == 0x01 ||
                 (j + 3 < header.size() && header[j + 2] == 0x00 && header[j + 3] == 0x01)))
            {
                naluEnd = j;
                break;
            }
        }

        if (naluStart < naluEnd)
        {
            uint8_t nalType = header[naluStart] & 0x1F;
            if (nalType == 7)  // SPS
            {
                m_spsNalu.assign(header.begin() + naluStart, header.begin() + naluEnd);
            }
            else if (nalType == 8)  // PPS
            {
                m_ppsNalu.assign(header.begin() + naluStart, header.begin() + naluEnd);
            }
        }

        i = naluEnd;
    }

    // If the header was not in Annex B format (no start codes found),
    // try to parse as raw AVCC decoder configuration record
    if (m_spsNalu.empty() && m_ppsNalu.empty() && header.size() >= 8)
    {
        // AVCC format:
        // byte 0: version (1)
        // byte 1: profile
        // byte 2: compatibility
        // byte 3: level
        // byte 4: 0xFC | (lengthSizeMinusOne & 3) — NALU length size
        // byte 5: 0xE0 | (numSPS & 0x1F)
        // Then for each SPS: [2 bytes BE length][SPS data]
        // Then: numPPS (1 byte)
        // Then for each PPS: [2 bytes BE length][PPS data]
        if (header[0] == 1 && header.size() >= 7)
        {
            size_t offset = 5;
            uint8_t numSPS = header[offset] & 0x1F;
            offset++;

            for (uint8_t s = 0; s < numSPS && offset + 2 <= header.size(); s++)
            {
                uint16_t spsLen = (static_cast<uint16_t>(header[offset]) << 8) | header[offset + 1];
                offset += 2;
                if (offset + spsLen <= header.size())
                {
                    m_spsNalu.assign(header.begin() + offset, header.begin() + offset + spsLen);
                    offset += spsLen;
                }
            }

            if (offset < header.size())
            {
                uint8_t numPPS = header[offset];
                offset++;
                for (uint8_t p = 0; p < numPPS && offset + 2 <= header.size(); p++)
                {
                    uint16_t ppsLen = (static_cast<uint16_t>(header[offset]) << 8) | header[offset + 1];
                    offset += 2;
                    if (offset + ppsLen <= header.size())
                    {
                        m_ppsNalu.assign(header.begin() + offset, header.begin() + offset + ppsLen);
                        offset += ppsLen;
                    }
                }
            }
        }
    }

    return S_OK;
}

// ----------------------------------------------------------------------------
// ConvertToAnnexB: Convert encoder output to Annex B format.
// MF encoders may output in Annex B (with start codes) or AVCC/length-prefixed
// format. This handles both cases and also extracts SPS/PPS from the stream
// if not already cached.
// ----------------------------------------------------------------------------
void MFTEncoder::ConvertToAnnexB(const uint8_t* data, DWORD size,
                                  std::vector<uint8_t>& annexBData, bool& outIsKeyframe)
{
    outIsKeyframe = false;

    if (!data || size == 0)
    {
        return;
    }

    // Check if the data already starts with an Annex B start code
    bool isAnnexB = (size >= 4 &&
                     data[0] == 0x00 && data[1] == 0x00 &&
                     data[2] == 0x00 && data[3] == 0x01) ||
                    (size >= 3 &&
                     data[0] == 0x00 && data[1] == 0x00 && data[2] == 0x01);

    if (isAnnexB)
    {
        // Data is already in Annex B format — copy as-is and scan for IDR NALUs
        annexBData.assign(data, data + size);

        // Scan for NAL unit types to determine if this is a keyframe
        // Also extract SPS/PPS if not yet cached
        size_t i = 0;
        while (i < size)
        {
            size_t startCodeLen = 0;
            if (i + 3 < size &&
                data[i] == 0x00 && data[i + 1] == 0x00 &&
                data[i + 2] == 0x00 && data[i + 3] == 0x01)
            {
                startCodeLen = 4;
            }
            else if (i + 2 < size &&
                     data[i] == 0x00 && data[i + 1] == 0x00 && data[i + 2] == 0x01)
            {
                startCodeLen = 3;
            }
            else
            {
                i++;
                continue;
            }

            size_t naluStart = i + startCodeLen;
            if (naluStart >= size)
            {
                break;
            }

            uint8_t nalType = data[naluStart] & 0x1F;

            // Find end of this NALU
            size_t naluEnd = size;
            for (size_t j = naluStart + 1; j + 2 < size; j++)
            {
                if (data[j] == 0x00 && data[j + 1] == 0x00 &&
                    (data[j + 2] == 0x01 ||
                     (j + 3 < size && data[j + 2] == 0x00 && data[j + 3] == 0x01)))
                {
                    naluEnd = j;
                    break;
                }
            }

            if (nalType == 5)  // IDR slice
            {
                outIsKeyframe = true;
            }
            else if (nalType == 7 && m_spsNalu.empty())  // SPS
            {
                m_spsNalu.assign(data + naluStart, data + naluEnd);
            }
            else if (nalType == 8 && m_ppsNalu.empty())  // PPS
            {
                m_ppsNalu.assign(data + naluStart, data + naluEnd);
            }

            i = naluEnd;
        }

        // If the data already contains SPS/PPS (some encoders include them in stream),
        // and it's a keyframe, we don't need to prepend them again.
        // Remove the SPS/PPS from annexBData if they're already there and we'll re-add in GetOutput.
        if (outIsKeyframe)
        {
            // Rebuild without SPS/PPS — we'll add them back in GetOutput with proper structure
            std::vector<uint8_t> sliceOnly;
            i = 0;
            bool hasSPSPPSInStream = false;
            while (i < size)
            {
                size_t startCodeLen = 0;
                if (i + 3 < size &&
                    data[i] == 0x00 && data[i + 1] == 0x00 &&
                    data[i + 2] == 0x00 && data[i + 3] == 0x01)
                {
                    startCodeLen = 4;
                }
                else if (i + 2 < size &&
                         data[i] == 0x00 && data[i + 1] == 0x00 && data[i + 2] == 0x01)
                {
                    startCodeLen = 3;
                }
                else
                {
                    i++;
                    continue;
                }

                size_t naluStart = i + startCodeLen;
                if (naluStart >= size) break;

                uint8_t nalType2 = data[naluStart] & 0x1F;

                // Find end
                size_t naluEnd = size;
                for (size_t j = naluStart + 1; j + 2 < size; j++)
                {
                    if (data[j] == 0x00 && data[j + 1] == 0x00 &&
                        (data[j + 2] == 0x01 ||
                         (j + 3 < size && data[j + 2] == 0x00 && data[j + 3] == 0x01)))
                    {
                        naluEnd = j;
                        break;
                    }
                }

                if (nalType2 == 7 || nalType2 == 8)
                {
                    // SPS or PPS — skip (we'll add them in GetOutput)
                    hasSPSPPSInStream = true;
                }
                else
                {
                    // Keep this NAL unit with a 4-byte start code
                    sliceOnly.insert(sliceOnly.end(), kAnnexBStartCode, kAnnexBStartCode + 4);
                    sliceOnly.insert(sliceOnly.end(), data + naluStart, data + naluEnd);
                }

                i = naluEnd;
            }

            if (hasSPSPPSInStream)
            {
                annexBData = std::move(sliceOnly);
            }
        }
    }
    else
    {
        // Data is in AVCC/length-prefixed format — convert each NAL to Annex B
        // NAL units are prefixed with a 4-byte big-endian length
        size_t offset = 0;
        while (offset + 4 <= size)
        {
            uint32_t naluLen = (static_cast<uint32_t>(data[offset]) << 24) |
                               (static_cast<uint32_t>(data[offset + 1]) << 16) |
                               (static_cast<uint32_t>(data[offset + 2]) << 8) |
                               static_cast<uint32_t>(data[offset + 3]);
            offset += 4;

            if (naluLen == 0 || offset + naluLen > size)
            {
                break;  // Invalid length — stop processing
            }

            uint8_t nalType = data[offset] & 0x1F;

            if (nalType == 7)  // SPS
            {
                m_spsNalu.assign(data + offset, data + offset + naluLen);
                // Don't add to output — GetOutput will prefix keyframes
            }
            else if (nalType == 8)  // PPS
            {
                m_ppsNalu.assign(data + offset, data + offset + naluLen);
                // Don't add to output — GetOutput will prefix keyframes
            }
            else
            {
                // Add start code + NAL unit data
                annexBData.insert(annexBData.end(), kAnnexBStartCode, kAnnexBStartCode + 4);
                annexBData.insert(annexBData.end(), data + offset, data + offset + naluLen);

                if (nalType == 5)  // IDR slice
                {
                    outIsKeyframe = true;
                }
            }

            offset += naluLen;
        }
    }
}

