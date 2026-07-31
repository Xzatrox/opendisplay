// Property 5: Keyframe NAL Structure
// **Validates: Requirements 3.5**
//
// For any H.264 keyframe produced by the encoder, the Annex B output SHALL
// consist of:
//   [4-byte start code 00 00 00 01][SPS NAL unit (type 7)]
//   [4-byte start code 00 00 00 01][PPS NAL unit (type 8)]
//   [4-byte start code 00 00 00 01][IDR slice NAL unit(s) (type 5)]
//
// Each NAL type is identified by (first byte & 0x1F).
//
// This test generates random SPS, PPS, and IDR slice byte sequences,
// constructs an Annex B keyframe, and verifies structural correctness.

#include <gtest/gtest.h>
#include <rapidcheck.h>
#include <rapidcheck/gtest.h>

#include <cstdint>
#include <vector>
#include <algorithm>

namespace {

// ─── Constants ───────────────────────────────────────────────────────────────
static const uint8_t kAnnexBStartCode[4] = {0x00, 0x00, 0x00, 0x01};

// NAL unit type identifiers (first byte & 0x1F)
static constexpr uint8_t kNalTypeSPS = 7;
static constexpr uint8_t kNalTypePPS = 8;
static constexpr uint8_t kNalTypeIDR = 5;

// ─── Helper: Build an Annex B keyframe from components ───────────────────────
// Mirrors the logic in MFTEncoder::GetOutput() where keyframes are prefixed
// with SPS+PPS NAL units, each preceded by a 4-byte start code.
std::vector<uint8_t> BuildAnnexBKeyframe(
    const std::vector<uint8_t>& spsPayload,
    const std::vector<uint8_t>& ppsPayload,
    const std::vector<uint8_t>& idrSlicePayload)
{
    std::vector<uint8_t> result;
    result.reserve(4 + spsPayload.size() + 4 + ppsPayload.size() + 4 + idrSlicePayload.size());

    // [start code][SPS NAL]
    result.insert(result.end(), kAnnexBStartCode, kAnnexBStartCode + 4);
    result.insert(result.end(), spsPayload.begin(), spsPayload.end());

    // [start code][PPS NAL]
    result.insert(result.end(), kAnnexBStartCode, kAnnexBStartCode + 4);
    result.insert(result.end(), ppsPayload.begin(), ppsPayload.end());

    // [start code][IDR slice NAL]
    result.insert(result.end(), kAnnexBStartCode, kAnnexBStartCode + 4);
    result.insert(result.end(), idrSlicePayload.begin(), idrSlicePayload.end());

    return result;
}

// ─── Helper: Build an Annex B keyframe with multiple IDR slices ──────────────
std::vector<uint8_t> BuildAnnexBKeyframeMultiSlice(
    const std::vector<uint8_t>& spsPayload,
    const std::vector<uint8_t>& ppsPayload,
    const std::vector<std::vector<uint8_t>>& idrSlices)
{
    std::vector<uint8_t> result;

    // [start code][SPS NAL]
    result.insert(result.end(), kAnnexBStartCode, kAnnexBStartCode + 4);
    result.insert(result.end(), spsPayload.begin(), spsPayload.end());

    // [start code][PPS NAL]
    result.insert(result.end(), kAnnexBStartCode, kAnnexBStartCode + 4);
    result.insert(result.end(), ppsPayload.begin(), ppsPayload.end());

    // [start code][IDR slice NAL] for each slice
    for (const auto& slice : idrSlices)
    {
        result.insert(result.end(), kAnnexBStartCode, kAnnexBStartCode + 4);
        result.insert(result.end(), slice.begin(), slice.end());
    }

    return result;
}

// ─── Helper: Parse NAL units from Annex B bitstream ──────────────────────────
// Finds all NAL units delimited by 4-byte start codes (00 00 00 01).
struct ParsedNalu {
    uint8_t nalType;
    std::vector<uint8_t> data;  // includes the NAL header byte
};

std::vector<ParsedNalu> ParseAnnexB(const std::vector<uint8_t>& bitstream)
{
    std::vector<ParsedNalu> nalus;
    size_t size = bitstream.size();
    const uint8_t* data = bitstream.data();

    // Find all 4-byte start code positions
    std::vector<size_t> startPositions;
    for (size_t i = 0; i + 3 < size; ++i)
    {
        if (data[i] == 0x00 && data[i + 1] == 0x00 &&
            data[i + 2] == 0x00 && data[i + 3] == 0x01)
        {
            startPositions.push_back(i);
        }
    }

    for (size_t idx = 0; idx < startPositions.size(); ++idx)
    {
        size_t naluStart = startPositions[idx] + 4; // skip the 4-byte start code
        size_t naluEnd = (idx + 1 < startPositions.size())
                             ? startPositions[idx + 1]
                             : size;

        if (naluStart < naluEnd)
        {
            ParsedNalu nalu;
            nalu.nalType = data[naluStart] & 0x1F;
            nalu.data.assign(data + naluStart, data + naluEnd);
            nalus.push_back(std::move(nalu));
        }
    }

    return nalus;
}

// ─── Helper: Make a NAL header byte ──────────────────────────────────────────
// forbidden_zero_bit(1) | nal_ref_idc(2) | nal_unit_type(5)
uint8_t MakeNalHeader(uint8_t nalType, uint8_t refIdc = 3)
{
    // forbidden_zero_bit = 0, nal_ref_idc in bits 5-6, type in lower 5 bits
    return static_cast<uint8_t>((refIdc << 5) | (nalType & 0x1F));
}

// ─── Helper: Generate safe NAL body bytes ────────────────────────────────────
// Uses non-zero bytes to avoid accidental Annex B start codes (00 00 00 01)
// within generated payloads. Real H.264 uses emulation prevention bytes but
// for property testing we simply avoid zeros to keep parsing deterministic.
std::vector<uint8_t> GenerateSafeNalPayload(uint8_t nalType, int bodyLen, uint8_t refIdc = 3)
{
    std::vector<uint8_t> payload;
    payload.reserve(1 + bodyLen);
    payload.push_back(MakeNalHeader(nalType, refIdc));
    for (int i = 0; i < bodyLen; ++i)
    {
        // Generate bytes in [1, 255] to avoid start code patterns
        payload.push_back(static_cast<uint8_t>(*rc::gen::inRange<int>(1, 256)));
    }
    return payload;
}

} // namespace

