# Implementation Plan: Windows Second Display

## Overview

This plan implements the Windows 11 sender platform for OpenDisplay, including an IDD virtual display driver, DXGI capture, Media Foundation H.264 encoding, USB (AMDS) and WiFi transport, wire protocol compatibility, input injection, and a Mac receiver mode. The implementation uses C++ (with CMake + WDK) for all Windows components and Swift for the Mac receiver mode.

## Tasks

- [x] 1. Set up project structure and build system
  - [x] 1.1 Create CMake super-project structure with App, Driver, Shared, and Tests directories
    - Create `Windows/CMakeLists.txt` top-level CMake project (minimum 3.21)
    - Create `Windows/App/CMakeLists.txt` for the Win32/WinUI 3 application target
    - Create `Windows/Driver/CMakeLists.txt` for the UMDF IDD driver target (requires WDK)
    - Create `Windows/Shared/DriverInterface.h` with IOCTL codes and shared structs (MonitorCreateParams, MonitorCreateResult, MonitorResizeParams)
    - Create `Windows/Tests/CMakeLists.txt` with Google Test + RapidCheck integration
    - Add CMake checks that emit descriptive errors if Windows SDK 10.0.22621.0, WDK, or VS 2022 are missing
    - _Requirements: 9.1, 9.2, 9.3, 9.4, 9.5, 9.6_

  - [x] 1.2 Define core interfaces and header files for all components
    - Create `Windows/App/src/DesktopDuplicationCapture.h` with the capture interface
    - Create `Windows/App/src/MFTEncoder.h` with encoder interface and Config struct
    - Create `Windows/App/src/WireTransport.h` with transport and AMDS client interfaces
    - Create `Windows/App/src/BonjourBrowser.h` with discovery interface
    - Create `Windows/App/src/WindowsInputInjector.h` with input injection interface
    - Create `Windows/App/src/SessionController.h` with session orchestration interface
    - _Requirements: 1.1, 2.1, 3.1, 4.1, 5.1, 6.1, 7.1, 10.1_

- [x] 2. Implement IDD Virtual Display Driver
  - [x] 2.1 Implement driver entry point and IddCx adapter initialization
    - Create `Windows/Driver/src/Driver.cpp` with DriverEntry, EvtDriverDeviceAdd
    - Implement IddCx adapter callbacks: AdapterInitFinished, AdapterCommitModes
    - Create `Windows/Driver/OpenDisplayIdd.inf` installation manifest
    - Create `Windows/Driver/OpenDisplayIdd.rc` version resource
    - _Requirements: 1.1, 9.3_

  - [x] 2.2 Implement monitor creation, resize, and destruction via IddCx
    - Create `Windows/Driver/src/Monitor.cpp` with MonitorCreate, MonitorDestroy, MonitorAssignSwapChain, MonitorUnassignSwapChain
    - Implement DeviceIoControl handlers for IOCTL_CREATE_MONITOR, IOCTL_DESTROY_MONITOR, IOCTL_RESIZE_MONITOR
    - Validate resolution constraints: even numbers, 640×480 to 2732×2048, 60 Hz
    - Implement handle-close callback to remove all monitors on unexpected process termination
    - _Requirements: 1.1, 1.2, 1.3, 1.4, 1.5, 1.6, 1.7, 1.8, 10.7_

  - [x] 2.3 Implement SwapChain processor (no-op frame presentation for capture path)
    - Create `Windows/Driver/src/SwapChain.cpp` with SwapChainProcessor that accepts and releases frames
    - The driver presents frames so DXGI Desktop Duplication can capture them from the compositor
    - _Requirements: 1.3, 2.1_

  - [x] 2.4 Write property test for display resolution computation
    - **Property 1: Display Resolution Computation**
    - Test with random (width, height) pairs in [0, 5000] that floor(dim/2) rounded to nearest even is computed correctly
    - Verify acceptance only when result is in [640, 2732] × [480, 2048], and rejection for zero/negative inputs
    - **Validates: Requirements 1.2, 1.6, 1.8**

