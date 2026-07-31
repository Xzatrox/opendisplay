// Property 4: Keyframe Request Fulfillment
// **Validates: Requirements 3.4, 6.5**
//
// For any point during active streaming, when a "kf" control message is
// received from the receiver, the very next encoded frame output SHALL be
// an IDR frame prefixed with SPS and PPS NAL units.
//
// This test models the MFTEncoder keyframe request/fulfillment logic
// (m_forceKeyframe flag, RequestKeyframe(), SubmitFrame IDR forcing,
// GetOutput SPS+PPS prefixing) without requiring actual Media Foundation
// or D3D11 dependencies.

#include <gtest/gtest.h>
#include <rapidcheck.h>
#include <rapidcheck/gtest.h>

#include <cstdint>
#include <vector>

namespace {

// ─── Annex B helpers ─────────────────────────────────────────────────────────

static const uint8_t kStartCode[4] = { 0x00, 0x00, 0x00, 0x01 };

// NAL unit type constants
static constexpr uint8_t kNalTypeSPS = 7;
static constexpr uint8_t kNalTypePPS = 8;
static constexpr uint8_t kNalTypeIDR = 5;
static constexpr uint8_t kNalTypeNonIDR = 1;

/// Check if a buffer starts with a 4-byte Annex B start code at the given offset.
inline bool HasStartCodeAt(const std::vector<uint8_t>& data, size_t offset) {
    if (offset + 4 > data.size()) return false;
    return data[offset] == 0x00 && data[offset + 1] == 0x00 &&
           data[offset + 2] == 0x00 && data[offset + 3] == 0x01;
}

/// Extract NAL unit type (lower 5 bits) from the first byte after a start code.
inline uint8_t GetNalType(const std::vector<uint8_t>& data, size_t offset) {
    if (offset >= data.size()) return 0xFF;
    return data[offset] & 0x1F;
}

/// Parse all NAL units from an Annex B bitstream and return their types.
inline std::vector<uint8_t> ParseNalTypes(const std::vector<uint8_t>& data) {
    std::vector<uint8_t> types;
    for (size_t i = 0; i + 4 < data.size(); ++i) {
        if (HasStartCodeAt(data, i)) {
            types.push_back(GetNalType(data, i + 4));
            i += 4; // skip past start code (loop will increment too)
        }
    }
    return types;
}

// ─── Model of the MFTEncoder keyframe request/fulfillment logic ──────────────
//
// Mirrors the behavior in MFTEncoder.cpp:
//   - m_forceKeyframe starts as true (first frame is IDR)
//   - RequestKeyframe() sets m_forceKeyframe = true
//   - SubmitFrame() atomically reads-and-clears m_forceKeyframe;
//     if it was true, that submission produces an IDR
//   - GetOutput() for an IDR prefixes the output with SPS+PPS NAL units
//
// The model simulates:
//   - A stream of frames being submitted and output retrieved
//   - Random keyframe requests at arbitrary positions
//   - Verification that the next output after a kf request is always IDR with SPS+PPS

class KeyframeRequestModel {
public:
    /// Simulated SPS data (arbitrary bytes for testing NAL structure)
    static constexpr uint8_t kFakeSPS = 0x67;  // NAL type 7 with forbidden=0, nri=3
    /// Simulated PPS data (arbitrary bytes for testing NAL structure)
    static constexpr uint8_t kFakePPS = 0x68;  // NAL type 8 with forbidden=0, nri=3

    KeyframeRequestModel()
        : m_forceKeyframe(true)  // First frame is always IDR
    {
        // Set up fake SPS and PPS NAL unit data
        m_spsNalu = { kFakeSPS, 0x42, 0x00, 0x1E };  // SPS: type=7, plus dummy bytes
        m_ppsNalu = { kFakePPS, 0xCE, 0x3C, 0x80 };  // PPS: type=8, plus dummy bytes
    }

    /// Request a keyframe (simulates receiver sending "kf" message).
    void RequestKeyframe() {
        m_forceKeyframe = true;
    }

    /// Submit a frame for encoding and produce output.
    /// Returns the encoded Annex B data and whether it's a keyframe.
    struct EncodeResult {
        std::vector<uint8_t> annexBData;
        bool isKeyframe;
    };

