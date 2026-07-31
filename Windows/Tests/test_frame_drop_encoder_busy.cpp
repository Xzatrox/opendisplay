// Property 3: Frame Drop on Encoder Busy
// **Validates: Requirements 2.6, 3.6**
//
// For any interleaving of frame-capture arrivals and encode-completion events,
// a new capture SHALL be submitted to the encoder only when pendingEncodes < 1,
// and SHALL be dropped (without forcing a keyframe on the subsequent frame) when
// pendingEncodes >= 1. The H.264 reference chain remains valid after a drop
// (next frame is a normal P-frame, no forced keyframe).
//
// This test models the MFTEncoder's SubmitFrame/GetOutput pendingEncodes logic
// without requiring actual Media Foundation dependencies.

#include <gtest/gtest.h>
#include <rapidcheck.h>
#include <rapidcheck/gtest.h>

#include <cstdint>
#include <vector>
#include <atomic>
#include <algorithm>

namespace {

// ─── Event types for the interleaving model ──────────────────────────────────
enum class EventType : uint8_t {
    CaptureArrival,    // A new frame captured from DXGI Desktop Duplication
    EncodeComplete,    // Encoder finishes processing a frame (GetOutput succeeds)
};

// ─── Model of the MFTEncoder's frame drop logic ──────────────────────────────
// Mirrors the behavior in MFTEncoder.cpp:
//   - SubmitFrame: if pendingEncodes >= 1, return MF_E_NOTACCEPTING (drop)
//   - SubmitFrame: if pendingEncodes < 1, accept and increment pendingEncodes
//   - GetOutput: on success, decrement pendingEncodes
//   - After a drop, the next frame submitted is a normal P-frame (no forced IDR)
//   - m_forceKeyframe is only set via RequestKeyframe() or on first frame

class EncoderFrameDropModel {
public:
    struct FrameResult {
        bool submitted;       // true if frame was accepted by encoder
        bool dropped;         // true if frame was dropped (encoder busy)
        bool wasKeyframe;     // true if this submitted frame would be encoded as IDR
    };

    /// Submit a captured frame to the encoder.
    /// Returns whether the frame was submitted or dropped, and whether it's a keyframe.
    FrameResult SubmitFrame() {
        FrameResult result{};

        if (m_pendingEncodes >= 1) {
            // Encoder busy — drop the frame
            result.submitted = false;
            result.dropped = true;
            result.wasKeyframe = false;
            m_lastWasDropped = true;
            m_totalDropped++;
            return result;
        }

        // Encoder available — accept the frame
        result.submitted = true;
        result.dropped = false;

        // Determine if this frame is a keyframe
        if (m_forceKeyframe) {
            result.wasKeyframe = true;
            m_forceKeyframe = false;
        } else {
            // After a drop, the next submitted frame is a normal P-frame
            // (no forced keyframe). This is the key property being tested.
            result.wasKeyframe = false;
        }

        m_pendingEncodes++;
        m_lastWasDropped = false;
        m_totalSubmitted++;
        m_submissionLog.push_back(result);
        return result;
    }

    /// Simulate encode completion (GetOutput success).
    /// Returns true if there was a pending encode to complete.
    bool CompleteEncode() {
        if (m_pendingEncodes <= 0) {
            return false; // No pending encode to complete
        }
        m_pendingEncodes--;
        m_totalCompleted++;
        return true;
    }

    /// Request a keyframe (simulates receiving a "kf" control message).
    void RequestKeyframe() {
        m_forceKeyframe = true;
    }

