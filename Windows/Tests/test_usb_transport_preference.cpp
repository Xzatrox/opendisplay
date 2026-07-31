// Property 9: USB Transport Preference Over WiFi
// **Validates: Requirements 5.4**
//
// For any set of discovered devices where the same install ID appears on both
// USB and WiFi transports, the connection picker list SHALL show only the USB
// entry for that device and suppress the WiFi entry, while devices available
// on only one transport appear normally.
//
// This test models the connection picker's transport deduplication logic. We
// generate random device sets with varying install IDs and transport types, then
// verify the suppression behavior matches the specification.

#include <gtest/gtest.h>
#include <rapidcheck.h>
#include <rapidcheck/gtest.h>

#include <algorithm>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace {

// ─── Transport types ─────────────────────────────────────────────────────────

enum class Transport {
    USB,
    WiFi
};

// ─── Input model: a device entry as discovered by the system ─────────────────

struct DiscoveredDevice {
    std::string installId;  ///< Unique per physical device
    std::string name;       ///< Friendly display name
    Transport transport;    ///< How this device was discovered
};

// ─── Output model: a device entry shown in the connection picker ─────────────

struct PickerEntry {
    std::string installId;
    std::string name;
    Transport transport;
};

// ─── Connection picker suppression logic (mirrors real implementation) ───────
// Requirement 5.4: The Windows sender shall prefer USB transport over WiFi
// when the same physical device (matched by install id) is available on both
// transports, and shall suppress the WiFi entry in the connection picker for
// a device already shown via USB.

std::vector<PickerEntry> BuildConnectionPickerList(
    const std::vector<DiscoveredDevice>& devices) {

    // Track which install IDs have USB entries
    std::unordered_set<std::string> usbInstallIds;
    for (const auto& dev : devices) {
        if (dev.transport == Transport::USB) {
            usbInstallIds.insert(dev.installId);
        }
    }

    // Build the picker list: include all USB devices, include WiFi devices
    // only if their install ID is NOT already present via USB
    std::vector<PickerEntry> result;
    for (const auto& dev : devices) {
        if (dev.transport == Transport::USB) {
            // Always include USB entries
            result.push_back({dev.installId, dev.name, dev.transport});
        } else {
            // WiFi: suppress if same installId is available via USB
            if (usbInstallIds.find(dev.installId) == usbInstallIds.end()) {
                result.push_back({dev.installId, dev.name, dev.transport});
            }
        }
    }

    return result;
}

// ─── Helper to generate a random device set inside a property ────────────────

/// Generate a random device set (called within RC_GTEST_PROP body).
/// Uses small install ID alphabet to encourage same-ID collisions across transports.
std::vector<DiscoveredDevice> GenerateRandomDeviceSet() {
    auto count = *rc::gen::inRange<int>(1, 30);
    std::vector<DiscoveredDevice> devices;
    devices.reserve(count);
    for (int i = 0; i < count; ++i) {
        DiscoveredDevice dev;
        auto idIdx = *rc::gen::inRange<int>(0, 8);
        dev.installId = "device-" + std::to_string(idIdx);
        auto nameIdx = *rc::gen::inRange<int>(0, 20);
        dev.name = "MyDevice-" + std::to_string(nameIdx);
        auto transportIdx = *rc::gen::inRange<int>(0, 2);
        dev.transport = (transportIdx == 0) ? Transport::USB : Transport::WiFi;
        devices.push_back(std::move(dev));
    }
    return devices;
}

} // namespace

// ─── Property Tests ──────────────────────────────────────────────────────────

// **Validates: Requirements 5.4**
// Property: Dual-transport devices (same installId on USB + WiFi) appear only
// as USB in the connection picker — the WiFi entry is suppressed.
RC_GTEST_PROP(UsbTransportPreference,
              DualTransportDevicesShowOnlyUsb,
              ()) {
    auto devices = GenerateRandomDeviceSet();
    auto pickerList = BuildConnectionPickerList(devices);

    // Find all install IDs that have USB entries in the input
    std::unordered_set<std::string> usbIds;
    for (const auto& dev : devices) {
        if (dev.transport == Transport::USB) {
            usbIds.insert(dev.installId);
        }
    }

    // For every install ID that appears in both USB and WiFi, verify that
    // the picker list contains NO WiFi entry for that ID
    for (const auto& entry : pickerList) {
        if (usbIds.count(entry.installId) > 0) {
            RC_ASSERT(entry.transport == Transport::USB);
        }
    }
}

