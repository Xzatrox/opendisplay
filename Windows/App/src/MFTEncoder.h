#pragma once

#include <atomic>
#include <cstdint>
#include <vector>
#include <d3d11.h>
#include <mfidl.h>
#include <mftransform.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

/// Hardware-accelerated H.264 encoder using Media Foundation Transform.
/// Produces Annex B bitstream compatible with the existing iOS receiver.
///
/// Validates: Requirements 3.1
class MFTEncoder {
public:
    /// Encoder configuration parameters.
    struct Config {
        uint32_t width;            ///< Frame width in pixels (must be even)
        uint32_t height;           ///< Frame height in pixels (must be even)
        uint32_t fps = 60;         ///< Target frame rate
        uint32_t bitrate;          ///< Target bitrate from quality preset (best=18Mbps, balanced=10Mbps, fast=6Mbps)
        bool hardwareOnly = true;  ///< If true, fail when no HW encoder; if false, fall back to software
    };

    /// Create encoder session with given configuration.
    /// Activates hardware MFT (Intel QSV / NVIDIA NVENC / AMD AMF).
    /// @param config Encoder parameters including resolution and bitrate.
    /// @param device The D3D11 device for GPU-resident texture input.
    /// @return S_OK on success, or an appropriate error HRESULT.
    HRESULT Initialize(const Config& config, ID3D11Device* device);

    /// Submit a frame for encoding.
    /// @param texture GPU-resident texture from Desktop Duplication capture.
    /// @param captureTimestampMs Capture timestamp in Unix epoch milliseconds.
    /// @return S_OK on success, MF_E_NOTACCEPTING if encoder is busy (caller should drop frame).
    HRESULT SubmitFrame(ID3D11Texture2D* texture, int64_t captureTimestampMs);

    /// Force next output to be IDR (keyframe request from receiver "kf" message).
    void RequestKeyframe();

    /// Retrieve encoded output (called from output callback or polling).
    /// @param annexBData Receives the encoded H.264 Annex B NAL units.
    /// @param isKeyframe Set to true if the output is an IDR frame (prefixed with SPS+PPS).
    /// @return S_OK with NAL units, or MF_E_TRANSFORM_NEED_MORE_INPUT if no output ready.
    HRESULT GetOutput(std::vector<uint8_t>& annexBData, bool& isKeyframe);

    /// Shutdown and release encoder session.
    void Shutdown();

    /// @return true if using a hardware encoder, false if software fallback.
    bool IsHardwareEncoder() const;

    /// @return true if the encoder has encountered an error and is no longer usable.
    bool HasError() const;

private:
    /// Extract SPS and PPS NAL units from the encoder's output media type.
    HRESULT ExtractSPSPPS();

    /// Convert AVCC/length-prefixed NAL units to Annex B format with 4-byte start codes.
    /// @param data Raw buffer from the encoder output.
    /// @param size Size of the raw buffer in bytes.
    /// @param annexBData Output vector receiving Annex B formatted NAL units.
    /// @param outIsKeyframe Set to true if any NAL unit is IDR (type 5).
    void ConvertToAnnexB(const uint8_t* data, DWORD size,
                         std::vector<uint8_t>& annexBData, bool& outIsKeyframe);

    ComPtr<IMFTransform> m_transform;
    ComPtr<ID3D11Device> m_device;
    std::atomic<bool> m_forceKeyframe{true};  ///< IDR on first frame
    std::atomic<int> m_pendingEncodes{0};
    Config m_config{};
    bool m_isHardware{false};     ///< True when using hardware encoder
    bool m_hasError{false};       ///< True when encoder has failed; ceases encoding until next session

    /// Cached SPS and PPS NAL units (extracted after first successful encode or from output type)
    std::vector<uint8_t> m_spsNalu;   ///< Sequence Parameter Set NAL unit (without start code)
    std::vector<uint8_t> m_ppsNalu;   ///< Picture Parameter Set NAL unit (without start code)
};
