#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

#include <Windows.h>

#include <nlohmann/json.hpp>

/// Information extracted from the receiver's "hello" message.
struct HelloInfo {
    int pixelsWide = 0;     ///< Native panel width in pixels
    int pixelsHigh = 0;     ///< Native panel height in pixels
    double scale = 2.0;     ///< Device scale factor (e.g. 2.0 or 3.0)
    std::string device;     ///< Device type: "iPad", "iPhone", "Mac" (optional)
    std::string id;         ///< Per-install UUID (optional)
    int pv = 1;             ///< Protocol version (defaults to 1 if absent)
};

/// Protocol handler configuration constants.
struct ProtocolConfig {
    int senderPv = 2;                   ///< This build's protocol version
    int senderMinPeer = 1;              ///< Minimum peer version we support
    std::string appStoreUrl =           ///< App Store URL for update prompt
        "https://apps.apple.com/app/opendisplay/id6741082870";
};

/// Callback types for session events dispatched by the ProtocolHandler.
struct ProtocolCallbacks {
    /// Called when a valid hello is received with the parsed info.
    std::function<void(const HelloInfo&)> onHello;

    /// Called when the receiver sends "sleeping" — tear down virtual display.
    std::function<void()> onSleeping;

    /// Called when the receiver sends "closing" — end session, no reconnect.
    std::function<void()> onClosing;

    /// Called when a pong is received, with the round-trip time in milliseconds.
    std::function<void(int64_t rttMs)> onPong;
};

/// Implements the session protocol message handling layer between WireTransport
/// (which delivers parsed JSON control messages) and the SessionController.
///
/// Responsibilities:
/// - Process incoming hello → send welcome (and updateRequired if needed)
/// - Periodic ping every 2 seconds with Unix timestamp
/// - Respond to incoming pong
/// - Handle sleeping/closing lifecycle messages
/// - Log and continue on unknown message types
///
/// Validates: Requirements 6.2, 6.3, 6.5, 6.6, 6.7, 6.8, 6.10
class ProtocolHandler {
public:
    /// Construct with a send function (typically bound to WireTransport::SendControl)
    /// and configuration.
    /// @param sendFn Function to send a JSON string over the wire.
    /// @param config Protocol version configuration.
    explicit ProtocolHandler(std::function<void(const std::string&)> sendFn,
                             ProtocolConfig config = {});

    ~ProtocolHandler();

    /// Set callbacks for protocol events.
    void SetCallbacks(ProtocolCallbacks callbacks);

    /// Handle an incoming control message (dispatched from WireTransport).
    /// Routes to the appropriate handler based on the "type" field.
    /// Unknown types are logged and ignored (no disconnect).
    /// @param msg The parsed JSON control message.
    void HandleMessage(const nlohmann::json& msg);

    /// Start the periodic ping timer (every 2 seconds).
    /// Must be called after the connection is established and hello is received.
    void StartPingTimer();

    /// Stop the periodic ping timer.
    void StopPingTimer();

    /// @return true if a hello has been received and the session is active.
    bool IsSessionActive() const;

    /// @return The last received HelloInfo, or a default if none received yet.
    HelloInfo GetLastHello() const;

private:
    /// Handle incoming "hello" message from the receiver.
    void HandleHello(const nlohmann::json& msg);

    /// Handle incoming "pong" message from the receiver.
    void HandlePong(const nlohmann::json& msg);

    /// Handle incoming "sleeping" message from the receiver.
    void HandleSleeping();

    /// Handle incoming "closing" message from the receiver.
    void HandleClosing();

    /// Send a "welcome" response to the receiver.
    void SendWelcome();

    /// Send an "updateRequired" message to the receiver.
    /// @param deviceKind The receiver device type (e.g. "iPad", "iPhone").
    void SendUpdateRequired(const std::string& deviceKind);

    /// Send a "ping" message with the current Unix timestamp in milliseconds.
    void SendPing();

    /// Background thread function for the periodic ping timer.
    void PingLoop();

    // Send function (bound to WireTransport::SendControl)
    std::function<void(const std::string&)> m_sendFn;

    // Configuration
    ProtocolConfig m_config;

    // Callbacks
    ProtocolCallbacks m_callbacks;

    // State
    HelloInfo m_lastHello;
    std::atomic<bool> m_sessionActive{false};
    mutable std::mutex m_mutex;

    // Ping timer
    std::thread m_pingThread;
    std::atomic<bool> m_pingRunning{false};

    /// Ping interval: 2 seconds.
    static constexpr auto kPingIntervalMs = std::chrono::milliseconds(2000);
};
