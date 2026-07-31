#pragma once
// ─── Stub dns_sd.h for builds without Bonjour SDK ───────────────────────────
// Provides minimal type definitions and function declarations so
// BonjourBrowser.h and BonjourBrowser.cpp compile without the actual
// Apple Bonjour SDK installed.

#include <cstdint>

#ifdef _WIN32
#include <Windows.h>
#define DNSSD_API __stdcall
#else
#define DNSSD_API
#endif

// ─── Types ───────────────────────────────────────────────────────────────────

typedef struct _DNSServiceRef_t* DNSServiceRef;
typedef uint32_t DNSServiceFlags;
typedef int32_t DNSServiceErrorType;

// ─── Constants ───────────────────────────────────────────────────────────────

#define kDNSServiceErr_NoError 0
#define kDNSServiceFlagsAdd 0x2

// ─── Callback Types ──────────────────────────────────────────────────────────

typedef void (DNSSD_API *DNSServiceBrowseReply)(
    DNSServiceRef sdRef,
    DNSServiceFlags flags,
    uint32_t interfaceIndex,
    DNSServiceErrorType errorCode,
    const char* serviceName,
    const char* regtype,
    const char* replyDomain,
    void* context);

typedef void (DNSSD_API *DNSServiceResolveReply)(
    DNSServiceRef sdRef,
    DNSServiceFlags flags,
    uint32_t interfaceIndex,
    DNSServiceErrorType errorCode,
    const char* fullname,
    const char* hosttarget,
    uint16_t port,
    uint16_t txtLen,
    const unsigned char* txtRecord,
    void* context);

// ─── Function Declarations (stub implementations in component_stubs.cpp) ─────

#ifdef __cplusplus
extern "C" {
#endif

DNSServiceErrorType DNSSD_API DNSServiceBrowse(
    DNSServiceRef* sdRef,
    DNSServiceFlags flags,
    uint32_t interfaceIndex,
    const char* regtype,
    const char* domain,
    DNSServiceBrowseReply callBack,
    void* context);

DNSServiceErrorType DNSSD_API DNSServiceResolve(
    DNSServiceRef* sdRef,
    DNSServiceFlags flags,
    uint32_t interfaceIndex,
    const char* name,
    const char* regtype,
    const char* domain,
    DNSServiceResolveReply callBack,
    void* context);

DNSServiceErrorType DNSSD_API DNSServiceProcessResult(DNSServiceRef sdRef);

void DNSSD_API DNSServiceRefDeallocate(DNSServiceRef sdRef);

#ifdef __cplusplus
}
#endif
