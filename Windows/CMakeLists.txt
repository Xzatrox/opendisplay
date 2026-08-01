cmake_minimum_required(VERSION 3.21)
project(OpenDisplay VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ─── Platform and toolchain checks ───────────────────────────────────────────

if(NOT WIN32)
    message(FATAL_ERROR "This project requires Windows. Current platform is not supported.")
endif()

# Require Visual Studio 2022 (MSVC v143+)
if(MSVC)
    if(MSVC_TOOLSET_VERSION LESS 143)
        message(FATAL_ERROR
            "Visual Studio 2022 (toolset v143) or later is required.\n"
            "Detected toolset version: ${MSVC_TOOLSET_VERSION}\n"
            "Please install Visual Studio 2022 with the 'Desktop development with C++' workload."
        )
    endif()
else()
    message(FATAL_ERROR
        "MSVC compiler is required. Please configure CMake with Visual Studio 2022 generator.\n"
        "Example: cmake -G \"Visual Studio 17 2022\" .."
    )
endif()

# ─── Windows SDK check (minimum 10.0.22621.0) ────────────────────────────────

if(NOT DEFINED CMAKE_SYSTEM_VERSION)
    set(CMAKE_SYSTEM_VERSION "10.0.22621.0")
endif()

if(CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION)
    set(_WIN_SDK_VERSION "${CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION}")
elseif(DEFINED ENV{WindowsSDKVersion})
    # Strip trailing backslash from environment variable
    string(REGEX REPLACE "\\\\$" "" _WIN_SDK_VERSION "$ENV{WindowsSDKVersion}")
else()
    set(_WIN_SDK_VERSION "")
endif()

if(_WIN_SDK_VERSION)
    if(_WIN_SDK_VERSION VERSION_LESS "10.0.22621.0")
        message(FATAL_ERROR
            "Windows SDK 10.0.22621.0 (Windows 11 22H2) or later is required.\n"
            "Detected SDK version: ${_WIN_SDK_VERSION}\n"
            "Please install the Windows 11 SDK (10.0.22621.0) via Visual Studio Installer\n"
            "or download from https://developer.microsoft.com/en-us/windows/downloads/windows-sdk/"
        )
    endif()
else()
    message(WARNING
        "Could not detect Windows SDK version. Ensure Windows SDK 10.0.22621.0 or later is installed.\n"
        "Install via Visual Studio Installer → Individual Components → Windows 11 SDK (10.0.22621.0)."
    )
endif()

# ─── Windows Driver Kit (WDK) check ──────────────────────────────────────────

set(_WDK_FOUND FALSE)

# Check local deps/WDK (NuGet package) first
set(_WDK_NUGET_DIR "${CMAKE_SOURCE_DIR}/deps/WDK/Microsoft.Windows.WDK.x64.10.0.26100.2454/c")
if(EXISTS "${_WDK_NUGET_DIR}/Include/10.0.26100.0/um")
    set(_WDK_FOUND TRUE)
    set(WDK_ROOT "${_WDK_NUGET_DIR}")
endif()

# Then check common WDK system installation paths
if(NOT _WDK_FOUND)
    set(_WDK_SEARCH_PATHS
        "$ENV{WDKContentRoot}"
        "C:/Program Files (x86)/Windows Kits/10"
        "C:/Program Files/Windows Kits/10"
    )

    foreach(_WDK_PATH ${_WDK_SEARCH_PATHS})
        if(EXISTS "${_WDK_PATH}/Include/wdk")
            set(_WDK_FOUND TRUE)
            set(WDK_ROOT "${_WDK_PATH}")
            break()
        endif()
        if(EXISTS "${_WDK_PATH}/Include/10.0.22621.0/km")
            set(_WDK_FOUND TRUE)
            set(WDK_ROOT "${_WDK_PATH}")
            break()
        endif()
    endforeach()
endif()

if(NOT _WDK_FOUND)
    message(WARNING
        "Windows Driver Kit (WDK) was not found. The Driver target will not be built.\n"
        "To build the IDD driver, install WDK 10.0.22621.0 or later:\n"
        "  1. Install Visual Studio 2022 with 'Desktop development with C++'\n"
        "  2. Download WDK from https://learn.microsoft.com/en-us/windows-hardware/drivers/download-the-wdk\n"
        "  3. Ensure the WDK VS extension is installed.\n"
        "Set WDKContentRoot environment variable if installed in a non-standard location."
    )
endif()

# ─── Sub-projects ─────────────────────────────────────────────────────────────

# Application target (Win32/WinUI 3)
add_subdirectory(App)

# Driver target (UMDF IDD — only if WDK is available)
if(_WDK_FOUND)
    add_subdirectory(Driver)
else()
    message(STATUS "Skipping Driver target (WDK not found).")
endif()

# Tests
add_subdirectory(Tests)