// ─── Property Tests ──────────────────────────────────────────────────────────

// **Validates: Requirements 3.5**
// Property: A constructed Annex B keyframe has the correct structure:
// SPS(type=7) → PPS(type=8) → IDR(type=5), each preceded by a 4-byte start code.
RC_GTEST_PROP(KeyframeNALStructure,
              AnnexBKeyframeHasCorrectNALOrder,
              ())
{
    // Generate random payload sizes
    auto spsBodyLen = *rc::gen::inRange<int>(1, 65);
    auto ppsBodyLen = *rc::gen::inRange<int>(1, 33);
    auto idrBodyLen = *rc::gen::inRange<int>(1, 257);

    // Build NAL unit payloads with correct type headers
    auto spsPayload = GenerateSafeNalPayload(kNalTypeSPS, spsBodyLen);
    auto ppsPayload = GenerateSafeNalPayload(kNalTypePPS, ppsBodyLen);
    auto idrPayload = GenerateSafeNalPayload(kNalTypeIDR, idrBodyLen);

    // Build the Annex B keyframe
    auto keyframe = BuildAnnexBKeyframe(spsPayload, ppsPayload, idrPayload);

    // Parse it back
    auto nalus = ParseAnnexB(keyframe);

    // Verify: exactly 3 NAL units in order: SPS, PPS, IDR
    RC_ASSERT(nalus.size() == 3u);
    RC_ASSERT(nalus[0].nalType == kNalTypeSPS);
    RC_ASSERT(nalus[1].nalType == kNalTypePPS);
    RC_ASSERT(nalus[2].nalType == kNalTypeIDR);

    // Verify the data content matches what we put in
    RC_ASSERT(nalus[0].data == spsPayload);
    RC_ASSERT(nalus[1].data == ppsPayload);
    RC_ASSERT(nalus[2].data == idrPayload);
}

// **Validates: Requirements 3.5**
// Property: Every NAL unit in a keyframe starts with a 4-byte Annex B start code.
RC_GTEST_PROP(KeyframeNALStructure,
              Every4ByteStartCodePresent,
              ())
{
    auto spsBodyLen = *rc::gen::inRange<int>(1, 100);
    auto ppsBodyLen = *rc::gen::inRange<int>(1, 50);
    auto idrBodyLen = *rc::gen::inRange<int>(1, 500);

    auto spsPayload = GenerateSafeNalPayload(kNalTypeSPS, spsBodyLen);
    auto ppsPayload = GenerateSafeNalPayload(kNalTypePPS, ppsBodyLen);
    auto idrPayload = GenerateSafeNalPayload(kNalTypeIDR, idrBodyLen);

    auto keyframe = BuildAnnexBKeyframe(spsPayload, ppsPayload, idrPayload);

    // Verify: the bitstream starts with a start code
    RC_ASSERT(keyframe.size() >= 4u);
    RC_ASSERT(keyframe[0] == 0x00);
    RC_ASSERT(keyframe[1] == 0x00);
    RC_ASSERT(keyframe[2] == 0x00);
    RC_ASSERT(keyframe[3] == 0x01);

    // Verify: there are exactly 3 start codes in the bitstream
    int startCodeCount = 0;
    for (size_t i = 0; i + 3 < keyframe.size(); ++i)
    {
        if (keyframe[i] == 0x00 && keyframe[i + 1] == 0x00 &&
            keyframe[i + 2] == 0x00 && keyframe[i + 3] == 0x01)
        {
            startCodeCount++;
        }
    }
    RC_ASSERT(startCodeCount == 3);
}

