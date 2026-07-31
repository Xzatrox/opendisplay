#include "WireTransport.h"

#include <cstring>
#include <sstream>
#include <string>
#include <vector>
#include <ws2tcpip.h>

// ----------------------------------------------------------------------------
// AmdsClient — Apple Mobile Device Service client for USB communication
// with iOS devices on Windows.
//
// AMDS is the Windows equivalent of macOS usbmuxd. It provides the same
// plist-based protocol over either a named pipe (\\.\pipe\usbmux) or a TCP
// socket (localhost:27015). This client implements device enumeration,
// attach/detach subscription, TCP tunnel creation, and device name resolution
// via lockdownd.
//
// Wire format (usbmuxd plist protocol):
//   [4B LE total length][4B LE version=1][4B LE type=8][4B LE tag][XML plist]
//
// Validates: Requirements 4.1, 4.2, 4.3, 4.4, 4.5, 4.6, 4.7
// ----------------------------------------------------------------------------

namespace {

// usbmuxd protocol constants
static constexpr uint32_t kUsbmuxVersion = 1;
static constexpr uint32_t kUsbmuxTypePlist = 8;
static constexpr uint32_t kUsbmuxHeaderSize = 16;

// Lockdownd port on iOS devices
static constexpr uint16_t kLockdownPort = 62078;

// Default receiver port
static constexpr uint16_t kDefaultReceiverPort = 9000;

// Named pipe path for AMDS
static constexpr const char* kPipePath = "\\\\.\\pipe\\usbmux";

// TCP fallback
static constexpr const char* kTcpHost = "127.0.0.1";
static constexpr uint16_t kTcpPort = 27015;

// Max message size sanity check (1MB)
static constexpr uint32_t kMaxMessageSize = 1 * 1024 * 1024;

// ----------------------------------------------------------------------------
// Helper: Write a 4-byte little-endian uint32 into a buffer.
// ----------------------------------------------------------------------------
static void WriteLE32(uint8_t* dest, uint32_t value)
{
    dest[0] = static_cast<uint8_t>(value & 0xFF);
    dest[1] = static_cast<uint8_t>((value >> 8) & 0xFF);
    dest[2] = static_cast<uint8_t>((value >> 16) & 0xFF);
    dest[3] = static_cast<uint8_t>((value >> 24) & 0xFF);
}

// ----------------------------------------------------------------------------
// Helper: Read a 4-byte little-endian uint32 from a buffer.
// ----------------------------------------------------------------------------
static uint32_t ReadLE32(const uint8_t* src)
{
    return static_cast<uint32_t>(src[0]) |
           (static_cast<uint32_t>(src[1]) << 8) |
           (static_cast<uint32_t>(src[2]) << 16) |
           (static_cast<uint32_t>(src[3]) << 24);
}

// ----------------------------------------------------------------------------
// Helper: Write a 4-byte big-endian uint32 into a buffer.
// ----------------------------------------------------------------------------
static void WriteAmds_BE32(uint8_t* dest, uint32_t value)
{
    dest[0] = static_cast<uint8_t>((value >> 24) & 0xFF);
    dest[1] = static_cast<uint8_t>((value >> 16) & 0xFF);
    dest[2] = static_cast<uint8_t>((value >> 8) & 0xFF);
    dest[3] = static_cast<uint8_t>(value & 0xFF);
}

// ----------------------------------------------------------------------------
// Helper: Read a 4-byte big-endian uint32 from a buffer.
// ----------------------------------------------------------------------------
static uint32_t ReadAmds_BE32(const uint8_t* src)
{
    return (static_cast<uint32_t>(src[0]) << 24) |
           (static_cast<uint32_t>(src[1]) << 16) |
           (static_cast<uint32_t>(src[2]) << 8) |
           static_cast<uint32_t>(src[3]);
}

// ----------------------------------------------------------------------------
// Helper: Build an XML plist string from key-value pairs.
// We use minimal hand-crafted XML to avoid pulling in a full plist library.
// All values are strings or integers as needed by the usbmuxd protocol.
// ----------------------------------------------------------------------------
static std::string BuildPlist(
    const std::vector<std::pair<std::string, std::string>>& stringFields,
    const std::vector<std::pair<std::string, int>>& intFields = {})
{
    std::ostringstream ss;
    ss << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
       << "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
       << "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
       << "<plist version=\"1.0\">\n<dict>\n";

    for (const auto& [key, value] : stringFields)
    {
        ss << "\t<key>" << key << "</key>\n"
           << "\t<string>" << value << "</string>\n";
    }
    for (const auto& [key, value] : intFields)
    {
        ss << "\t<key>" << key << "</key>\n"
           << "\t<integer>" << value << "</integer>\n";
    }

    ss << "</dict>\n</plist>\n";
    return ss.str();
}

// ----------------------------------------------------------------------------
// Helper: Build a usbmuxd protocol message (header + plist body).
// Returns the complete wire-format message ready to send.
// ----------------------------------------------------------------------------
static std::vector<uint8_t> BuildUsbmuxMessage(const std::string& plistBody,
                                                uint32_t tag = 1)
{
    uint32_t totalLength = kUsbmuxHeaderSize +
                           static_cast<uint32_t>(plistBody.size());

    std::vector<uint8_t> message(totalLength);
    WriteLE32(message.data() + 0, totalLength);        // total length
    WriteLE32(message.data() + 4, kUsbmuxVersion);     // version = 1
    WriteLE32(message.data() + 8, kUsbmuxTypePlist);   // type = 8 (plist)
    WriteLE32(message.data() + 12, tag);               // tag (correlation)

    std::memcpy(message.data() + kUsbmuxHeaderSize,
                plistBody.data(), plistBody.size());

    return message;
}

// ----------------------------------------------------------------------------
// Helper: Send data over a named pipe handle.
// Returns S_OK on success, E_FAIL on error.
// ----------------------------------------------------------------------------
static HRESULT PipeSendAll(HANDLE pipe, const uint8_t* data, size_t length)
{
    size_t totalSent = 0;
    while (totalSent < length)
    {
        DWORD toWrite = static_cast<DWORD>(
            (std::min)(length - totalSent, static_cast<size_t>(MAXDWORD)));
        DWORD written = 0;
        if (!WriteFile(pipe, data + totalSent, toWrite, &written, nullptr))
        {
            return E_FAIL;
        }
        if (written == 0) return E_FAIL;
        totalSent += written;
    }
    return S_OK;
}

// ----------------------------------------------------------------------------
// Helper: Receive exactly `length` bytes from a named pipe.
// Returns S_OK on success, E_FAIL on error.
// ----------------------------------------------------------------------------
static HRESULT PipeRecvAll(HANDLE pipe, uint8_t* buffer, size_t length)
{
    size_t totalRecv = 0;
    while (totalRecv < length)
    {
        DWORD toRead = static_cast<DWORD>(
            (std::min)(length - totalRecv, static_cast<size_t>(MAXDWORD)));
        DWORD bytesRead = 0;
        if (!ReadFile(pipe, buffer + totalRecv, toRead, &bytesRead, nullptr))
        {
            return E_FAIL;
        }
        if (bytesRead == 0) return E_FAIL;
        totalRecv += bytesRead;
    }
    return S_OK;
}

// ----------------------------------------------------------------------------
// Helper: Send data over a TCP socket.
// Returns S_OK on success, E_FAIL on error.
// ----------------------------------------------------------------------------
static HRESULT TcpSendAll(SOCKET sock, const uint8_t* data, size_t length)
{
    size_t totalSent = 0;
    while (totalSent < length)
    {
        int toSend = static_cast<int>(
            (std::min)(length - totalSent, static_cast<size_t>(INT_MAX)));
        int sent = ::send(sock, reinterpret_cast<const char*>(data + totalSent),
                          toSend, 0);
        if (sent == SOCKET_ERROR || sent == 0)
        {
            return E_FAIL;
        }
        totalSent += static_cast<size_t>(sent);
    }
    return S_OK;
}

// ----------------------------------------------------------------------------
// Helper: Receive exactly `length` bytes from a TCP socket.
// Returns S_OK on success, E_FAIL on error.
// ----------------------------------------------------------------------------
static HRESULT TcpRecvAll(SOCKET sock, uint8_t* buffer, size_t length)
{
    size_t totalRecv = 0;
    while (totalRecv < length)
    {
        int toRecv = static_cast<int>(
            (std::min)(length - totalRecv, static_cast<size_t>(INT_MAX)));
        int received = ::recv(sock, reinterpret_cast<char*>(buffer + totalRecv),
                              toRecv, 0);
        if (received <= 0)
        {
            return E_FAIL;
        }
        totalRecv += static_cast<size_t>(received);
    }
    return S_OK;
}

// ----------------------------------------------------------------------------
// Unified send/recv that dispatches to pipe or TCP based on transport.
// ----------------------------------------------------------------------------
static HRESULT SendMessage(HANDLE pipe, SOCKET sock, bool usePipe,
                           const std::vector<uint8_t>& message)
{
    if (usePipe)
    {
        return PipeSendAll(pipe, message.data(), message.size());
    }
    return TcpSendAll(sock, message.data(), message.size());
}

static HRESULT RecvBytes(HANDLE pipe, SOCKET sock, bool usePipe,
                         uint8_t* buffer, size_t length)
{
    if (usePipe)
    {
        return PipeRecvAll(pipe, buffer, length);
    }
    return TcpRecvAll(sock, buffer, length);
}

// ----------------------------------------------------------------------------
// Helper: Read a full usbmuxd response message (header + body).
// Returns the plist body as a string. Returns empty string on error.
// ----------------------------------------------------------------------------
static HRESULT ReadUsbmuxResponse(HANDLE pipe, SOCKET sock, bool usePipe,
                                   std::string& outBody)
{
    uint8_t header[kUsbmuxHeaderSize];
    HRESULT hr = RecvBytes(pipe, sock, usePipe, header, kUsbmuxHeaderSize);
    if (FAILED(hr)) return hr;

    uint32_t totalLength = ReadLE32(header + 0);
    if (totalLength < kUsbmuxHeaderSize || totalLength > kMaxMessageSize)
    {
        return E_UNEXPECTED;
    }

    uint32_t bodyLength = totalLength - kUsbmuxHeaderSize;
    if (bodyLength == 0)
    {
        outBody.clear();
        return S_OK;
    }

    std::vector<uint8_t> body(bodyLength);
    hr = RecvBytes(pipe, sock, usePipe, body.data(), bodyLength);
    if (FAILED(hr)) return hr;

    outBody.assign(body.begin(), body.end());
    return S_OK;
}

// ----------------------------------------------------------------------------
// Helper: Extract a string value from a simple XML plist response.
// Searches for <key>keyName</key> followed by <string>value</string>.
// This is a minimal parser sufficient for usbmuxd responses.
// ----------------------------------------------------------------------------
static std::string ExtractPlistString(const std::string& plist,
                                      const std::string& keyName)
{
    std::string keyTag = "<key>" + keyName + "</key>";
    size_t pos = plist.find(keyTag);
    if (pos == std::string::npos) return "";

    pos = plist.find("<string>", pos + keyTag.size());
    if (pos == std::string::npos) return "";
    pos += 8; // skip "<string>"

    size_t end = plist.find("</string>", pos);
    if (end == std::string::npos) return "";

    return plist.substr(pos, end - pos);
}

// ----------------------------------------------------------------------------
// Helper: Extract an integer value from a simple XML plist response.
// Searches for <key>keyName</key> followed by <integer>value</integer>.
// ----------------------------------------------------------------------------
static int ExtractPlistInteger(const std::string& plist,
                               const std::string& keyName, int defaultValue = -1)
{
    std::string keyTag = "<key>" + keyName + "</key>";
    size_t pos = plist.find(keyTag);
    if (pos == std::string::npos) return defaultValue;

    pos = plist.find("<integer>", pos + keyTag.size());
    if (pos == std::string::npos) return defaultValue;
    pos += 9; // skip "<integer>"

    size_t end = plist.find("</integer>", pos);
    if (end == std::string::npos) return defaultValue;

    try
    {
        return std::stoi(plist.substr(pos, end - pos));
    }
    catch (...)
    {
        return defaultValue;
    }
}

// ----------------------------------------------------------------------------
// Helper: Extract all device entries from a ListDevices response.
// The response contains <array> of <dict> entries with Properties sub-dicts.
// Each device has: DeviceID (int), SerialNumber (string), ConnectionType (string)
// ----------------------------------------------------------------------------
static std::vector<AmdsClient::Device> ParseDeviceList(const std::string& plist)
{
    std::vector<AmdsClient::Device> devices;

    // Find each DeviceID/SerialNumber pair within the device list.
    // We look for USB-connected devices only (ConnectionType = USB).
    size_t searchPos = 0;
    while (true)
    {
        // Find next DeviceID entry
        size_t deviceIdPos = plist.find("<key>DeviceID</key>", searchPos);
        if (deviceIdPos == std::string::npos) break;

        // Check if this device is USB connected
        size_t connTypePos = plist.find("<key>ConnectionType</key>", deviceIdPos);
        size_t nextDevicePos = plist.find("<key>DeviceID</key>",
                                          deviceIdPos + 19);
        // Only consider ConnectionType within this device's scope
        bool isUsb = false;
        if (connTypePos != std::string::npos &&
            (nextDevicePos == std::string::npos || connTypePos < nextDevicePos))
        {
            std::string connType = ExtractPlistString(
                plist.substr(connTypePos), "ConnectionType");
            isUsb = (connType == "USB");
        }
        else
        {
            // If no ConnectionType found, assume USB (legacy behavior)
            isUsb = true;
        }

        if (isUsb)
        {
            int deviceID = ExtractPlistInteger(plist.substr(deviceIdPos),
                                               "DeviceID");
            std::string udid = ExtractPlistString(plist.substr(deviceIdPos),
                                                  "SerialNumber");
            if (deviceID >= 0 && !udid.empty())
            {
                AmdsClient::Device dev;
                dev.deviceID = deviceID;
                dev.udid = udid;
                dev.name = "iPhone / iPad (USB)"; // fallback, resolved later
                devices.push_back(std::move(dev));
            }
        }

        searchPos = deviceIdPos + 19;
    }

    return devices;
}

} // anonymous namespace

