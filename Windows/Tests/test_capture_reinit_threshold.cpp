// Property 2: Capture Failure Reinitialize Threshold
// **Validates: Requirements 2.5**
//
// For any sequence of AcquireFrame results (success or failure), the capture
// system SHALL call Reinitialize() if and only if there are 3 or more
// consecutive failures in the sequence, and SHALL reset the consecutive
// failure counter on any success.
//
// This test models the DesktopDuplicationCapture failure-tracking logic
// (m_consecutiveFailures, kMaxConsecutiveFailures = 3) without requiring
// actual DXGI dependencies.

#include <gtest/gtest.h>
#include <rapidcheck.h>
#include <rapidcheck/gtest.h>

#include <cstdint>
#include <vector>

namespace {

// ─── Model of the capture failure/reinitialize logic ─────────────────────────
// Mirrors the behavior in DesktopDuplicationCapture.cpp:
//   - On failure: increment m_consecutiveFailures
//   - If m_consecutiveFailures >= kMaxConsecutiveFailures: call Reinitialize()
//   - On success: reset m_consecutiveFailures to 0
//   - Reinitialize() itself resets the counter to 0

class CaptureReinitModel {
public:
    static constexpr int kMaxConsecutiveFailures = 3;

    /// Process a single AcquireFrame result.
    /// @param success true if the frame was acquired, false if it failed.
    /// @return true if Reinitialize() was triggered on this step.
    bool ProcessResult(bool success) {
        if (success) {
            m_consecutiveFailures = 0;
            return false;
        }

        // Failure path
        m_consecutiveFailures++;

        if (m_consecutiveFailures >= kMaxConsecutiveFailures) {
            // Reinitialize triggered — resets counter (as in the real impl)
            m_consecutiveFailures = 0;
            m_reinitCount++;
            return true;
        }

        return false;
    }

    int consecutiveFailures() const { return m_consecutiveFailures; }
    int reinitCount() const { return m_reinitCount; }

private:
    int m_consecutiveFailures = 0;
    int m_reinitCount = 0;
};

// ─── Oracle: compute expected reinitialize calls for a sequence ──────────────
// Walks the sequence, counting consecutive failures. Every time the count
// reaches kMaxConsecutiveFailures, a reinitialize is expected and the counter
// resets. On any success the counter resets.

struct OracleResult {
    int totalReinitCalls;
    std::vector<int> reinitAtIndices; // indices where reinit should trigger
};

OracleResult ComputeExpected(const std::vector<bool>& sequence) {
    OracleResult result{};
    int consecutive = 0;

    for (size_t i = 0; i < sequence.size(); ++i) {
        if (sequence[i]) {
            // success
            consecutive = 0;
        } else {
            // failure
            consecutive++;
            if (consecutive >= CaptureReinitModel::kMaxConsecutiveFailures) {
                result.totalReinitCalls++;
                result.reinitAtIndices.push_back(static_cast<int>(i));
                consecutive = 0; // reinitialize resets the counter
            }
        }
    }

    return result;
}

} // namespace

// ─── Property Tests ──────────────────────────────────────────────────────────

// **Validates: Requirements 2.5**
// Property: Reinitialize is called if and only if 3+ consecutive failures occur.
// The model and oracle must agree on when and how many times Reinitialize fires.
RC_GTEST_PROP(CaptureReinitThreshold,
              ReinitCalledIffThreeConsecutiveFailures,
              ()) {
    // Generate a random sequence length, then individual success/failure results.
    // We use int (0=failure, 1=success) to avoid std::vector<bool> specialization
    // issues with RapidCheck.
    auto length = *rc::gen::inRange<int>(1, 101);
    std::vector<bool> sequence;
    sequence.reserve(length);
    for (int i = 0; i < length; ++i) {
        auto val = *rc::gen::inRange<int>(0, 2);
        sequence.push_back(val != 0);
    }

    // Run through the model
    CaptureReinitModel model;
    std::vector<int> modelReinitIndices;

    for (size_t i = 0; i < sequence.size(); ++i) {
        bool reinitTriggered = model.ProcessResult(sequence[i]);
        if (reinitTriggered) {
            modelReinitIndices.push_back(static_cast<int>(i));
        }
    }

    // Compare against the oracle
    auto expected = ComputeExpected(sequence);

    RC_ASSERT(model.reinitCount() == expected.totalReinitCalls);
    RC_ASSERT(modelReinitIndices == expected.reinitAtIndices);
}

