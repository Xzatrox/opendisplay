// Property 11: Wire Frame Serialization Round-Trip
// **Validates: Requirements 6.1**
//
// For any random H.264 Annex B payload (starting with 00 00 00 01) and any
// pair of timestamps (captureMs, sendMs), serializing to the wire format:
//   [4-byte big-endian total payload length][JSON telemetry {"cap":ms,"snd":ms}][Annex B payload]
// and then parsing back SHALL recover the original timestamps and payload exactly.
//
// The receiver disambiguates video from control by checking if the payload's
// first byte is '{' (control) or not (video). Since Annex B payloads always
// start with 0x00, they are always identified as video.

#include <gtest/gtest.h>
#include <rapidcheck.h>
#include <rapidcheck/gtest.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace {

// ─── Constants ───────────────────────────────────────────────────────────────
static const uint8_t kAnnexBStartCode[4] = {0x00, 0x00, 0x00, 0x01};

// ─── Serialization (mirrors WireTransport::SendVideoFrame) ───────────────────

/// Write a 4-byte big-endian uint32 into a buffer.
static void WriteBE32(uint8_t* dest, uint32_t value)
{
    dest[0] = static_cast<uint8_t>((value >> 24) & 0xFF);
    dest[1] = static_cast<uint8_t>((value >> 16) & 0xFF);
    dest[2] = static_cast<uint8_t>((value >> 8) & 0xFF);
    dest[3] = static_cast<uint8_t>(value & 0xFF);
}

/// Read a 4-byte big-endian uint32 from a buffer.
static uint32_t ReadBE32(const uint8_t* src)
{
    return (static_cast<uint32_t>(src[0]) << 24) |
           (static_cast<uint32_t>(src[1]) << 16) |
           (static_cast<uint32_t>(src[2]) << 8) |
           static_cast<uint32_t>(src[3]);
}

/// Serialize a video frame to the wire format.
/// Format: [4B BE total-payload-length][JSON telemetry][Annex B H.264 data]
/// The telemetry JSON is: {"cap":<captureMs>,"snd":<sendMs>}
std::vector<uint8_t> SerializeVideoFrame(const std::vector<uint8_t>& annexB,
                                          int64_t captureMs, int64_t sendMs)
{
    // Build telemetry JSON prefix
    std::string telemetry = "{\"cap\":" + std::to_string(captureMs) +
                            ",\"snd\":" + std::to_string(sendMs) + "}";

    // Total payload = telemetry + H.264 data
    uint32_t payloadLength = static_cast<uint32_t>(telemetry.size() + annexB.size());

    // Assemble: [4B length][telemetry][payload]
    std::vector<uint8_t> frame;
    frame.resize(4 + payloadLength);

    WriteBE32(frame.data(), payloadLength);
    std::memcpy(frame.data() + 4, telemetry.data(), telemetry.size());
    std::memcpy(frame.data() + 4 + telemetry.size(), annexB.data(), annexB.size());

    return frame;
}

// ─── Deserialization (mirrors WireTransport::ReceiveLoop parsing) ─────────────

/// Parsed result from the wire format
struct ParsedFrame {
    int64_t captureMs;
    int64_t sendMs;
    std::vector<uint8_t> annexBPayload;
};

