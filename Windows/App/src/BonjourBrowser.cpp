#include "BonjourBrowser.h"

#include <algorithm>
#include <cstring>
#include <string>
#include <thread>

#ifndef _WIN32
#include <arpa/inet.h>
#else
#include <winsock2.h>
#endif

// Service type we browse for
static constexpr const char* kServiceType = "_opensidecar._tcp";

// Default values for missing TXT record fields
static constexpr const char* kDefaultInstallId = "unknown";
static constexpr int kDefaultProtocolVersion = 1;

// ─── TXT Record Parsing Helpers ─────────────────────────────────────────────

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

/// Extract a DiscoveredService's TXT fields from parsed key-value pairs.
/// Applies defaults for missing fields per requirements 5.2 and 8.6.
static void ApplyTxtFields(BonjourBrowser::DiscoveredService& service,
                           const std::vector<std::pair<std::string, std::string>>& fields) {
    // Start with defaults
    service.installId = kDefaultInstallId;
    service.protocolVersion = kDefaultProtocolVersion;
    service.deviceType = "";

    for (const auto& [key, value] : fields) {
        if (key == "id") {
            service.installId = value.empty() ? kDefaultInstallId : value;
        } else if (key == "pv") {
            try {
                service.protocolVersion = std::stoi(value);
            } catch (...) {
                service.protocolVersion = kDefaultProtocolVersion;
            }
        } else if (key == "device") {
            service.deviceType = value;
        }
    }
}

// ─── DNS-SD Callback: Resolve Reply ─────────────────────────────────────────

struct ResolveContext {
    BonjourBrowser* browser;
    std::string serviceName;
};

static void DNSSD_API ResolveCallback(
    DNSServiceRef sdRef,
    DNSServiceFlags flags,
    uint32_t interfaceIndex,
    DNSServiceErrorType errorCode,
    const char* fullname,
    const char* hosttarget,
    uint16_t port, // network byte order
    uint16_t txtLen,
    const unsigned char* txtRecord,
    void* context) {

    auto* ctx = static_cast<ResolveContext*>(context);
    if (!ctx || !ctx->browser) {
        delete ctx;
        return;
    }

    if (errorCode != kDNSServiceErr_NoError) {
        delete ctx;
        DNSServiceRefDeallocate(sdRef);
        return;
    }

    BonjourBrowser::DiscoveredService service;
    service.name = ctx->serviceName;
    service.host = hosttarget ? hosttarget : "";
    service.port = ntohs(port);

    // Parse TXT record for id, pv, device fields
    auto fields = ParseTxtRecord(txtLen, txtRecord);
    ApplyTxtFields(service, fields);

    // Add or update the service in the browser's list
    {
        std::lock_guard<std::mutex> lock(ctx->browser->m_mutex);

        // Check if service already exists (by name), update it
        auto it = std::find_if(ctx->browser->m_services.begin(),
                               ctx->browser->m_services.end(),
                               [&](const BonjourBrowser::DiscoveredService& s) {
                                   return s.name == service.name;
                               });

        if (it != ctx->browser->m_services.end()) {
            *it = std::move(service);
        } else {
            ctx->browser->m_services.push_back(std::move(service));
        }
    }

    // Notify callback with updated service list
    if (ctx->browser->m_callback) {
        auto services = ctx->browser->GetServices();
        ctx->browser->m_callback(services);
    }

    delete ctx;
    DNSServiceRefDeallocate(sdRef);
}

// ─── DNS-SD Callback: Browse Reply ──────────────────────────────────────────

static void DNSSD_API BrowseCallback(
    DNSServiceRef sdRef,
    DNSServiceFlags flags,
    uint32_t interfaceIndex,
    DNSServiceErrorType errorCode,
    const char* serviceName,
    const char* regtype,
    const char* replyDomain,
    void* context) {

    auto* browser = static_cast<BonjourBrowser*>(context);
    if (!browser) {
        return;
    }

    if (errorCode != kDNSServiceErr_NoError) {
        return;
    }

    std::string name = serviceName ? serviceName : "";

    if (flags & kDNSServiceFlagsAdd) {
        // Service added — resolve it to get host, port, and TXT record
        auto* resolveCtx = new ResolveContext{browser, name};
        DNSServiceRef resolveRef = nullptr;

        DNSServiceErrorType err = DNSServiceResolve(
            &resolveRef,
            0, // flags
            interfaceIndex,
            serviceName,
            regtype,
            replyDomain,
            ResolveCallback,
            resolveCtx);

        if (err == kDNSServiceErr_NoError && resolveRef) {
            // Process the resolve reply (single-shot)
            DNSServiceProcessResult(resolveRef);
        } else {
            delete resolveCtx;
            if (resolveRef) {
                DNSServiceRefDeallocate(resolveRef);
            }
        }
    } else {
        // Service removed
        bool changed = false;
        {
            std::lock_guard<std::mutex> lock(browser->m_mutex);
            auto it = std::find_if(browser->m_services.begin(),
                                   browser->m_services.end(),
                                   [&](const BonjourBrowser::DiscoveredService& s) {
                                       return s.name == name;
                                   });
            if (it != browser->m_services.end()) {
                browser->m_services.erase(it);
                changed = true;
            }
        }

        if (changed && browser->m_callback) {
            auto services = browser->GetServices();
            browser->m_callback(services);
        }
    }
}

// ─── BonjourBrowser Public Interface ────────────────────────────────────────

HRESULT BonjourBrowser::StartBrowsing() {
    // If already browsing, stop first
    if (m_browseRef) {
        StopBrowsing();
    }

    DNSServiceErrorType err = DNSServiceBrowse(
        &m_browseRef,
        0, // flags
        0, // interfaceIndex (all interfaces)
        kServiceType,
        nullptr, // domain (default)
        BrowseCallback,
        this);

    if (err != kDNSServiceErr_NoError) {
        m_browseRef = nullptr;
        return E_FAIL;
    }

    // Launch a background thread to process DNS-SD events
    std::thread([this]() {
        while (m_browseRef) {
            DNSServiceErrorType processErr = DNSServiceProcessResult(m_browseRef);
            if (processErr != kDNSServiceErr_NoError) {
                break;
            }
        }
    }).detach();

    return S_OK;
}

void BonjourBrowser::StopBrowsing() {
    if (m_browseRef) {
        DNSServiceRefDeallocate(m_browseRef);
        m_browseRef = nullptr;
    }

    std::lock_guard<std::mutex> lock(m_mutex);
    m_services.clear();
}

std::vector<BonjourBrowser::DiscoveredService> BonjourBrowser::GetServices() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_services;
}

void BonjourBrowser::SetCallback(ServiceCallback callback) {
    m_callback = std::move(callback);
}