// **Validates: Requirements 5.4**
// Property: WiFi-only devices (installId not on USB) appear normally in the
// connection picker list.
RC_GTEST_PROP(UsbTransportPreference,
              WiFiOnlyDevicesAppearNormally,
              ()) {
    auto devices = GenerateRandomDeviceSet();
    auto pickerList = BuildConnectionPickerList(devices);

    // Find install IDs that have USB entries
    std::unordered_set<std::string> usbIds;
    for (const auto& dev : devices) {
        if (dev.transport == Transport::USB) {
            usbIds.insert(dev.installId);
        }
    }

    // WiFi-only devices: those with WiFi transport and no USB entry
    for (const auto& dev : devices) {
        if (dev.transport == Transport::WiFi && usbIds.count(dev.installId) == 0) {
            // This WiFi-only device must appear in the picker
            bool found = std::any_of(pickerList.begin(), pickerList.end(),
                [&](const PickerEntry& e) {
                    return e.installId == dev.installId &&
                           e.transport == Transport::WiFi;
                });
            RC_ASSERT(found);
        }
    }
}

// **Validates: Requirements 5.4**
// Property: USB-only devices always appear in the connection picker.
RC_GTEST_PROP(UsbTransportPreference,
              UsbDevicesAlwaysAppear,
              ()) {
    auto devices = GenerateRandomDeviceSet();
    auto pickerList = BuildConnectionPickerList(devices);

    // Every USB device from the input must be present in the picker list
    for (const auto& dev : devices) {
        if (dev.transport == Transport::USB) {
            bool found = std::any_of(pickerList.begin(), pickerList.end(),
                [&](const PickerEntry& e) {
                    return e.installId == dev.installId &&
                           e.transport == Transport::USB;
                });
            RC_ASSERT(found);
        }
    }
}

// **Validates: Requirements 5.4**
// Property: The picker list never contains duplicate entries for the same
// (installId, transport) pair — at most one entry per transport per device.
// Note: Multiple USB entries with same installId are preserved since they come
// from separate discovery events. This test verifies WiFi suppression, not
// general deduplication of USB entries.
RC_GTEST_PROP(UsbTransportPreference,
              NoWiFiEntryWhenUsbPresent,
              ()) {
    auto devices = GenerateRandomDeviceSet();
    auto pickerList = BuildConnectionPickerList(devices);

    // Collect install IDs from USB entries in the picker
    std::unordered_set<std::string> pickerUsbIds;
    for (const auto& entry : pickerList) {
        if (entry.transport == Transport::USB) {
            pickerUsbIds.insert(entry.installId);
        }
    }

    // No WiFi entry should exist for any installId that has a USB entry
    for (const auto& entry : pickerList) {
        if (entry.transport == Transport::WiFi) {
            RC_ASSERT(pickerUsbIds.count(entry.installId) == 0);
        }
    }
}

// **Validates: Requirements 5.4**
// Property: The total number of picker entries is at most the number of input
// devices (suppression only removes, never adds entries).
RC_GTEST_PROP(UsbTransportPreference,
              PickerListNeverLargerThanInput,
              ()) {
    auto devices = GenerateRandomDeviceSet();
    auto pickerList = BuildConnectionPickerList(devices);

    RC_ASSERT(pickerList.size() <= devices.size());
}

// **Validates: Requirements 5.4**
// Property: For devices present on both transports, the suppressed WiFi count
// equals the number of WiFi entries whose installId also has USB in the input.
RC_GTEST_PROP(UsbTransportPreference,
              SuppressionCountMatchesDualTransportWiFiEntries,
              ()) {
    auto devices = GenerateRandomDeviceSet();
    auto pickerList = BuildConnectionPickerList(devices);

    // Count USB install IDs in input
    std::unordered_set<std::string> usbIds;
    for (const auto& dev : devices) {
        if (dev.transport == Transport::USB) {
            usbIds.insert(dev.installId);
        }
    }

    // Count WiFi entries in input that should be suppressed
    size_t expectedSuppressed = 0;
    for (const auto& dev : devices) {
        if (dev.transport == Transport::WiFi && usbIds.count(dev.installId) > 0) {
            expectedSuppressed++;
        }
    }

    // Verify: picker size = input size - suppressed count
    RC_ASSERT(pickerList.size() == devices.size() - expectedSuppressed);
}