- [x] 3. Implement DXGI Desktop Duplication Screen Capture
  - [x] 3.1 Implement DesktopDuplicationCapture initialization and frame acquisition
    - Create `Windows/App/src/DesktopDuplicationCapture.cpp`
    - Implement Initialize() to open IDXGIOutputDuplication on the virtual display's output
    - Implement AcquireFrame() with timeout-based non-blocking acquisition
    - Implement ReleaseFrame() for returning frames to DXGI
    - Deliver GPU-resident ID3D11Texture2D to encoder (zero-copy when supported)
    - _Requirements: 2.1, 2.2, 2.3, 2.4_

  - [x] 3.2 Implement capture failure recovery and reinitialize logic
    - Track consecutive failures; after 3, call Reinitialize()
    - Implement Reinitialize() for access-lost errors (mode change, secure desktop)
    - Reset consecutive failure counter on any success
    - Retry acquisition within 100ms on transient errors
    - _Requirements: 2.5_

  - [x] 3.3 Write property test for capture failure reinitialize threshold
    - **Property 2: Capture Failure Reinitialize Threshold**
    - Generate random sequences of success/failure results
    - Verify Reinitialize() called iff 3+ consecutive failures, counter resets on success
    - **Validates: Requirements 2.5**

- [x] 4. Implement Media Foundation H.264 Encoder
  - [x] 4.1 Implement MFTEncoder initialization and configuration
    - Create `Windows/App/src/MFTEncoder.cpp`
    - Implement Initialize() with MFT hardware encoder activation (Intel QSV / NVIDIA NVENC / AMD AMF)
    - Configure: H.264 High profile, CBR low-latency, no B-frames, real-time priority, max keyframe interval 3600
    - Implement quality presets: best=18Mbps, balanced=10Mbps, fast=6Mbps
    - Implement software encoder fallback when no hardware encoder available
    - _Requirements: 3.1, 3.2, 3.7, 3.8_

  - [x] 4.2 Implement frame submission, keyframe control, and output retrieval
    - Implement SubmitFrame() with pendingEncodes tracking; drop frame when pendingEncodes >= 1
    - Implement RequestKeyframe() to force IDR on next output
    - Implement GetOutput() returning Annex B NAL units with SPS/PPS prefixed on keyframes
    - Ensure first frame of session is always IDR
    - Handle encoder errors: log and cease encoding until next session
    - _Requirements: 3.3, 3.4, 3.5, 3.6, 3.9_

  - [x] 4.3 Write property test for frame drop on encoder busy
    - **Property 3: Frame Drop on Encoder Busy**
    - Generate random interleavings of capture arrivals and encode-completion events
    - Verify frames submitted only when pendingEncodes < 1, dropped otherwise, no forced keyframe after drop
    - **Validates: Requirements 2.6, 3.6**

  - [x] 4.4 Write property test for keyframe request fulfillment
    - **Property 4: Keyframe Request Fulfillment**
    - Generate random stream positions and kf message timings
    - Verify next encoded frame after kf is IDR prefixed with SPS+PPS
    - **Validates: Requirements 3.4, 6.5**

  - [x] 4.5 Write property test for keyframe NAL structure
    - **Property 5: Keyframe NAL Structure**
    - Generate random SPS/PPS/slice byte sequences
    - Verify Annex B output structure: [start code][SPS type=7][start code][PPS type=8][start code][IDR slice type=5]
    - **Validates: Requirements 3.5**

- [x] 5. Checkpoint - Ensure driver, capture, and encoder build and pass tests
  - Ensure all tests pass, ask the user if questions arise.