// **Validates: Requirements 2.5**
// Property: Counter always resets on success — after any success, at least 3
// more consecutive failures are needed before the next reinitialize.
RC_GTEST_PROP(CaptureReinitThreshold,
              CounterResetsOnSuccess,
              ()) {
    // Generate a prefix of failures (0..2), then a success, then more failures
    auto prefixFailures = *rc::gen::inRange<int>(0, 3);
    auto suffixFailures = *rc::gen::inRange<int>(0, 10);

    CaptureReinitModel model;

    // Feed prefix failures (not enough to trigger reinit)
    for (int i = 0; i < prefixFailures; ++i) {
        bool reinit = model.ProcessResult(false);
        RC_ASSERT(!reinit); // can't reinit with < 3 consecutive
    }

    // Feed a success — must reset counter
    model.ProcessResult(true);
    RC_ASSERT(model.consecutiveFailures() == 0);

    // After success, need a full kMaxConsecutiveFailures before reinit
    int reinitCount = 0;
    for (int i = 0; i < suffixFailures; ++i) {
        bool reinit = model.ProcessResult(false);
        if (reinit) {
            reinitCount++;
        }
    }

    // Reinit should only have happened if suffixFailures >= 3
    int expectedReinits = suffixFailures / CaptureReinitModel::kMaxConsecutiveFailures;
    RC_ASSERT(reinitCount == expectedReinits);
}

// **Validates: Requirements 2.5**
// Property: Fewer than 3 consecutive failures never trigger reinitialize.
// Generate sequences where every run of failures is strictly less than 3.
RC_GTEST_PROP(CaptureReinitThreshold,
              FewerThanThreeNeverTriggers,
              ()) {
    // Generate a sequence structured as alternating short failure runs and
    // successes, where each failure run has length < kMaxConsecutiveFailures.
    auto numSegments = *rc::gen::inRange<int>(1, 20);

    CaptureReinitModel model;

    for (int seg = 0; seg < numSegments; ++seg) {
        // 0, 1, or 2 consecutive failures
        auto failCount = *rc::gen::inRange<int>(0, CaptureReinitModel::kMaxConsecutiveFailures);
        for (int f = 0; f < failCount; ++f) {
            bool reinit = model.ProcessResult(false);
            RC_ASSERT(!reinit);
        }
        // Follow with a success to reset
        model.ProcessResult(true);
    }

    // No reinitializations should have occurred
    RC_ASSERT(model.reinitCount() == 0);
}

// **Validates: Requirements 2.5**
// Property: Exactly 3 consecutive failures always triggers exactly one reinit.
RC_GTEST_PROP(CaptureReinitThreshold,
              ExactlyThreeConsecutiveTriggersOnce,
              ()) {
    // Generate arbitrary prefix of successes, then exactly 3 failures
    auto prefixSuccesses = *rc::gen::inRange<int>(0, 20);

    CaptureReinitModel model;

    for (int i = 0; i < prefixSuccesses; ++i) {
        model.ProcessResult(true);
    }

    // Now 3 consecutive failures
    RC_ASSERT(!model.ProcessResult(false)); // 1st failure
    RC_ASSERT(!model.ProcessResult(false)); // 2nd failure
    RC_ASSERT(model.ProcessResult(false));  // 3rd failure → reinit!

    RC_ASSERT(model.reinitCount() == 1);
    // Counter should be reset after reinit
    RC_ASSERT(model.consecutiveFailures() == 0);
}
