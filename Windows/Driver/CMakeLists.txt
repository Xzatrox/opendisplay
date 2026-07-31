cmake_minimum_required(VERSION 3.21)

# ─── OpenDisplay IDD Virtual Display Driver (UMDF) ───────────────────────────
#
# This target requires the Windows Driver Kit (WDK) to build.
# It produces a UMDF driver that uses the IddCx framework to create
# virtual monitors visible to the Windows desktop compositor.
#
# Build with: cmake --build . --target OpenDisplayIdd
# Note: The driver must be test-signed for development use.

# Verify WDK is available (should be checked at top-level, but guard here too)
if(NOT DEFINED WDK_ROOT)
    message(FATAL_ERROR
        "WDK_ROOT is not defined. The Driver target requires the Windows Driver Kit.\n"
        "Please install WDK 10.0.22621.0 or later and re-run CMake."
    )
endif()

set(DRIVER_SOURCES
    src/Driver.cpp
    src/Monitor.cpp
    src/SwapChain.cpp
)

# UMDF drivers are built as DLLs
add_library(OpenDisplayIdd SHARED ${DRIVER_SOURCES})

target_include_directories(OpenDisplayIdd
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/Shared
        "${WDK_ROOT}/Include/10.0.22621.0/um"
        "${WDK_ROOT}/Include/10.0.22621.0/shared"
        "${WDK_ROOT}/Include/10.0.22621.0/km"
)

target_compile_definitions(OpenDisplayIdd
    PRIVATE
        UNICODE
        _UNICODE
        WIN32_LEAN_AND_MEAN
        NOMINMAX
        _DRIVER
        UMDF_VERSION_MAJOR=2
        UMDF_VERSION_MINOR=31
)

target_link_directories(OpenDisplayIdd
    PRIVATE
        "${WDK_ROOT}/Lib/10.0.22621.0/um/x64"
)

target_link_libraries(OpenDisplayIdd
    PRIVATE
        WdfDriverEntry
        WdfLdr
        IddCx
        ntdll
)

set_target_properties(OpenDisplayIdd PROPERTIES
    SUFFIX ".dll"
    OUTPUT_NAME "OpenDisplayIdd"
)

# Resource file for driver version information
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/OpenDisplayIdd.rc")
    target_sources(OpenDisplayIdd PRIVATE OpenDisplayIdd.rc)
endif()
