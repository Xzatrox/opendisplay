#pragma once

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <Windows.h>

#include "DesktopDuplicationCapture.h"
#include "MFTEncoder.h"
#include "AmfEncoder.h"
#include "WireTransport.h"
#include "WindowsInputInjector.h"

// Forward declarations
class ProtocolHandler;
class BonjourBrowser;
class AmdsClient;
struct HelloInfo;

/// Information about a discovered device (USB or WiFi).
struct DeviceInfo {
    std::string name;        ///< Device friendly name
    std::string udid;        ///< Device UDID (for USB) or install ID (for WiFi)
    std::string host;        ///< Hostname/IP (for WiFi connections)
    uint16_t port = 9000;    ///< Target port
    bool isUSB = false;      ///< true if connected via USB/AMDS
};

/// Quality preset for the streaming session.
enum class StreamQuality {
    Best,      ///< 18 Mbps
    Balanced,  ///< 10 Mbps
    Fast       ///< 6 Mbps
};

/// Orchestrates the overall session lifecycle: device enumeration, connection
/// management, virtual display lifecycle, encoder pipeline, liveness monitoring,
/// and reconnection logic. Supports up to 4 concurrent sessions.
///
/// Validates: Requirements 10.1, 10.2, 10.3, 10.4, 10.6, 4.8
class SessionController {
public:
    /// Session state machine states.
    enum class State {
        Idle,           ///< No active session
        Connecting,     ///< Establishing transport connection
        WaitingForHello,///< Connected, waiting for receiver's hello message
        Streaming,      ///< Active streaming session
        Reconnecting,   ///< Connection lost, attempting to reconnect
        Ended           ///< Session terminated
    };

    SessionController();
    ~SessionController();

    /// Start a session to a discovered device.
    /// Creates virtual display, initializes capture/encoder pipeline, connects transport.
    /// @param device The target device information.
    /// @param quality The streaming quality preset.
    /// @return S_OK on success, or an appropriate error HRESULT.
    HRESULT StartSession(const DeviceInfo& device, StreamQuality quality);

    /// End the current session gracefully.
    /// Tears down virtual display, closes TCP, releases encoder, stops capture.
    void EndSession();

    /// @return The current session state.
    State GetState() const;

    /// Enable auto-connect for a previously paired USB device.
    /// When the device with matching installId is attached, auto-connect within 5 seconds.
    /// @param installId The install identity of the device to auto-connect.
    void EnableAutoConnect(const std::string& installId);

    /// Disable auto-connect for the specified device.
    /// @param installId The install identity to stop auto-connecting.
    void DisableAutoConnect(const std::string& installId);

    /// Notify the session controller that a USB device has been detached.
    /// If an active session is using this device, attempts WiFi failover.
    /// @param udid The UDID of the detached USB device.
    void OnUSBDeviceDetached(const std::string& udid);

    /// Notify the session controller that a USB device has been attached.
    /// If the device matches a previously paired auto-connect identity,
    /// automatically starts a session.
    /// @param device The device info for the newly attached USB device.
    void OnUSBDeviceAttached(const DeviceInfo& device);

    /// Set the Bonjour browser instance for WiFi failover resolution.
    /// @param browser Shared pointer to the active BonjourBrowser.
    void SetBonjourBrowser(std::shared_ptr<BonjourBrowser> browser);

    /// Shut down ALL active sessions within 3 seconds total. Called on app exit.
    /// Each session's EndSession() is invoked concurrently. If the 3-second deadline
    /// expires, remaining sessions rely on the driver's handle-close callback
    /// (task 2.2, Req 10.7) to remove orphaned virtual displays on process exit.
    ///
    /// Validates: Requirements 1.4, 10.5
    static void ShutdownAllSessions();

    /// @return The number of currently active sessions (for multi-session manager).
    static int GetActiveSessionCount();

    /// @return true if a new session can be started (under kMaxSessions limit).
    static bool CanStartNewSession();

    /// Callback type for state change notifications.
    using StateCallback = std::function<void(State, const std::string& status)>;

