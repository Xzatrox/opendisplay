// AMF-based H.264 hardware encoder using AMD Advanced Media Framework.
// The AMF runtime (amfrt64.dll) is installed with AMD GPU drivers.
// This implementation loads AMF dynamically at runtime.

#include "AmfEncoder.h"

#include <iostream>
#include <chrono>
#include <thread>
#include <d3d11.h>

// AMF SDK headers
#include "components/VideoEncoderVCE.h"
#include "core/Factory.h"
#include "core/Context.h"
#include "core/Trace.h"

// AMF is loaded dynamically — the DLL ships with AMD drivers
#define AMF_DLL_NAME L"amfrt64.dll"
#define AMF_INIT_FUNCTION_NAME "AMFInit"

typedef AMF_RESULT(AMF_CDECL_CALL* AMFInit_Fn)(amf_uint64 version, amf::AMFFactory** ppFactory);

struct AmfEncoder::Impl {
    HMODULE hAmfDll = nullptr;
    amf::AMFFactory* factory = nullptr;
    amf::AMFContextPtr context;
    amf::AMFComponentPtr encoder;
    ComPtr<ID3D11Device> device;
    int64_t frameIndex = 0;
    bool firstFrame = true;
};

AmfEncoder::AmfEncoder() : m_impl(new Impl()) {}

AmfEncoder::~AmfEncoder() {
    Shutdown();
    delete m_impl;
}

HRESULT AmfEncoder::Initialize(const Config& config, ID3D11Device* device)
{
    if (!device || config.width == 0 || config.height == 0) {
        return E_INVALIDARG;
    }

    m_config = config;
    m_impl->device = device;

    // Load AMF runtime DLL
    m_impl->hAmfDll = LoadLibraryW(AMF_DLL_NAME);
    if (!m_impl->hAmfDll) {
        std::cerr << "[AmfEncoder] Failed to load amfrt64.dll — AMD driver not installed?\n";
        return E_FAIL;
    }

    // Get AMF initialization function
    auto amfInit = reinterpret_cast<AMFInit_Fn>(
        GetProcAddress(m_impl->hAmfDll, AMF_INIT_FUNCTION_NAME));
    if (!amfInit) {
        std::cerr << "[AmfEncoder] AMFInit not found in DLL\n";
        FreeLibrary(m_impl->hAmfDll);
        m_impl->hAmfDll = nullptr;
        return E_FAIL;
    }

    // Initialize AMF
    AMF_RESULT res = amfInit(AMF_FULL_VERSION, &m_impl->factory);
    if (res != AMF_OK || !m_impl->factory) {
        std::cerr << "[AmfEncoder] AMFInit failed: " << res << "\n";
        return E_FAIL;
    }

    // Create AMF context
    res = m_impl->factory->CreateContext(&m_impl->context);
    if (res != AMF_OK) {
        std::cerr << "[AmfEncoder] CreateContext failed: " << res << "\n";
        return E_FAIL;
    }

    // Initialize context with our D3D11 device
    res = m_impl->context->InitDX11(device);
    if (res != AMF_OK) {
        std::cerr << "[AmfEncoder] InitDX11 failed: " << res << "\n";
        return E_FAIL;
    }
    std::cerr << "[AmfEncoder] AMF context initialized with D3D11 device\n";

    // Create H.264 encoder component
    res = m_impl->factory->CreateComponent(m_impl->context, AMFVideoEncoderVCE_AVC,
                                            &m_impl->encoder);
    if (res != AMF_OK) {
        std::cerr << "[AmfEncoder] CreateComponent(AVC) failed: " << res << "\n";
        return E_FAIL;
    }

    // Configure encoder for low-latency streaming
    m_impl->encoder->SetProperty(AMF_VIDEO_ENCODER_USAGE, AMF_VIDEO_ENCODER_USAGE_LOW_LATENCY);
    m_impl->encoder->SetProperty(AMF_VIDEO_ENCODER_PROFILE, AMF_VIDEO_ENCODER_PROFILE_HIGH);
    m_impl->encoder->SetProperty(AMF_VIDEO_ENCODER_RATE_CONTROL_METHOD,
                                  AMF_VIDEO_ENCODER_RATE_CONTROL_METHOD_CBR);
    m_impl->encoder->SetProperty(AMF_VIDEO_ENCODER_TARGET_BITRATE, (amf_int64)config.bitrate);
    m_impl->encoder->SetProperty(AMF_VIDEO_ENCODER_FRAMERATE, AMFConstructRate(config.fps, 1));
    m_impl->encoder->SetProperty(AMF_VIDEO_ENCODER_B_PIC_PATTERN, (amf_int64)0); // No B-frames
    m_impl->encoder->SetProperty(AMF_VIDEO_ENCODER_QUALITY_PRESET,
                                  AMF_VIDEO_ENCODER_QUALITY_PRESET_SPEED);
    m_impl->encoder->SetProperty(AMF_VIDEO_ENCODER_LOWLATENCY_MODE, true);
    m_impl->encoder->SetProperty(AMF_VIDEO_ENCODER_IDR_PERIOD, (amf_int64)3600);

    // Initialize the encoder with frame dimensions
    res = m_impl->encoder->Init(amf::AMF_SURFACE_BGRA, config.width, config.height);
    if (res != AMF_OK) {
        std::cerr << "[AmfEncoder] Encoder Init(" << config.width << "x"
                  << config.height << ") failed: " << res << "\n";
        return E_FAIL;
    }

    std::cerr << "[AmfEncoder] Initialized: " << config.width << "x" << config.height
              << " @ " << config.bitrate / 1000000 << " Mbps (HW)\n";
    m_initialized = true;
    m_forceKeyframe.store(true);
    return S_OK;
}

