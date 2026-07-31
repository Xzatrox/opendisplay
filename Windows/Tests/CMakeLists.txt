cmake_minimum_required(VERSION 3.21)
project(OpenDisplayTests LANGUAGES CXX)

# ─── OpenDisplay Tests (Google Test + RapidCheck) ─────────────────────────────

include(FetchContent)

# ─── Google Test ──────────────────────────────────────────────────────────────
FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG        v1.14.0
)

# Prevent GoogleTest from overriding compiler/linker options on Windows
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(googletest)

# ─── RapidCheck (property-based testing) ──────────────────────────────────────
FetchContent_Declare(
    rapidcheck
    GIT_REPOSITORY https://github.com/emil-e/rapidcheck.git
    GIT_TAG        master
)

# Enable RapidCheck's Google Test integration
set(RC_ENABLE_GTEST ON CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(rapidcheck)

# ─── nlohmann/json (header-only JSON library) ─────────────────────────────────
# Use the already-fetched source from the top-level build if available,
# otherwise fetch from git.
set(_NLOHMANN_LOCAL_SRC "${CMAKE_CURRENT_SOURCE_DIR}/../build/_deps/nlohmann_json-src")
if(EXISTS "${_NLOHMANN_LOCAL_SRC}/CMakeLists.txt")
    FetchContent_Declare(
        nlohmann_json
        SOURCE_DIR "${_NLOHMANN_LOCAL_SRC}"
    )
else()
    FetchContent_Declare(
        nlohmann_json
        GIT_REPOSITORY https://github.com/nlohmann/json.git
        GIT_TAG        v3.11.3
    )
endif()
set(JSON_BuildTests OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(nlohmann_json)

# ─── Test executable ──────────────────────────────────────────────────────────

enable_testing()

set(TEST_SOURCES
    test_main.cpp
    test_resolution.cpp
    test_input_normalization.cpp
    test_capture_reinit_threshold.cpp
    test_frame_drop_encoder_busy.cpp
    test_keyframe_request_fulfillment.cpp
    test_keyframe_nal_structure.cpp
    test_wifi_retry.cpp
    test_amds_transport_selection.cpp
    test_usbmux_plist_roundtrip.cpp
    test_wire_frame_roundtrip.cpp
    test_exponential_backoff.cpp
    test_bonjour_txt_parsing.cpp
    test_usb_transport_preference.cpp
    test_protocol_handler.cpp
    test_protocol_version_gating.cpp
    test_control_message_wire_constraints.cpp
    test_input_injection_state_machine.cpp
    test_session_shutdown.cpp
    stubs/component_stubs.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../App/src/ProtocolHandler.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../App/src/SessionController.cpp
)

add_executable(OpenDisplayTests ${TEST_SOURCES})

target_include_directories(OpenDisplayTests
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/stubs
        ${CMAKE_CURRENT_SOURCE_DIR}/../Shared
        ${CMAKE_CURRENT_SOURCE_DIR}/../App/src
)

target_link_libraries(OpenDisplayTests
    PRIVATE
        GTest::gtest
        GTest::gtest_main
        rapidcheck
        rapidcheck_gtest
        nlohmann_json::nlohmann_json
        setupapi
        ws2_32
)

target_compile_definitions(OpenDisplayTests
    PRIVATE
        UNICODE
        _UNICODE
        WIN32_LEAN_AND_MEAN
        NOMINMAX
)

include(GoogleTest)
gtest_discover_tests(OpenDisplayTests)
