#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "ProtocolHandler.h"

using json = nlohmann::json;

// ─── Test Fixture ────────────────────────────────────────────────────────────

class ProtocolHandlerTest : public ::testing::Test {
protected:
    void SetUp() override {
        m_sentMessages.clear();
        m_config.senderPv = 2;
        m_config.senderMinPeer = 1;
        m_config.appStoreUrl = "https://apps.apple.com/app/opendisplay/id6741082870";

        m_handler = std::make_unique<ProtocolHandler>(
            [this](const std::string& msg) {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_sentMessages.push_back(msg);
            },
            m_config);
    }

    void TearDown() override {
        if (m_handler) {
            m_handler->StopPingTimer();
        }
    }

    std::vector<std::string> GetSentMessages() {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_sentMessages;
    }

    json GetSentJson(size_t index) {
        auto msgs = GetSentMessages();
        EXPECT_LT(index, msgs.size());
        return json::parse(msgs[index]);
    }

    ProtocolConfig m_config;
    std::unique_ptr<ProtocolHandler> m_handler;
    std::vector<std::string> m_sentMessages;
    std::mutex m_mutex;
};

// ─── Hello / Welcome Tests ───────────────────────────────────────────────────

TEST_F(ProtocolHandlerTest, HelloExtractsAllFields) {
    json hello = {
        {"type", "hello"},
        {"pixelsWide", 2048},
        {"pixelsHigh", 1536},
        {"scale", 2.0},
        {"device", "iPad"},
        {"id", "abc-123"},
        {"pv", 2}
    };

    m_handler->HandleMessage(hello);

    auto info = m_handler->GetLastHello();
    EXPECT_EQ(info.pixelsWide, 2048);
    EXPECT_EQ(info.pixelsHigh, 1536);
    EXPECT_DOUBLE_EQ(info.scale, 2.0);
    EXPECT_EQ(info.device, "iPad");
    EXPECT_EQ(info.id, "abc-123");
    EXPECT_EQ(info.pv, 2);
    EXPECT_TRUE(m_handler->IsSessionActive());
}

TEST_F(ProtocolHandlerTest, HelloDefaultsPvTo1WhenAbsent) {
    json hello = {
        {"type", "hello"},
        {"pixelsWide", 2048},
        {"pixelsHigh", 1536},
        {"scale", 2.0}
    };

    m_handler->HandleMessage(hello);

    auto info = m_handler->GetLastHello();
    EXPECT_EQ(info.pv, 1);
}

TEST_F(ProtocolHandlerTest, HelloOptionalFieldsDefaultToEmpty) {
    json hello = {
        {"type", "hello"},
        {"pixelsWide", 1024},
        {"pixelsHigh", 768},
        {"scale", 3.0}
    };

    m_handler->HandleMessage(hello);

    auto info = m_handler->GetLastHello();
    EXPECT_EQ(info.device, "");
    EXPECT_EQ(info.id, "");
}

TEST_F(ProtocolHandlerTest, HelloSendsWelcomeResponse) {
    json hello = {
        {"type", "hello"},
        {"pixelsWide", 2048},
        {"pixelsHigh", 1536},
        {"scale", 2.0},
        {"pv", 2}
    };

    m_handler->HandleMessage(hello);

    auto msgs = GetSentMessages();
    ASSERT_GE(msgs.size(), 1u);

    auto welcome = json::parse(msgs[0]);
    EXPECT_EQ(welcome["type"], "welcome");
    EXPECT_EQ(welcome["pv"], 2);
    EXPECT_EQ(welcome["min"], 1);
}

TEST_F(ProtocolHandlerTest, WelcomeUsesConfiguredValues) {
    m_config.senderPv = 5;
    m_config.senderMinPeer = 3;
    m_handler = std::make_unique<ProtocolHandler>(
        [this](const std::string& msg) {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_sentMessages.push_back(msg);
        },
        m_config);

    json hello = {
        {"type", "hello"},
        {"pixelsWide", 2048},
        {"pixelsHigh", 1536},
        {"scale", 2.0},
        {"pv", 5}
    };

    m_handler->HandleMessage(hello);

    auto welcome = GetSentJson(0);
    EXPECT_EQ(welcome["pv"], 5);
    EXPECT_EQ(welcome["min"], 3);
}

// ─── Protocol Version Gating Tests ──────────────────────────────────────────