    /// Set callback for session state changes (for UI updates).
    /// @param cb Invoked with the new state and a human-readable status string.
    void SetStateCallback(StateCallback cb);

private:
    /// Transition to a new state and notify the callback.
    void SetState(State newState, const std::string& status = "");

    /// The capture -> encode -> send pipeline loop (runs on m_pipelineThread).
    void PipelineLoop();

    /// Liveness monitoring loop (runs on m_livenessThread).
    /// Checks m_lastReceived against kPongTimeout every second.
    /// On timeout, transitions to Reconnecting state.
    void LivenessLoop();

    /// Attempt reconnection with exponential backoff.
    /// Retries connection up to kMaxReconnectAttempts times with
    /// delay = min(2^(attempt-1) seconds, kReconnectBackoffCapSec).
    /// @return true if reconnection succeeded, false if all attempts exhausted.
    bool AttemptReconnect();

    /// Compute reconnection backoff delay for a given attempt.
    /// @param attempt 1-based attempt number.
    /// @return Delay in milliseconds: min(2^(attempt-1) * 1000, cap).
    static uint32_t ComputeReconnectBackoffMs(int attempt);

    /// Attempt WiFi failover when USB is detached.
    /// Looks up the device's WiFi address via BonjourBrowser and attempts
    /// connection with a 10-second grace period.
    /// @return true if failover succeeded, false otherwise.
    bool AttemptWiFiFailover();

    /// Handle an incoming control message from the transport layer.
    void OnControlMessage(const nlohmann::json& msg);

    /// Handle the hello message: create/resize virtual display to match receiver.
    void OnHelloReceived(const struct HelloInfo& info);

    /// Handle "sleeping" message: tear down virtual display, await wake.
    void OnSleeping();

    /// Handle "closing" message: end session without reconnection.
    void OnClosing();

    /// Stop the liveness monitoring thread.
    void StopLivenessMonitor();

    /// Start the liveness monitoring thread.
    void StartLivenessMonitor();

    // Pipeline components (one per session)
    std::unique_ptr<DesktopDuplicationCapture> m_capture;
    std::unique_ptr<MFTEncoder> m_encoder;
    std::unique_ptr<AmfEncoder> m_amfEncoder;
    std::unique_ptr<WireTransport> m_transport;
    std::unique_ptr<WindowsInputInjector> m_input;
    std::unique_ptr<ProtocolHandler> m_protocol;

    // State machine
    std::atomic<State> m_state{State::Idle};
    StateCallback m_stateCallback;
    mutable std::mutex m_mutex;

    // Session configuration
    DeviceInfo m_device;
    StreamQuality m_quality = StreamQuality::Balanced;

    // Auto-connect: set of install IDs for previously paired USB devices
    std::set<std::string> m_autoConnectInstallIds;
    mutable std::mutex m_autoConnectMutex;

    // Virtual display
    HANDLE m_driverHandle = INVALID_HANDLE_VALUE;
    uint32_t m_monitorId = 0;
    uint32_t m_displayWidth = 0;
    uint32_t m_displayHeight = 0;

    // Pipeline thread
    std::thread m_pipelineThread;
    std::atomic<bool> m_pipelineRunning{false};

    // Liveness monitoring
    std::chrono::steady_clock::time_point m_lastReceived;
    std::thread m_livenessThread;
    std::atomic<bool> m_livenessRunning{false};
    static constexpr auto kPingInterval = std::chrono::seconds(2);
    static constexpr auto kPongTimeout = std::chrono::seconds(5);
    static constexpr int kMaxReconnectAttempts = 3;
    static constexpr int kReconnectBackoffCapSec = 10;

    // WiFi failover
    std::shared_ptr<BonjourBrowser> m_bonjourBrowser;

    // Multi-session support (up to 4 concurrent)
    static constexpr int kMaxSessions = 4;
    static std::atomic<int> s_activeSessionCount;

    // Static registry of all active SessionController instances for ShutdownAllSessions()
    static std::mutex s_registryMutex;
    static std::vector<SessionController*> s_activeSessions;
};
