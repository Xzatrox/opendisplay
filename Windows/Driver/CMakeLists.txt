cmake_minimum_required(VERSION 3.21)

# ─── OpenDisplay IDD Virtual Display Driver (UMDF 2.31) ──────────────────────
#
# Builds as a UMDF DLL using the IddCx framework.
# Requires WDK NuGet package in Windows/deps/WDK/

# Locate WDK from NuGet package in deps
set(WDK_NUGET_DIR "${CMAKE_SOURCE_DIR}/deps/WDK/Microsoft.Windows.WDK.x64.10.0.26100.2454/c")

if(NOT EXISTS "${WDK_NUGET_DIR}/Include/10.0.26100.0/um")
    message(FATAL_ERROR
        "WDK NuGet package not found at ${WDK_NUGET_DIR}.\n"
        "Run: nuget install Microsoft.Windows.WDK.x64 -Version 10.0.26100.2454 -OutputDirectory Windows/deps/WDK"
    )
endif()

set(WDK_INC "${WDK_NUGET_DIR}/Include/10.0.26100.0")
set(WDK_LIB "${WDK_NUGET_DIR}/Lib/10.0.26100.0/um/x64")
set(WDK_WDF_INC "${WDK_NUGET_DIR}/Include/wdf/umdf/2.31")
set(WDK_WDF_LIB "${WDK_NUGET_DIR}/Lib/wdf/umdf/x64/2.31")
set(WDK_IDDCX_INC "${WDK_NUGET_DIR}/Include/10.0.26100.0/um/iddcx/1.4")
set(WDK_IDDCX_LIB "${WDK_NUGET_DIR}/Lib/10.0.26100.0/um/x64/iddcx/1.4")

set(DRIVER_SOURCES
    src/Driver.cpp
    src/Monitor.cpp
    src/SwapChain.cpp
)

# UMDF drivers are user-mode DLLs
add_library(OpenDisplayIdd SHARED ${DRIVER_SOURCES})

target_include_directories(OpenDisplayIdd
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/Shared
        "${WDK_INC}/um"
        "${WDK_INC}/shared"
        "${WDK_INC}/km"
        "${WDK_WDF_INC}"
        "${WDK_IDDCX_INC}"
)

target_compile_definitions(OpenDisplayIdd
    PRIVATE
        UNICODE
        _UNICODE
        WIN32_LEAN_AND_MEAN
        NOMINMAX
        UMDF_USING_NTSTATUS
        UMDF_VERSION_MAJOR=2
        UMDF_VERSION_MINOR=31
        IDDCX_VERSION_MAJOR=1
        IDDCX_VERSION_MINOR=4
        _WIN32_WINNT=0x0A00
        WINNT=1
)

# UMDF drivers link against WdfDriverStubUm (provides DriverEntry thunk)
# and the IddCx stub library
target_link_directories(OpenDisplayIdd
    PRIVATE
        "${WDK_LIB}"
        "${WDK_WDF_LIB}"
        "${WDK_IDDCX_LIB}"
)

target_link_libraries(OpenDisplayIdd
    PRIVATE
        WdfDriverStubUm
        iddcxstub
        ntdll
        ucrt
        onecoreuap
)

# UMDF driver entry point is not main/WinMain — it's FxDriverEntry
# which is provided by WdfDriverStubUm.lib
set_target_properties(OpenDisplayIdd PROPERTIES
    SUFFIX ".dll"
    OUTPUT_NAME "OpenDisplayIdd"
    LINK_FLAGS "/ENTRY:FxDriverEntry /SUBSYSTEM:NATIVE"
)

# Resource file for driver version information
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/OpenDisplayIdd.rc")
    target_sources(OpenDisplayIdd PRIVATE OpenDisplayIdd.rc)
endif()