TEST_F(ProtocolHandlerTest, UpdateRequiredSentWhenReceiverPvBelowMin) {
    m_config.senderMinPeer = 2;
    m_handler = std::make_unique<ProtocolHandler>(
        [this](const std::string& msg) {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_sentMessages.push_back(msg);
        },
        m_config);

    json hello = {
        {"type", "hello"},
        {"pixelsWide", 2048},
        {"pixelsHigh", 1536},
        {"scale", 2.0},
        {"device", "iPad"},
        {"pv", 1}
    };

    m_handler->HandleMessage(hello);

    auto msgs = GetSentMessages();
    ASSERT_EQ(msgs.size(), 2u); // welcome + updateRequired

    auto updateRequired = json::parse(msgs[1]);
    EXPECT_EQ(updateRequired["type"], "updateRequired");
    EXPECT_EQ(updateRequired["target"], "ios");
    EXPECT_TRUE(updateRequired.contains("store"));
    EXPECT_TRUE(updateRequired.contains("message"));
    // Message should mention the device kind
    std::string message = updateRequired["message"];
    EXPECT_NE(message.find("iPad"), std::string::npos);
}

TEST_F(ProtocolHandlerTest, UpdateRequiredNotSentWhenReceiverPvMeetsMin) {
    m_config.senderMinPeer = 1;
    m_handler = std::make_unique<ProtocolHandler>(
        [this](const std::string& msg) {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_sentMessages.push_back(msg);
        },
        m_config);

    json hello = {
        {"type", "hello"},
        {"pixelsWide", 2048},
        {"pixelsHigh", 1536},
        {"scale", 2.0},
        {"pv", 1}
    };

    m_handler->HandleMessage(hello);

    auto msgs = GetSentMessages();
    ASSERT_EQ(msgs.size(), 1u); // Only welcome, no updateRequired
}

TEST_F(ProtocolHandlerTest, UpdateRequiredNotSentWhenReceiverPvAboveMin) {
    m_config.senderMinPeer = 1;
    m_handler = std::make_unique<ProtocolHandler>(
        [this](const std::string& msg) {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_sentMessages.push_back(msg);
        },
        m_config);

    json hello = {
        {"type", "hello"},
        {"pixelsWide", 2048},
        {"pixelsHigh", 1536},
        {"scale", 2.0},
        {"pv", 3}
    };

    m_handler->HandleMessage(hello);

    auto msgs = GetSentMessages();
    ASSERT_EQ(msgs.size(), 1u); // Only welcome
}

TEST_F(ProtocolHandlerTest, UpdateRequiredUsesDeviceFieldFromHello) {
    m_config.senderMinPeer = 2;
    m_handler = std::make_unique<ProtocolHandler>(
        [this](const std::string& msg) {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_sentMessages.push_back(msg);
        },
        m_config);

    json hello = {
        {"type", "hello"},
        {"pixelsWide", 2048},
        {"pixelsHigh", 1536},
        {"scale", 2.0},
        {"device", "iPhone"},
        {"pv", 1}
    };

    m_handler->HandleMessage(hello);

    auto updateRequired = GetSentJson(1);
    std::string message = updateRequired["message"];
    EXPECT_NE(message.find("iPhone"), std::string::npos);
}

TEST_F(ProtocolHandlerTest, UpdateRequiredUsesDeviceFallbackWhenAbsent) {
    m_config.senderMinPeer = 2;
    m_handler = std::make_unique<ProtocolHandler>(
        [this](const std::string& msg) {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_sentMessages.push_back(msg);
        },
        m_config);

    // No "device" field
    json hello = {
        {"type", "hello"},
        {"pixelsWide", 2048},
        {"pixelsHigh", 1536},
        {"scale", 2.0},
        {"pv", 1}
    };

    m_handler->HandleMessage(hello);

    auto updateRequired = GetSentJson(1);
    std::string message = updateRequired["message"];
    EXPECT_NE(message.find("device"), std::string::npos);
}

// ─── Ping / Pong Tests ──────────────────────────────────────────────────────

TEST_F(ProtocolHandlerTest, PingTimerSendsPingMessages) {
    json hello = {
        {"type", "hello"},
        {"pixelsWide", 2048},
        {"pixelsHigh", 1536},
        {"scale", 2.0},
        {"pv", 2}
    };
    m_handler->HandleMessage(hello);

    // Clear the welcome message
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_sentMessages.clear();
    }

    m_handler->StartPingTimer();

    // Wait enough for at least one ping (2s interval + buffer)
    std::this_thread::sleep_for(std::chrono::milliseconds(2500));

    m_handler->StopPingTimer();

    auto msgs = GetSentMessages();
    ASSERT_GE(msgs.size(), 1u);

    auto ping = json::parse(msgs[0]);
    EXPECT_EQ(ping["type"], "ping");
    EXPECT_TRUE(ping.contains("t"));
    EXPECT_TRUE(ping["t"].is_number());

    // Timestamp should be close to now (within 5 seconds)
    auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    int64_t pingT = ping["t"].get<int64_t>();
    EXPECT_LT(std::abs(now - pingT), 5000);
}

