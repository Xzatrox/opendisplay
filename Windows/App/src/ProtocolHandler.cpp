#include "ProtocolHandler.h"

#include <chrono>
#include <iostream>
#include <sstream>

// ─── Construction / Destruction ──────────────────────────────────────────────

ProtocolHandler::ProtocolHandler(std::function<void(const std::string&)> sendFn,
                                 ProtocolConfig config)
    : m_sendFn(std::move(sendFn))
    , m_config(std::move(config))
{
}

ProtocolHandler::~ProtocolHandler()
{
    StopPingTimer();
}

// ─── Public Interface ────────────────────────────────────────────────────────

void ProtocolHandler::SetCallbacks(ProtocolCallbacks callbacks)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_callbacks = std::move(callbacks);
}

void ProtocolHandler::HandleMessage(const nlohmann::json& msg)
{
    if (!msg.contains("type") || !msg["type"].is_string()) {
        // No type field — log and continue.
        std::cerr << "[ProtocolHandler] received message without 'type' field, ignoring\n";
        return;
    }

    const std::string type = msg["type"].get<std::string>();

    if (type == "hello") {
        HandleHello(msg);
    } else if (type == "pong") {
        HandlePong(msg);
    } else if (type == "sleeping") {
        HandleSleeping();
    } else if (type == "closing") {
        HandleClosing();
    } else {
        // Unknown message type: log and continue (don't disconnect).
        // Requirements 6.10: log unknown type and continue processing.
        std::cerr << "[ProtocolHandler] unknown message type: \"" << type
                  << "\", ignoring\n";
    }
}

void ProtocolHandler::StartPingTimer()
{
    if (m_pingRunning.exchange(true)) {
        return; // Already running
    }
    m_pingThread = std::thread(&ProtocolHandler::PingLoop, this);
}

void ProtocolHandler::StopPingTimer()
{
    m_pingRunning.store(false);
    if (m_pingThread.joinable()) {
        m_pingThread.join();
    }
}

bool ProtocolHandler::IsSessionActive() const
{
    return m_sessionActive.load();
}

HelloInfo ProtocolHandler::GetLastHello() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_lastHello;
}

// ─── Message Handlers ────────────────────────────────────────────────────────

void ProtocolHandler::HandleHello(const nlohmann::json& msg)
{
    HelloInfo info;

    // Extract required fields (pixelsWide, pixelsHigh, scale).
    if (msg.contains("pixelsWide") && msg["pixelsWide"].is_number()) {
        info.pixelsWide = msg["pixelsWide"].get<int>();
    }
    if (msg.contains("pixelsHigh") && msg["pixelsHigh"].is_number()) {
        info.pixelsHigh = msg["pixelsHigh"].get<int>();
    }
    if (msg.contains("scale") && msg["scale"].is_number()) {
        info.scale = msg["scale"].get<double>();
    }

    // Extract optional fields with defaults.
    if (msg.contains("device") && msg["device"].is_string()) {
        info.device = msg["device"].get<std::string>();
    }
    if (msg.contains("id") && msg["id"].is_string()) {
        info.id = msg["id"].get<std::string>();
    }
    // Protocol version defaults to 1 when absent (matching existing behavior).
    if (msg.contains("pv") && msg["pv"].is_number_integer()) {
        info.pv = msg["pv"].get<int>();
    } else {
        info.pv = 1;
    }

    // Store the hello info.
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_lastHello = info;
    }
    m_sessionActive.store(true);

    // Send welcome response: Requirements 6.2.
    SendWelcome();

    // Protocol version gating: Requirements 6.6.
    // If receiver's protocol version is below our minimum supported peer,
    // send updateRequired message.
    if (info.pv < m_config.senderMinPeer) {
        std::string deviceKind = info.device.empty() ? "device" : info.device;
        SendUpdateRequired(deviceKind);
    }

    // Notify callback.
    std::function<void(const HelloInfo&)> cb;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        cb = m_callbacks.onHello;
    }
    if (cb) {
        cb(info);
    }
}

void ProtocolHandler::HandlePong(const nlohmann::json& msg)
{
    // Extract original timestamp to compute RTT.
    int64_t rttMs = 0;
    if (msg.contains("t") && msg["t"].is_number()) {
        int64_t originalT = msg["t"].get<int64_t>();
        auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch());
        rttMs = now.count() - originalT;
    }

    // Notify callback with the round-trip time.
    std::function<void(int64_t)> cb;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        cb = m_callbacks.onPong;
    }
    if (cb) {
        cb(rttMs);
    }
}

void ProtocolHandler::HandleSleeping()
{
    // Requirements 6.7: tear down virtual display, await wake.
    // The session stays alive for reconnect-on-wake — the SessionController
    // handles the virtual display teardown via the callback.
    std::function<void()> cb;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        cb = m_callbacks.onSleeping;
    }
    if (cb) {
        cb();
    }
}

void ProtocolHandler::HandleClosing()
{
    // Requirements 6.8: end session, no reconnect.
    m_sessionActive.store(false);
    StopPingTimer();

    std::function<void()> cb;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        cb = m_callbacks.onClosing;
    }
    if (cb) {
        cb();
    }
}

// ─── Outgoing Messages ───────────────────────────────────────────────────────

void ProtocolHandler::SendWelcome()
{
    // Requirements 6.2: welcome message with type, pv, min fields.
    nlohmann::json welcome;
    welcome["type"] = "welcome";
    welcome["pv"] = m_config.senderPv;
    welcome["min"] = m_config.senderMinPeer;

    m_sendFn(welcome.dump());
}

void ProtocolHandler::SendUpdateRequired(const std::string& deviceKind)
{
    // Requirements 6.6: updateRequired message with target, store, message.
    nlohmann::json msg;
    msg["type"] = "updateRequired";
    msg["target"] = "ios";
    msg["store"] = m_config.appStoreUrl;
    msg["message"] = "This " + deviceKind +
        " app is too old for this Windows sender. Update OpenDisplay from the"
        " App Store to reconnect.";

    m_sendFn(msg.dump());
}

void ProtocolHandler::SendPing()
{
    // Requirements 6.3: ping with timestamp field "t" in Unix epoch milliseconds.
    auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch());

    nlohmann::json ping;
    ping["type"] = "ping";
    ping["t"] = now.count();

    m_sendFn(ping.dump());
}

// ─── Ping Timer ──────────────────────────────────────────────────────────────

void ProtocolHandler::PingLoop()
{
    while (m_pingRunning.load()) {
        // Sleep in small increments to allow timely shutdown.
        auto deadline = std::chrono::steady_clock::now() + kPingIntervalMs;
        while (std::chrono::steady_clock::now() < deadline) {
            if (!m_pingRunning.load()) {
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        if (m_pingRunning.load() && m_sessionActive.load()) {
            SendPing();
        }
    }
}