- [x] 6. Implement Wire Protocol and Transport Layer
  - [x] 6.1 Implement WireTransport for framed TCP video and control messages
    - Create `Windows/App/src/WireTransport.cpp`
    - Implement SendVideoFrame(): serialize [4B BE length][JSON telemetry {"cap":ms,"snd":ms}][Annex B payload]
    - Implement SendControl(): serialize JSON control messages (< 32KB, starts with '{', no NUL)
    - Implement receive thread that dispatches to control handler (JSON starting with '{') vs video
    - Track pendingSends; drop frames when pendingSends >= 3
    - _Requirements: 6.1, 6.9, 6.10_

  - [x] 6.2 Implement AMDS client for USB communication with iOS devices
    - Create `Windows/App/src/AmdsClient.cpp`
    - Implement Connect() with named pipe (`\\.\pipe\usbmux`) primary, TCP (localhost:27015) fallback
    - Implement ListDevices() using usbmuxd plist protocol (16-byte LE header + XML plist body)
    - Implement Subscribe() for attach/detach events
    - Implement CreateTunnel() to establish TCP tunnel to device port 9000
    - Implement GetDeviceName() via lockdownd (port 62078) with 5-second timeout
    - Handle AMDS unavailable: emit "Install iTunes/Apple Devices" message
    - _Requirements: 4.1, 4.2, 4.3, 4.4, 4.5, 4.6, 4.7_

  - [x] 6.3 Implement WiFi transport via direct TCP connection
    - Implement ConnectWiFi() with direct TCP to discovered host:port
    - Implement exponential backoff retry: 1s, 2s, 4s, 8s, 10s cap, max 5 attempts
    - Handle USB detach failover to WiFi (10-second grace period)
    - _Requirements: 5.3, 5.5, 4.8_

  - [x] 6.4 Write property test for AMDS transport selection
    - **Property 6: AMDS Transport Selection**
    - Generate random combinations of pipe availability and TCP availability
    - Verify: use pipe when available, TCP only when pipe unavailable, error when neither available
    - **Validates: Requirements 4.1**

  - [x] 6.5 Write property test for usbmux plist serialization round-trip
    - **Property 7: Usbmux Plist Serialization Round-Trip**
    - Generate random valid usbmux messages (ListDevices, Connect, Listen) with arbitrary fields
    - Verify: serialize to wire format and parse back produces equivalent message
    - **Validates: Requirements 4.2**

  - [x] 6.6 Write property test for wire frame serialization round-trip
    - **Property 11: Wire Frame Serialization Round-Trip**
    - Generate random payloads (starting with 00 00 00 01) and timestamp pairs
    - Verify: serialize to [4B BE len][JSON telemetry][payload] and parse back recovers originals
    - **Validates: Requirements 6.1**

  - [x] 6.7 Write property test for exponential backoff computation
    - **Property 10: Exponential Backoff Computation**
    - Generate random (attempt, maxAttempts, cap) tuples
    - Verify: delay = min(2^(n-1), cap), session termination after max attempts exhausted
    - **Validates: Requirements 5.5, 10.4**

- [x] 7. Implement Bonjour Discovery
  - [x] 7.1 Implement BonjourBrowser for DNS-SD service discovery
    - Create `Windows/App/src/BonjourBrowser.cpp`
    - Implement StartBrowsing() for "_opensidecar._tcp" service type
    - Parse TXT records: "id" (default "unknown"), "pv" (default 1), "device" field
    - Distinguish Mac receivers (device="Mac") from iOS receivers
    - Implement service change callback for UI updates
    - _Requirements: 5.1, 5.2, 8.6_

  - [x] 7.2 Write property test for Bonjour TXT record parsing with defaults
    - **Property 8: Bonjour TXT Record Parsing with Defaults**
    - Generate random subsets of {id, pv, device} fields
    - Verify: correct defaults applied for missing fields, device field distinguishes Mac
    - **Validates: Requirements 5.2, 8.6**

  - [x] 7.3 Write property test for USB transport preference over WiFi
    - **Property 9: USB Transport Preference Over WiFi**
    - Generate random device sets with varying transports and install IDs
    - Verify: same install ID on both USB and WiFi shows only USB, suppresses WiFi entry
    - **Validates: Requirements 5.4**