// ============================================================================
// AmdsClient public implementation
// ============================================================================

// ----------------------------------------------------------------------------
// Connect: Establish connection to AMDS service.
//
// Strategy: Try named pipe first (\\.\pipe\usbmux), which is the preferred
// transport. If the pipe is unavailable, fall back to TCP (localhost:27015).
// If neither is available, AMDS is not installed — the user needs iTunes or
// Apple Devices from the Microsoft Store.
//
// Validates: Requirements 4.1, 4.7
// ----------------------------------------------------------------------------
HRESULT AmdsClient::Connect()
{
    // Close any existing connection first
    if (m_pipe != INVALID_HANDLE_VALUE)
    {
        CloseHandle(m_pipe);
        m_pipe = INVALID_HANDLE_VALUE;
    }
    if (m_tcpSocket != INVALID_SOCKET)
    {
        closesocket(m_tcpSocket);
        m_tcpSocket = INVALID_SOCKET;
    }
    m_usePipe = false;

    // Attempt 1: Named pipe (\\.\pipe\usbmux)
    HANDLE pipe = CreateFileA(
        kPipePath,
        GENERIC_READ | GENERIC_WRITE,
        0,              // no sharing
        nullptr,        // default security
        OPEN_EXISTING,
        0,              // default attributes
        nullptr);       // no template

    if (pipe != INVALID_HANDLE_VALUE)
    {
        // Set pipe to message mode for framed communication
        DWORD mode = PIPE_READMODE_BYTE;
        SetNamedPipeHandleState(pipe, &mode, nullptr, nullptr);

        m_pipe = pipe;
        m_usePipe = true;
        return S_OK;
    }

    // Attempt 2: TCP fallback (localhost:27015)
    addrinfo hints = {};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    addrinfo* result = nullptr;
    std::string portStr = std::to_string(kTcpPort);
    int ret = ::getaddrinfo(kTcpHost, portStr.c_str(), &hints, &result);
    if (ret != 0 || !result)
    {
        // AMDS not available — instruct user to install iTunes/Apple Devices
        return HRESULT_FROM_WIN32(ERROR_SERVICE_NOT_FOUND);
    }

    SOCKET sock = ::socket(result->ai_family, result->ai_socktype,
                           result->ai_protocol);
    if (sock == INVALID_SOCKET)
    {
        ::freeaddrinfo(result);
        return HRESULT_FROM_WIN32(ERROR_SERVICE_NOT_FOUND);
    }

    if (::connect(sock, result->ai_addr,
                  static_cast<int>(result->ai_addrlen)) != 0)
    {
        ::closesocket(sock);
        ::freeaddrinfo(result);
        // Neither pipe nor TCP available — AMDS is not installed
        return HRESULT_FROM_WIN32(ERROR_SERVICE_NOT_FOUND);
    }

    ::freeaddrinfo(result);

    // Disable Nagle for responsive protocol messaging
    int noDelay = 1;
    ::setsockopt(sock, IPPROTO_TCP, TCP_NODELAY,
                 reinterpret_cast<const char*>(&noDelay), sizeof(noDelay));

    m_tcpSocket = sock;
    m_usePipe = false;
    return S_OK;
}

