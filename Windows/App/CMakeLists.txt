cmake_minimum_required(VERSION 3.21)

# ─── OpenDisplay Windows Sender Application ──────────────────────────────────

include(FetchContent)

# ─── nlohmann/json (header-only JSON library) ─────────────────────────────────
FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG        v3.11.3
)

FetchContent_MakeAvailable(nlohmann_json)

set(APP_SOURCES
    src/main.cpp
    src/DesktopDuplicationCapture.cpp
    src/MFTEncoder.cpp
    src/WireTransport.cpp
    src/AmdsClient.cpp
    src/BonjourBrowser.cpp
    src/ProtocolHandler.cpp
    src/WindowsInputInjector.cpp
    src/SessionController.cpp
    src/UI/MainWindow.cpp
    src/UI/ConnectionPicker.cpp
    src/dns_sd_stubs.cpp
)

# WinUI 3 / Win32 application executable
add_executable(OpenDisplayApp WIN32 ${APP_SOURCES})

target_include_directories(OpenDisplayApp
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/Shared
        ${CMAKE_SOURCE_DIR}/Tests/stubs
)

target_compile_definitions(OpenDisplayApp
    PRIVATE
        UNICODE
        _UNICODE
        WIN32_LEAN_AND_MEAN
        NOMINMAX
        _WIN32_WINNT=0x0A00
)

target_link_libraries(OpenDisplayApp
    PRIVATE
        d3d11
        dxgi
        mf
        mfplat
        mfreadwrite
        mfuuid
        strmiids
        ws2_32
        user32
        ole32
        setupapi
        nlohmann_json::nlohmann_json
)

# Set Windows subsystem
set_target_properties(OpenDisplayApp PROPERTIES
    VS_GLOBAL_ROOTNAMESPACE "OpenDisplay"
    OUTPUT_NAME "OpenDisplay"
)
