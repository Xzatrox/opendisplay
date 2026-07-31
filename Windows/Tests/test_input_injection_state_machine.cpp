// Property 14: Input Injection Coordinate Mapping and State Machine
// **Validates: Requirements 6.4, 7.1, 7.2, 7.3, 7.4, 7.6, 7.7**
//
// For any random display bounds and touch message sequences with arbitrary
// coordinates and phases, the input injector SHALL:
//   1. Clamp normalized coordinates to [0, 1] before mapping
//   2. Discard "moved" events when no left button down is active
//   3. Transition to down state on "began", to up state on "ended"/"cancelled"
//   4. Map coordinates: screenX = displayLeft + (clamp(normX) * displayWidth)
//
// This test models the WindowsInputInjector state machine without requiring
// actual SendInput API calls. We verify the state transitions and coordinate
// mapping independently.

#include <gtest/gtest.h>
#include <rapidcheck.h>
#include <rapidcheck/gtest.h>

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace {

// ─── Coordinate clamping model ───────────────────────────────────────────────

inline double Clamp01(double v) {
    if (v < 0.0) return 0.0;
    if (v > 1.0) return 1.0;
    return v;
}

// ─── Display bounds structure ────────────────────────────────────────────────

struct DisplayBounds {
    int left;
    int top;
    int right;
    int bottom;

    int width() const { return right - left; }
    int height() const { return bottom - top; }
};

// ─── Coordinate mapping model ────────────────────────────────────────────────
// Maps normalized [0,1] to absolute screen position within display bounds:
//   screenX = displayLeft + (clamp(normX) * displayWidth)
//   screenY = displayTop  + (clamp(normY) * displayHeight)

struct ScreenPoint {
    double x;
    double y;
};

inline ScreenPoint NormalizedToScreen(double normX, double normY,
                                      const DisplayBounds& bounds) {
    double cX = Clamp01(normX);
    double cY = Clamp01(normY);
    double screenX = bounds.left + (cX * bounds.width());
    double screenY = bounds.top  + (cY * bounds.height());
    return {screenX, screenY};
}

// ─── Touch phase enum ────────────────────────────────────────────────────────

enum class TouchPhase {
    Began,
    Moved,
    Ended,
    Cancelled,
};

inline std::string PhaseToString(TouchPhase phase) {
    switch (phase) {
        case TouchPhase::Began:     return "began";
        case TouchPhase::Moved:     return "moved";
        case TouchPhase::Ended:     return "ended";
        case TouchPhase::Cancelled: return "cancelled";
    }
    return "unknown";
}

// ─── Touch event ─────────────────────────────────────────────────────────────

struct TouchEvent {
    TouchPhase phase;
    double normX;
    double normY;
};

// ─── Injected event types (what the model would send to SendInput) ───────────

enum class InjectedEventType {
    MouseDownAndMove,  // began: LEFTDOWN + MOVE
    MouseMove,         // moved with button down: MOVE (drag)
    MouseUp,           // ended/cancelled: LEFTUP
};

struct InjectedEvent {
    InjectedEventType type;
    ScreenPoint position;
};

// ─── State Machine Model ─────────────────────────────────────────────────────
// Models WindowsInputInjector's HandleTouch state machine.

class InputInjectorModel {
public:
    explicit InputInjectorModel(const DisplayBounds& bounds)
        : m_bounds(bounds), m_isDown(false), m_lastPoint{0, 0} {}

    /// Process a touch event. Returns true if an event was injected (not discarded).
    bool HandleTouch(const TouchEvent& event) {
        ScreenPoint pos = NormalizedToScreen(event.normX, event.normY, m_bounds);

        if (event.phase == TouchPhase::Began) {
            m_isDown = true;
            m_lastPoint = pos;
            m_injectedEvents.push_back({InjectedEventType::MouseDownAndMove, pos});
            return true;
        }
        else if (event.phase == TouchPhase::Moved) {
            if (!m_isDown) {
                // Discard — no button currently down
                m_discardedCount++;
                return false;
            }
            m_lastPoint = pos;
            m_injectedEvents.push_back({InjectedEventType::MouseMove, pos});
            return true;
        }
        else if (event.phase == TouchPhase::Ended || event.phase == TouchPhase::Cancelled) {
            m_injectedEvents.push_back({InjectedEventType::MouseUp, m_lastPoint});
            m_isDown = false;
            return true;
        }
        return false;
    }

    bool isDown() const { return m_isDown; }
    int discardedCount() const { return m_discardedCount; }
    const std::vector<InjectedEvent>& injectedEvents() const { return m_injectedEvents; }
    ScreenPoint lastPoint() const { return m_lastPoint; }

private:
    DisplayBounds m_bounds;
    bool m_isDown;
    ScreenPoint m_lastPoint;
    std::vector<InjectedEvent> m_injectedEvents;
    int m_discardedCount = 0;
};

// ─── Oracle: independent verification ────────────────────────────────────────

struct OracleState {
    bool isDown = false;
    ScreenPoint lastPoint{0, 0};
};

struct OracleResult {
    bool injected;            // true if an event would be injected
    bool discarded;           // true if moved was discarded
    bool isNowDown;           // state after this event
};

OracleResult OracleHandleTouch(OracleState& state, const TouchEvent& event,
                               const DisplayBounds& bounds) {
    OracleResult result{};
    ScreenPoint pos = NormalizedToScreen(event.normX, event.normY, bounds);

    switch (event.phase) {
        case TouchPhase::Began:
            state.isDown = true;
            state.lastPoint = pos;
            result.injected = true;
            result.discarded = false;
            result.isNowDown = true;
            break;

        case TouchPhase::Moved:
            if (!state.isDown) {
                result.injected = false;
                result.discarded = true;
                result.isNowDown = false;
            } else {
                state.lastPoint = pos;
                result.injected = true;
                result.discarded = false;
                result.isNowDown = true;
            }
            break;

        case TouchPhase::Ended:
        case TouchPhase::Cancelled:
            result.injected = true;
            result.discarded = false;
            state.isDown = false;
            result.isNowDown = false;
            break;
    }

    return result;
}

// ─── RapidCheck generators ───────────────────────────────────────────────────

rc::Gen<DisplayBounds> genDisplayBounds() {
    return rc::gen::apply(
        [](int left, int top, int width, int height) {
            return DisplayBounds{left, top, left + width, top + height};
        },
        rc::gen::inRange(-2000, 2001),   // left
        rc::gen::inRange(-2000, 2001),   // top
        rc::gen::inRange(100, 3001),     // width (positive, realistic)
        rc::gen::inRange(100, 3001)      // height (positive, realistic)
    );
}

rc::Gen<TouchPhase> genTouchPhase() {
    return rc::gen::element(
        TouchPhase::Began,
        TouchPhase::Moved,
        TouchPhase::Ended,
        TouchPhase::Cancelled
    );
}

rc::Gen<double> genNormCoord() {
    // Generate coordinates that may be outside [0,1] to test clamping
    return rc::gen::map(rc::gen::inRange(-200, 1201), [](int v) {
        return static_cast<double>(v) / 1000.0;
    });
}

rc::Gen<TouchEvent> genTouchEvent() {
    return rc::gen::apply(
        [](TouchPhase phase, double x, double y) {
            return TouchEvent{phase, x, y};
        },
        genTouchPhase(),
        genNormCoord(),
        genNormCoord()
    );
}

} // namespace