// ----------------------------------------------------------------------------
// ListDevices: Enumerate currently attached iOS devices.
//
// Sends a ListDevices plist request and parses the response to extract
// device information. Only USB-connected devices are returned (WiFi-paired
// devices are handled by the Bonjour discovery path).
//
// Validates: Requirements 4.2
// ----------------------------------------------------------------------------
HRESULT AmdsClient::ListDevices(std::vector<Device>& devices)
{
    devices.clear();

    if (!IsAvailable())
    {
        return E_NOT_VALID_STATE;
    }

    // Build the ListDevices request plist
    std::string plistBody = BuildPlist(
        {{"MessageType", "ListDevices"},
         {"ProgName", "OpenDisplay"},
         {"ClientVersionString", "OpenDisplay"}});

    std::vector<uint8_t> message = BuildUsbmuxMessage(plistBody);

    // Send request
    HRESULT hr = SendMessage(m_pipe, m_tcpSocket, m_usePipe, message);
    if (FAILED(hr)) return hr;

    // Read response
    std::string responseBody;
    hr = ReadUsbmuxResponse(m_pipe, m_tcpSocket, m_usePipe, responseBody);
    if (FAILED(hr)) return hr;

    if (responseBody.empty())
    {
        return S_OK; // No devices — empty list is valid
    }

    // Parse the device list from the response plist
    devices = ParseDeviceList(responseBody);
    return S_OK;
}

