#pragma once
// AMF-based H.264 hardware encoder for AMD GPUs.
// Uses the AMD Advanced Media Framework SDK directly for guaranteed
// GPU hardware encoding without MFT D3D device manager issues.

#include <atomic>
#include <cstdint>
#include <vector>
#include <d3d11.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

// Forward declarations for AMF types (avoids including full AMF headers here)
namespace amf {
    class AMFContext;
    class AMFComponent;
    class AMFSurface;
    class AMFData;
}

class AmfEncoder {
public:
    struct Config {
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t fps = 60;
        uint32_t bitrate = 10'000'000;
    };

    AmfEncoder();
    ~AmfEncoder();

    // Initialize AMF encoder with a D3D11 device (must be on AMD GPU)
    HRESULT Initialize(const Config& config, ID3D11Device* device);

    // Submit a BGRA texture for encoding. AMF handles color conversion internally.
    // Returns S_OK, MF_E_NOTACCEPTING if busy, or error.
    HRESULT SubmitFrame(ID3D11Texture2D* texture, int64_t captureTimestampMs);

    // Force next output to be IDR keyframe
    void RequestKeyframe();

    // Get encoded Annex B output. Returns S_OK with data, or S_FALSE if no output ready.
    HRESULT GetOutput(std::vector<uint8_t>& annexBData, bool& isKeyframe);

    // Shutdown and release
    void Shutdown();

    bool IsInitialized() const { return m_initialized; }

private:
    struct Impl;
    Impl* m_impl = nullptr;
    bool m_initialized = false;
    std::atomic<bool> m_forceKeyframe{true};
    Config m_config{};
};