// ─── Property Tests ──────────────────────────────────────────────────────────

// **Validates: Requirements 6.4, 7.1, 7.2, 7.3, 7.4, 7.6, 7.7**
// Property: Coordinates outside [0,1] are always clamped before mapping.
// The mapped screen position must always lie within the display bounds.
RC_GTEST_PROP(InputInjectionStateMachine,
              CoordinatesAlwaysClampedToDisplayBounds,
              ()) {
    auto bounds = *genDisplayBounds();
    auto normX = *genNormCoord();
    auto normY = *genNormCoord();

    auto screenPos = NormalizedToScreen(normX, normY, bounds);

    // After clamping and mapping, screen position must be within display bounds
    RC_ASSERT(screenPos.x >= static_cast<double>(bounds.left));
    RC_ASSERT(screenPos.x <= static_cast<double>(bounds.right));
    RC_ASSERT(screenPos.y >= static_cast<double>(bounds.top));
    RC_ASSERT(screenPos.y <= static_cast<double>(bounds.bottom));
}

// **Validates: Requirements 6.4, 7.1, 7.2, 7.3, 7.4, 7.6, 7.7**
// Property: "moved" without prior "began" is always discarded (no event injected).
RC_GTEST_PROP(InputInjectionStateMachine,
              MovedWithoutBeganIsDiscarded,
              ()) {
    auto bounds = *genDisplayBounds();

    // Generate a sequence that starts with moved events (no began first)
    auto numMoves = *rc::gen::inRange(1, 20);

    InputInjectorModel model(bounds);
    OracleState oracle{};

    for (int i = 0; i < numMoves; ++i) {
        double normX = *genNormCoord();
        double normY = *genNormCoord();
        TouchEvent event{TouchPhase::Moved, normX, normY};

        bool modelInjected = model.HandleTouch(event);
        auto oracleResult = OracleHandleTouch(oracle, event, bounds);

        // Both must agree: moved without down is discarded
        RC_ASSERT(!modelInjected);
        RC_ASSERT(oracleResult.discarded);
        RC_ASSERT(!oracleResult.injected);
        RC_ASSERT(!model.isDown());
        RC_ASSERT(!oracle.isDown);
    }

    // All moves should have been discarded
    RC_ASSERT(model.discardedCount() == numMoves);
    RC_ASSERT(model.injectedEvents().empty());
}

