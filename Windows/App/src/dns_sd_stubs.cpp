// ─── DNS-SD API stub implementations ─────────────────────────────────────────
// Provides no-op implementations of the Bonjour DNS-SD functions when
// the Apple Bonjour SDK is not installed. This allows the application to
// compile and link; Bonjour discovery will simply fail gracefully.
//
// When the Bonjour SDK is properly installed, this file should be removed
// from the build and the real dnssd.lib linked instead.

#include "dns_sd.h"

extern "C" {

DNSServiceErrorType DNSSD_API DNSServiceBrowse(
    DNSServiceRef* sdRef,
    DNSServiceFlags /*flags*/,
    uint32_t /*interfaceIndex*/,
    const char* /*regtype*/,
    const char* /*domain*/,
    DNSServiceBrowseReply /*callBack*/,
    void* /*context*/)
{
    if (sdRef) *sdRef = nullptr;
    // Return error to indicate Bonjour is unavailable
    return -1;
}

DNSServiceErrorType DNSSD_API DNSServiceResolve(
    DNSServiceRef* sdRef,
    DNSServiceFlags /*flags*/,
    uint32_t /*interfaceIndex*/,
    const char* /*name*/,
    const char* /*regtype*/,
    const char* /*domain*/,
    DNSServiceResolveReply /*callBack*/,
    void* /*context*/)
{
    if (sdRef) *sdRef = nullptr;
    return -1;
}

DNSServiceErrorType DNSSD_API DNSServiceProcessResult(DNSServiceRef /*sdRef*/)
{
    return -1;
}

void DNSSD_API DNSServiceRefDeallocate(DNSServiceRef /*sdRef*/)
{
    // No-op
}

} // extern "C"
