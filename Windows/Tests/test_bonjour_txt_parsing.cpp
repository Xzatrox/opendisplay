// Property 8: Bonjour TXT Record Parsing with Defaults
// **Validates: Requirements 5.2, 8.6**
//
// For any TXT record containing an arbitrary subset of fields {id, pv, device},
// parsing SHALL produce: the "id" value as-is when present or "unknown" when
// absent, the "pv" value parsed as integer when present or 1 when absent, and
// the "device" value as-is when present (used to distinguish "Mac" receivers
// from iOS).
//
// This test models the TXT record parsing logic from BonjourBrowser.cpp without
// requiring actual DNS-SD / Bonjour network dependencies.

#include <gtest/gtest.h>
#include <rapidcheck.h>
#include <rapidcheck/gtest.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace {

// ─── Constants matching BonjourBrowser.cpp ────────────────────────────────────

static constexpr const char* kDefaultInstallId = "unknown";
static constexpr int kDefaultProtocolVersion = 1;

// ─── Model of TXT record parsing (mirrors BonjourBrowser.cpp) ────────────────

/// Parse a DNS-SD TXT record blob into key-value pairs.
/// TXT records are formatted as: [length byte][key=value] repeated.
static std::vector<std::pair<std::string, std::string>>
ParseTxtRecord(uint16_t txtLen, const unsigned char* txtRecord) {
    std::vector<std::pair<std::string, std::string>> result;
    uint16_t offset = 0;

    while (offset < txtLen) {
        uint8_t entryLen = txtRecord[offset];
        offset++;

        if (entryLen == 0 || offset + entryLen > txtLen) {
            break;
        }

        std::string entry(reinterpret_cast<const char*>(txtRecord + offset), entryLen);
        offset += entryLen;

        auto eqPos = entry.find('=');
        if (eqPos != std::string::npos) {
            std::string key = entry.substr(0, eqPos);
            std::string value = entry.substr(eqPos + 1);
            result.emplace_back(std::move(key), std::move(value));
        }
    }

    return result;
}

/// Parsed TXT record result with defaults applied.
struct ParsedTxtResult {
    std::string installId;
    int protocolVersion;
    std::string deviceType;
};

/// Apply defaults for missing fields per requirements 5.2 and 8.6.
static ParsedTxtResult ApplyTxtFields(
    const std::vector<std::pair<std::string, std::string>>& fields) {
    ParsedTxtResult result;
    result.installId = kDefaultInstallId;
    result.protocolVersion = kDefaultProtocolVersion;
    result.deviceType = "";

    for (size_t i = 0; i < fields.size(); ++i) {
        const std::string& key = fields[i].first;
        const std::string& value = fields[i].second;
        if (key == "id") {
            result.installId = value.empty() ? std::string(kDefaultInstallId) : value;
        } else if (key == "pv") {
            try {
                result.protocolVersion = std::stoi(value);
            } catch (...) {
                result.protocolVersion = kDefaultProtocolVersion;
            }
        } else if (key == "device") {
            result.deviceType = value;
        }
    }

    return result;
}

// ─── TXT record serialization (builds raw DNS-SD TXT record bytes) ───────────

/// Build a raw DNS-SD TXT record blob from key-value pairs.
/// Each entry is: [1-byte length][key=value].
static std::vector<unsigned char> BuildTxtRecord(
    const std::vector<std::pair<std::string, std::string>>& entries) {
    std::vector<unsigned char> blob;

    for (size_t i = 0; i < entries.size(); ++i) {
        std::string entry = entries[i].first + "=" + entries[i].second;
        if (entry.size() > 255) {
            // TXT record entry max is 255 bytes; truncate for safety
            entry = entry.substr(0, 255);
        }
        blob.push_back(static_cast<unsigned char>(entry.size()));
        blob.insert(blob.end(), entry.begin(), entry.end());
    }

    return blob;
}