    // Accessors for verification
    int pendingEncodes() const { return m_pendingEncodes; }
    bool lastWasDropped() const { return m_lastWasDropped; }
    int totalSubmitted() const { return m_totalSubmitted; }
    int totalDropped() const { return m_totalDropped; }
    int totalCompleted() const { return m_totalCompleted; }
    const std::vector<FrameResult>& submissionLog() const { return m_submissionLog; }

private:
    int m_pendingEncodes = 0;
    bool m_forceKeyframe = true;   // First frame is always IDR
    bool m_lastWasDropped = false;
    int m_totalSubmitted = 0;
    int m_totalDropped = 0;
    int m_totalCompleted = 0;
    std::vector<FrameResult> m_submissionLog;
};

// ─── Oracle: independently verify the frame drop invariants ──────────────────

struct OracleState {
    int pendingEncodes = 0;
    bool forceKeyframe = true;  // First frame is always IDR
};

struct OracleResult {
    bool shouldSubmit;    // Frame should be accepted
    bool shouldDrop;      // Frame should be dropped
    bool shouldBeKeyframe; // If submitted, should it be a keyframe
};

OracleResult OracleSubmitFrame(OracleState& state) {
    OracleResult result{};

    if (state.pendingEncodes >= 1) {
        result.shouldSubmit = false;
        result.shouldDrop = true;
        result.shouldBeKeyframe = false;
        return result;
    }

    result.shouldSubmit = true;
    result.shouldDrop = false;

    if (state.forceKeyframe) {
        result.shouldBeKeyframe = true;
        state.forceKeyframe = false;
    } else {
        result.shouldBeKeyframe = false;
    }

    state.pendingEncodes++;
    return result;
}

bool OracleCompleteEncode(OracleState& state) {
    if (state.pendingEncodes <= 0) return false;
    state.pendingEncodes--;
    return true;
}

} // namespace

// ─── Property Tests ──────────────────────────────────────────────────────────

// **Validates: Requirements 2.6, 3.6**
// Property: Frames are submitted only when pendingEncodes < 1, dropped otherwise.
// The model and oracle must agree on every event outcome.
RC_GTEST_PROP(FrameDropEncoderBusy,
              FramesSubmittedOnlyWhenEncoderFree,
              ()) {
    // Generate a random interleaving of capture arrivals and encode completions
    auto numEvents = *rc::gen::inRange<int>(1, 101);

    EncoderFrameDropModel model;
    OracleState oracle{};

    for (int i = 0; i < numEvents; ++i) {
        // Randomly choose event type: capture arrival or encode completion
        // Bias slightly towards capture arrivals (60/40) to stress the drop path
        auto eventChoice = *rc::gen::inRange<int>(0, 10);
        EventType event = (eventChoice < 6) ? EventType::CaptureArrival
                                            : EventType::EncodeComplete;

        if (event == EventType::CaptureArrival) {
            auto modelResult = model.SubmitFrame();
            auto oracleResult = OracleSubmitFrame(oracle);

            // Model and oracle must agree
            RC_ASSERT(modelResult.submitted == oracleResult.shouldSubmit);
            RC_ASSERT(modelResult.dropped == oracleResult.shouldDrop);
            RC_ASSERT(modelResult.wasKeyframe == oracleResult.shouldBeKeyframe);

            // Core invariant: submitted iff pendingEncodes was < 1 before this call
            // (We can't directly check "before" but the model tracks it)
            if (modelResult.submitted) {
                RC_ASSERT(!modelResult.dropped);
            } else {
                RC_ASSERT(modelResult.dropped);
            }
        } else {
            // Encode completion
            bool modelCompleted = model.CompleteEncode();
            bool oracleCompleted = OracleCompleteEncode(oracle);
            RC_ASSERT(modelCompleted == oracleCompleted);
        }

        // Invariant: pendingEncodes must always be in [0, 1]
        RC_ASSERT(model.pendingEncodes() >= 0);
        RC_ASSERT(model.pendingEncodes() <= 1);
        RC_ASSERT(oracle.pendingEncodes >= 0);
        RC_ASSERT(oracle.pendingEncodes <= 1);
    }
}

// **Validates: Requirements 2.6, 3.6**
// Property: After a frame drop, the next submitted frame is a normal P-frame
// (no forced keyframe). Dropping a frame does not corrupt the H.264 reference chain.
RC_GTEST_PROP(FrameDropEncoderBusy,
              NoForcedKeyframeAfterDrop,
              ()) {
    // Generate a sequence that guarantees at least one drop followed by a submission:
    // 1. Submit a frame (accepted, encoder becomes busy)
    // 2. Try to submit another frame (dropped, encoder busy)
    // 3. Complete the encode (encoder becomes free)
    // 4. Submit another frame (accepted) — this must NOT be a keyframe

    EncoderFrameDropModel model;

    // First frame is always IDR (initial keyframe)
    auto firstResult = model.SubmitFrame();
    RC_ASSERT(firstResult.submitted);
    RC_ASSERT(firstResult.wasKeyframe); // First frame is always IDR

    // Generate some number of capture arrivals while encoder is busy (all should drop)
    auto numDrops = *rc::gen::inRange<int>(1, 10);
    for (int i = 0; i < numDrops; ++i) {
        auto dropResult = model.SubmitFrame();
        RC_ASSERT(dropResult.dropped);
        RC_ASSERT(!dropResult.submitted);
        RC_ASSERT(!dropResult.wasKeyframe);
    }

    // Complete the encode (free the encoder)
    bool completed = model.CompleteEncode();
    RC_ASSERT(completed);

    // Now submit the next frame — it must be a P-frame, NOT a keyframe
    auto afterDropResult = model.SubmitFrame();
    RC_ASSERT(afterDropResult.submitted);
    RC_ASSERT(!afterDropResult.dropped);
    RC_ASSERT(!afterDropResult.wasKeyframe); // Key property: no forced keyframe after drop
}

