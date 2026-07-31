// OpenDisplay Tests — Property Test: Display Resolution Computation
// **Validates: Requirements 1.2, 1.6, 1.8**
//
// Property 1: Display Resolution Computation
// For any receiver panel dimensions (pixelsWide, pixelsHigh) where both are
// positive integers, the computed virtual display resolution SHALL equal
// (floor(pixelsWide / 2) rounded down to the nearest even number,
//  floor(pixelsHigh / 2) rounded down to the nearest even number),
// AND the result SHALL be accepted only if both axes fall within
// [640, 2732] × [480, 2048], AND zero or negative dimensions SHALL always
// be rejected.

#include <gtest/gtest.h>
#include <rapidcheck.h>
#include <rapidcheck/gtest.h>

#include <cstdint>

// ─── Standalone Resolution Computation (mirrors Driver/src/Monitor.cpp) ──────
//
// We cannot link the driver into the test target, so we replicate the
// computation logic here. The driver's ValidateResolution() validates
// pre-computed values; this function performs the full pipeline:
//   1. Reject zero inputs
//   2. Compute floor(dim / 2) rounded down to nearest even: (dim / 2) & ~1
//   3. Validate result is in [640, 2732] × [480, 2048]

namespace resolution {

static constexpr uint32_t kMinWidth = 640;
static constexpr uint32_t kMaxWidth = 2732;
static constexpr uint32_t kMinHeight = 480;
static constexpr uint32_t kMaxHeight = 2048;

struct ComputeResult {
    uint32_t virtualWidth;
    uint32_t virtualHeight;
    bool accepted;
};

/// Compute the virtual display resolution from receiver panel dimensions.
/// Returns the computed dimensions and whether they pass validation.
inline ComputeResult ComputeAndValidateResolution(uint32_t pixelsWide, uint32_t pixelsHigh)
{
    ComputeResult result{};

    // Step 1: Reject zero inputs
    if (pixelsWide == 0 || pixelsHigh == 0) {
        result.virtualWidth = 0;
        result.virtualHeight = 0;
        result.accepted = false;
        return result;
    }

    // Step 2: Compute floor(dim / 2) rounded down to nearest even
    // Integer division already floors. Then mask off the lowest bit to
    // round down to even: (dim / 2) & ~1u
    result.virtualWidth = (pixelsWide / 2) & ~1u;
    result.virtualHeight = (pixelsHigh / 2) & ~1u;

    // Step 3: Validate result is in [640, 2732] × [480, 2048]
    result.accepted =
        (result.virtualWidth >= kMinWidth) &&
        (result.virtualWidth <= kMaxWidth) &&
        (result.virtualHeight >= kMinHeight) &&
        (result.virtualHeight <= kMaxHeight);

    return result;
}

} // namespace resolution

// ─── Property Tests ──────────────────────────────────────────────────────────

// Property 1: Display Resolution Computation
// For random (width, height) in [0, 5000], verify the computation is correct
// and acceptance/rejection matches the range constraints.
RC_GTEST_PROP(DisplayResolution, ComputationIsCorrect, ())
{
    // Generate random dimensions in [0, 5000]
    auto pixelsWide = *rc::gen::inRange<uint32_t>(0, 5001);
    auto pixelsHigh = *rc::gen::inRange<uint32_t>(0, 5001);

    auto result = resolution::ComputeAndValidateResolution(pixelsWide, pixelsHigh);

    if (pixelsWide == 0 || pixelsHigh == 0) {
        // Zero inputs must always be rejected
        RC_ASSERT(!result.accepted);
    } else {
        // Verify the computation: floor(dim / 2) rounded down to nearest even
        uint32_t expectedWidth = (pixelsWide / 2) & ~1u;
        uint32_t expectedHeight = (pixelsHigh / 2) & ~1u;

        RC_ASSERT(result.virtualWidth == expectedWidth);
        RC_ASSERT(result.virtualHeight == expectedHeight);

        // Verify acceptance matches range constraints
        bool inRange =
            (expectedWidth >= resolution::kMinWidth) &&
            (expectedWidth <= resolution::kMaxWidth) &&
            (expectedHeight >= resolution::kMinHeight) &&
            (expectedHeight <= resolution::kMaxHeight);

        RC_ASSERT(result.accepted == inRange);
    }
}

// Property 1b: Zero inputs always rejected
RC_GTEST_PROP(DisplayResolution, ZeroInputsAlwaysRejected, ())
{
    // Generate at least one zero dimension
    auto pixelsWide = *rc::gen::inRange<uint32_t>(0, 5001);
    auto pixelsHigh = *rc::gen::inRange<uint32_t>(0, 5001);

    // Force at least one to be zero
    auto choice = *rc::gen::inRange<int>(0, 3);
    if (choice == 0) {
        pixelsWide = 0;
    } else if (choice == 1) {
        pixelsHigh = 0;
    } else {
        pixelsWide = 0;
        pixelsHigh = 0;
    }

    auto result = resolution::ComputeAndValidateResolution(pixelsWide, pixelsHigh);
    RC_ASSERT(!result.accepted);
}

// Property 1c: The computation never produces odd numbers
RC_GTEST_PROP(DisplayResolution, NeverProducesOddNumbers, ())
{
    // Generate non-zero dimensions in [1, 5000]
    auto pixelsWide = *rc::gen::inRange<uint32_t>(1, 5001);
    auto pixelsHigh = *rc::gen::inRange<uint32_t>(1, 5001);

    auto result = resolution::ComputeAndValidateResolution(pixelsWide, pixelsHigh);

    // Result must always be even (or zero for rejected zero inputs)
    RC_ASSERT((result.virtualWidth % 2) == 0);
    RC_ASSERT((result.virtualHeight % 2) == 0);
}