// **Validates: Requirements 6.4, 7.1, 7.2, 7.3, 7.4, 7.6, 7.7**
// Property: "began" transitions to down state, "ended"/"cancelled" transitions to up state.
// Model and oracle must agree on state transitions for any random event sequence.
RC_GTEST_PROP(InputInjectionStateMachine,
              StateTransitionsMatchOracleForRandomSequences,
              ()) {
    auto bounds = *genDisplayBounds();
    auto numEvents = *rc::gen::inRange(1, 50);

    InputInjectorModel model(bounds);
    OracleState oracle{};

    for (int i = 0; i < numEvents; ++i) {
        auto event = *genTouchEvent();

        bool modelInjected = model.HandleTouch(event);
        auto oracleResult = OracleHandleTouch(oracle, event, bounds);

        // Model and oracle must agree on injection/discard decision
        RC_ASSERT(modelInjected == oracleResult.injected);

        // State must remain consistent
        RC_ASSERT(model.isDown() == oracle.isDown);

        // Verify state transitions
        if (event.phase == TouchPhase::Began) {
            RC_ASSERT(model.isDown());
            RC_ASSERT(modelInjected);
        }
        else if (event.phase == TouchPhase::Ended || event.phase == TouchPhase::Cancelled) {
            RC_ASSERT(!model.isDown());
            RC_ASSERT(modelInjected);
        }
        else if (event.phase == TouchPhase::Moved) {
            if (!oracleResult.discarded) {
                // Was in down state before, still in down state
                RC_ASSERT(model.isDown());
            }
        }
    }
}

// **Validates: Requirements 6.4, 7.1, 7.2, 7.3, 7.4, 7.6, 7.7**
// Property: Mapping formula is correct:
//   screenX = displayLeft + (clamp(normX) * displayWidth)
//   screenY = displayTop  + (clamp(normY) * displayHeight)
RC_GTEST_PROP(InputInjectionStateMachine,
              MappingFormulaIsCorrect,
              ()) {
    auto bounds = *genDisplayBounds();
    auto normX = *genNormCoord();
    auto normY = *genNormCoord();

    auto screenPos = NormalizedToScreen(normX, normY, bounds);

    // Independently compute expected values
    double clampedX = Clamp01(normX);
    double clampedY = Clamp01(normY);
    double expectedX = bounds.left + (clampedX * bounds.width());
    double expectedY = bounds.top  + (clampedY * bounds.height());

    RC_ASSERT(std::abs(screenPos.x - expectedX) < 1e-9);
    RC_ASSERT(std::abs(screenPos.y - expectedY) < 1e-9);
}

