// Property 15: Mac Receiver Input Normalization
// Validates: Requirements 8.4
//
// The Mac receiver normalizes mouse coordinates as:
//   rawX = mouseX / viewWidth
//   rawY = 1.0 - (mouseY / viewHeight)  [y-flipped for video space]
//   result_x = clamp(rawX, 0.0, 1.0)
//   result_y = clamp(rawY, 0.0, 1.0)

#include <gtest/gtest.h>
#include <rapidcheck.h>
#include <rapidcheck/gtest.h>

#include <algorithm>
#include <cmath>

namespace {

// Implements the Mac receiver's input normalization algorithm.
// macOS origin is bottom-left; video space is top-left, so y is flipped.
struct NormalizedCoord {
    double x;
    double y;
};

inline double clamp01(double v) {
    return std::min(1.0, std::max(0.0, v));
}

inline NormalizedCoord normalizeInput(double mouseX, double mouseY,
                                      double viewWidth, double viewHeight) {
    double rawX = mouseX / viewWidth;
    double rawY = 1.0 - (mouseY / viewHeight);
    return {clamp01(rawX), clamp01(rawY)};
}

} // namespace

// **Validates: Requirements 8.4**
RC_GTEST_PROP(MacReceiverInputNormalization,
              NormalizedCoordsAlwaysInUnitRange,
              ()) {
    // Generate mouseX in [-100, 2000] and mouseY in [-100, 2000]
    auto mouseX = *rc::gen::inRange(-100, 2001);
    auto mouseY = *rc::gen::inRange(-100, 2001);
    // Generate viewWidth in [1, 5000] and viewHeight in [1, 5000]
    auto viewWidth = *rc::gen::inRange(1, 5001);
    auto viewHeight = *rc::gen::inRange(1, 5001);

    auto result = normalizeInput(static_cast<double>(mouseX),
                                 static_cast<double>(mouseY),
                                 static_cast<double>(viewWidth),
                                 static_cast<double>(viewHeight));

    // Result must always be clamped to [0, 1]
    RC_ASSERT(result.x >= 0.0);
    RC_ASSERT(result.x <= 1.0);
    RC_ASSERT(result.y >= 0.0);
    RC_ASSERT(result.y <= 1.0);
}

// **Validates: Requirements 8.4**
RC_GTEST_PROP(MacReceiverInputNormalization,
              InRangeInputsNotClamped,
              ()) {
    // Generate view dimensions in [1, 5000]
    auto viewWidth = *rc::gen::inRange(1, 5001);
    auto viewHeight = *rc::gen::inRange(1, 5001);
    // Generate mouseX in [0, viewWidth] and mouseY in [0, viewHeight]
    auto mouseX = *rc::gen::inRange(0, viewWidth + 1);
    auto mouseY = *rc::gen::inRange(0, viewHeight + 1);

    auto result = normalizeInput(static_cast<double>(mouseX),
                                 static_cast<double>(mouseY),
                                 static_cast<double>(viewWidth),
                                 static_cast<double>(viewHeight));

    double expectedX = static_cast<double>(mouseX) / static_cast<double>(viewWidth);
    double expectedY = 1.0 - (static_cast<double>(mouseY) / static_cast<double>(viewHeight));

    // When mouse is within bounds, no clamping should occur
    RC_ASSERT(std::abs(result.x - expectedX) < 1e-9);
    RC_ASSERT(std::abs(result.y - expectedY) < 1e-9);
}

// **Validates: Requirements 8.4**
RC_GTEST_PROP(MacReceiverInputNormalization,
              EdgeCaseOrigin,
              ()) {
    // Generate arbitrary view dimensions
    auto viewWidth = *rc::gen::inRange(1, 5001);
    auto viewHeight = *rc::gen::inRange(1, 5001);

    // mousePos at (0, 0) → (0.0, 1.0)
    // rawX = 0/viewWidth = 0.0
    // rawY = 1.0 - (0/viewHeight) = 1.0
    auto result = normalizeInput(0.0, 0.0,
                                 static_cast<double>(viewWidth),
                                 static_cast<double>(viewHeight));

    RC_ASSERT(std::abs(result.x - 0.0) < 1e-9);
    RC_ASSERT(std::abs(result.y - 1.0) < 1e-9);
}

// **Validates: Requirements 8.4**
RC_GTEST_PROP(MacReceiverInputNormalization,
              EdgeCaseMaxBounds,
              ()) {
    // Generate arbitrary view dimensions
    auto viewWidth = *rc::gen::inRange(1, 5001);
    auto viewHeight = *rc::gen::inRange(1, 5001);

    // mousePos at (viewWidth, viewHeight) → (1.0, 0.0)
    // rawX = viewWidth/viewWidth = 1.0
    // rawY = 1.0 - (viewHeight/viewHeight) = 0.0
    auto result = normalizeInput(static_cast<double>(viewWidth),
                                 static_cast<double>(viewHeight),
                                 static_cast<double>(viewWidth),
                                 static_cast<double>(viewHeight));

    RC_ASSERT(std::abs(result.x - 1.0) < 1e-9);
    RC_ASSERT(std::abs(result.y - 0.0) < 1e-9);
}