// ----------------------------------------------------------------------------
// Subscribe: Subscribe to device attach/detach events.
//
// Sends a Listen request to usbmuxd. After the initial Result response,
// the connection delivers Attached/Detached notifications as they occur.
// This method starts a background thread to read events and invoke the
// callback.
//
// Validates: Requirements 4.2
// ----------------------------------------------------------------------------
HRESULT AmdsClient::Subscribe(DeviceCallback callback)
{
    if (!IsAvailable())
    {
        return E_NOT_VALID_STATE;
    }

    // Build the Listen request plist
    std::string plistBody = BuildPlist(
        {{"MessageType", "Listen"},
         {"ProgName", "OpenDisplay"},
         {"ClientVersionString", "OpenDisplay"}});

    std::vector<uint8_t> message = BuildUsbmuxMessage(plistBody);

    // Send the Listen request
    HRESULT hr = SendMessage(m_pipe, m_tcpSocket, m_usePipe, message);
    if (FAILED(hr)) return hr;

    // Read the initial Result response (should be Result with Number=0)
    std::string responseBody;
    hr = ReadUsbmuxResponse(m_pipe, m_tcpSocket, m_usePipe, responseBody);
    if (FAILED(hr)) return hr;

    // Verify it's a successful result
    if (!responseBody.empty())
    {
        int resultNum = ExtractPlistInteger(responseBody, "Number", -1);
        if (resultNum != 0)
        {
            return E_FAIL;
        }
    }

    // Start a background thread to read attach/detach events.
    // We capture the connection state by value since this thread owns it
    // for the lifetime of the subscription.
    HANDLE pipe = m_pipe;
    SOCKET sock = m_tcpSocket;
    bool usePipe = m_usePipe;

    std::thread eventThread([pipe, sock, usePipe, callback]() {
        while (true)
        {
            std::string eventBody;
            HRESULT hr = ReadUsbmuxResponse(pipe, sock, usePipe, eventBody);
            if (FAILED(hr)) break; // Connection lost

            if (eventBody.empty()) continue;

            std::string msgType = ExtractPlistString(eventBody, "MessageType");

            if (msgType == "Attached")
            {
                int deviceID = ExtractPlistInteger(eventBody, "DeviceID");
                std::string udid = ExtractPlistString(eventBody, "SerialNumber");

                // Check ConnectionType is USB
                std::string connType = ExtractPlistString(eventBody,
                                                          "ConnectionType");
                if (connType != "USB" && !connType.empty()) continue;

                if (deviceID >= 0 && !udid.empty())
                {
                    Device dev;
                    dev.deviceID = deviceID;
                    dev.udid = udid;
                    dev.name = "iPhone / iPad (USB)";
                    callback(dev, true);
                }
            }
            else if (msgType == "Detached")
            {
                int deviceID = ExtractPlistInteger(eventBody, "DeviceID");
                if (deviceID >= 0)
                {
                    Device dev;
                    dev.deviceID = deviceID;
                    dev.udid = "";
                    dev.name = "";
                    callback(dev, false);
                }
            }
        }
    });

    eventThread.detach();
    return S_OK;
}

