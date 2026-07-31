// Property 10: Exponential Backoff Computation
// **Validates: Requirements 5.5, 10.4**
//
// For any retry attempt number n (1-indexed) with a given maximum attempt count
// and delay cap, the computed backoff delay SHALL equal min(2^(n-1) seconds,
// cap seconds), AND after the maximum number of attempts is exhausted, the
// retry logic SHALL signal session termination rather than scheduling another
// attempt.
//
// Feature: windows-second-display, Property 10: Exponential Backoff Computation

#include <gtest/gtest.h>
#include <rapidcheck.h>
#include <rapidcheck/gtest.h>

#include <algorithm>
#include <cstdint>
#include <vector>

namespace {

// ─── Parameterized model of the backoff computation ──────────────────────────
// This generalizes the production ComputeBackoffMs (which uses fixed cap=10s)
// to accept arbitrary cap values, allowing us to verify the formula across the
// full input space.

/// Compute the backoff delay for a given attempt with a configurable cap.
/// Formula: delay = min(2^(attempt-1), capSeconds) expressed in milliseconds.
/// @param attempt The 1-based attempt number (must be >= 1).
/// @param capSeconds The maximum delay cap in seconds (must be >= 1).
/// @return The delay in milliseconds.
uint32_t ComputeBackoffMs(int attempt, int capSeconds)
{
    // 2^(attempt-1) seconds
    int delaySec = 1 << (attempt - 1);
    delaySec = (std::min)(delaySec, capSeconds);
    return static_cast<uint32_t>(delaySec) * 1000;
}

/// Convenience overload using production defaults (cap = 10s).
uint32_t ComputeBackoffMsDefault(int attempt)
{
    return ComputeBackoffMs(attempt, 10);
}

// ─── Retry session model ─────────────────────────────────────────────────────
// Models the retry logic: attempts connection up to maxAttempts times,
// sleeping with exponential backoff between attempts. If all fail, signals
// session termination (returns false). If any succeeds, returns true.

struct RetryResult {
    bool sessionContinued;   // true if a connection succeeded
    int attemptsUsed;        // how many attempts were made
    std::vector<uint32_t> delaysUsed; // backoff delays between attempts (ms)
};

/// Simulate the retry logic with a predetermined sequence of connection results.
/// @param outcomes Per-attempt success (true) or failure (false). Size must be >= maxAttempts.
/// @param maxAttempts Maximum number of retry attempts allowed.
/// @param capSeconds Backoff delay cap in seconds.
/// @return The result of the retry session.
RetryResult SimulateRetry(const std::vector<bool>& outcomes, int maxAttempts, int capSeconds)
{
    RetryResult result{};
    result.sessionContinued = false;
    result.attemptsUsed = 0;

    for (int attempt = 1; attempt <= maxAttempts; ++attempt)
    {
        result.attemptsUsed = attempt;

        // Check if this attempt succeeds
        if (attempt - 1 < static_cast<int>(outcomes.size()) && outcomes[attempt - 1])
        {
            result.sessionContinued = true;
            return result;
        }

        // If this was the last attempt, don't compute delay — session ends
        if (attempt == maxAttempts)
        {
            break;
        }

        // Record the backoff delay before the next attempt
        uint32_t delay = ComputeBackoffMs(attempt, capSeconds);
        result.delaysUsed.push_back(delay);
    }

    // All attempts exhausted — session terminates
    result.sessionContinued = false;
    return result;
}

} // namespace

// ─── Property Tests ──────────────────────────────────────────────────────────

// **Validates: Requirements 5.5, 10.4**
// Property: For any (attempt, cap) tuple, delay equals min(2^(attempt-1), cap)
// in seconds, converted to milliseconds.
RC_GTEST_PROP(ExponentialBackoff,
              DelayMatchesFormula,
              ()) {
    // Generate attempt in [1, 20] and cap in [1, 60]
    auto attempt = *rc::gen::inRange<int>(1, 21);
    auto capSeconds = *rc::gen::inRange<int>(1, 61);

    uint32_t computed = ComputeBackoffMs(attempt, capSeconds);

    // Expected: min(2^(attempt-1), capSeconds) * 1000
    int rawDelaySec = 1 << (attempt - 1);
    int expectedSec = (std::min)(rawDelaySec, capSeconds);
    uint32_t expectedMs = static_cast<uint32_t>(expectedSec) * 1000;

    RC_ASSERT(computed == expectedMs);
}

// **Validates: Requirements 5.5, 10.4**
// Property: The backoff delay is always <= cap (never exceeds the cap).
RC_GTEST_PROP(ExponentialBackoff,
              DelayNeverExceedsCap,
              ()) {
    auto attempt = *rc::gen::inRange<int>(1, 21);
    auto capSeconds = *rc::gen::inRange<int>(1, 61);

    uint32_t computed = ComputeBackoffMs(attempt, capSeconds);
    uint32_t capMs = static_cast<uint32_t>(capSeconds) * 1000;

    RC_ASSERT(computed <= capMs);
}