- [x] 8. Implement Wire Protocol Handshake and Control Messages
  - [x] 8.1 Implement hello/welcome handshake and session protocol messages
    - Handle incoming "hello" message: extract pixelsWide, pixelsHigh, scale, device, id, pv
    - Send "welcome" response with type, pv, min fields
    - Implement protocol version gating: send "updateRequired" if receiverPv < senderMinPeer
    - Implement ping/pong: send ping every 2s with timestamp, respond to pong
    - Handle "sleeping" → tear down virtual display, await wake
    - Handle "closing" → end session, no reconnect
    - Handle unknown message types: log and continue
    - _Requirements: 6.2, 6.3, 6.5, 6.6, 6.7, 6.8, 6.10_

  - [x] 8.2 Write property test for protocol version gating
    - **Property 12: Protocol Version Gating**
    - Generate random (receiverPv, senderMinPeer) integer pairs
    - Verify: updateRequired sent iff receiverPv < senderMinPeer, otherwise session proceeds
    - **Validates: Requirements 6.6**

  - [x] 8.3 Write property test for control message wire constraints
    - **Property 13: Control Message Wire Constraints**
    - Generate random control message payloads (welcome, updateRequired, ping, pong)
    - Verify: serialized JSON < 32,768 bytes, starts with '{', no NUL bytes
    - **Validates: Requirements 6.9**

- [x] 9. Checkpoint - Ensure transport, discovery, and protocol tests pass
  - Ensure all tests pass, ask the user if questions arise.

- [x] 10. Implement Windows Input Injection
  - [x] 10.1 Implement WindowsInputInjector for touch and scroll translation
    - Create `Windows/App/src/WindowsInputInjector.cpp`
    - Implement SetDisplayBounds() to configure the virtual display's bounding rectangle
    - Implement HandleTouch() with state machine: began→down, moved+down→drag, moved+no-down→discard, ended/cancelled→up
    - Implement HandleScroll() with dx/dy in display pixels, natural-scrolling sign convention
    - Implement NormalizedToScreen(): map [0,1] coords to absolute screen position via MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESKTOP
    - Clamp normalized coordinates to [0, 1] before mapping
    - Use SendInput API for all injected events
    - _Requirements: 6.4, 7.1, 7.2, 7.3, 7.4, 7.5, 7.6, 7.7_

  - [x] 10.2 Write property test for input injection coordinate mapping and state machine
    - **Property 14: Input Injection Coordinate Mapping and State Machine**
    - Generate random display bounds and touch message sequences with arbitrary coordinates/phases
    - Verify: clamping, correct screen mapping, state transitions, discard on moved without down
    - **Validates: Requirements 6.4, 7.1, 7.2, 7.3, 7.4, 7.6, 7.7**

- [x] 11. Implement Session Controller
  - [x] 11.1 Implement SessionController orchestrating the full pipeline
    - Create `Windows/App/src/SessionController.cpp`
    - Implement StartSession(): enumerate devices, create virtual display, init capture, init encoder, connect transport
    - Implement state machine: Idle → Connecting → WaitingForHello → Streaming → Reconnecting → Ended
    - On hello received: create/resize virtual display to match receiver dimensions
    - Wire capture → encoder → transport pipeline with frame drop logic
    - Dispatch incoming control messages (touch, scroll, kf, sleeping, closing) to appropriate handlers
    - _Requirements: 10.1, 10.2, 10.3, 10.5_

  - [x] 11.2 Implement liveness monitoring and reconnection logic
    - Send ping every 2 seconds; expect pong within 5 seconds
    - On liveness timeout: attempt reconnect with exponential backoff (1s→10s cap), max 3 attempts
    - On USB detach: attempt WiFi failover with 10-second grace period
    - Auto-connect to previously paired USB devices on attach
    - Support up to 4 concurrent sessions
    - _Requirements: 10.3, 10.4, 10.6, 4.8, 10.2_

  - [x] 11.3 Implement graceful shutdown and resource cleanup
    - On EndSession(): tear down virtual display, close TCP, release encoder, stop capture
    - On app exit: clean up all sessions within 3 seconds
    - On unexpected termination: driver removes displays via handle-close callback (implemented in 2.2)
    - _Requirements: 1.4, 10.5, 10.7_

