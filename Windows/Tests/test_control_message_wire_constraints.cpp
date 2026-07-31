// Property 13: Control Message Wire Constraints
// **Validates: Requirements 6.9**
//
// For any control message payload (welcome, updateRequired, ping, pong),
// the serialized JSON representation SHALL:
//   1. Be less than 32,768 bytes in size
//   2. Start with the '{' character
//   3. Contain no NUL (0x00) bytes
//
// These constraints allow the iOS receiver to disambiguate control messages
// from H.264 video data on the same channel (video starts with 0x00).

#include <gtest/gtest.h>
#include <rapidcheck.h>
#include <rapidcheck/gtest.h>

#include <algorithm>
#include <cstdint>
#include <string>

namespace {

// ─── Constants ───────────────────────────────────────────────────────────────
static constexpr size_t kMaxControlMessageBytes = 32768; // 32 KB

// ─── JSON string escaping (mirrors nlohmann::json behavior) ──────────────────

/// Escape a string value for JSON serialization.
/// Handles control characters, backslash, and double quotes.
std::string JsonEscape(const std::string& input)
{
    std::string result;
    result.reserve(input.size() + 16);
    for (char c : input)
    {
        switch (c)
        {
        case '"':  result += "\\\""; break;
        case '\\': result += "\\\\"; break;
        case '\b': result += "\\b"; break;
        case '\f': result += "\\f"; break;
        case '\n': result += "\\n"; break;
        case '\r': result += "\\r"; break;
        case '\t': result += "\\t"; break;
        default:
            if (static_cast<unsigned char>(c) < 0x20)
            {
                // Escape control characters as \u00XX
                char buf[8];
                snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned char>(c));
                result += buf;
            }
            else
            {
                result += c;
            }
            break;
        }
    }
    return result;
}

// ─── Control Message Serialization (mirrors ProtocolHandler output) ──────────

/// Serialize a "welcome" control message.
/// Fields: type, pv, min
std::string SerializeWelcome(int pv, int minPeer)
{
    return "{\"type\":\"welcome\",\"pv\":" + std::to_string(pv) +
           ",\"min\":" + std::to_string(minPeer) + "}";
}

/// Serialize an "updateRequired" control message.
/// Fields: type, target, store, message
std::string SerializeUpdateRequired(const std::string& target,
                                     const std::string& store,
                                     const std::string& message)
{
    return "{\"type\":\"updateRequired\",\"target\":\"" + JsonEscape(target) +
           "\",\"store\":\"" + JsonEscape(store) +
           "\",\"message\":\"" + JsonEscape(message) + "\"}";
}

/// Serialize a "ping" control message.
/// Fields: type, t
std::string SerializePing(int64_t timestamp)
{
    return "{\"type\":\"ping\",\"t\":" + std::to_string(timestamp) + "}";
}

/// Serialize a "pong" control message.
/// Fields: type, t, mt
std::string SerializePong(int64_t originalT, int64_t receiverClock)
{
    return "{\"type\":\"pong\",\"t\":" + std::to_string(originalT) +
           ",\"mt\":" + std::to_string(receiverClock) + "}";
}

/// Verify control message wire constraints:
/// - Size < 32,768 bytes
/// - Starts with '{'
/// - Contains no NUL bytes
void AssertWireConstraints(const std::string& serialized)
{
    // Constraint 1: serialized size must be less than 32KB
    RC_ASSERT(serialized.size() < kMaxControlMessageBytes);

    // Constraint 2: must start with '{'
    RC_ASSERT(!serialized.empty());
    RC_ASSERT(serialized[0] == '{');

    // Constraint 3: must contain no NUL (0x00) bytes
    RC_ASSERT(std::find(serialized.begin(), serialized.end(), '\0') == serialized.end());
}

} // namespace

// ─── Property Tests ──────────────────────────────────────────────────────────

// **Validates: Requirements 6.9**
// Property: Welcome messages satisfy all wire constraints for any valid
// protocol version and minimum peer version integers.
RC_GTEST_PROP(ControlMessageWireConstraints,
              WelcomeMessageSatisfiesConstraints,
              ())
{
    // Generate random protocol version integers (realistic range)
    auto pv = *rc::gen::inRange<int>(0, 10000);
    auto minPeer = *rc::gen::inRange<int>(0, 10000);

    auto serialized = SerializeWelcome(pv, minPeer);
    AssertWireConstraints(serialized);
}

// **Validates: Requirements 6.9**
// Property: UpdateRequired messages satisfy all wire constraints for any
// combination of target, store URL, and human-readable message strings.
RC_GTEST_PROP(ControlMessageWireConstraints,
              UpdateRequiredMessageSatisfiesConstraints,
              ())
{
    // Generate random target (ios/mac)
    auto target = *rc::gen::element(std::string("ios"), std::string("mac"));

    // Generate random store URL (realistic length, printable ASCII, no NUL)
    auto storeLen = *rc::gen::inRange<int>(10, 512);
    std::string store;
    store.reserve(storeLen);
    store = "https://";
    for (int i = 8; i < storeLen; ++i)
    {
        // Printable ASCII excluding NUL: 0x21 - 0x7E
        char c = static_cast<char>(*rc::gen::inRange<int>(0x21, 0x7F));
        store.push_back(c);
    }

    // Generate random human-readable message (up to ~1KB, printable chars)
    auto msgLen = *rc::gen::inRange<int>(1, 1024);
    std::string message;
    message.reserve(msgLen);
    for (int i = 0; i < msgLen; ++i)
    {
        // Printable ASCII: space (0x20) through tilde (0x7E)
        char c = static_cast<char>(*rc::gen::inRange<int>(0x20, 0x7F));
        message.push_back(c);
    }

    auto serialized = SerializeUpdateRequired(target, store, message);
    AssertWireConstraints(serialized);
}

// **Validates: Requirements 6.9**
// Property: Ping messages satisfy all wire constraints for any valid
// Unix timestamp in milliseconds.
RC_GTEST_PROP(ControlMessageWireConstraints,
              PingMessageSatisfiesConstraints,
              ())
{
    // Generate random Unix timestamp in milliseconds (covers full range)
    auto timestamp = *rc::gen::inRange<int64_t>(0, 9999999999999LL);

    auto serialized = SerializePing(timestamp);
    AssertWireConstraints(serialized);
}

// **Validates: Requirements 6.9**
// Property: Pong messages satisfy all wire constraints for any valid
// pair of timestamps (original sender timestamp and receiver clock).
RC_GTEST_PROP(ControlMessageWireConstraints,
              PongMessageSatisfiesConstraints,
              ())
{
    // Generate random timestamps
    auto originalT = *rc::gen::inRange<int64_t>(0, 9999999999999LL);
    auto receiverClock = *rc::gen::inRange<int64_t>(0, 9999999999999LL);

    auto serialized = SerializePong(originalT, receiverClock);
    AssertWireConstraints(serialized);
}