    EncodeResult SubmitAndGetOutput() {
        EncodeResult result;

        // Read and clear m_forceKeyframe (atomic exchange in real impl)
        bool forceIdr = m_forceKeyframe;
        m_forceKeyframe = false;

        result.isKeyframe = forceIdr;

        if (forceIdr) {
            // IDR frame: prefix with SPS + PPS, then IDR slice
            // [start code][SPS][start code][PPS][start code][IDR slice]
            result.annexBData.insert(result.annexBData.end(), kStartCode, kStartCode + 4);
            result.annexBData.insert(result.annexBData.end(), m_spsNalu.begin(), m_spsNalu.end());

            result.annexBData.insert(result.annexBData.end(), kStartCode, kStartCode + 4);
            result.annexBData.insert(result.annexBData.end(), m_ppsNalu.begin(), m_ppsNalu.end());

            result.annexBData.insert(result.annexBData.end(), kStartCode, kStartCode + 4);
            // IDR slice NAL: type 5 with nri=3 → 0x65
            result.annexBData.push_back(0x65);
            // Dummy slice data
            result.annexBData.push_back(0x88);
            result.annexBData.push_back(0x04);
        } else {
            // Non-IDR frame: just a P-frame slice
            // [start code][non-IDR slice]
            result.annexBData.insert(result.annexBData.end(), kStartCode, kStartCode + 4);
            // Non-IDR slice NAL: type 1 with nri=2 → 0x41
            result.annexBData.push_back(0x41);
            // Dummy slice data
            result.annexBData.push_back(0x9A);
            result.annexBData.push_back(0x02);
        }

        m_frameCount++;
        return result;
    }

    bool forceKeyframePending() const { return m_forceKeyframe; }
    int frameCount() const { return m_frameCount; }

private:
    bool m_forceKeyframe;
    int m_frameCount = 0;
    std::vector<uint8_t> m_spsNalu;
    std::vector<uint8_t> m_ppsNalu;
};

/// Verify that an Annex B bitstream is a valid IDR frame with SPS+PPS prefix.
/// Expected structure: [start code][SPS (type=7)][start code][PPS (type=8)][start code][IDR (type=5)]
inline bool IsValidIDRWithSPSPPS(const std::vector<uint8_t>& data) {
    auto nalTypes = ParseNalTypes(data);
    if (nalTypes.size() < 3) return false;

    // First NAL must be SPS (type 7)
    if (nalTypes[0] != kNalTypeSPS) return false;
    // Second NAL must be PPS (type 8)
    if (nalTypes[1] != kNalTypePPS) return false;
    // Third NAL must be IDR (type 5)
    if (nalTypes[2] != kNalTypeIDR) return false;

    // Each NAL must be preceded by a 4-byte start code
    if (!HasStartCodeAt(data, 0)) return false;

    return true;
}

/// Verify that an Annex B bitstream is a non-IDR frame (no SPS/PPS prefix).
inline bool IsNonIDRFrame(const std::vector<uint8_t>& data) {
    auto nalTypes = ParseNalTypes(data);
    if (nalTypes.empty()) return false;

    // No SPS or PPS should be present
    for (auto t : nalTypes) {
        if (t == kNalTypeSPS || t == kNalTypePPS || t == kNalTypeIDR) return false;
    }
    // Should contain at least one non-IDR slice
    for (auto t : nalTypes) {
        if (t == kNalTypeNonIDR) return true;
    }
    return false;
}

// ─── Actions for model-based testing ─────────────────────────────────────────

enum class Action : int {
    SubmitFrame = 0,
    RequestKeyframe = 1
};

} // namespace

// ─── Property Tests ──────────────────────────────────────────────────────────

// **Validates: Requirements 3.4, 6.5**
// Property: After RequestKeyframe(), the very next encoded output is IDR with SPS+PPS.
// Generates random sequences of frame submissions and keyframe requests, then
// verifies that every frame produced after a kf request is an IDR with SPS+PPS.
RC_GTEST_PROP(KeyframeRequestFulfillment,
              NextFrameAfterKfIsIDRWithSPSPPS,
              ()) {
    // Generate a random number of initial frames before the first kf request
    auto initialFrames = *rc::gen::inRange<int>(1, 20);
    // Generate random positions within the stream where kf requests occur
    auto numKfRequests = *rc::gen::inRange<int>(1, 10);

    KeyframeRequestModel model;

    // Submit the first frame (always IDR due to m_forceKeyframe=true at init)
    auto firstResult = model.SubmitAndGetOutput();
    RC_ASSERT(firstResult.isKeyframe);
    RC_ASSERT(IsValidIDRWithSPSPPS(firstResult.annexBData));

    // Submit initial non-IDR frames
    for (int i = 1; i < initialFrames; ++i) {
        auto result = model.SubmitAndGetOutput();
        RC_ASSERT(!result.isKeyframe);
        RC_ASSERT(IsNonIDRFrame(result.annexBData));
    }

    // For each kf request, submit some frames before it, then request kf,
    // then verify the very next frame is IDR with SPS+PPS
    for (int kf = 0; kf < numKfRequests; ++kf) {
        // Optionally submit some non-IDR frames between kf requests
        auto framesBetween = *rc::gen::inRange<int>(0, 15);
        for (int f = 0; f < framesBetween; ++f) {
            auto result = model.SubmitAndGetOutput();
            RC_ASSERT(!result.isKeyframe);
            RC_ASSERT(IsNonIDRFrame(result.annexBData));
        }

        // Request keyframe (simulates receiver sending "kf")
        model.RequestKeyframe();
        RC_ASSERT(model.forceKeyframePending());

        // The very next encoded output MUST be IDR with SPS+PPS
        auto kfResult = model.SubmitAndGetOutput();
        RC_ASSERT(kfResult.isKeyframe);
        RC_ASSERT(IsValidIDRWithSPSPPS(kfResult.annexBData));
        RC_ASSERT(!model.forceKeyframePending());  // Flag consumed

        // Subsequent frames should be non-IDR (until next kf request)
        auto framesAfter = *rc::gen::inRange<int>(1, 5);
        for (int f = 0; f < framesAfter; ++f) {
            auto result = model.SubmitAndGetOutput();
            RC_ASSERT(!result.isKeyframe);
            RC_ASSERT(IsNonIDRFrame(result.annexBData));
        }
    }
}