- [x] 12. Implement Mac Receiver Mode
  - [x] 12.1 Implement MacReceiver TCP listener and Bonjour advertisement
    - Create/extend Mac receiver in `Mac/MacReceiver.swift`
    - Implement NWListener on TCP port 9000 (configurable)
    - Advertise via Bonjour "_opensidecar._tcp" with TXT record: id, pv, device="Mac"
    - On connection accepted: send hello with native display resolution, scale, device="Mac"
    - Handle connection loss: display "Connection lost" message, continue advertising
    - _Requirements: 8.1, 8.2, 8.5, 8.6, 8.7_

  - [x] 12.2 Implement H.264 decoding and Metal rendering for Mac receiver
    - Decode incoming H.264 Annex B frames using VideoToolbox VTDecompressionSession
    - Render decoded frames full-screen using the existing MetalVideoRenderer (adapted for macOS)
    - Parse framed TCP wire protocol: deframe [4B len][telemetry][H.264 payload]
    - On malformed/undecodable frame: send "kf" request, display last good frame
    - _Requirements: 8.3, 8.8_

  - [x] 12.3 Implement Mac receiver input forwarding to Windows sender
    - Capture mouse/trackpad events on the Mac receiver display
    - Normalize coordinates: x = screenX / displayWidth, y = screenY / displayHeight, clamped to [0,1]
    - Forward as touch control messages (began/moved/ended phases) and scroll messages (dx/dy)
    - Use existing wire protocol JSON format for all control messages
    - _Requirements: 8.4_

  - [x] 12.4 Write property test for Mac receiver input normalization
    - **Property 15: Mac Receiver Input Normalization**
    - Generate random (mousePos, displayDims) pairs
    - Verify: normalized coords = screenPos / displayDims, clamped to [0, 1]
    - **Validates: Requirements 8.4**

- [x] 13. Implement Windows Sender Application UI
  - [x] 13.1 Implement connection picker UI and main window
    - Create `Windows/App/src/UI/MainWindow.xaml` and `MainWindow.cpp`
    - Create `Windows/App/src/UI/ConnectionPicker.xaml` and `ConnectionPicker.cpp`
    - Display enumerated USB devices and WiFi-discovered services
    - Show USB entries preferentially; suppress WiFi entry for devices also on USB
    - Show session state, quality preset selector, and error messages
    - Implement auto-connect toggle per device
    - _Requirements: 10.1, 10.2, 5.4_

  - [x] 13.2 Implement application entry point and component wiring
    - Create `Windows/App/src/main.cpp` with WinMain entry point
    - Initialize COM, Media Foundation, Winsock
    - Wire UI events to SessionController
    - Handle app exit: invoke graceful shutdown on all sessions
    - _Requirements: 9.2, 10.5_

- [x] 14. Final checkpoint - Full build and all tests pass
  - Ensure all tests pass, ask the user if questions arise.

## Notes

- Tasks marked with `*` are optional and can be skipped for faster MVP
- Each task references specific requirements for traceability
- Checkpoints ensure incremental validation
- Property tests validate universal correctness properties from the design document using RapidCheck (C++) integrated with Google Test
- Unit tests validate specific examples and edge cases
- The IDD driver (task 2) must be built and test-signed before capture (task 3) can target the virtual display
- The Mac receiver (task 12) is a Swift target in the existing Xcode project and can be developed in parallel with Windows components
- All Windows components use C++ with CMake; the Mac receiver uses Swift with the existing project structure

## Task Dependency Graph

```json
{
  "waves": [
    { "id": 0, "tasks": ["1.1", "1.2"] },
    { "id": 1, "tasks": ["2.1", "12.1"] },
    { "id": 2, "tasks": ["2.2", "2.3", "12.2", "12.3"] },
    { "id": 3, "tasks": ["2.4", "3.1", "12.4"] },
    { "id": 4, "tasks": ["3.2", "3.3", "4.1"] },
    { "id": 5, "tasks": ["4.2", "6.1"] },
    { "id": 6, "tasks": ["4.3", "4.4", "4.5", "6.2", "6.3"] },
    { "id": 7, "tasks": ["6.4", "6.5", "6.6", "6.7", "7.1"] },
    { "id": 8, "tasks": ["7.2", "7.3", "8.1"] },
    { "id": 9, "tasks": ["8.2", "8.3", "10.1"] },
    { "id": 10, "tasks": ["10.2", "11.1"] },
    { "id": 11, "tasks": ["11.2", "11.3"] },
    { "id": 12, "tasks": ["13.1", "13.2"] }
  ]
}
```
