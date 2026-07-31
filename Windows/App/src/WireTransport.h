#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#include <WinSock2.h>
#include <Windows.h>

// Use the official forward declaration header for nlohmann::json
#include <nlohmann/json_fwd.hpp>

/// Implements the framed TCP protocol for video and control messages,
/// compatible with the existing iOS receiver. Handles both USB (via AMDS)
/// and WiFi (direct TCP) transports.
///
/// Wire format: [4B big-endian length][JSON telemetry {"cap":ms,"snd":ms}][Annex B payload]
/// Control messages: JSON < 32KB, starts with '{', no NUL bytes.
///
/// Validates: Requirements 4.1, 6.1
class WireTransport {
public:
    /// Connect to an iOS device over USB via AMDS tunnel.
    /// @param deviceUdid The UDID of the iOS device to connect to.
    /// @param port The device-side port (default 9000).
    /// @return S_OK on success, or an appropriate error HRESULT.
    HRESULT ConnectUSB(const std::string& deviceUdid, uint16_t port = 9000);

    /// Connect to a receiver over WiFi (direct TCP).
    /// @param host The hostname or IP address of the receiver.
    /// @param port The port number of the receiver.
    /// @return S_OK on success, or an appropriate error HRESULT.
    HRESULT ConnectWiFi(const std::string& host, uint16_t port);

    /// Connect to a receiver over WiFi with exponential backoff retry.
    /// Retries with delays: 1s, 2s, 4s, 8s, 10s (capped), max 5 attempts.
    /// If all attempts fail, the session should be ended by the caller.
    /// @param host The hostname or IP address of the receiver.
    /// @param port The port number of the receiver.
    /// @return S_OK on success, or E_FAIL if all retry attempts are exhausted.
    ///
    /// Validates: Requirements 5.5
    HRESULT ConnectWiFiWithRetry(const std::string& host, uint16_t port);

    /// Attempt failover from USB to WiFi transport with a grace period.
    /// Called when USB is detached during an active session. Waits up to
    /// 10 seconds for WiFi discovery/connection before giving up.
    /// @param host The hostname or IP of the WiFi-discovered device.
    /// @param port The port number of the receiver.
    /// @return S_OK if WiFi failover succeeds within the grace period,
    ///         or E_FAIL if the grace period expires without a connection.
    ///
    /// Validates: Requirements 4.8
    HRESULT FailoverToWiFi(const std::string& host, uint16_t port);

    /// Send a video frame with telemetry prefix.
    /// Format: [4B BE len][{"cap":<captureMs>,"snd":<sendMs>}][Annex B payload]
    /// Drops frame if pendingSends >= kMaxPendingSends.
    /// @param annexB The H.264 Annex B encoded frame data.
    /// @param captureMs Capture timestamp in Unix epoch milliseconds.
    /// @param sendMs Send timestamp in Unix epoch milliseconds.
    /// @return S_OK on success, or error if connection lost or send queue full.
    HRESULT SendVideoFrame(const std::vector<uint8_t>& annexB,
                           int64_t captureMs, int64_t sendMs);

    /// Send a JSON control message (< 32KB, starts with '{', no NUL).
    /// @param json The serialized JSON control message string.
    /// @return S_OK on success, or error if connection lost.
    HRESULT SendControl(const std::string& json);

    /// Callback type for received control messages.
    using ControlHandler = std::function<void(const nlohmann::json&)>;

    /// Set the handler for incoming control messages from the receiver.
    /// The receive thread dispatches JSON messages (starting with '{') to this handler.
    /// @param handler The callback function to invoke on received control messages.
    void SetControlHandler(ControlHandler handler);

    /// @return true if the transport has an active connection.
    bool IsConnected() const;

    /// Disconnect and clean up the transport.
    void Disconnect();

private:
    /// Background receive loop — reads framed messages and dispatches them.
    void ReceiveLoop();

    /// Compute the backoff delay for a given retry attempt.
    /// Formula: min(2^(attempt-1) seconds, kMaxBackoffSeconds cap)
    /// @param attempt The 1-based attempt number.
    /// @return The delay in milliseconds.
    static uint32_t ComputeBackoffMs(int attempt);

    SOCKET m_socket = INVALID_SOCKET;
    std::thread m_receiveThread;
    ControlHandler m_controlHandler;
    std::atomic<int> m_pendingSends{0};
    static constexpr int kMaxPendingSends = 3;
    static constexpr int kMaxRetryAttempts = 5;
    static constexpr int kMaxBackoffSeconds = 10;
    static constexpr int kFailoverGracePeriodMs = 10000;
};

/// AMDS (Apple Mobile Device Service) client for USB communication with iOS devices.
/// Connects via named pipe (\\.\pipe\usbmux) or TCP fallback (localhost:27015).
/// Implements the usbmuxd plist protocol for device enumeration and tunneling.
///
/// Validates: Requirements 4.1
class AmdsClient {
public:
    /// Represents an attached iOS device.
    struct Device {
        int deviceID;        ///< AMDS-assigned device identifier
        std::string udid;    ///< Device UDID (unique identifier)
        std::string name;    ///< Device friendly name (from lockdownd or fallback)
    };

    /// Connect to AMDS service.
    /// Attempts named pipe first (\\.\pipe\usbmux), falls back to TCP (localhost:27015).
    /// @return S_OK on success, or error if AMDS is unavailable.
    HRESULT Connect();

    /// List currently attached iOS devices.
    /// @param devices Receives the list of attached devices.
    /// @return S_OK on success, or error if communication fails.
    HRESULT ListDevices(std::vector<Device>& devices);

    /// Subscribe to device attach/detach events.
    /// @param callback Invoked with the device and whether it was attached (true) or detached (false).
    /// @return S_OK on success, or error if subscription fails.
    using DeviceCallback = std::function<void(const Device&, bool attached)>;
    HRESULT Subscribe(DeviceCallback callback);

    /// Create a TCP tunnel to a port on the device.
    /// @param deviceID The AMDS device identifier.
    /// @param port The device-side port to tunnel to (typically 9000).
    /// @param outSocket Receives the connected socket for the tunnel.
    /// @return S_OK on success, or error if tunnel creation fails.
    HRESULT CreateTunnel(int deviceID, uint16_t port, SOCKET& outSocket);

    /// Resolve device friendly name via lockdownd (port 62078).
    /// @param deviceID The AMDS device identifier.
    /// @param outName Receives the device's friendly name.
    /// @param timeoutMs Maximum time to wait for the name query (default 5000ms).
    /// @return S_OK on success, or error/timeout if name resolution fails.
    HRESULT GetDeviceName(int deviceID, std::string& outName,
                          uint32_t timeoutMs = 5000);

    /// @return true if AMDS service is available (pipe or TCP connected).
    bool IsAvailable() const;

private:
    HANDLE m_pipe = INVALID_HANDLE_VALUE;
    SOCKET m_tcpSocket = INVALID_SOCKET;
    bool m_usePipe = false;  ///< true if named pipe connected
};
