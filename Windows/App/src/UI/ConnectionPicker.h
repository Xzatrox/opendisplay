// OpenDisplay Windows Sender — Connection Picker Control
//
// Displays discovered USB and WiFi devices. USB entries are preferred;
// devices available on both USB and WiFi (same installId) show only USB.
//
// Validates: Requirements 10.1, 5.4

#pragma once

#include <algorithm>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include <Windows.h>

#include "../SessionController.h"

namespace OpenDisplay {

/// Connection picker UI control for displaying and selecting discovered devices.
/// Shows USB devices preferentially; suppresses WiFi entries for devices
/// also visible on USB (matched by install ID / UDID).
class ConnectionPicker {
public:
    /// Callback type for when the user requests a connection to a device.
    using ConnectCallback = std::function<void(const DeviceInfo&)>;

    /// Callback type for when the user toggles auto-connect for a device.
    using AutoConnectCallback = std::function<void(const std::string& installId, bool enabled)>;

    ConnectionPicker() = default;
    ~ConnectionPicker() = default;

    /// Set the callback invoked when the user clicks "Connect" on a device.
    void SetConnectCallback(ConnectCallback callback) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_connectCallback = std::move(callback);
    }

    /// Set the callback invoked when auto-connect is toggled.
    void SetAutoConnectCallback(AutoConnectCallback callback) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_autoConnectCallback = std::move(callback);
    }

    /// Update the list of WiFi-discovered devices.
    /// Merges with current USB devices, suppressing WiFi entries when
    /// the same installId is already present via USB.
    /// @param devices The complete list of WiFi-discovered devices.
    void UpdateWiFiDevices(const std::vector<DeviceInfo>& devices) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_wifiDevices = devices;
        RebuildDisplayList();
    }

    /// Add a USB device to the picker. If a WiFi entry exists for the same
    /// installId, the WiFi entry is suppressed in the display list.
    /// @param device The USB device info to add.
    void AddUSBDevice(const DeviceInfo& device) {
        std::lock_guard<std::mutex> lock(m_mutex);
        // Avoid duplicates
        for (const auto& existing : m_usbDevices) {
            if (existing.udid == device.udid) {
                return;
            }
        }
        m_usbDevices.push_back(device);
        RebuildDisplayList();
    }

    /// Remove a USB device from the picker by UDID.
    /// If a WiFi entry exists for the same installId, it becomes visible again.
    /// @param udid The UDID of the device to remove.
    void RemoveUSBDevice(const std::string& udid) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_usbDevices.erase(
            std::remove_if(m_usbDevices.begin(), m_usbDevices.end(),
                [&udid](const DeviceInfo& d) { return d.udid == udid; }),
            m_usbDevices.end());
        RebuildDisplayList();
    }

    /// Get the current display list (thread-safe snapshot).
    std::vector<DeviceInfo> GetDisplayList() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_displayList;
    }

private:
    /// Rebuild the merged display list with USB preference.
    /// USB entries always shown; WiFi entries suppressed when same installId on USB.
    void RebuildDisplayList() {
        m_displayList.clear();

        // Add all USB devices first
        for (const auto& usb : m_usbDevices) {
            m_displayList.push_back(usb);
        }

        // Add WiFi devices only if not already present via USB (by installId/udid)
        for (const auto& wifi : m_wifiDevices) {
            bool suppressedByUSB = false;
            for (const auto& usb : m_usbDevices) {
                if (usb.udid == wifi.udid && !wifi.udid.empty()) {
                    suppressedByUSB = true;
                    break;
                }
            }
            if (!suppressedByUSB) {
                m_displayList.push_back(wifi);
            }
        }
    }

    mutable std::mutex m_mutex;
    ConnectCallback m_connectCallback;
    AutoConnectCallback m_autoConnectCallback;

    std::vector<DeviceInfo> m_usbDevices;
    std::vector<DeviceInfo> m_wifiDevices;
    std::vector<DeviceInfo> m_displayList;
};

} // namespace OpenDisplay