// ─── RapidCheck generators ───────────────────────────────────────────────────

/// Generate a non-empty printable ASCII string (no '=' or NUL, suitable for
/// TXT record values). Limited in length to avoid exceeding 255-byte entry limit.
static rc::Gen<std::string> genTxtValue() {
    return rc::gen::withSize([](int size) {
        int len = std::max(1, std::min(size, 50));
        return rc::gen::container<std::string>(
            len,
            rc::gen::oneOf(
                rc::gen::inRange<char>('0', '9' + 1),
                rc::gen::inRange<char>('A', 'Z' + 1),
                rc::gen::inRange<char>('a', 'z' + 1),
                rc::gen::element<char>('-', '_', '.', '/', ':')
            )
        );
    });
}

/// Generate a valid protocol version string (integer as string).
static rc::Gen<std::string> genPvValue() {
    return rc::gen::map(
        rc::gen::inRange<int>(1, 100),
        [](int v) { return std::to_string(v); }
    );
}

/// Generate a boolean indicating whether a field is present.
static rc::Gen<bool> genFieldPresent() {
    return rc::gen::arbitrary<bool>();
}

} // namespace

// ─── Property Tests ──────────────────────────────────────────────────────────

// **Validates: Requirements 5.2, 8.6**
// Property: When "id" field is present in TXT record, parsed installId equals
// the value; when absent, it defaults to "unknown".
RC_GTEST_PROP(BonjourTxtParsing,
              IdFieldPresentOrDefaultsToUnknown,
              ()) {
    bool hasId = *genFieldPresent();
    bool hasPv = *genFieldPresent();
    bool hasDevice = *genFieldPresent();

    std::string idValue = *genTxtValue();
    std::string pvValue = *genPvValue();
    std::string deviceValue = *genTxtValue();

    // Build TXT record with the selected subset of fields
    std::vector<std::pair<std::string, std::string>> entries;
    if (hasId) entries.emplace_back("id", idValue);
    if (hasPv) entries.emplace_back("pv", pvValue);
    if (hasDevice) entries.emplace_back("device", deviceValue);

    auto blob = BuildTxtRecord(entries);
    auto fields = ParseTxtRecord(static_cast<uint16_t>(blob.size()), blob.data());
    auto result = ApplyTxtFields(fields);

    // Verify id field behavior
    if (hasId) {
        RC_ASSERT(result.installId == idValue);
    } else {
        RC_ASSERT(result.installId == std::string(kDefaultInstallId));
    }
}

// **Validates: Requirements 5.2, 8.6**
// Property: When "pv" field is present with a valid integer string, parsed
// protocolVersion equals that integer; when absent, it defaults to 1.
RC_GTEST_PROP(BonjourTxtParsing,
              PvFieldPresentOrDefaultsToOne,
              ()) {
    bool hasId = *genFieldPresent();
    bool hasPv = *genFieldPresent();
    bool hasDevice = *genFieldPresent();

    std::string idValue = *genTxtValue();
    int pvInt = *rc::gen::inRange<int>(1, 100);
    std::string pvValue = std::to_string(pvInt);
    std::string deviceValue = *genTxtValue();

    std::vector<std::pair<std::string, std::string>> entries;
    if (hasId) entries.emplace_back("id", idValue);
    if (hasPv) entries.emplace_back("pv", pvValue);
    if (hasDevice) entries.emplace_back("device", deviceValue);

    auto blob = BuildTxtRecord(entries);
    auto fields = ParseTxtRecord(static_cast<uint16_t>(blob.size()), blob.data());
    auto result = ApplyTxtFields(fields);

    // Verify pv field behavior
    if (hasPv) {
        RC_ASSERT(result.protocolVersion == pvInt);
    } else {
        RC_ASSERT(result.protocolVersion == kDefaultProtocolVersion);
    }
}