TEST_F(ProtocolHandlerTest, PongCallbackWithRtt) {
    int64_t receivedRtt = -1;
    ProtocolCallbacks callbacks;
    callbacks.onPong = [&](int64_t rtt) { receivedRtt = rtt; };
    m_handler->SetCallbacks(callbacks);

    auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    json pong = {
        {"type", "pong"},
        {"t", now - 50},  // Simulating 50ms RTT
        {"mt", now}
    };

    m_handler->HandleMessage(pong);

    EXPECT_GE(receivedRtt, 48); // Allow small timing variance
    EXPECT_LE(receivedRtt, 100);
}

// ─── Sleeping / Closing Tests ────────────────────────────────────────────────

TEST_F(ProtocolHandlerTest, SleepingInvokesCallback) {
    bool sleepingCalled = false;
    ProtocolCallbacks callbacks;
    callbacks.onSleeping = [&]() { sleepingCalled = true; };
    m_handler->SetCallbacks(callbacks);

    json msg = {{"type", "sleeping"}};
    m_handler->HandleMessage(msg);

    EXPECT_TRUE(sleepingCalled);
}

TEST_F(ProtocolHandlerTest, ClosingInvokesCallbackAndEndsSession) {
    // First establish a session
    json hello = {
        {"type", "hello"},
        {"pixelsWide", 2048},
        {"pixelsHigh", 1536},
        {"scale", 2.0},
        {"pv", 2}
    };
    m_handler->HandleMessage(hello);
    EXPECT_TRUE(m_handler->IsSessionActive());

    bool closingCalled = false;
    ProtocolCallbacks callbacks;
    callbacks.onClosing = [&]() { closingCalled = true; };
    m_handler->SetCallbacks(callbacks);

    json msg = {{"type", "closing"}};
    m_handler->HandleMessage(msg);

    EXPECT_TRUE(closingCalled);
    EXPECT_FALSE(m_handler->IsSessionActive());
}

// ─── Unknown Message Type Tests ──────────────────────────────────────────────

TEST_F(ProtocolHandlerTest, UnknownTypeDoesNotCrashOrDisconnect) {
    json msg = {{"type", "futureFeature"}, {"data", 42}};

    // Should not throw and session state should be unchanged
    EXPECT_NO_THROW(m_handler->HandleMessage(msg));
    EXPECT_FALSE(m_handler->IsSessionActive());
}

TEST_F(ProtocolHandlerTest, MessageWithoutTypeFieldIsIgnored) {
    json msg = {{"data", "no type field"}};

    EXPECT_NO_THROW(m_handler->HandleMessage(msg));
}

TEST_F(ProtocolHandlerTest, UnknownTypeAfterSessionDoesNotEndIt) {
    // Establish session
    json hello = {
        {"type", "hello"},
        {"pixelsWide", 2048},
        {"pixelsHigh", 1536},
        {"scale", 2.0},
        {"pv", 2}
    };
    m_handler->HandleMessage(hello);
    EXPECT_TRUE(m_handler->IsSessionActive());

    // Unknown message should not affect session
    json unknown = {{"type", "someNewFeature"}};
    m_handler->HandleMessage(unknown);

    EXPECT_TRUE(m_handler->IsSessionActive());
}

// ─── Hello Callback Test ─────────────────────────────────────────────────────

TEST_F(ProtocolHandlerTest, HelloCallbackIsInvoked) {
    HelloInfo receivedInfo;
    bool helloCalled = false;
    ProtocolCallbacks callbacks;
    callbacks.onHello = [&](const HelloInfo& info) {
        receivedInfo = info;
        helloCalled = true;
    };
    m_handler->SetCallbacks(callbacks);

    json hello = {
        {"type", "hello"},
        {"pixelsWide", 2732},
        {"pixelsHigh", 2048},
        {"scale", 2.0},
        {"device", "iPad"},
        {"id", "test-id"},
        {"pv", 2}
    };

    m_handler->HandleMessage(hello);

    EXPECT_TRUE(helloCalled);
    EXPECT_EQ(receivedInfo.pixelsWide, 2732);
    EXPECT_EQ(receivedInfo.pixelsHigh, 2048);
    EXPECT_EQ(receivedInfo.device, "iPad");
    EXPECT_EQ(receivedInfo.id, "test-id");
}

// ─── Multiple Hello (Rotation) Test ─────────────────────────────────────────

TEST_F(ProtocolHandlerTest, SecondHelloUpdatesStoredInfo) {
    json hello1 = {
        {"type", "hello"},
        {"pixelsWide", 2048},
        {"pixelsHigh", 1536},
        {"scale", 2.0},
        {"pv", 2}
    };
    m_handler->HandleMessage(hello1);

    // Rotated — swapped dimensions
    json hello2 = {
        {"type", "hello"},
        {"pixelsWide", 1536},
        {"pixelsHigh", 2048},
        {"scale", 2.0},
        {"pv", 2}
    };
    m_handler->HandleMessage(hello2);

    auto info = m_handler->GetLastHello();
    EXPECT_EQ(info.pixelsWide, 1536);
    EXPECT_EQ(info.pixelsHigh, 2048);
}