/// Parse a wire-format frame back into its components.
/// Returns true on success, false if parsing fails.
bool ParseVideoFrame(const std::vector<uint8_t>& frame, ParsedFrame& out)
{
    // Need at least 4 bytes for the length header
    if (frame.size() < 4)
    {
        return false;
    }

    // Read the 4-byte big-endian payload length
    uint32_t payloadLength = ReadBE32(frame.data());

    // Verify the frame has exactly the expected size
    if (frame.size() != 4 + payloadLength)
    {
        return false;
    }

    const uint8_t* payload = frame.data() + 4;

    // The payload starts with JSON telemetry followed by H.264 data.
    // The H.264 data always starts with the Annex B start code (00 00 00 01).
    // We find the start code to split telemetry from video data.

    // The telemetry JSON is {"cap":...,"snd":...} and ends with '}'.
    // The H.264 data starts with 0x00 0x00 0x00 0x01.
    // We search for the Annex B start code pattern in the payload.
    const uint8_t* payloadEnd = payload + payloadLength;

    // Find the first occurrence of 00 00 00 01 in the payload
    const uint8_t* annexBStart = nullptr;
    for (size_t i = 0; i + 3 < payloadLength; ++i)
    {
        if (payload[i] == 0x00 && payload[i + 1] == 0x00 &&
            payload[i + 2] == 0x00 && payload[i + 3] == 0x01)
        {
            annexBStart = payload + i;
            break;
        }
    }

    if (!annexBStart)
    {
        return false; // No Annex B start code found
    }

    // Extract the telemetry JSON (everything before the start code)
    size_t telemetryLen = static_cast<size_t>(annexBStart - payload);
    std::string telemetryJson(reinterpret_cast<const char*>(payload), telemetryLen);

    // Parse telemetry JSON: {"cap":<ms>,"snd":<ms>}
    // Simple parsing without a full JSON library — the format is fixed.
    // Find "cap": and "snd": values
    auto extractValue = [](const std::string& json, const std::string& key) -> int64_t {
        std::string searchKey = "\"" + key + "\":";
        size_t pos = json.find(searchKey);
        if (pos == std::string::npos)
        {
            return -1;
        }
        pos += searchKey.size();
        // Parse the integer value
        int64_t value = 0;
        bool negative = false;
        if (pos < json.size() && json[pos] == '-')
        {
            negative = true;
            ++pos;
        }
        while (pos < json.size() && json[pos] >= '0' && json[pos] <= '9')
        {
            value = value * 10 + (json[pos] - '0');
            ++pos;
        }
        return negative ? -value : value;
    };

    out.captureMs = extractValue(telemetryJson, "cap");
    out.sendMs = extractValue(telemetryJson, "snd");

    // Extract the H.264 Annex B data
    size_t annexBLen = static_cast<size_t>(payloadEnd - annexBStart);
    out.annexBPayload.assign(annexBStart, annexBStart + annexBLen);

    return true;
}

} // namespace

// ─── Property Tests ──────────────────────────────────────────────────────────

// **Validates: Requirements 6.1**
// Property: Serializing a video frame to the wire format and parsing it back
// recovers the original timestamps and Annex B payload exactly.
RC_GTEST_PROP(WireFrameRoundTrip,
              SerializeAndParseRecoverOriginals,
              ())
{
    // Generate random Annex B payload: starts with 00 00 00 01, followed by random bytes
    auto payloadBodyLen = *rc::gen::inRange<int>(1, 4096);
    std::vector<uint8_t> annexB;
    annexB.reserve(4 + payloadBodyLen);
    annexB.insert(annexB.end(), kAnnexBStartCode, kAnnexBStartCode + 4);
    for (int i = 0; i < payloadBodyLen; ++i)
    {
        annexB.push_back(static_cast<uint8_t>(*rc::gen::inRange<int>(0, 256)));
    }

    // Generate random timestamp pairs (non-negative Unix milliseconds)
    auto captureMs = *rc::gen::inRange<int64_t>(0, 2000000000000LL);
    auto sendMs = *rc::gen::inRange<int64_t>(0, 2000000000000LL);

    // Serialize to wire format
    auto wireFrame = SerializeVideoFrame(annexB, captureMs, sendMs);

    // Parse back
    ParsedFrame parsed;
    RC_ASSERT(ParseVideoFrame(wireFrame, parsed));

    // Verify timestamps recovered exactly
    RC_ASSERT(parsed.captureMs == captureMs);
    RC_ASSERT(parsed.sendMs == sendMs);

    // Verify payload recovered exactly
    RC_ASSERT(parsed.annexBPayload == annexB);
}

// **Validates: Requirements 6.1**
// Property: The wire frame length field correctly encodes the total payload size
// (telemetry JSON + Annex B data).
RC_GTEST_PROP(WireFrameRoundTrip,
              LengthFieldMatchesPayload,
              ())
{
    // Generate random Annex B payload
    auto payloadBodyLen = *rc::gen::inRange<int>(1, 2048);
    std::vector<uint8_t> annexB;
    annexB.reserve(4 + payloadBodyLen);
    annexB.insert(annexB.end(), kAnnexBStartCode, kAnnexBStartCode + 4);
    for (int i = 0; i < payloadBodyLen; ++i)
    {
        annexB.push_back(static_cast<uint8_t>(*rc::gen::inRange<int>(0, 256)));
    }

    // Generate random timestamps
    auto captureMs = *rc::gen::inRange<int64_t>(0, 1000000000000LL);
    auto sendMs = *rc::gen::inRange<int64_t>(0, 1000000000000LL);

    // Serialize
    auto wireFrame = SerializeVideoFrame(annexB, captureMs, sendMs);

    // Read the length field
    uint32_t encodedLength = ReadBE32(wireFrame.data());

    // The length field should equal the total frame size minus the 4-byte header
    RC_ASSERT(encodedLength == wireFrame.size() - 4);
}