// ----------------------------------------------------------------------------
// CreateTunnel: Establish a TCP tunnel to a port on the iOS device.
//
// Sends a Connect request to usbmuxd with the DeviceID and PortNumber.
// On success (Result Number=0), the underlying connection becomes a
// transparent byte pipe to the device's TCP port. We return a new socket
// or pipe handle wrapped as a SOCKET for consistency with the transport
// layer.
//
// Note: The port number must be in network byte order (big-endian) per the
// usbmuxd protocol. We swap bytes here: (port << 8) | (port >> 8) for
// a 16-bit value packed into an integer field.
//
// Validates: Requirements 4.2, 4.5
// ----------------------------------------------------------------------------
HRESULT AmdsClient::CreateTunnel(int deviceID, uint16_t port, SOCKET& outSocket)
{
    outSocket = INVALID_SOCKET;

    // We need a fresh connection to AMDS for the tunnel, because after a
    // successful Connect, the underlying transport becomes the tunnel itself.
    // Open a new pipe or TCP connection.
    HANDLE tunnelPipe = INVALID_HANDLE_VALUE;
    SOCKET tunnelSock = INVALID_SOCKET;
    bool tunnelUsePipe = false;

    // Try pipe first
    tunnelPipe = CreateFileA(
        kPipePath,
        GENERIC_READ | GENERIC_WRITE,
        0, nullptr, OPEN_EXISTING, 0, nullptr);

    if (tunnelPipe != INVALID_HANDLE_VALUE)
    {
        DWORD mode = PIPE_READMODE_BYTE;
        SetNamedPipeHandleState(tunnelPipe, &mode, nullptr, nullptr);
        tunnelUsePipe = true;
    }
    else
    {
        // TCP fallback
        addrinfo hints = {};
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_protocol = IPPROTO_TCP;

        addrinfo* result = nullptr;
        std::string portStr = std::to_string(kTcpPort);
        int ret = ::getaddrinfo(kTcpHost, portStr.c_str(), &hints, &result);
        if (ret != 0 || !result)
        {
            return E_FAIL;
        }

        tunnelSock = ::socket(result->ai_family, result->ai_socktype,
                              result->ai_protocol);
        if (tunnelSock == INVALID_SOCKET)
        {
            ::freeaddrinfo(result);
            return E_FAIL;
        }

        if (::connect(tunnelSock, result->ai_addr,
                      static_cast<int>(result->ai_addrlen)) != 0)
        {
            ::closesocket(tunnelSock);
            ::freeaddrinfo(result);
            return E_FAIL;
        }
        ::freeaddrinfo(result);

        int noDelay = 1;
        ::setsockopt(tunnelSock, IPPROTO_TCP, TCP_NODELAY,
                     reinterpret_cast<const char*>(&noDelay), sizeof(noDelay));
        tunnelUsePipe = false;
    }

    // Port in network byte order (big-endian uint16 packed into int)
    int portNetworkOrder = static_cast<int>(
        (static_cast<uint16_t>(port) << 8) |
        (static_cast<uint16_t>(port) >> 8));

    // Build Connect request
    std::string plistBody = BuildPlist(
        {{"MessageType", "Connect"},
         {"ProgName", "OpenDisplay"},
         {"ClientVersionString", "OpenDisplay"}},
        {{"DeviceID", deviceID},
         {"PortNumber", portNetworkOrder}});

    std::vector<uint8_t> message = BuildUsbmuxMessage(plistBody);

    // Send Connect request
    HRESULT hr = SendMessage(tunnelPipe, tunnelSock, tunnelUsePipe, message);
    if (FAILED(hr))
    {
        if (tunnelUsePipe) CloseHandle(tunnelPipe);
        else ::closesocket(tunnelSock);
        return hr;
    }

    // Read Connect result
    std::string responseBody;
    hr = ReadUsbmuxResponse(tunnelPipe, tunnelSock, tunnelUsePipe, responseBody);
    if (FAILED(hr))
    {
        if (tunnelUsePipe) CloseHandle(tunnelPipe);
        else ::closesocket(tunnelSock);
        return hr;
    }

    // Check result — Number=0 means success
    int resultNum = ExtractPlistInteger(responseBody, "Number", -1);
    if (resultNum != 0)
    {
        if (tunnelUsePipe) CloseHandle(tunnelPipe);
        else ::closesocket(tunnelSock);

        // Result code 3 = connection refused (app not running on device)
        if (resultNum == 3) return HRESULT_FROM_WIN32(ERROR_CONNECTION_REFUSED);
        // Result code 2 = bad device (unplugged)
        if (resultNum == 2) return HRESULT_FROM_WIN32(ERROR_DEVICE_NOT_CONNECTED);
        return E_FAIL;
    }

    // Success! The connection is now a transparent tunnel to the device port.
    // For pipe-based tunnels, we need to return a SOCKET abstraction.
    // Since the WireTransport expects a SOCKET, for pipe tunnels we create
    // a socket pair or use the TCP socket directly.
    if (tunnelUsePipe)
    {
        // The pipe is now a raw byte tunnel to the device.
        // Store it and return a sentinel — the caller will need to use
        // pipe-based I/O. For the WireTransport integration (task 6.1),
        // we wrap this in a socket-like interface.
        // For now, return INVALID_SOCKET and let the caller know the
        // tunnel is pipe-based by checking the pipe handle.
        // In practice, AMDS on Windows with iTunes installed typically
        // provides TCP on 27015, so pipe tunnels are rare.
        CloseHandle(tunnelPipe);
        return E_NOTIMPL; // Pipe-based tunnel not yet wrapped as socket
    }

    // TCP tunnel — the socket is now a direct pipe to the device port.
    outSocket = tunnelSock;
    return S_OK;
}

