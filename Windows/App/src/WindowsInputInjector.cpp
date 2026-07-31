// Ensure MOUSEEVENTF_VIRTUALDESKTOP is available
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0602
#endif

#include "WindowsInputInjector.h"
#include <algorithm>
#include <cmath>

// Fallback definition if not provided by the SDK
#ifndef MOUSEEVENTF_VIRTUALDESKTOP
#define MOUSEEVENTF_VIRTUALDESKTOP 0x4000
#endif

// ─── Clamp01 ────────────────────────────────────────────────────────────────────

double WindowsInputInjector::Clamp01(double v)
{
    if (v < 0.0) return 0.0;
    if (v > 1.0) return 1.0;
    return v;
}

// ─── SetDisplayBounds ───────────────────────────────────────────────────────────

void WindowsInputInjector::SetDisplayBounds(RECT bounds)
{
    m_displayBounds = bounds;
}

// ─── NormalizedToScreen ─────────────────────────────────────────────────────────
// Maps normalized [0,1] coordinates to an absolute screen position suitable for
// SendInput with MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESKTOP.
//
// Step 1: Map normalized coords to pixel position within the virtual display.
//   screenX = displayLeft + (normX × displayWidth)
//   screenY = displayTop  + (normY × displayHeight)
//
// Step 2: Normalize to virtual desktop coordinates (0..65535 range).
//   absX = (screenX - virtualDesktopLeft) × 65535 / virtualDesktopWidth
//   absY = (screenY - virtualDesktopTop)  × 65535 / virtualDesktopHeight

POINT WindowsInputInjector::NormalizedToScreen(double normX, double normY) const
{
    // Clamp inputs to [0, 1]
    normX = Clamp01(normX);
    normY = Clamp01(normY);

    // Compute pixel position within the virtual display
    double displayWidth  = static_cast<double>(m_displayBounds.right - m_displayBounds.left);
    double displayHeight = static_cast<double>(m_displayBounds.bottom - m_displayBounds.top);
    double screenX = m_displayBounds.left + (normX * displayWidth);
    double screenY = m_displayBounds.top  + (normY * displayHeight);

    // Get virtual desktop metrics
    int vdLeft   = GetSystemMetrics(SM_XVIRTUALSCREEN);
    int vdTop    = GetSystemMetrics(SM_YVIRTUALSCREEN);
    int vdWidth  = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    int vdHeight = GetSystemMetrics(SM_CYVIRTUALSCREEN);

    // Normalize to 0..65535 range relative to virtual desktop
    LONG absX = 0;
    LONG absY = 0;
    if (vdWidth > 0) {
        absX = static_cast<LONG>(std::round((screenX - vdLeft) * 65535.0 / vdWidth));
    }
    if (vdHeight > 0) {
        absY = static_cast<LONG>(std::round((screenY - vdTop) * 65535.0 / vdHeight));
    }

    return POINT{ absX, absY };
}

// ─── HandleTouch ────────────────────────────────────────────────────────────────
// Touch-to-Mouse state machine:
//   began           → MOUSEEVENTF_LEFTDOWN + MOUSEEVENTF_MOVE (position + press)
//   moved + m_isDown → MOUSEEVENTF_MOVE (drag, button held)
//   moved + !m_isDown → discard (no injection)
//   ended/cancelled → MOUSEEVENTF_LEFTUP

void WindowsInputInjector::HandleTouch(const std::string& phase, double normX, double normY)
{
    POINT absPos = NormalizedToScreen(normX, normY);

    if (phase == "began") {
        // Move to position and press left button
        INPUT inputs[1] = {};
        inputs[0].type = INPUT_MOUSE;
        inputs[0].mi.dx = absPos.x;
        inputs[0].mi.dy = absPos.y;
        inputs[0].mi.dwFlags = MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESKTOP
                             | MOUSEEVENTF_MOVE | MOUSEEVENTF_LEFTDOWN;

        SendInput(1, inputs, sizeof(INPUT));
        m_isDown = true;
        m_lastPoint = absPos;
    }
    else if (phase == "moved") {
        if (!m_isDown) {
            // Discard — no button currently down
            return;
        }

        // Drag — move with button held
        INPUT inputs[1] = {};
        inputs[0].type = INPUT_MOUSE;
        inputs[0].mi.dx = absPos.x;
        inputs[0].mi.dy = absPos.y;
        inputs[0].mi.dwFlags = MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESKTOP
                             | MOUSEEVENTF_MOVE;

        SendInput(1, inputs, sizeof(INPUT));
        m_lastPoint = absPos;
    }
    else if (phase == "ended" || phase == "cancelled") {
        // Release left button at last known position
        INPUT inputs[1] = {};
        inputs[0].type = INPUT_MOUSE;
        inputs[0].mi.dx = m_lastPoint.x;
        inputs[0].mi.dy = m_lastPoint.y;
        inputs[0].mi.dwFlags = MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESKTOP
                             | MOUSEEVENTF_LEFTUP;

        SendInput(1, inputs, sizeof(INPUT));
        m_isDown = false;
    }
    // Unknown phase — ignore
}

// ─── HandleScroll ───────────────────────────────────────────────────────────────
// Injects scroll wheel events using natural-scrolling sign convention.
// dx/dy are in display pixels. Windows WHEEL_DELTA = 120 per notch.
// We scale pixel deltas to wheel units (1 pixel = 1 wheel delta unit for
// smooth scrolling on modern systems).
//
// Natural scrolling: positive dy = scroll content up (same as finger swipe up),
// which on Windows means positive WHEEL_DATA (scroll forward/up).
// Positive dx = scroll content left, which means positive horizontal scroll data.

void WindowsInputInjector::HandleScroll(double dx, double dy)
{
    // Inject vertical scroll if non-zero
    if (dy != 0.0) {
        INPUT input = {};
        input.type = INPUT_MOUSE;
        input.mi.dwFlags = MOUSEEVENTF_WHEEL;
        // Natural scrolling: positive dy means scroll up (positive mouseData)
        input.mi.mouseData = static_cast<DWORD>(std::round(dy));

        SendInput(1, &input, sizeof(INPUT));
    }

    // Inject horizontal scroll if non-zero
    if (dx != 0.0) {
        INPUT input = {};
        input.type = INPUT_MOUSE;
        input.mi.dwFlags = MOUSEEVENTF_HWHEEL;
        // Natural scrolling: positive dx means scroll right (positive mouseData)
        input.mi.mouseData = static_cast<DWORD>(std::round(dx));

        SendInput(1, &input, sizeof(INPUT));
    }
}