// **Validates: Requirements 2.6, 3.6**
// Property: The total number of submitted + dropped frames equals total captures,
// and completed encodes never exceeds submitted frames.
RC_GTEST_PROP(FrameDropEncoderBusy,
              AccountingInvariantsHold,
              ()) {
    auto numEvents = *rc::gen::inRange<int>(1, 200);

    EncoderFrameDropModel model;
    int totalCaptures = 0;

    for (int i = 0; i < numEvents; ++i) {
        auto eventChoice = *rc::gen::inRange<int>(0, 10);

        if (eventChoice < 6) {
            // Capture arrival
            model.SubmitFrame();
            totalCaptures++;
        } else {
            // Encode completion
            model.CompleteEncode();
        }
    }

    // Accounting: submitted + dropped == total capture arrivals
    RC_ASSERT(model.totalSubmitted() + model.totalDropped() == totalCaptures);

    // Completed encodes cannot exceed submitted frames
    RC_ASSERT(model.totalCompleted() <= model.totalSubmitted());

    // Pending encodes is the difference between submitted and completed
    RC_ASSERT(model.pendingEncodes() == model.totalSubmitted() - model.totalCompleted());
}

// **Validates: Requirements 2.6, 3.6**
// Property: pendingEncodes is always exactly 0 or 1 (never negative, never > 1).
// This validates the frame drop threshold of 1.
RC_GTEST_PROP(FrameDropEncoderBusy,
              PendingEncodesNeverExceedsOne,
              ()) {
    auto numEvents = *rc::gen::inRange<int>(1, 300);

    EncoderFrameDropModel model;

    for (int i = 0; i < numEvents; ++i) {
        // Use weighted random for varied interleavings
        auto eventChoice = *rc::gen::inRange<int>(0, 100);

        if (eventChoice < 70) {
            // Heavy capture pressure — tests that drops keep pendingEncodes bounded
            model.SubmitFrame();
        } else {
            model.CompleteEncode();
        }

        // Core invariant: pendingEncodes in [0, 1]
        RC_ASSERT(model.pendingEncodes() >= 0);
        RC_ASSERT(model.pendingEncodes() <= 1);
    }
}

// **Validates: Requirements 2.6, 3.6**
// Property: A keyframe request (RequestKeyframe) only affects the next SUBMITTED
// frame, not a dropped frame. If frames are dropped after RequestKeyframe, the
// keyframe must still be delivered on the next successfully submitted frame.
RC_GTEST_PROP(FrameDropEncoderBusy,
              KeyframeRequestSurvivedDrops,
              ()) {
    EncoderFrameDropModel model;

    // Submit and complete the first frame (IDR)
    auto first = model.SubmitFrame();
    RC_ASSERT(first.submitted && first.wasKeyframe);
    model.CompleteEncode();

    // Submit a frame (P-frame, encoder now busy)
    auto second = model.SubmitFrame();
    RC_ASSERT(second.submitted && !second.wasKeyframe);

    // Request a keyframe while encoder is busy
    model.RequestKeyframe();

    // Generate some capture arrivals that should all be dropped (encoder busy)
    auto numDrops = *rc::gen::inRange<int>(0, 5);
    for (int i = 0; i < numDrops; ++i) {
        auto dropResult = model.SubmitFrame();
        RC_ASSERT(dropResult.dropped);
        // Dropped frames don't consume the keyframe request
        RC_ASSERT(!dropResult.wasKeyframe);
    }

    // Complete the pending encode
    model.CompleteEncode();

    // Next submitted frame should be the keyframe (request was preserved through drops)
    auto keyframeResult = model.SubmitFrame();
    RC_ASSERT(keyframeResult.submitted);
    RC_ASSERT(keyframeResult.wasKeyframe); // Keyframe request fulfilled on next submission
}

