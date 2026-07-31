#include "WireTransport.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <sstream>
#include <ws2tcpip.h>

// We use simple string formatting for the JSON telemetry prefix rather than
// pulling in a full JSON library, since the format is fixed and lightweight.
// For parsing incoming control messages we use nlohmann/json (header-only).
#include <nlohmann/json.hpp>

// ----------------------------------------------------------------------------
// Helper: Write a 4-byte big-endian uint32 into a buffer.
// ----------------------------------------------------------------------------
static void WriteBE32(uint8_t* dest, uint32_t value)
{
    dest[0] = static_cast<uint8_t>((value >> 24) & 0xFF);
    dest[1] = static_cast<uint8_t>((value >> 16) & 0xFF);
    dest[2] = static_cast<uint8_t>((value >> 8) & 0xFF);
    dest[3] = static_cast<uint8_t>(value & 0xFF);
}

// ----------------------------------------------------------------------------
// Helper: Read a 4-byte big-endian uint32 from a buffer.
// ----------------------------------------------------------------------------
static uint32_t ReadBE32(const uint8_t* src)
{
    return (static_cast<uint32_t>(src[0]) << 24) |
           (static_cast<uint32_t>(src[1]) << 16) |
           (static_cast<uint32_t>(src[2]) << 8) |
           static_cast<uint32_t>(src[3]);
}

// ----------------------------------------------------------------------------
// Helper: Send all bytes on a socket, handling partial sends.
// Returns S_OK on success, or E_FAIL if the connection is broken.
// ----------------------------------------------------------------------------
static HRESULT SendAll(SOCKET sock, const uint8_t* data, size_t length)
{
    size_t totalSent = 0;
    while (totalSent < length)
    {
        int toSend = static_cast<int>((std::min)(length - totalSent,
                                                  static_cast<size_t>(INT_MAX)));
        int sent = ::send(sock, reinterpret_cast<const char*>(data + totalSent), toSend, 0);
        if (sent == SOCKET_ERROR || sent == 0)
        {
            return E_FAIL;
        }
        totalSent += static_cast<size_t>(sent);
    }
    return S_OK;
}

// ----------------------------------------------------------------------------
// Helper: Receive exactly `length` bytes from a socket.
// Returns S_OK on success, or E_FAIL if the connection is broken/closed.
// ----------------------------------------------------------------------------
static HRESULT RecvAll(SOCKET sock, uint8_t* buffer, size_t length)
{
    size_t totalRecv = 0;
    while (totalRecv < length)
    {
        int toRecv = static_cast<int>((std::min)(length - totalRecv,
                                                  static_cast<size_t>(INT_MAX)));
        int received = ::recv(sock, reinterpret_cast<char*>(buffer + totalRecv), toRecv, 0);
        if (received <= 0)
        {
            return E_FAIL;
        }
        totalRecv += static_cast<size_t>(received);
    }
    return S_OK;
}

// ----------------------------------------------------------------------------
// ConnectUSB: Connect to an iOS device over USB via AMDS tunnel.
//
// This is a placeholder that sets up the socket once the AMDS client (task 6.2)
// provides the tunneled socket. For now, it stores the intent and returns
// E_NOTIMPL since the AMDS tunnel creation is in a separate task.
//
// Validates: Requirements 4.1
// ----------------------------------------------------------------------------
HRESULT WireTransport::ConnectUSB(const std::string& deviceUdid, uint16_t port)
{
    (void)deviceUdid;
    (void)port;

    // Full AMDS tunnel establishment is implemented in AmdsClient (task 6.2).
    // Once the tunnel socket is created, it will be assigned to m_socket and
    // the receive thread will be started.
    return E_NOTIMPL;
}

