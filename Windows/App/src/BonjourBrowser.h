#pragma once

#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include <dns_sd.h>

/// Discovers iOS and Mac receivers on the local network using DNS-SD/mDNS.
/// Browses for "_opensidecar._tcp" services and reads TXT records for
/// device identity and protocol version.
///
/// Validates: Requirements 5.1
class BonjourBrowser {
public:
    /// Represents a discovered receiver service on the network.
    struct DiscoveredService {
        std::string name;           ///< Service instance name
        std::string host;           ///< Resolved hostname or IP address
        uint16_t port;              ///< TCP port number
        std::string installId;      ///< TXT "id" field (default "unknown" if absent)
        int protocolVersion;        ///< TXT "pv" field (default 1 if absent)
        std::string deviceType;     ///< TXT "device" field ("Mac" or absent for iOS)
    };

    /// Start browsing for "_opensidecar._tcp" services on the local network.
    /// @return S_OK on success, or error if Bonjour is unavailable.
    HRESULT StartBrowsing();

    /// Stop browsing and release DNS-SD resources.
    void StopBrowsing();

    /// Get current list of discovered services (thread-safe).
    /// @return A snapshot of currently discovered services.
    std::vector<DiscoveredService> GetServices() const;

    /// Callback type for service list changes.
    using ServiceCallback = std::function<void(const std::vector<DiscoveredService>&)>;

    /// Set callback for when the discovered service list changes.
    /// @param callback Invoked with the updated service list on add/remove/update events.
    void SetCallback(ServiceCallback callback);

private:
    DNSServiceRef m_browseRef = nullptr;
    std::vector<DiscoveredService> m_services;
    mutable std::mutex m_mutex;
    ServiceCallback m_callback;

    // DNS-SD callbacks need access to private members
    friend void DNSSD_API BrowseCallback(
        DNSServiceRef, DNSServiceFlags, uint32_t, DNSServiceErrorType,
        const char*, const char*, const char*, void*);
    friend void DNSSD_API ResolveCallback(
        DNSServiceRef, DNSServiceFlags, uint32_t, DNSServiceErrorType,
        const char*, const char*, uint16_t, uint16_t,
        const unsigned char*, void*);
};