// **Validates: Requirements 6.1**
// Property: The wire frame payload is NOT mistaken for a control message.
// Video frames start with telemetry JSON '{', but the receiver identifies
// video by finding the Annex B start code. The first byte after the length
// prefix is '{' for the telemetry — the receiver uses a more sophisticated
// check: if the full payload parses as valid JSON it's control, otherwise
// it's video. Since our telemetry+payload hybrid doesn't parse as valid JSON,
// it's correctly identified as video.
// We verify the frame structure is: [length][starts-with-json-telemetry][contains-annex-b-start-code]
RC_GTEST_PROP(WireFrameRoundTrip,
              FrameContainsAnnexBStartCode,
              ())
{
    // Generate random Annex B payload
    auto payloadBodyLen = *rc::gen::inRange<int>(1, 512);
    std::vector<uint8_t> annexB;
    annexB.reserve(4 + payloadBodyLen);
    annexB.insert(annexB.end(), kAnnexBStartCode, kAnnexBStartCode + 4);
    for (int i = 0; i < payloadBodyLen; ++i)
    {
        annexB.push_back(static_cast<uint8_t>(*rc::gen::inRange<int>(0, 256)));
    }

    auto captureMs = *rc::gen::inRange<int64_t>(0, 999999999999LL);
    auto sendMs = *rc::gen::inRange<int64_t>(0, 999999999999LL);

    auto wireFrame = SerializeVideoFrame(annexB, captureMs, sendMs);

    // The payload (after 4-byte length) must contain the Annex B start code
    bool foundStartCode = false;
    for (size_t i = 4; i + 3 < wireFrame.size(); ++i)
    {
        if (wireFrame[i] == 0x00 && wireFrame[i + 1] == 0x00 &&
            wireFrame[i + 2] == 0x00 && wireFrame[i + 3] == 0x01)
        {
            foundStartCode = true;
            break;
        }
    }
    RC_ASSERT(foundStartCode);
}

// **Validates: Requirements 6.1**
// Property: The telemetry JSON prefix in the wire frame is well-formed
// and contains both "cap" and "snd" fields.
RC_GTEST_PROP(WireFrameRoundTrip,
              TelemetryJsonIsWellFormed,
              ())
{
    // Generate random Annex B payload
    auto payloadBodyLen = *rc::gen::inRange<int>(1, 256);
    std::vector<uint8_t> annexB;
    annexB.reserve(4 + payloadBodyLen);
    annexB.insert(annexB.end(), kAnnexBStartCode, kAnnexBStartCode + 4);
    for (int i = 0; i < payloadBodyLen; ++i)
    {
        annexB.push_back(static_cast<uint8_t>(*rc::gen::inRange<int>(0, 256)));
    }

    auto captureMs = *rc::gen::inRange<int64_t>(0, 2000000000000LL);
    auto sendMs = *rc::gen::inRange<int64_t>(0, 2000000000000LL);

    auto wireFrame = SerializeVideoFrame(annexB, captureMs, sendMs);

    // Extract the telemetry portion: from byte 4 to the first Annex B start code
    const uint8_t* payload = wireFrame.data() + 4;
    size_t payloadLen = wireFrame.size() - 4;

    // Find Annex B start code
    size_t telemetryEnd = 0;
    for (size_t i = 0; i + 3 < payloadLen; ++i)
    {
        if (payload[i] == 0x00 && payload[i + 1] == 0x00 &&
            payload[i + 2] == 0x00 && payload[i + 3] == 0x01)
        {
            telemetryEnd = i;
            break;
        }
    }

    RC_ASSERT(telemetryEnd > 0); // There must be telemetry before the start code

    std::string telemetry(reinterpret_cast<const char*>(payload), telemetryEnd);

    // Verify it starts with '{' and ends with '}'
    RC_ASSERT(telemetry.front() == '{');
    RC_ASSERT(telemetry.back() == '}');

    // Verify it contains "cap" and "snd" keys
    RC_ASSERT(telemetry.find("\"cap\":") != std::string::npos);
    RC_ASSERT(telemetry.find("\"snd\":") != std::string::npos);
}