// ----------------------------------------------------------------------------
// ConnectWiFi: Connect to a receiver over WiFi (direct TCP).
//
// Establishes a TCP connection to the specified host:port, then starts the
// receive thread to handle incoming control messages.
//
// Validates: Requirements 5.3
// ----------------------------------------------------------------------------
HRESULT WireTransport::ConnectWiFi(const std::string& host, uint16_t port)
{
    if (host.empty() || port == 0)
    {
        return E_INVALIDARG;
    }

    // Resolve the host address
    addrinfo hints = {};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    std::string portStr = std::to_string(port);
    addrinfo* result = nullptr;
    int ret = ::getaddrinfo(host.c_str(), portStr.c_str(), &hints, &result);
    if (ret != 0 || !result)
    {
        return HRESULT_FROM_WIN32(WSAGetLastError());
    }

    // Try each resolved address until one connects
    SOCKET sock = INVALID_SOCKET;
    for (addrinfo* addr = result; addr != nullptr; addr = addr->ai_next)
    {
        sock = ::socket(addr->ai_family, addr->ai_socktype, addr->ai_protocol);
        if (sock == INVALID_SOCKET)
        {
            continue;
        }

        if (::connect(sock, addr->ai_addr, static_cast<int>(addr->ai_addrlen)) == 0)
        {
            break; // Connected successfully
        }

        ::closesocket(sock);
        sock = INVALID_SOCKET;
    }

    ::freeaddrinfo(result);

    if (sock == INVALID_SOCKET)
    {
        return E_FAIL;
    }

    // Disable Nagle's algorithm for low-latency streaming
    int noDelay = 1;
    ::setsockopt(sock, IPPROTO_TCP, TCP_NODELAY,
                 reinterpret_cast<const char*>(&noDelay), sizeof(noDelay));

    m_socket = sock;
    m_pendingSends.store(0);

    // Start the receive thread for incoming messages
    m_receiveThread = std::thread([this]() {
        ReceiveLoop();
    });

    return S_OK;
}

// ----------------------------------------------------------------------------
// ConnectWiFiWithRetry: Connect to a receiver over WiFi with exponential
// backoff retry logic.
//
// Retry schedule: 1s, 2s, 4s, 8s, 10s (capped at kMaxBackoffSeconds).
// Maximum attempts: kMaxRetryAttempts (5).
// If all attempts fail, returns E_FAIL — the caller should end the session.
//
// Validates: Requirements 5.5
// ----------------------------------------------------------------------------
HRESULT WireTransport::ConnectWiFiWithRetry(const std::string& host, uint16_t port)
{
    for (int attempt = 1; attempt <= kMaxRetryAttempts; ++attempt)
    {
        HRESULT hr = ConnectWiFi(host, port);
        if (SUCCEEDED(hr))
        {
            return S_OK;
        }

        // If this was the last attempt, don't sleep — just fail
        if (attempt == kMaxRetryAttempts)
        {
            break;
        }

        // Sleep with exponential backoff before the next attempt
        uint32_t delayMs = ComputeBackoffMs(attempt);
        std::this_thread::sleep_for(std::chrono::milliseconds(delayMs));
    }

    // All retry attempts exhausted — session should be ended
    return E_FAIL;
}

// ----------------------------------------------------------------------------
// FailoverToWiFi: Attempt failover from USB to WiFi with a 10-second grace
// period.
//
// Called when a USB device is detached during an active session. Uses
// ConnectWiFiWithRetry with the constraint that total elapsed time must not
// exceed the grace period. The retry backoff schedule (1s+2s+4s+... ) fits
// within 10 seconds for the first few attempts, allowing a reasonable number
// of connection tries before the grace period expires.
//
// Validates: Requirements 4.8
// ----------------------------------------------------------------------------
HRESULT WireTransport::FailoverToWiFi(const std::string& host, uint16_t port)
{
    if (host.empty() || port == 0)
    {
        return E_INVALIDARG;
    }

    auto deadline = std::chrono::steady_clock::now() +
                    std::chrono::milliseconds(kFailoverGracePeriodMs);

    for (int attempt = 1; attempt <= kMaxRetryAttempts; ++attempt)
    {
        // Check if we've exceeded the grace period
        if (std::chrono::steady_clock::now() >= deadline)
        {
            break;
        }

        HRESULT hr = ConnectWiFi(host, port);
        if (SUCCEEDED(hr))
        {
            return S_OK;
        }

        // If this was the last attempt, don't sleep — just fail
        if (attempt == kMaxRetryAttempts)
        {
            break;
        }

        // Sleep with exponential backoff, but don't exceed the grace period
        uint32_t delayMs = ComputeBackoffMs(attempt);
        auto now = std::chrono::steady_clock::now();
        auto remainingMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - now).count();

        if (remainingMs <= 0)
        {
            break;
        }

        uint32_t actualDelay = (std::min)(delayMs,
                                           static_cast<uint32_t>(remainingMs));
        std::this_thread::sleep_for(std::chrono::milliseconds(actualDelay));
    }

    // Grace period expired or all attempts failed — end the session
    return E_FAIL;
}