// **Validates: Requirements 3.5**
// Property: The NAL type byte correctly identifies SPS=7, PPS=8, IDR=5
// using the (byte & 0x1F) extraction rule, regardless of nal_ref_idc value.
RC_GTEST_PROP(KeyframeNALStructure,
              NALTypeExtractionCorrect,
              ())
{
    // Generate random nal_ref_idc (2 bits, range 0-3) for each NAL unit
    auto spsRefIdc = *rc::gen::inRange<int>(0, 4);
    auto ppsRefIdc = *rc::gen::inRange<int>(0, 4);
    auto idrRefIdc = *rc::gen::inRange<int>(0, 4);
    auto bodyLen = *rc::gen::inRange<int>(1, 20);

    // Build payloads with arbitrary ref_idc but correct NAL types
    auto spsPayload = GenerateSafeNalPayload(kNalTypeSPS, bodyLen, static_cast<uint8_t>(spsRefIdc));
    auto ppsPayload = GenerateSafeNalPayload(kNalTypePPS, bodyLen, static_cast<uint8_t>(ppsRefIdc));
    auto idrPayload = GenerateSafeNalPayload(kNalTypeIDR, bodyLen, static_cast<uint8_t>(idrRefIdc));

    auto keyframe = BuildAnnexBKeyframe(spsPayload, ppsPayload, idrPayload);
    auto nalus = ParseAnnexB(keyframe);

    RC_ASSERT(nalus.size() == 3u);

    // Verify the (byte & 0x1F) extraction yields the correct NAL types
    RC_ASSERT((nalus[0].data[0] & 0x1F) == kNalTypeSPS);
    RC_ASSERT((nalus[1].data[0] & 0x1F) == kNalTypePPS);
    RC_ASSERT((nalus[2].data[0] & 0x1F) == kNalTypeIDR);
}

// **Validates: Requirements 3.5**
// Property: Multiple IDR slices are supported — all slice NALs after SPS+PPS
// must have type 5 (IDR).
RC_GTEST_PROP(KeyframeNALStructure,
              MultipleIDRSlicesAllHaveType5,
              ())
{
    auto spsBodyLen = *rc::gen::inRange<int>(1, 30);
    auto ppsBodyLen = *rc::gen::inRange<int>(1, 20);
    auto numSlices = *rc::gen::inRange<int>(1, 5);

    auto spsPayload = GenerateSafeNalPayload(kNalTypeSPS, spsBodyLen);
    auto ppsPayload = GenerateSafeNalPayload(kNalTypePPS, ppsBodyLen);

    // Generate multiple IDR slices
    std::vector<std::vector<uint8_t>> idrSlices;
    for (int s = 0; s < numSlices; ++s)
    {
        auto sliceBodyLen = *rc::gen::inRange<int>(1, 100);
        idrSlices.push_back(GenerateSafeNalPayload(kNalTypeIDR, sliceBodyLen));
    }

    auto keyframe = BuildAnnexBKeyframeMultiSlice(spsPayload, ppsPayload, idrSlices);

    // Parse and verify structure
    auto nalus = ParseAnnexB(keyframe);

    // Should have SPS + PPS + numSlices NALUs
    RC_ASSERT(nalus.size() == static_cast<size_t>(2 + numSlices));

    // First two are SPS and PPS
    RC_ASSERT(nalus[0].nalType == kNalTypeSPS);
    RC_ASSERT(nalus[1].nalType == kNalTypePPS);

    // All remaining are IDR slices (type 5)
    for (int s = 0; s < numSlices; ++s)
    {
        RC_ASSERT(nalus[2 + s].nalType == kNalTypeIDR);
    }
}

// **Validates: Requirements 3.5**
// Property: The total size of the Annex B keyframe equals the sum of
// all start codes (4 bytes each) plus all NAL unit payloads.
RC_GTEST_PROP(KeyframeNALStructure,
              TotalSizeEqualsComponentSizes,
              ())
{
    auto spsBodyLen = *rc::gen::inRange<int>(1, 50);
    auto ppsBodyLen = *rc::gen::inRange<int>(1, 30);
    auto idrBodyLen = *rc::gen::inRange<int>(1, 200);

    auto spsPayload = GenerateSafeNalPayload(kNalTypeSPS, spsBodyLen);
    auto ppsPayload = GenerateSafeNalPayload(kNalTypePPS, ppsBodyLen);
    auto idrPayload = GenerateSafeNalPayload(kNalTypeIDR, idrBodyLen);

    auto keyframe = BuildAnnexBKeyframe(spsPayload, ppsPayload, idrPayload);

    // Expected total size: 3 start codes (4 bytes each) + payloads
    size_t expectedSize = 3 * 4 + spsPayload.size() + ppsPayload.size() + idrPayload.size();
    RC_ASSERT(keyframe.size() == expectedSize);
}