// **Validates: Requirements 5.2, 8.6**
// Property: When "device" field is present, the parsed deviceType equals the
// value (allowing distinction of "Mac" from iOS); when absent, deviceType is
// empty string.
RC_GTEST_PROP(BonjourTxtParsing,
              DeviceFieldDistinguishesMacFromIOS,
              ()) {
    bool hasId = *genFieldPresent();
    bool hasPv = *genFieldPresent();
    bool hasDevice = *genFieldPresent();
    // For device field, sometimes generate "Mac" specifically
    bool isMac = *genFieldPresent();

    std::string idValue = *genTxtValue();
    std::string pvValue = *genPvValue();
    std::string deviceValue = isMac ? "Mac" : *genTxtValue();

    std::vector<std::pair<std::string, std::string>> entries;
    if (hasId) entries.emplace_back("id", idValue);
    if (hasPv) entries.emplace_back("pv", pvValue);
    if (hasDevice) entries.emplace_back("device", deviceValue);

    auto blob = BuildTxtRecord(entries);
    auto fields = ParseTxtRecord(static_cast<uint16_t>(blob.size()), blob.data());
    auto result = ApplyTxtFields(fields);

    // Verify device field behavior
    if (hasDevice) {
        RC_ASSERT(result.deviceType == deviceValue);
        // When device is "Mac", it should be distinguishable
        if (isMac) {
            RC_ASSERT(result.deviceType == "Mac");
        }
    } else {
        RC_ASSERT(result.deviceType.empty());
    }
}

// **Validates: Requirements 5.2, 8.6**
// Property: For any random subset of {id, pv, device} fields, the full parsing
// pipeline (raw TXT blob → parse → apply defaults) produces consistent results
// where all three defaults are correctly applied together.
RC_GTEST_PROP(BonjourTxtParsing,
              AllDefaultsAppliedCorrectlyForRandomSubset,
              ()) {
    bool hasId = *genFieldPresent();
    bool hasPv = *genFieldPresent();
    bool hasDevice = *genFieldPresent();

    std::string idValue = *genTxtValue();
    int pvInt = *rc::gen::inRange<int>(1, 100);
    std::string pvValue = std::to_string(pvInt);
    std::string deviceValue = *rc::gen::oneOf(
        rc::gen::just(std::string("Mac")),
        genTxtValue()
    );

    std::vector<std::pair<std::string, std::string>> entries;
    if (hasId) entries.emplace_back("id", idValue);
    if (hasPv) entries.emplace_back("pv", pvValue);
    if (hasDevice) entries.emplace_back("device", deviceValue);

    auto blob = BuildTxtRecord(entries);
    auto fields = ParseTxtRecord(static_cast<uint16_t>(blob.size()), blob.data());
    auto result = ApplyTxtFields(fields);

    // Verify all fields together
    std::string expectedId = hasId ? idValue : std::string(kDefaultInstallId);
    int expectedPv = hasPv ? pvInt : kDefaultProtocolVersion;
    std::string expectedDevice = hasDevice ? deviceValue : "";

    RC_ASSERT(result.installId == expectedId);
    RC_ASSERT(result.protocolVersion == expectedPv);
    RC_ASSERT(result.deviceType == expectedDevice);
}

// **Validates: Requirements 5.2, 8.6**
// Property: An empty TXT record (no fields at all) produces all defaults:
// id="unknown", pv=1, device="" (empty).
RC_GTEST_PROP(BonjourTxtParsing,
              EmptyTxtRecordProducesAllDefaults,
              ()) {
    // Generate an empty TXT record
    std::vector<unsigned char> emptyBlob;
    auto fields = ParseTxtRecord(0, emptyBlob.data());
    auto result = ApplyTxtFields(fields);

    RC_ASSERT(result.installId == std::string(kDefaultInstallId));
    RC_ASSERT(result.protocolVersion == kDefaultProtocolVersion);
    RC_ASSERT(result.deviceType.empty());
}
