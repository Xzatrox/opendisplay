// ─── DNS-SD API dynamic loader ───────────────────────────────────────────────
// Dynamically loads dnssd.dll at runtime from known locations. This avoids a
// hard compile-time dependency on the Bonjour SDK while still enabling mDNS
// discovery when Bonjour is installed (via iTunes, Apple Devices, or the
// standalone Bonjour Print Services installer).
//
// If dnssd.dll cannot be found, all functions return error gracefully.

#include "dns_sd.h"
#include <windows.h>
#include <string>
#include <vector>

namespace {

// Function pointer types matching dns_sd.h signatures
using PFN_DNSServiceBrowse = DNSServiceErrorType (DNSSD_API*)(
    DNSServiceRef*, DNSServiceFlags, uint32_t, const char*, const char*,
    DNSServiceBrowseReply, void*);
using PFN_DNSServiceResolve = DNSServiceErrorType (DNSSD_API*)(
    DNSServiceRef*, DNSServiceFlags, uint32_t, const char*, const char*,
    const char*, DNSServiceResolveReply, void*);
using PFN_DNSServiceProcessResult = DNSServiceErrorType (DNSSD_API*)(DNSServiceRef);
using PFN_DNSServiceRefDeallocate = void (DNSSD_API*)(DNSServiceRef);

// Resolved function pointers (null if DLL not found)
static PFN_DNSServiceBrowse g_pBrowse = nullptr;
static PFN_DNSServiceResolve g_pResolve = nullptr;
static PFN_DNSServiceProcessResult g_pProcessResult = nullptr;
static PFN_DNSServiceRefDeallocate g_pDeallocate = nullptr;

static HMODULE g_hDnsSd = nullptr;
static bool g_initialized = false;

// Attempt to load dnssd.dll from various known locations
static void InitializeDnsSd() {
    if (g_initialized) return;
    g_initialized = true;

    // Try standard system path first (Bonjour Print Services installs here)
    const wchar_t* searchPaths[] = {
        L"dnssd.dll",  // System PATH (includes Bonjour install dir)
        L"C:\\Program Files\\Bonjour\\dnssd.dll",
        L"C:\\Program Files (x86)\\Bonjour\\dnssd.dll",
    };

    for (const auto* path : searchPaths) {
        g_hDnsSd = LoadLibraryW(path);
        if (g_hDnsSd) break;
    }

    // If not found in standard locations, try known bundled copies
    if (!g_hDnsSd) {
        // Apple Devices / iTunes installs Bonjour as a Windows service
        // but may not put dnssd.dll on PATH. Check Program Files.
        WIN32_FIND_DATAW fd;
        const wchar_t* searchGlobs[] = {
            L"C:\\Program Files\\Apple\\Apple Devices\\dnssd.dll",
            L"C:\\Program Files\\Common Files\\Apple\\Apple Application Support\\dnssd.dll",
        };
        for (const auto* glob : searchGlobs) {
            HANDLE hFind = FindFirstFileW(glob, &fd);
            if (hFind != INVALID_HANDLE_VALUE) {
                FindClose(hFind);
                g_hDnsSd = LoadLibraryW(glob);
                if (g_hDnsSd) break;
            }
        }
    }

    if (!g_hDnsSd) return;

    // Resolve function pointers
    g_pBrowse = reinterpret_cast<PFN_DNSServiceBrowse>(
        GetProcAddress(g_hDnsSd, "DNSServiceBrowse"));
    g_pResolve = reinterpret_cast<PFN_DNSServiceResolve>(
        GetProcAddress(g_hDnsSd, "DNSServiceResolve"));
    g_pProcessResult = reinterpret_cast<PFN_DNSServiceProcessResult>(
        GetProcAddress(g_hDnsSd, "DNSServiceProcessResult"));
    g_pDeallocate = reinterpret_cast<PFN_DNSServiceRefDeallocate>(
        GetProcAddress(g_hDnsSd, "DNSServiceRefDeallocate"));

    // If any critical function is missing, treat as unavailable
    if (!g_pBrowse || !g_pResolve || !g_pProcessResult || !g_pDeallocate) {
        FreeLibrary(g_hDnsSd);
        g_hDnsSd = nullptr;
        g_pBrowse = nullptr;
        g_pResolve = nullptr;
        g_pProcessResult = nullptr;
        g_pDeallocate = nullptr;
    }
}

} // namespace

extern "C" {

DNSServiceErrorType DNSSD_API DNSServiceBrowse(
    DNSServiceRef* sdRef,
    DNSServiceFlags flags,
    uint32_t interfaceIndex,
    const char* regtype,
    const char* domain,
    DNSServiceBrowseReply callBack,
    void* context)
{
    InitializeDnsSd();
    if (!g_pBrowse) {
        if (sdRef) *sdRef = nullptr;
        return -1; // kDNSServiceErr_Unknown
    }
    return g_pBrowse(sdRef, flags, interfaceIndex, regtype, domain, callBack, context);
}

DNSServiceErrorType DNSSD_API DNSServiceResolve(
    DNSServiceRef* sdRef,
    DNSServiceFlags flags,
    uint32_t interfaceIndex,
    const char* name,
    const char* regtype,
    const char* domain,
    DNSServiceResolveReply callBack,
    void* context)
{
    InitializeDnsSd();
    if (!g_pResolve) {
        if (sdRef) *sdRef = nullptr;
        return -1;
    }
    return g_pResolve(sdRef, flags, interfaceIndex, name, regtype, domain, callBack, context);
}

DNSServiceErrorType DNSSD_API DNSServiceProcessResult(DNSServiceRef sdRef)
{
    InitializeDnsSd();
    if (!g_pProcessResult) return -1;
    return g_pProcessResult(sdRef);
}

void DNSSD_API DNSServiceRefDeallocate(DNSServiceRef sdRef)
{
    InitializeDnsSd();
    if (g_pDeallocate && sdRef) {
        g_pDeallocate(sdRef);
    }
}

} // extern "C"