// ----------------------------------------------------------------------------
// ComputeBackoffMs: Calculate the exponential backoff delay for a given attempt.
//
// Formula: delay = min(2^(attempt-1) seconds, kMaxBackoffSeconds cap)
// Attempt 1 → 1s, Attempt 2 → 2s, Attempt 3 → 4s,
// Attempt 4 → 8s, Attempt 5 → 10s (capped)
// ----------------------------------------------------------------------------
uint32_t WireTransport::ComputeBackoffMs(int attempt)
{
    // 2^(attempt-1) seconds, capped at kMaxBackoffSeconds
    int delaySec = 1 << (attempt - 1);  // 1, 2, 4, 8, 16, ...
    delaySec = (std::min)(delaySec, kMaxBackoffSeconds);
    return static_cast<uint32_t>(delaySec) * 1000;
}

// ----------------------------------------------------------------------------
// SendVideoFrame: Send a video frame with telemetry prefix.
//
// Wire format: [4B BE total-payload-length][JSON telemetry][Annex B H.264 data]
//
// The telemetry JSON prefix is: {"cap":<captureMs>,"snd":<sendMs>}
// The Annex B payload starts with 00 00 00 01 start codes.
//
// Frame dropping: If pendingSends >= kMaxPendingSends (3), the frame is dropped
// to avoid unbounded queue growth and maintain low latency.
//
// Validates: Requirements 6.1, 6.9
// ----------------------------------------------------------------------------
HRESULT WireTransport::SendVideoFrame(const std::vector<uint8_t>& annexB,
                                       int64_t captureMs, int64_t sendMs)
{
    if (m_socket == INVALID_SOCKET)
    {
        return E_NOT_VALID_STATE;
    }

    // Drop frame if send queue is full (back-pressure from slow network)
    if (m_pendingSends.load() >= kMaxPendingSends)
    {
        return S_FALSE; // Frame dropped — not an error, just back-pressure
    }

    // Build the telemetry JSON prefix using simple string formatting.
    // Format: {"cap":<unix_ms>,"snd":<unix_ms>}
    std::string telemetry = "{\"cap\":" + std::to_string(captureMs) +
                            ",\"snd\":" + std::to_string(sendMs) + "}";

    // Calculate total payload length (telemetry + H.264 data)
    uint32_t payloadLength = static_cast<uint32_t>(telemetry.size() + annexB.size());

    // Assemble the framed message: [4B length][telemetry][payload]
    std::vector<uint8_t> frame;
    frame.resize(4 + payloadLength);

    // Write 4-byte big-endian length prefix
    WriteBE32(frame.data(), payloadLength);

    // Write telemetry JSON
    std::memcpy(frame.data() + 4, telemetry.data(), telemetry.size());

    // Write H.264 Annex B data
    std::memcpy(frame.data() + 4 + telemetry.size(), annexB.data(), annexB.size());

    // Track pending sends
    m_pendingSends.fetch_add(1);

    // Send the complete framed message
    HRESULT hr = SendAll(m_socket, frame.data(), frame.size());

    m_pendingSends.fetch_sub(1);

    if (FAILED(hr))
    {
        // Connection is broken
        return hr;
    }

    return S_OK;
}

// ----------------------------------------------------------------------------
// SendControl: Send a JSON control message on the video channel.
//
// Control messages must be:
// - Under 32KB in size
// - Start with '{' (so the receiver can distinguish from video)
// - Contain no NUL (0x00) bytes
//
// Wire format: [4B BE length][JSON payload]
//
// Validates: Requirements 6.9
// ----------------------------------------------------------------------------
HRESULT WireTransport::SendControl(const std::string& json)
{
    if (m_socket == INVALID_SOCKET)
    {
        return E_NOT_VALID_STATE;
    }

    // Validate control message constraints
    if (json.empty() || json[0] != '{')
    {
        return E_INVALIDARG;
    }

    // Must be under 32KB
    static constexpr size_t kMaxControlSize = 32768;
    if (json.size() >= kMaxControlSize)
    {
        return E_INVALIDARG;
    }

    // Must not contain NUL bytes
    if (json.find('\0') != std::string::npos)
    {
        return E_INVALIDARG;
    }

    uint32_t payloadLength = static_cast<uint32_t>(json.size());

    // Assemble the framed message: [4B length][JSON payload]
    std::vector<uint8_t> frame;
    frame.resize(4 + payloadLength);

    WriteBE32(frame.data(), payloadLength);
    std::memcpy(frame.data() + 4, json.data(), json.size());

    // Send the complete framed control message
    HRESULT hr = SendAll(m_socket, frame.data(), frame.size());
    if (FAILED(hr))
    {
        return hr;
    }

    return S_OK;
}