// ----------------------------------------------------------------------------
// GetDeviceName: Resolve the device's friendly name via lockdownd.
//
// Lockdownd runs on port 62078 on the iOS device. We tunnel to it via AMDS,
// then send a GetValue request for DeviceName. Lockdownd uses a different
// framing than usbmuxd: [4B BE length][XML plist body].
//
// This has a timeout to avoid blocking the UI thread if the device is
// unresponsive.
//
// Validates: Requirements 4.3, 4.4
// ----------------------------------------------------------------------------
HRESULT AmdsClient::GetDeviceName(int deviceID, std::string& outName,
                                   uint32_t timeoutMs)
{
    outName.clear();

    // Create a tunnel to lockdownd (port 62078) on the device
    SOCKET lockdownSock = INVALID_SOCKET;
    HRESULT hr = CreateTunnel(deviceID, kLockdownPort, lockdownSock);
    if (FAILED(hr))
    {
        // Fallback: use generic name if we can't reach lockdownd
        outName = "iPhone / iPad (USB)";
        return hr;
    }

    // Set receive timeout on the socket
    DWORD timeout = static_cast<DWORD>(timeoutMs);
    ::setsockopt(lockdownSock, SOL_SOCKET, SO_RCVTIMEO,
                 reinterpret_cast<const char*>(&timeout), sizeof(timeout));

    // Build the lockdownd GetValue request for DeviceName.
    // Lockdownd framing: [4B BE body-length][XML plist body]
    std::string lockdownPlist = BuildPlist(
        {{"Request", "GetValue"},
         {"Key", "DeviceName"},
         {"Label", "OpenDisplay"}});

    // Build the framed lockdownd message (big-endian length prefix)
    uint32_t bodyLen = static_cast<uint32_t>(lockdownPlist.size());
    std::vector<uint8_t> lockdownMsg(4 + bodyLen);
    WriteAmds_BE32(lockdownMsg.data(), bodyLen);
    std::memcpy(lockdownMsg.data() + 4, lockdownPlist.data(), bodyLen);

    // Send the request over the tunnel
    hr = TcpSendAll(lockdownSock, lockdownMsg.data(), lockdownMsg.size());
    if (FAILED(hr))
    {
        ::closesocket(lockdownSock);
        outName = "iPhone / iPad (USB)";
        return hr;
    }

    // Read the response header (4 bytes, big-endian length)
    uint8_t respHeader[4];
    hr = TcpRecvAll(lockdownSock, respHeader, 4);
    if (FAILED(hr))
    {
        ::closesocket(lockdownSock);
        outName = "iPhone / iPad (USB)";
        return hr;
    }

    uint32_t respLen = ReadAmds_BE32(respHeader);
    if (respLen == 0 || respLen > 65536)
    {
        ::closesocket(lockdownSock);
        outName = "iPhone / iPad (USB)";
        return E_UNEXPECTED;
    }

    // Read the response body
    std::vector<uint8_t> respBody(respLen);
    hr = TcpRecvAll(lockdownSock, respBody.data(), respLen);
    ::closesocket(lockdownSock);

    if (FAILED(hr))
    {
        outName = "iPhone / iPad (USB)";
        return hr;
    }

    // Parse the DeviceName from the response plist
    std::string respStr(respBody.begin(), respBody.end());
    std::string name = ExtractPlistString(respStr, "Value");

    if (name.empty())
    {
        outName = "iPhone / iPad (USB)";
        return S_FALSE; // Got response but no name — use fallback
    }

    outName = name;
    return S_OK;
}

// ----------------------------------------------------------------------------
// IsAvailable: Check if we have an active connection to AMDS.
//
// Returns true if either the named pipe or TCP socket is connected.
// Returns false if AMDS is not installed/running (the user needs to install
// iTunes or Apple Devices from the Microsoft Store).
//
// Validates: Requirements 4.7
// ----------------------------------------------------------------------------
bool AmdsClient::IsAvailable() const
{
    return (m_usePipe && m_pipe != INVALID_HANDLE_VALUE) ||
           (!m_usePipe && m_tcpSocket != INVALID_SOCKET);
}
