// MacReceiverInputHandler — captures mouse/trackpad events on the receiver's
// rendering window and forwards them to the Windows sender as touch/scroll
// control messages.
//
// Coordinates are normalized to [0,1] relative to the receiver window's view,
// with origin at top-left (video space). macOS has origin at bottom-left, so
// the y-axis is flipped: y = 1.0 - (mouseLocation.y / viewHeight).
//
// NSEvent types are mapped to touch phases:
//   mouseDown / leftMouseDown   → "began"
//   mouseDragged / leftMouseDragged → "moved"
//   mouseUp / leftMouseUp       → "ended"
//
// Scroll events (scrollWheel) extract deltaX/deltaY and forward as scroll
// control messages.

import AppKit

/// Monitors mouse/trackpad events on the Mac receiver's display window and
/// forwards them as normalized touch/scroll control messages to the sender.
final class MacReceiverInputHandler {

    // MARK: - Callbacks

    /// Called with (phase, normalizedX, normalizedY) when a touch event occurs.
    var onTouch: ((String, Double, Double) -> Void)?
    /// Called with (dx, dy) when a scroll event occurs.
    var onScroll: ((Double, Double) -> Void)?

    // MARK: - State

    /// The window being monitored for input events.
    private weak var window: NSWindow?

    /// Local event monitors installed via NSEvent.addLocalMonitorForEvents.
    private var mouseDownMonitor: Any?
    private var mouseDraggedMonitor: Any?
    private var mouseUpMonitor: Any?
    private var scrollMonitor: Any?

    // MARK: - Lifecycle

    /// Attach the input handler to the given window. Events are captured
    /// relative to the window's content view bounds.
    func attach(to window: NSWindow) {
        // Detach from any previous window first.
        detach()
        self.window = window

        // Monitor left mouse down → "began"
        mouseDownMonitor = NSEvent.addLocalMonitorForEvents(matching: .leftMouseDown) {
            [weak self] event in
            self?.handleMouseEvent(event, phase: "began")
            return event
        }

        // Monitor left mouse dragged → "moved"
        mouseDraggedMonitor = NSEvent.addLocalMonitorForEvents(matching: .leftMouseDragged) {
            [weak self] event in
            self?.handleMouseEvent(event, phase: "moved")
            return event
        }

        // Monitor left mouse up → "ended"
        mouseUpMonitor = NSEvent.addLocalMonitorForEvents(matching: .leftMouseUp) {
            [weak self] event in
            self?.handleMouseEvent(event, phase: "ended")
            return event
        }

        // Monitor scroll wheel → scroll control message
        scrollMonitor = NSEvent.addLocalMonitorForEvents(matching: .scrollWheel) {
            [weak self] event in
            self?.handleScrollEvent(event)
            return event
        }
    }

    /// Remove all event monitors and release the window reference.
    func detach() {
        if let monitor = mouseDownMonitor {
            NSEvent.removeMonitor(monitor)
            mouseDownMonitor = nil
        }
        if let monitor = mouseDraggedMonitor {
            NSEvent.removeMonitor(monitor)
            mouseDraggedMonitor = nil
        }
        if let monitor = mouseUpMonitor {
            NSEvent.removeMonitor(monitor)
            mouseUpMonitor = nil
        }
        if let monitor = scrollMonitor {
            NSEvent.removeMonitor(monitor)
            scrollMonitor = nil
        }
        window = nil
    }

    deinit {
        detach()
    }

    // MARK: - Event handling

    /// Process a mouse event (down/dragged/up) and forward as a touch message.
    private func handleMouseEvent(_ event: NSEvent, phase: String) {
        guard let window = self.window,
              event.window === window,
              let contentView = window.contentView else { return }

        // Get the mouse location in the content view's coordinate space.
        let locationInWindow = event.locationInWindow
        let locationInView = contentView.convert(locationInWindow, from: nil)
        let viewBounds = contentView.bounds

        // Ensure the event is within the view bounds (ignore events outside).
        guard viewBounds.width > 0, viewBounds.height > 0 else { return }

        // Normalize coordinates to [0, 1] relative to the view.
        // macOS origin is bottom-left; video space origin is top-left → flip y.
        let rawX = locationInView.x / viewBounds.width
        let rawY = 1.0 - (locationInView.y / viewBounds.height)

        // Clamp to [0, 1].
        let x = clamp01(rawX)
        let y = clamp01(rawY)

        onTouch?(phase, x, y)
    }

    /// Process a scroll wheel event and forward as a scroll message.
    private func handleScrollEvent(_ event: NSEvent) {
        guard let window = self.window,
              event.window === window else { return }

        let dx = Double(event.scrollingDeltaX)
        let dy = Double(event.scrollingDeltaY)

        // Only forward if there's meaningful scroll delta.
        guard dx != 0 || dy != 0 else { return }

        onScroll?(dx, dy)
    }

    // MARK: - Helpers

    /// Clamp a value to [0, 1].
    private func clamp01(_ value: Double) -> Double {
        return min(1.0, max(0.0, value))
    }
}
