#pragma once

#include <string>
#include <Windows.h>

/// Translates normalized touch coordinates and scroll deltas from the receiver
/// into Windows input events on the virtual display using the SendInput API.
///
/// Uses MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESKTOP for correct positioning
/// across multi-monitor configurations.
///
/// Validates: Requirements 6.1, 7.1
class WindowsInputInjector {
public:
    /// Configure with the virtual display's bounding rectangle in screen coordinates.
    /// @param bounds The RECT defining the virtual display's position and size on the desktop.
    void SetDisplayBounds(RECT bounds);

    /// Handle a touch event from the receiver.
    /// Maps normalized coordinates to absolute screen position and injects mouse events.
    /// Phases: "began" → left button down, "moved" → drag (if down) or discard,
    ///         "ended"/"cancelled" → left button up.
    /// @param phase The touch phase ("began", "moved", "ended", "cancelled").
    /// @param normX Normalized X coordinate in [0.0, 1.0] (clamped if outside).
    /// @param normY Normalized Y coordinate in [0.0, 1.0] (clamped if outside).
    void HandleTouch(const std::string& phase, double normX, double normY);

    /// Handle a scroll event from the receiver.
    /// Injects scroll wheel events using natural-scrolling sign convention.
    /// @param dx Horizontal scroll delta in display pixels.
    /// @param dy Vertical scroll delta in display pixels.
    void HandleScroll(double dx, double dy);

private:
    RECT m_displayBounds{};
    bool m_isDown = false;
    POINT m_lastPoint{};

    /// Map normalized [0,1] coordinates to absolute screen pixel position.
    /// Computes: screenX = displayLeft + (normX × displayWidth)
    ///           screenY = displayTop + (normY × displayHeight)
    /// Then normalizes to virtual desktop coordinates for SendInput.
    POINT NormalizedToScreen(double normX, double normY) const;

    /// Clamp a value to the range [0.0, 1.0].
    static double Clamp01(double v);
};