// ----------------------------------------------------------------------------
// SetControlHandler: Set the callback for incoming control messages.
// The receive thread dispatches JSON messages (starting with '{') to this handler.
// ----------------------------------------------------------------------------
void WireTransport::SetControlHandler(ControlHandler handler)
{
    m_controlHandler = std::move(handler);
}

// ----------------------------------------------------------------------------
// IsConnected: Returns true if the transport has an active socket connection.
// ----------------------------------------------------------------------------
bool WireTransport::IsConnected() const
{
    return m_socket != INVALID_SOCKET;
}

// ----------------------------------------------------------------------------
// Disconnect: Tear down the transport, close the socket, and stop the
// receive thread.
// ----------------------------------------------------------------------------
void WireTransport::Disconnect()
{
    // Close the socket — this will unblock any recv() in the receive thread
    if (m_socket != INVALID_SOCKET)
    {
        ::shutdown(m_socket, SD_BOTH);
        ::closesocket(m_socket);
        m_socket = INVALID_SOCKET;
    }

    // Wait for the receive thread to finish
    if (m_receiveThread.joinable())
    {
        m_receiveThread.join();
    }

    m_pendingSends.store(0);
    m_controlHandler = nullptr;
}

// ----------------------------------------------------------------------------
// ReceiveLoop: Background thread that reads framed messages from the socket
// and dispatches them to the appropriate handler.
//
// Message discrimination:
// - If the first byte of the payload is '{' (0x7B), it's a JSON control message.
// - Otherwise, it's video data (not dispatched here — video is push-only from
//   the sender's perspective; however, the receiver can send control messages
//   that we need to handle).
//
// The receive thread runs until the socket is closed or an error occurs.
//
// Validates: Requirements 6.10
// ----------------------------------------------------------------------------
void WireTransport::ReceiveLoop()
{
    // Buffer for reading the 4-byte length header
    uint8_t lengthBuf[4];

    while (m_socket != INVALID_SOCKET)
    {
        // Read the 4-byte big-endian length prefix
        HRESULT hr = RecvAll(m_socket, lengthBuf, 4);
        if (FAILED(hr))
        {
            break; // Connection closed or error
        }

        uint32_t payloadLength = ReadBE32(lengthBuf);

        // Sanity check: reject absurdly large messages (> 16MB)
        static constexpr uint32_t kMaxMessageSize = 16 * 1024 * 1024;
        if (payloadLength == 0 || payloadLength > kMaxMessageSize)
        {
            break; // Protocol error — disconnect
        }

        // Read the complete payload
        std::vector<uint8_t> payload(payloadLength);
        hr = RecvAll(m_socket, payload.data(), payloadLength);
        if (FAILED(hr))
        {
            break; // Connection closed or error
        }

        // Dispatch based on the first byte of the payload:
        // '{' (0x7B) = JSON control message, anything else = video data
        if (payload[0] == '{')
        {
            // Parse as JSON control message
            if (m_controlHandler)
            {
                try
                {
                    std::string jsonStr(payload.begin(), payload.end());
                    nlohmann::json msg = nlohmann::json::parse(jsonStr);
                    m_controlHandler(msg);
                }
                catch (const nlohmann::json::parse_error&)
                {
                    // Malformed JSON — log and continue (Requirement 6.10:
                    // handle unknown/malformed messages without disconnecting)
                }
            }
        }
        // else: video data from the other end — in the sender's case, we don't
        // expect to receive video frames, but we silently ignore them per the
        // robust protocol handling requirement.
    }
}