// **Validates: Requirements 3.4, 6.5**
// Property: Multiple consecutive keyframe requests without intervening frames
// still result in exactly one IDR on the next output.
RC_GTEST_PROP(KeyframeRequestFulfillment,
              MultipleKfRequestsBeforeFrameProducesSingleIDR,
              ()) {
    KeyframeRequestModel model;

    // Consume the initial forced IDR
    auto firstResult = model.SubmitAndGetOutput();
    RC_ASSERT(firstResult.isKeyframe);

    // Submit some normal frames
    auto normalFrames = *rc::gen::inRange<int>(1, 10);
    for (int i = 0; i < normalFrames; ++i) {
        model.SubmitAndGetOutput();
    }

    // Issue multiple kf requests without any frame submissions in between
    auto numRequests = *rc::gen::inRange<int>(2, 10);
    for (int i = 0; i < numRequests; ++i) {
        model.RequestKeyframe();
    }

    // The flag should be set (multiple requests are idempotent, result is still true)
    RC_ASSERT(model.forceKeyframePending());

    // Next frame is IDR with SPS+PPS
    auto kfResult = model.SubmitAndGetOutput();
    RC_ASSERT(kfResult.isKeyframe);
    RC_ASSERT(IsValidIDRWithSPSPPS(kfResult.annexBData));

    // Frame after that is NOT IDR (the flag was consumed once)
    auto nextResult = model.SubmitAndGetOutput();
    RC_ASSERT(!nextResult.isKeyframe);
    RC_ASSERT(IsNonIDRFrame(nextResult.annexBData));
}

// **Validates: Requirements 3.4, 6.5**
// Property: Random interleaving of actions — every frame output immediately after
// a kf request (with no other frame in between) is always IDR with SPS+PPS.
RC_GTEST_PROP(KeyframeRequestFulfillment,
              RandomActionSequenceRespectsFulfillment,
              ()) {
    // Generate a random sequence of actions
    auto sequenceLength = *rc::gen::inRange<int>(5, 50);

    KeyframeRequestModel model;
    bool kfPendingBeforeSubmit = model.forceKeyframePending(); // true initially

    for (int i = 0; i < sequenceLength; ++i) {
        auto action = *rc::gen::inRange<int>(0, 3); // bias toward more SubmitFrame

        if (action == 0) {
            // RequestKeyframe action
            model.RequestKeyframe();
            kfPendingBeforeSubmit = true;
        } else {
            // SubmitFrame action (more likely than kf request, simulating real streaming)
            bool expectedKeyframe = kfPendingBeforeSubmit;
            auto result = model.SubmitAndGetOutput();

            if (expectedKeyframe) {
                // This frame MUST be IDR with SPS+PPS
                RC_ASSERT(result.isKeyframe);
                RC_ASSERT(IsValidIDRWithSPSPPS(result.annexBData));
            } else {
                // This frame MUST NOT be IDR
                RC_ASSERT(!result.isKeyframe);
                RC_ASSERT(IsNonIDRFrame(result.annexBData));
            }

            // After submission, the flag is consumed
            kfPendingBeforeSubmit = false;
        }
    }
}

// **Validates: Requirements 3.4, 6.5**
// Property: The first frame of a session is always IDR with SPS+PPS,
// regardless of whether RequestKeyframe() was explicitly called.
RC_GTEST_PROP(KeyframeRequestFulfillment,
              FirstFrameAlwaysIDR,
              ()) {
    KeyframeRequestModel model;

    // Optionally call RequestKeyframe before the first frame (should be no-op
    // since m_forceKeyframe is already true)
    auto callKfFirst = *rc::gen::inRange<int>(0, 2);
    if (callKfFirst) {
        model.RequestKeyframe();
    }

    // First output must be IDR with SPS+PPS
    auto result = model.SubmitAndGetOutput();
    RC_ASSERT(result.isKeyframe);
    RC_ASSERT(IsValidIDRWithSPSPPS(result.annexBData));

    // Verify NAL structure: SPS(7), PPS(8), IDR(5)
    auto nalTypes = ParseNalTypes(result.annexBData);
    RC_ASSERT(nalTypes.size() >= 3);
    RC_ASSERT(nalTypes[0] == kNalTypeSPS);
    RC_ASSERT(nalTypes[1] == kNalTypePPS);
    RC_ASSERT(nalTypes[2] == kNalTypeIDR);
}