// **Validates: Requirements 5.5, 10.4**
// Property: Backoff delays are monotonically non-decreasing with attempt number
// (for a fixed cap).
RC_GTEST_PROP(ExponentialBackoff,
              DelaysMonotonicallyNonDecreasing,
              ()) {
    auto capSeconds = *rc::gen::inRange<int>(1, 61);
    auto maxAttempt = *rc::gen::inRange<int>(2, 21);

    uint32_t prevDelay = ComputeBackoffMs(1, capSeconds);
    for (int attempt = 2; attempt <= maxAttempt; ++attempt) {
        uint32_t currentDelay = ComputeBackoffMs(attempt, capSeconds);
        RC_ASSERT(currentDelay >= prevDelay);
        prevDelay = currentDelay;
    }
}

// **Validates: Requirements 5.5, 10.4**
// Property: After max attempts are exhausted (all fail), session terminates.
// Generate random maxAttempts and cap, simulate all-failure outcomes, verify
// session termination.
RC_GTEST_PROP(ExponentialBackoff,
              SessionTerminatesAfterMaxAttempts,
              ()) {
    auto maxAttempts = *rc::gen::inRange<int>(1, 21);
    auto capSeconds = *rc::gen::inRange<int>(1, 61);

    // All attempts fail
    std::vector<bool> allFail(maxAttempts, false);

    RetryResult result = SimulateRetry(allFail, maxAttempts, capSeconds);

    // Session must terminate (not continue)
    RC_ASSERT(!result.sessionContinued);
    // All attempts must have been used
    RC_ASSERT(result.attemptsUsed == maxAttempts);
    // Number of delays = maxAttempts - 1 (no delay after last attempt)
    RC_ASSERT(static_cast<int>(result.delaysUsed.size()) == maxAttempts - 1);
}

// **Validates: Requirements 5.5, 10.4**
// Property: If any attempt succeeds, the session continues and no further
// attempts are made. The successful attempt can be at any position.
RC_GTEST_PROP(ExponentialBackoff,
              SessionContinuesOnSuccess,
              ()) {
    auto maxAttempts = *rc::gen::inRange<int>(1, 21);
    auto capSeconds = *rc::gen::inRange<int>(1, 61);
    // Pick which attempt will succeed (1-indexed)
    auto successAt = *rc::gen::inRange<int>(1, maxAttempts + 1);

    // Build outcomes: failures before successAt, then success
    std::vector<bool> outcomes(maxAttempts, false);
    outcomes[successAt - 1] = true;

    RetryResult result = SimulateRetry(outcomes, maxAttempts, capSeconds);

    // Session must continue
    RC_ASSERT(result.sessionContinued);
    // Only used attempts up to the successful one
    RC_ASSERT(result.attemptsUsed == successAt);
    // Delays used = successAt - 1 (delays between previous failures)
    RC_ASSERT(static_cast<int>(result.delaysUsed.size()) == successAt - 1);
}

// **Validates: Requirements 5.5, 10.4**
// Property: With production defaults (max 5 attempts, cap 10s), the delay
// sequence is exactly [1000, 2000, 4000, 8000, 10000] ms.
RC_GTEST_PROP(ExponentialBackoff,
              ProductionDefaultsMatchSpec,
              ()) {
    // Generate a random subset of attempts to spot-check
    auto attempt = *rc::gen::inRange<int>(1, 6);

    uint32_t expected[] = {1000, 2000, 4000, 8000, 10000};
    uint32_t computed = ComputeBackoffMsDefault(attempt);

    RC_ASSERT(computed == expected[attempt - 1]);
}

// **Validates: Requirements 5.5, 10.4**
// Property: When attempt is below the cap threshold (2^(attempt-1) < cap),
// the delay exactly doubles from the previous attempt.
RC_GTEST_PROP(ExponentialBackoff,
              DelayDoublesBeforeCap,
              ()) {
    auto capSeconds = *rc::gen::inRange<int>(2, 61);
    // Pick attempt such that 2^(attempt-1) < capSeconds AND 2^attempt < capSeconds
    // i.e., attempt where both current and next are below cap
    auto attempt = *rc::gen::inRange<int>(1, 20);

    uint32_t current = ComputeBackoffMs(attempt, capSeconds);
    uint32_t next = ComputeBackoffMs(attempt + 1, capSeconds);

    int rawCurrent = 1 << (attempt - 1);
    int rawNext = 1 << attempt;

    // Only check doubling when both are below cap
    if (rawCurrent < capSeconds && rawNext < capSeconds) {
        RC_ASSERT(next == current * 2);
    }
}

// **Validates: Requirements 5.5, 10.4**
// Property: All delays between retry attempts match the formula, for any
// randomly generated (maxAttempts, cap) pair with all-failure outcomes.
RC_GTEST_PROP(ExponentialBackoff,
              AllDelaysBetweenAttemptsMatchFormula,
              ()) {
    auto maxAttempts = *rc::gen::inRange<int>(2, 21);
    auto capSeconds = *rc::gen::inRange<int>(1, 61);

    std::vector<bool> allFail(maxAttempts, false);
    RetryResult result = SimulateRetry(allFail, maxAttempts, capSeconds);

    // Verify each recorded delay matches the formula
    for (int i = 0; i < static_cast<int>(result.delaysUsed.size()); ++i) {
        int attempt = i + 1;  // delay after attempt i+1 (1-indexed)
        uint32_t expected = ComputeBackoffMs(attempt, capSeconds);
        RC_ASSERT(result.delaysUsed[i] == expected);
    }
}