// **Validates: Requirements 6.4, 7.1, 7.2, 7.3, 7.4, 7.6, 7.7**
// Property: A complete touch cycle (began → moved* → ended) produces exactly
// one down event, zero or more drag events, and one up event. No events are
// discarded within a valid cycle.
RC_GTEST_PROP(InputInjectionStateMachine,
              ValidTouchCycleProducesCorrectEvents,
              ()) {
    auto bounds = *genDisplayBounds();
    auto numMoves = *rc::gen::inRange(0, 30);

    InputInjectorModel model(bounds);

    // began
    double beganX = *genNormCoord();
    double beganY = *genNormCoord();
    bool beganInjected = model.HandleTouch({TouchPhase::Began, beganX, beganY});
    RC_ASSERT(beganInjected);
    RC_ASSERT(model.isDown());

    // moved (all should be injected since button is down)
    for (int i = 0; i < numMoves; ++i) {
        double moveX = *genNormCoord();
        double moveY = *genNormCoord();
        bool moveInjected = model.HandleTouch({TouchPhase::Moved, moveX, moveY});
        RC_ASSERT(moveInjected);
        RC_ASSERT(model.isDown());
    }

    // ended or cancelled
    bool useEnded = *rc::gen::element(true, false);
    TouchPhase endPhase = useEnded ? TouchPhase::Ended : TouchPhase::Cancelled;
    double endX = *genNormCoord();
    double endY = *genNormCoord();
    bool endInjected = model.HandleTouch({endPhase, endX, endY});
    RC_ASSERT(endInjected);
    RC_ASSERT(!model.isDown());

    // Verify event counts: 1 down + numMoves drags + 1 up
    const auto& events = model.injectedEvents();
    RC_ASSERT(static_cast<int>(events.size()) == 1 + numMoves + 1);
    RC_ASSERT(events.front().type == InjectedEventType::MouseDownAndMove);
    RC_ASSERT(events.back().type == InjectedEventType::MouseUp);

    for (int i = 1; i <= numMoves; ++i) {
        RC_ASSERT(events[i].type == InjectedEventType::MouseMove);
    }

    // No events were discarded in a valid cycle
    RC_ASSERT(model.discardedCount() == 0);
}

// **Validates: Requirements 6.4, 7.1, 7.2, 7.3, 7.4, 7.6, 7.7**
// Property: Clamp01 is idempotent — clamping an already-clamped value produces
// the same result. And values in [0,1] are unchanged.
RC_GTEST_PROP(InputInjectionStateMachine,
              Clamp01IsIdempotentAndPreservesValidRange,
              ()) {
    auto rawValue = *genNormCoord();
    double clamped = Clamp01(rawValue);

    // Idempotent: clamping again produces same result
    RC_ASSERT(Clamp01(clamped) == clamped);

    // Result is always in [0, 1]
    RC_ASSERT(clamped >= 0.0);
    RC_ASSERT(clamped <= 1.0);

    // If input was already in [0, 1], output equals input
    if (rawValue >= 0.0 && rawValue <= 1.0) {
        RC_ASSERT(clamped == rawValue);
    }
}

// **Validates: Requirements 6.4, 7.1, 7.2, 7.3, 7.4, 7.6, 7.7**
// Property: After "ended"/"cancelled", subsequent "moved" events are discarded
// until the next "began". This tests the full state machine reset.
RC_GTEST_PROP(InputInjectionStateMachine,
              MovedAfterEndIsDiscardedUntilNextBegan,
              ()) {
    auto bounds = *genDisplayBounds();

    InputInjectorModel model(bounds);

    // Complete a touch cycle: began → ended
    model.HandleTouch({TouchPhase::Began, 0.5, 0.5});
    RC_ASSERT(model.isDown());
    model.HandleTouch({TouchPhase::Ended, 0.5, 0.5});
    RC_ASSERT(!model.isDown());

    // Now generate moved events — all should be discarded
    auto numMoves = *rc::gen::inRange(1, 15);
    for (int i = 0; i < numMoves; ++i) {
        double moveX = *genNormCoord();
        double moveY = *genNormCoord();
        bool injected = model.HandleTouch({TouchPhase::Moved, moveX, moveY});
        RC_ASSERT(!injected);
        RC_ASSERT(!model.isDown());
    }

    RC_ASSERT(model.discardedCount() == numMoves);

    // A new "began" should work and re-enable moved handling
    bool beganInjected = model.HandleTouch({TouchPhase::Began, 0.3, 0.7});
    RC_ASSERT(beganInjected);
    RC_ASSERT(model.isDown());

    // Moved should now work again
    bool moveInjected = model.HandleTouch({TouchPhase::Moved, 0.4, 0.6});
    RC_ASSERT(moveInjected);
}
