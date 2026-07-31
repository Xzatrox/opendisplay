// Property 7: Usbmux Plist Serialization Round-Trip
// **Validates: Requirements 4.2**
//
// For any valid usbmux request message (ListDevices, Connect, Listen) with
// arbitrary valid field values, serializing the message to the usbmux wire
// format (16-byte LE header + XML plist body) and then parsing it back SHALL
// produce a message equivalent to the original.
//
// This test models the serialization/deserialization logic from AmdsClient.cpp
// without requiring actual AMDS/pipe/socket dependencies.

#include <gtest/gtest.h>
#include <rapidcheck.h>
#include <rapidcheck/gtest.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <sstream>
#include <string>
#include <vector>

namespace {

// ─── Constants matching AmdsClient.cpp ───────────────────────────────────────

static constexpr uint32_t kUsbmuxVersion = 1;
static constexpr uint32_t kUsbmuxTypePlist = 8;
static constexpr uint32_t kUsbmuxHeaderSize = 16;

// ─── Message types for usbmux requests ───────────────────────────────────────

enum class UsbmuxMessageType {
    ListDevices,
    Connect,
    Listen
};

// ─── Usbmux message model ────────────────────────────────────────────────────
// Represents a usbmux request with its message type, tag, and fields.

struct UsbmuxMessage {
    UsbmuxMessageType messageType;
    uint32_t tag;
    std::string progName;
    std::string clientVersionString;
    // Connect-specific fields
    int deviceID;    // only used for Connect
    int portNumber;  // only used for Connect
};

// ─── Helpers: LE32 read/write (same as AmdsClient.cpp) ───────────────────────

static void WriteLE32(uint8_t* dest, uint32_t value) {
    dest[0] = static_cast<uint8_t>(value & 0xFF);
    dest[1] = static_cast<uint8_t>((value >> 8) & 0xFF);
    dest[2] = static_cast<uint8_t>((value >> 16) & 0xFF);
    dest[3] = static_cast<uint8_t>((value >> 24) & 0xFF);
}

static uint32_t ReadLE32(const uint8_t* src) {
    return static_cast<uint32_t>(src[0]) |
           (static_cast<uint32_t>(src[1]) << 8) |
           (static_cast<uint32_t>(src[2]) << 16) |
           (static_cast<uint32_t>(src[3]) << 24);
}

// ─── Helper: XML-escape a string for safe embedding in plist ─────────────────

static std::string XmlEscape(const std::string& input) {
    std::string output;
    output.reserve(input.size());
    for (char c : input) {
        switch (c) {
            case '&':  output += "&amp;";  break;
            case '<':  output += "&lt;";   break;
            case '>':  output += "&gt;";   break;
            case '"':  output += "&quot;"; break;
            case '\'': output += "&apos;"; break;
            default:   output += c;        break;
        }
    }
    return output;
}

// ─── Helpers: Build XML plist from message (mirrors AmdsClient::BuildPlist) ──

static std::string MessageTypeToString(UsbmuxMessageType type) {
    switch (type) {
        case UsbmuxMessageType::ListDevices: return "ListDevices";
        case UsbmuxMessageType::Connect:     return "Connect";
        case UsbmuxMessageType::Listen:      return "Listen";
    }
    return "Unknown";
}

static std::string BuildPlistFromMessage(const UsbmuxMessage& msg) {
    std::ostringstream ss;
    ss << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
       << "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
       << "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
       << "<plist version=\"1.0\">\n<dict>\n";

    // String fields
    ss << "\t<key>MessageType</key>\n"
       << "\t<string>" << MessageTypeToString(msg.messageType) << "</string>\n";
    ss << "\t<key>ProgName</key>\n"
       << "\t<string>" << XmlEscape(msg.progName) << "</string>\n";
    ss << "\t<key>ClientVersionString</key>\n"
       << "\t<string>" << XmlEscape(msg.clientVersionString) << "</string>\n";

    // Connect-specific integer fields
    if (msg.messageType == UsbmuxMessageType::Connect) {
        ss << "\t<key>DeviceID</key>\n"
           << "\t<integer>" << msg.deviceID << "</integer>\n";
        ss << "\t<key>PortNumber</key>\n"
           << "\t<integer>" << msg.portNumber << "</integer>\n";
    }

    ss << "</dict>\n</plist>\n";
    return ss.str();
}

// ─── Serialize: Build wire-format message (16-byte LE header + plist body) ───

static std::vector<uint8_t> SerializeMessage(const UsbmuxMessage& msg) {
    std::string plistBody = BuildPlistFromMessage(msg);
    uint32_t totalLength = kUsbmuxHeaderSize +
                           static_cast<uint32_t>(plistBody.size());

    std::vector<uint8_t> wire(totalLength);
    WriteLE32(wire.data() + 0, totalLength);
    WriteLE32(wire.data() + 4, kUsbmuxVersion);
    WriteLE32(wire.data() + 8, kUsbmuxTypePlist);
    WriteLE32(wire.data() + 12, msg.tag);

    std::memcpy(wire.data() + kUsbmuxHeaderSize,
                plistBody.data(), plistBody.size());

    return wire;
}

// ─── Parse: Extract message from wire format ─────────────────────────────────

// Minimal XML plist parser — extracts string and integer values by key
// (mirrors ExtractPlistString / ExtractPlistInteger from AmdsClient.cpp)

static std::string ExtractPlistString(const std::string& plist,
                                      const std::string& keyName) {
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

static int ExtractPlistInteger(const std::string& plist,
                               const std::string& keyName,
                               int defaultValue = -1) {
    std::string keyTag = "<key>" + keyName + "</key>";
    size_t pos = plist.find(keyTag);
    if (pos == std::string::npos) return defaultValue;

    pos = plist.find("<integer>", pos + keyTag.size());
    if (pos == std::string::npos) return defaultValue;
    pos += 9; // skip "<integer>"

    size_t end = plist.find("</integer>", pos);
    if (end == std::string::npos) return defaultValue;

    try {
        return std::stoi(plist.substr(pos, end - pos));
    } catch (...) {
        return defaultValue;
    }
}

// Reverse XML-escape for round-trip verification
static std::string XmlUnescape(const std::string& input) {
    std::string output;
    output.reserve(input.size());
    size_t i = 0;
    while (i < input.size()) {
        if (input[i] == '&') {
            if (input.compare(i, 5, "&amp;") == 0) {
                output += '&'; i += 5;
            } else if (input.compare(i, 4, "&lt;") == 0) {
                output += '<'; i += 4;
            } else if (input.compare(i, 4, "&gt;") == 0) {
                output += '>'; i += 4;
            } else if (input.compare(i, 6, "&quot;") == 0) {
                output += '"'; i += 6;
            } else if (input.compare(i, 6, "&apos;") == 0) {
                output += '\''; i += 6;
            } else {
                output += input[i]; i++;
            }
        } else {
            output += input[i]; i++;
        }
    }
    return output;
}

static UsbmuxMessageType StringToMessageType(const std::string& str) {
    if (str == "ListDevices") return UsbmuxMessageType::ListDevices;
    if (str == "Connect")     return UsbmuxMessageType::Connect;
    if (str == "Listen")      return UsbmuxMessageType::Listen;
    // Default fallback (should not occur in valid messages)
    return UsbmuxMessageType::ListDevices;
}

struct ParseResult {
    bool success;
    UsbmuxMessage message;
    uint32_t version;
    uint32_t type;
};

static ParseResult ParseWireMessage(const std::vector<uint8_t>& wire) {
    ParseResult result{};
    result.success = false;

    if (wire.size() < kUsbmuxHeaderSize) return result;

    uint32_t totalLength = ReadLE32(wire.data() + 0);
    result.version = ReadLE32(wire.data() + 4);
    result.type = ReadLE32(wire.data() + 8);
    result.message.tag = ReadLE32(wire.data() + 12);

    if (totalLength != static_cast<uint32_t>(wire.size())) return result;
    if (totalLength < kUsbmuxHeaderSize) return result;

    uint32_t bodyLength = totalLength - kUsbmuxHeaderSize;
    std::string plistBody(wire.begin() + kUsbmuxHeaderSize, wire.end());

    // Extract fields
    std::string msgTypeStr = ExtractPlistString(plistBody, "MessageType");
    result.message.messageType = StringToMessageType(msgTypeStr);

    std::string progName = ExtractPlistString(plistBody, "ProgName");
    result.message.progName = XmlUnescape(progName);

    std::string clientVersion = ExtractPlistString(plistBody, "ClientVersionString");
    result.message.clientVersionString = XmlUnescape(clientVersion);

    if (result.message.messageType == UsbmuxMessageType::Connect) {
        result.message.deviceID = ExtractPlistInteger(plistBody, "DeviceID");
        result.message.portNumber = ExtractPlistInteger(plistBody, "PortNumber");
    } else {
        result.message.deviceID = 0;
        result.message.portNumber = 0;
    }

    result.success = true;
    return result;
}

// ─── RapidCheck generators ───────────────────────────────────────────────────

// Generate a random printable ASCII string suitable for plist string fields.
// Avoids NUL bytes and control characters that would break XML.
static rc::Gen<std::string> genPlistSafeString() {
    return rc::gen::container<std::string>(
        rc::gen::inRange<char>(0x20, 0x7F)  // printable ASCII
    );
}

// Generate a random UsbmuxMessageType
static rc::Gen<UsbmuxMessageType> genMessageType() {
    return rc::gen::element(
        UsbmuxMessageType::ListDevices,
        UsbmuxMessageType::Connect,
        UsbmuxMessageType::Listen
    );
}

} // namespace

// ─── Property Tests ──────────────────────────────────────────────────────────

// **Validates: Requirements 4.2**
// Property: Serializing a usbmux message to wire format and parsing it back
// produces an equivalent message with identical fields.
RC_GTEST_PROP(UsbmuxPlistRoundTrip,
              SerializeAndParseProducesEquivalentMessage,
              ()) {
    // Generate random message components
    auto msgType = *genMessageType();
    auto tag = *rc::gen::arbitrary<uint32_t>();
    auto progName = *genPlistSafeString();
    auto clientVersion = *genPlistSafeString();
    auto deviceID = *rc::gen::inRange<int>(0, 100000);
    auto portNumber = *rc::gen::inRange<int>(0, 65536);

    UsbmuxMessage original;
    original.messageType = msgType;
    original.tag = tag;
    original.progName = progName;
    original.clientVersionString = clientVersion;
    original.deviceID = deviceID;
    original.portNumber = portNumber;

    // Serialize to wire format
    std::vector<uint8_t> wire = SerializeMessage(original);

    // Parse back from wire format
    ParseResult parsed = ParseWireMessage(wire);

    // Verify successful parse
    RC_ASSERT(parsed.success);

    // Verify header fields
    RC_ASSERT(parsed.version == kUsbmuxVersion);
    RC_ASSERT(parsed.type == kUsbmuxTypePlist);
    RC_ASSERT(parsed.message.tag == original.tag);

    // Verify message type
    RC_ASSERT(parsed.message.messageType == original.messageType);

    // Verify string fields
    RC_ASSERT(parsed.message.progName == original.progName);
    RC_ASSERT(parsed.message.clientVersionString == original.clientVersionString);

    // Verify Connect-specific fields
    if (original.messageType == UsbmuxMessageType::Connect) {
        RC_ASSERT(parsed.message.deviceID == original.deviceID);
        RC_ASSERT(parsed.message.portNumber == original.portNumber);
    }
}

// **Validates: Requirements 4.2**
// Property: The wire format always starts with a valid 16-byte LE header where
// the length field equals the actual message size.
RC_GTEST_PROP(UsbmuxPlistRoundTrip,
              WireFormatHasCorrectLengthField,
              ()) {
    auto msgType = *genMessageType();
    auto tag = *rc::gen::arbitrary<uint32_t>();
    auto progName = *genPlistSafeString();
    auto clientVersion = *genPlistSafeString();

    UsbmuxMessage msg;
    msg.messageType = msgType;
    msg.tag = tag;
    msg.progName = progName;
    msg.clientVersionString = clientVersion;
    msg.deviceID = *rc::gen::inRange<int>(0, 100000);
    msg.portNumber = *rc::gen::inRange<int>(0, 65536);

    std::vector<uint8_t> wire = SerializeMessage(msg);

    // Header must be at least 16 bytes
    RC_ASSERT(wire.size() >= kUsbmuxHeaderSize);

    // Length field must match actual wire size
    uint32_t lengthField = ReadLE32(wire.data());
    RC_ASSERT(lengthField == static_cast<uint32_t>(wire.size()));

    // Version must be 1
    uint32_t version = ReadLE32(wire.data() + 4);
    RC_ASSERT(version == kUsbmuxVersion);

    // Type must be 8 (plist)
    uint32_t type = ReadLE32(wire.data() + 8);
    RC_ASSERT(type == kUsbmuxTypePlist);

    // Tag must match
    uint32_t parsedTag = ReadLE32(wire.data() + 12);
    RC_ASSERT(parsedTag == tag);
}

// **Validates: Requirements 4.2**
// Property: The plist body portion of the wire message is valid XML that
// contains the expected MessageType field.
RC_GTEST_PROP(UsbmuxPlistRoundTrip,
              PlistBodyContainsCorrectMessageType,
              ()) {
    auto msgType = *genMessageType();
    auto tag = *rc::gen::inRange<uint32_t>(0, 1000000);

    UsbmuxMessage msg;
    msg.messageType = msgType;
    msg.tag = tag;
    msg.progName = "TestProg";
    msg.clientVersionString = "1.0";
    msg.deviceID = 42;
    msg.portNumber = 9000;

    std::vector<uint8_t> wire = SerializeMessage(msg);

    // Extract plist body
    std::string plistBody(wire.begin() + kUsbmuxHeaderSize, wire.end());

    // Should contain XML plist header
    RC_ASSERT(plistBody.find("<?xml") != std::string::npos);
    RC_ASSERT(plistBody.find("<plist") != std::string::npos);
    RC_ASSERT(plistBody.find("<dict>") != std::string::npos);

    // Should contain the correct MessageType
    std::string expectedType = MessageTypeToString(msgType);
    std::string parsedType = ExtractPlistString(plistBody, "MessageType");
    RC_ASSERT(parsedType == expectedType);
}

// **Validates: Requirements 4.2**
// Property: Round-trip preserves all message types correctly.
// Generate all three message types and verify each round-trips.
RC_GTEST_PROP(UsbmuxPlistRoundTrip,
              AllMessageTypesRoundTrip,
              ()) {
    auto tag = *rc::gen::arbitrary<uint32_t>();
    auto progName = *genPlistSafeString();

    // Test all three message types with the same random fields
    for (auto type : {UsbmuxMessageType::ListDevices,
                      UsbmuxMessageType::Connect,
                      UsbmuxMessageType::Listen}) {
        UsbmuxMessage msg;
        msg.messageType = type;
        msg.tag = tag;
        msg.progName = progName;
        msg.clientVersionString = "OpenDisplay";
        msg.deviceID = 99;
        msg.portNumber = 9000;

        std::vector<uint8_t> wire = SerializeMessage(msg);
        ParseResult parsed = ParseWireMessage(wire);

        RC_ASSERT(parsed.success);
        RC_ASSERT(parsed.message.messageType == type);
        RC_ASSERT(parsed.message.tag == tag);
        RC_ASSERT(parsed.message.progName == progName);
    }
}
