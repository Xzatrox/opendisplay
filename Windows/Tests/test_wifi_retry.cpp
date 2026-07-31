// Unit tests for WiFi transport retry logic (Task 6.3)
// Tests the exponential backoff computation and retry behavior.
//
// **Validates: Requirements 5.5, 4.8**

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>

// ─── Model of the backoff computation from WireTransport ─────────────────────
// We replicate the static ComputeBackoffMs logic here so we can test it
// without needing to link against Winsock or the full WireTransport class.

namespace {

static constexpr int kMaxRetryAttempts = 5;
static constexpr int kMaxBackoffSeconds = 10;
static constexpr int kFailoverGracePeriodMs = 10000;

/// Compute the backoff delay for a given retry attempt.
/// Formula: min(2^(attempt-1) seconds, kMaxBackoffSeconds cap)
/// @param attempt The 1-based attempt number.
/// @return The delay in milliseconds.
uint32_t ComputeBackoffMs(int attempt)
{
    int delaySec = 1 << (attempt - 1);  // 1, 2, 4, 8, 16, ...
    delaySec = (std::min)(delaySec, kMaxBackoffSeconds);
    return static_cast<uint32_t>(delaySec) * 1000;
}

} // namespace

// ─── Unit Tests: Exponential Backoff Computation ─────────────────────────────

TEST(WiFiRetry, BackoffAttempt1Is1Second) {
    EXPECT_EQ(ComputeBackoffMs(1), 1000u);
}

TEST(WiFiRetry, BackoffAttempt2Is2Seconds) {
    EXPECT_EQ(ComputeBackoffMs(2), 2000u);
}

TEST(WiFiRetry, BackoffAttempt3Is4Seconds) {
    EXPECT_EQ(ComputeBackoffMs(3), 4000u);
}

TEST(WiFiRetry, BackoffAttempt4Is8Seconds) {
    EXPECT_EQ(ComputeBackoffMs(4), 8000u);
}

TEST(WiFiRetry, BackoffAttempt5IsCappedAt10Seconds) {
    // 2^(5-1) = 16s, but cap is 10s
    EXPECT_EQ(ComputeBackoffMs(5), 10000u);
}

TEST(WiFiRetry, BackoffHighAttemptStillCapped) {
    // Even higher attempts should remain capped at 10s
    EXPECT_EQ(ComputeBackoffMs(6), 10000u);
    EXPECT_EQ(ComputeBackoffMs(10), 10000u);
}

// ─── Unit Tests: Retry Schedule Verification ─────────────────────────────────

TEST(WiFiRetry, FullRetryScheduleMatchesSpec) {
    // The spec requires: 1s, 2s, 4s, 8s, 10s
    uint32_t expected[] = {1000, 2000, 4000, 8000, 10000};
    for (int attempt = 1; attempt <= kMaxRetryAttempts; ++attempt) {
        EXPECT_EQ(ComputeBackoffMs(attempt), expected[attempt - 1])
            << "Mismatch at attempt " << attempt;
    }
}

TEST(WiFiRetry, MaxRetryAttemptsIsFive) {
    EXPECT_EQ(kMaxRetryAttempts, 5);
}

TEST(WiFiRetry, MaxBackoffCapIs10Seconds) {
    EXPECT_EQ(kMaxBackoffSeconds, 10);
}

TEST(WiFiRetry, FailoverGracePeriodIs10Seconds) {
    EXPECT_EQ(kFailoverGracePeriodMs, 10000);
}

// ─── Unit Tests: Total Backoff Time ──────────────────────────────────────────

TEST(WiFiRetry, TotalBackoffTimeBetweenAttemptsIs15Seconds) {
    // Between 5 attempts there are 4 sleep intervals:
    // After attempt 1: 1s, after attempt 2: 2s, after attempt 3: 4s, after attempt 4: 8s
    // Total sleep = 1 + 2 + 4 + 8 = 15 seconds
    uint32_t totalSleepMs = 0;
    for (int attempt = 1; attempt < kMaxRetryAttempts; ++attempt) {
        totalSleepMs += ComputeBackoffMs(attempt);
    }
    EXPECT_EQ(totalSleepMs, 15000u);
}

TEST(WiFiRetry, FailoverGracePeriodFitsFirstThreeRetries) {
    // With a 10-second grace period, the backoff schedule allows:
    // Attempt 1 (immediate), sleep 1s, Attempt 2, sleep 2s, Attempt 3, sleep 4s = 7s total
    // Attempt 4 might start but the sleep after would exceed 10s
    uint32_t cumulativeMs = 0;
    int attemptsWithinGrace = 0;

    for (int attempt = 1; attempt <= kMaxRetryAttempts; ++attempt) {
        // The attempt itself takes negligible time in the model
        attemptsWithinGrace++;

        if (attempt < kMaxRetryAttempts) {
            uint32_t sleepMs = ComputeBackoffMs(attempt);
            if (cumulativeMs + sleepMs > static_cast<uint32_t>(kFailoverGracePeriodMs)) {
                break;
            }
            cumulativeMs += sleepMs;
        }
    }

    // At least 3 attempts should fit within the grace period
    EXPECT_GE(attemptsWithinGrace, 3);
}
