// Property 12: Protocol Version Gating
// **Validates: Requirements 6.6**
//
// For any (receiverPv, senderMinPeer) integer pair, the ProtocolHandler SHALL
// send an "updateRequired" message if and only if receiverPv < senderMinPeer.
// Otherwise the session proceeds normally (only a "welcome" message is sent).
//
// This test exercises the real ProtocolHandler using RapidCheck-generated
// (receiverPv, senderMinPeer) pairs and verifies the correct gating behavior.

#include <gtest/gtest.h>
#include <rapidcheck.h>
#include <rapidcheck/gtest.h>

#include <mutex>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "ProtocolHandler.h"

using json = nlohmann::json;

namespace {

// Helper: create a ProtocolHandler wired to capture sent messages.
struct TestHarness {
    std::vector<std::string> sentMessages;
    std::mutex mutex;
    std::unique_ptr<ProtocolHandler> handler;

    explicit TestHarness(int senderMinPeer, int senderPv = 2) {
        ProtocolConfig config;
        config.senderPv = senderPv;
        config.senderMinPeer = senderMinPeer;
        config.appStoreUrl = "https://apps.apple.com/app/opendisplay/id6741082870";

        handler = std::make_unique<ProtocolHandler>(
            [this](const std::string& msg) {
                std::lock_guard<std::mutex> lock(mutex);
                sentMessages.push_back(msg);
            },
            config);
    }

    ~TestHarness() {
        if (handler) {
            handler->StopPingTimer();
        }
    }

    std::vector<std::string> GetSent() {
        std::lock_guard<std::mutex> lock(mutex);
        return sentMessages;
    }

    void SendHello(int receiverPv) {
        json hello = {
            {"type", "hello"},
            {"pixelsWide", 2048},
            {"pixelsHigh", 1536},
            {"scale", 2.0},
            {"device", "iPad"},
            {"id", "test-install-id"},
            {"pv", receiverPv}
        };
        handler->HandleMessage(hello);
    }
};

// Check if any sent message has type "updateRequired"
bool HasUpdateRequired(const std::vector<std::string>& messages) {
    for (const auto& msg : messages) {
        auto j = json::parse(msg);
        if (j.contains("type") && j["type"] == "updateRequired") {
            return true;
        }
    }
    return false;
}

// Check if any sent message has type "welcome"
bool HasWelcome(const std::vector<std::string>& messages) {
    for (const auto& msg : messages) {
        auto j = json::parse(msg);
        if (j.contains("type") && j["type"] == "welcome") {
            return true;
        }
    }
    return false;
}

} // namespace

// ─── Property Tests ──────────────────────────────────────────────────────────

// **Validates: Requirements 6.6**
// Property: updateRequired is sent if and only if receiverPv < senderMinPeer.
RC_GTEST_PROP(ProtocolVersionGating,
              UpdateRequiredSentIffReceiverBelowMin,
              ()) {
    // Generate random protocol version values in a realistic range.
    // Using [0, 100] covers edge cases like 0 and typical version numbers.
    auto receiverPv = *rc::gen::inRange<int>(0, 101);
    auto senderMinPeer = *rc::gen::inRange<int>(0, 101);

    TestHarness harness(senderMinPeer);
    harness.SendHello(receiverPv);

    auto msgs = harness.GetSent();

    // Welcome should always be sent.
    RC_ASSERT(HasWelcome(msgs));

    if (receiverPv < senderMinPeer) {
        // updateRequired must be sent
        RC_ASSERT(HasUpdateRequired(msgs));
        // Exactly 2 messages: welcome + updateRequired
        RC_ASSERT(msgs.size() == 2u);
    } else {
        // updateRequired must NOT be sent
        RC_ASSERT(!HasUpdateRequired(msgs));
        // Exactly 1 message: welcome only
        RC_ASSERT(msgs.size() == 1u);
    }
}

// **Validates: Requirements 6.6**
// Property: When updateRequired IS sent, it contains the correct fields.
RC_GTEST_PROP(ProtocolVersionGating,
              UpdateRequiredHasCorrectFields,
              ()) {
    // Only generate pairs where receiverPv < senderMinPeer (guaranteed to trigger).
    auto senderMinPeer = *rc::gen::inRange<int>(1, 101);
    auto receiverPv = *rc::gen::inRange<int>(0, senderMinPeer);

    RC_PRE(receiverPv < senderMinPeer);

    TestHarness harness(senderMinPeer);
    harness.SendHello(receiverPv);

    auto msgs = harness.GetSent();
    RC_ASSERT(msgs.size() == 2u);

    auto updateMsg = json::parse(msgs[1]);
    RC_ASSERT(updateMsg["type"] == "updateRequired");
    RC_ASSERT(updateMsg["target"] == "ios");
    RC_ASSERT(updateMsg.contains("store"));
    RC_ASSERT(updateMsg["store"].is_string());
    RC_ASSERT(!updateMsg["store"].get<std::string>().empty());
    RC_ASSERT(updateMsg.contains("message"));
    RC_ASSERT(updateMsg["message"].is_string());
    RC_ASSERT(!updateMsg["message"].get<std::string>().empty());
}

// **Validates: Requirements 6.6**
// Property: When receiverPv >= senderMinPeer, the session proceeds (no
// updateRequired) and the session is marked active.
RC_GTEST_PROP(ProtocolVersionGating,
              SessionProceedsWhenVersionSufficient,
              ()) {
    // Generate pairs where receiverPv >= senderMinPeer.
    auto senderMinPeer = *rc::gen::inRange<int>(0, 100);
    auto receiverPv = *rc::gen::inRange<int>(senderMinPeer, 101);

    RC_PRE(receiverPv >= senderMinPeer);

    TestHarness harness(senderMinPeer);
    harness.SendHello(receiverPv);

    auto msgs = harness.GetSent();

    // Only welcome was sent — no updateRequired.
    RC_ASSERT(msgs.size() == 1u);
    RC_ASSERT(!HasUpdateRequired(msgs));

    // Session should be active (it proceeds).
    RC_ASSERT(harness.handler->IsSessionActive());
}

// **Validates: Requirements 6.6**
// Property: The boundary condition — receiverPv == senderMinPeer does NOT
// trigger updateRequired (only strictly less-than triggers it).
RC_GTEST_PROP(ProtocolVersionGating,
              BoundaryEqualDoesNotTrigger,
              ()) {
    // Same value for both — should NOT trigger updateRequired.
    auto version = *rc::gen::inRange<int>(0, 101);

    TestHarness harness(/*senderMinPeer=*/version);
    harness.SendHello(/*receiverPv=*/version);

    auto msgs = harness.GetSent();
    RC_ASSERT(msgs.size() == 1u);
    RC_ASSERT(!HasUpdateRequired(msgs));
    RC_ASSERT(harness.handler->IsSessionActive());
}