HRESULT AmfEncoder::SubmitFrame(ID3D11Texture2D* texture, int64_t captureTimestampMs)
{
    if (!m_initialized || !m_impl->encoder) return E_NOT_VALID_STATE;
    if (!texture) return E_INVALIDARG;

    // Create AMF surface from D3D11 texture (zero-copy — GPU stays on GPU)
    amf::AMFSurfacePtr surface;
    AMF_RESULT res = m_impl->context->CreateSurfaceFromDX11Native(
        texture, &surface, nullptr);
    if (res != AMF_OK) {
        std::cerr << "[AmfEncoder] CreateSurfaceFromDX11Native failed: " << res << "\n";
        return E_FAIL;
    }

    // Set timestamp
    surface->SetPts(captureTimestampMs * 10000); // AMF uses 100ns units

    // Force IDR if requested
    if (m_forceKeyframe.exchange(false)) {
        surface->SetProperty(AMF_VIDEO_ENCODER_FORCE_PICTURE_TYPE,
                             AMF_VIDEO_ENCODER_PICTURE_TYPE_IDR);
        surface->SetProperty(AMF_VIDEO_ENCODER_INSERT_SPS, true);
        surface->SetProperty(AMF_VIDEO_ENCODER_INSERT_PPS, true);
    }

    // Submit to encoder (non-blocking)
    res = m_impl->encoder->SubmitInput(surface);
    if (res == AMF_INPUT_FULL) {
        return 0x800401F0L; // MF_E_NOTACCEPTING equivalent
    }
    if (res != AMF_OK) {
        std::cerr << "[AmfEncoder] SubmitInput failed: " << res << "\n";
        return E_FAIL;
    }

    m_impl->frameIndex++;
    return S_OK;
}

void AmfEncoder::RequestKeyframe()
{
    m_forceKeyframe.store(true);
}

HRESULT AmfEncoder::GetOutput(std::vector<uint8_t>& annexBData, bool& isKeyframe)
{
    annexBData.clear();
    isKeyframe = false;

    if (!m_initialized || !m_impl->encoder) return E_NOT_VALID_STATE;

    amf::AMFDataPtr data;
    AMF_RESULT res = m_impl->encoder->QueryOutput(&data);
    if (res == AMF_REPEAT) {
        return S_FALSE; // No output ready yet
    }
    if (res != AMF_OK || !data) {
        return S_FALSE;
    }

    // Get the encoded buffer
    amf::AMFBufferPtr buffer(data);
    if (!buffer) return S_FALSE;

    // Copy Annex B data
    size_t size = buffer->GetSize();
    if (size == 0) return S_FALSE;

    annexBData.resize(size);
    memcpy(annexBData.data(), buffer->GetNative(), size);

    // Check if it's a keyframe
    amf_int64 outputType = 0;
    if (data->GetProperty(AMF_VIDEO_ENCODER_OUTPUT_DATA_TYPE, &outputType) == AMF_OK) {
        isKeyframe = (outputType == AMF_VIDEO_ENCODER_OUTPUT_DATA_TYPE_IDR);
    }

    return S_OK;
}

void AmfEncoder::Shutdown()
{
    if (m_impl->encoder) {
        m_impl->encoder->Terminate();
        m_impl->encoder = nullptr;
    }
    if (m_impl->context) {
        m_impl->context->Terminate();
        m_impl->context = nullptr;
    }
    m_impl->factory = nullptr;
    if (m_impl->hAmfDll) {
        FreeLibrary(m_impl->hAmfDll);
        m_impl->hAmfDll = nullptr;
    }
    m_impl->device.Reset();
    m_initialized = false;
}
