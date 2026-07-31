# Requirements Document

## Introduction

This feature extends the OpenDisplay project to support Windows 11 as a sender platform. Currently, OpenDisplay allows macOS to use an iPad or iPhone as a second display via USB or WiFi. This extension adds a Windows 11 host application that can use an iPad (via USB) or a Mac screen (via network) as a second display, reusing the existing iOS receiver app and wire protocol where possible.

The Windows sender must replicate the core pipeline: create a virtual display recognized by the OS, capture its framebuffer, encode to H.264, and stream over a length-prefixed TCP connection to a receiver. Two receiver targets are in scope: the existing iOS app (iPad connected via USB using Apple's usbmuxd protocol through iTunes/Apple Mobile Device Service on Windows) and a new macOS receiver mode (Mac acting as a display for Windows via network).

## Glossary

- **Windows_Sender**: The Windows 11 desktop application that creates a virtual display, captures screen content, encodes it to H.264, and streams it to a receiver device
- **Virtual_Display_Driver**: A Windows Indirect Display Driver (IDD) that creates a virtual monitor recognized by Windows Display Settings
- **IDD**: Indirect Display Driver — the Windows driver model for software-defined displays (IddCx framework)
- **AMDS**: Apple Mobile Device Service — the Windows service (installed with iTunes/Apple Devices) that provides usbmuxd-equivalent functionality for communicating with iOS devices over USB
- **iOS_Receiver**: The existing OpenDisplay iOS app running on iPad that listens for incoming video streams on TCP port 9000
- **Mac_Receiver**: A new mode of the existing Mac app that listens for incoming video streams from a Windows sender, acting as a display target rather than a sender
- **Wire_Protocol**: The existing framed TCP protocol — [4-byte big-endian length][payload] — used for both H.264 Annex B video frames and JSON control messages
- **Media_Foundation**: The Windows multimedia framework providing hardware-accelerated H.264 encoding via Media Foundation Transform (MFT)
- **DXGI_Desktop_Duplication**: The Windows Desktop Duplication API for efficiently capturing display framebuffers with GPU-resident textures
- **Bonjour_Discovery**: Zero-configuration network service discovery using mDNS/DNS-SD (Bonjour for Windows or mdns-sd equivalent)

## Requirements

### Requirement 1: Virtual Display Creation on Windows 11

**User Story:** As a Windows 11 user, I want OpenDisplay to create a virtual second monitor on my system, so that I can extend my desktop onto a remote device just like with a physical monitor.

#### Acceptance Criteria

1. WHEN the Windows_Sender starts a session, THE Virtual_Display_Driver SHALL create an IDD-based virtual monitor that appears in Windows Display Settings as an enumerated display available for window placement and desktop extension
2. WHEN the iOS_Receiver announces its panel size via the hello message, THE Virtual_Display_Driver SHALL configure the virtual display resolution to match the receiver's native pixel dimensions divided by 2 (matching the @2x HiDPI convention used by the Mac sender), rounding each axis down to the nearest even number
3. WHILE a session is active, THE Virtual_Display_Driver SHALL maintain the virtual display as an available extended desktop target such that windows placed on it remain positioned and visible until the session ends
4. WHEN the session ends, THE Virtual_Display_Driver SHALL remove the virtual display from the system within 2 seconds
5. WHEN the receiver sends a hello message with swapped dimensions (orientation change), THE Virtual_Display_Driver SHALL reconfigure the virtual display to the new resolution within 2 seconds while preserving the active network connection to the receiver
6. THE Virtual_Display_Driver SHALL support display resolutions from 640×480 up to 2732×2048 pixels (iPad Pro 12.9" native) at 60 Hz refresh rate
7. IF the Virtual_Display_Driver fails to create or reconfigure the virtual display, THEN THE Windows_Sender SHALL report an error indication to the user and SHALL NOT leave a partially-initialized display registered in the system
8. IF the iOS_Receiver hello message contains pixel dimensions of zero or dimensions exceeding 2732×2048, THEN THE Virtual_Display_Driver SHALL reject the configuration and THE Windows_Sender SHALL report an error indication to the user

### Requirement 2: Screen Capture on Windows 11

**User Story:** As a Windows 11 user, I want the virtual display content to be captured efficiently, so that it can be streamed to my receiver device with low latency.

#### Acceptance Criteria

1. WHILE a session is active, THE Windows_Sender SHALL capture the virtual display framebuffer using DXGI_Desktop_Duplication API
2. WHILE a session is active and content is changing on the virtual display, THE Windows_Sender SHALL capture frames at a rate of 60 frames per second
3. WHEN no content changes are reported by DXGI_Desktop_Duplication for at least one frame interval (16.7 milliseconds), THE Windows_Sender SHALL not encode or transmit redundant frames until the next content change is detected
4. THE Windows_Sender SHALL deliver captured frames as GPU-resident textures to the encoder without CPU-side copies where the hardware supports zero-copy paths, and SHALL fall back to a CPU-mediated copy if the GPU zero-copy path is unavailable
5. IF DXGI_Desktop_Duplication fails to acquire a frame, THEN THE Windows_Sender SHALL retry acquisition within 100 milliseconds, and IF acquisition fails 3 consecutive times, THEN THE Windows_Sender SHALL reinitialize the DXGI_Desktop_Duplication session and log the failure
6. IF the encoder has not completed processing the previous frame when a new captured frame is available, THEN THE Windows_Sender SHALL drop the new frame rather than queue it, to maintain a maximum capture-to-encode latency of no more than 32 milliseconds

### Requirement 3: H.264 Video Encoding on Windows 11

**User Story:** As a Windows 11 user, I want video encoding to use hardware acceleration, so that streaming to my second display has low latency and low CPU usage.

#### Acceptance Criteria

1. THE Windows_Sender SHALL encode captured frames to H.264 Annex B format using Media_Foundation hardware-accelerated encoding with the H.264 High profile
2. THE Windows_Sender SHALL configure the encoder with RealTime=true, no B-frames (AllowFrameReordering=false), MaxFrameDelayCount=0, PrioritizeEncodingSpeedOverQuality=true, low-latency rate control enabled, expected frame rate of 60 fps, and MaxKeyFrameInterval of 3600 frames
3. WHEN a streaming session starts, THE Windows_Sender SHALL produce a keyframe (IDR) as the first encoded frame
4. WHEN the Windows_Sender receives a JSON control message with type "kf" from the receiver, THE Windows_Sender SHALL force the next encoded frame to be a keyframe (IDR)
5. THE Windows_Sender SHALL prefix each keyframe with SPS and PPS NAL units, each delimited by a 4-byte Annex B start code (00 00 00 01), before the slice NAL units of that keyframe
6. WHILE the encoder has 1 pending encode in flight and a new capture arrives, THE Windows_Sender SHALL drop the new capture without forcing a keyframe on the subsequent frame (the H.264 reference chain remains valid as a normal P-frame)
7. THE Windows_Sender SHALL support configurable bitrate with three quality presets: best (18 Mbps), balanced (10 Mbps), and fast (6 Mbps)
8. IF no hardware H.264 encoder is available via Media Foundation, THEN THE Windows_Sender SHALL fall back to software encoding and display a warning indicating increased CPU usage and latency
9. IF the Media Foundation encoder session creation fails or the encoder returns an error during frame submission, THEN THE Windows_Sender SHALL log the error and cease encoding until the session is re-established on the next connection attempt

### Requirement 4: USB Communication with iPad via AMDS

**User Story:** As a Windows 11 user, I want to connect my iPad over USB for the lowest latency second display experience, so that I can use it without WiFi or network configuration.

#### Acceptance Criteria

1. THE Windows_Sender SHALL communicate with iOS devices over USB by connecting to the Apple Mobile Device Service (AMDS) named pipe (`\\.\pipe\usbmux`) or TCP socket (localhost port 27015) on the local machine, attempting the named pipe first and falling back to the TCP socket
2. THE Windows_Sender SHALL implement the usbmuxd plist-based protocol to list attached devices, subscribe to attach/detach events, and establish TCP tunnels to device port 9000
3. WHEN a USB iOS device is detected, THE Windows_Sender SHALL resolve the device's friendly name via lockdownd (DeviceName query on port 62078) within 5 seconds for display in the connection UI
4. IF the lockdownd DeviceName query fails or times out, THEN THE Windows_Sender SHALL display the device using a fallback label containing the device type and connection method (e.g., "iPhone / iPad (USB)") and remain connectable
5. WHEN the user selects a USB device, THE Windows_Sender SHALL establish a TCP tunnel through AMDS to the iOS_Receiver's listening port within 5 seconds
6. IF the TCP tunnel establishment fails or times out, THEN THE Windows_Sender SHALL display an error message indicating the device is not running the receiver app and allow the user to retry
7. IF AMDS is not installed or not running, THEN THE Windows_Sender SHALL display a message instructing the user to install iTunes or Apple Devices from the Microsoft Store
8. WHEN a USB device is detached during an active session, THE Windows_Sender SHALL attempt failover to the device's WiFi service if discovered via Bonjour, and end the session after a 10-second grace period if no alternate transport is available

### Requirement 5: WiFi Communication with iPad

**User Story:** As a Windows 11 user, I want to connect my iPad over WiFi for a cable-free second display experience, so that I can use my iPad as a second display without being tethered.

#### Acceptance Criteria

1. THE Windows_Sender SHALL discover iOS_Receiver instances on the local network using Bonjour DNS-SD service type "_opensidecar._tcp"
2. THE Windows_Sender SHALL read the TXT record fields "id" (install identity) and "pv" (protocol version) from discovered services, and IF either field is absent THEN THE Windows_Sender SHALL treat the missing "pv" as protocol version 1 and the missing "id" as unknown-identity
3. WHEN a WiFi device is selected, THE Windows_Sender SHALL connect to the iOS_Receiver's TCP endpoint within 5 seconds and begin the hello/welcome handshake
4. THE Windows_Sender SHALL prefer USB transport over WiFi when the same physical device (matched by install id) is available on both transports, and SHALL suppress the WiFi entry in the connection picker for a device already shown via USB
5. IF WiFi connection fails or is interrupted, THEN THE Windows_Sender SHALL retry the connection with exponential backoff (1s, 2s, 4s, 8s, 10s cap) for a maximum of 5 attempts before ending the session

### Requirement 6: Wire Protocol Compatibility

**User Story:** As a Windows 11 user, I want the Windows sender to be fully compatible with the existing iOS receiver app, so that I can use my iPad without needing a different receiver app.

#### Acceptance Criteria

1. THE Windows_Sender SHALL send video frames using the wire format: [4-byte big-endian length][JSON telemetry prefix `{"cap":<unix_ms>,"snd":<unix_ms>}`][Annex B H.264 payload starting with 00 00 00 01 start codes]
2. WHEN the iOS_Receiver sends a "hello" message, THE Windows_Sender SHALL respond with a welcome message containing the fields: type ("welcome"), pv (integer protocol version of this build), and min (integer minimum supported peer protocol version)
3. WHEN the iOS_Receiver sends a ping message containing a timestamp field "t", THE Windows_Sender SHALL respond within 2 seconds with a pong message containing the original "t" value and an "mt" field set to the sender's current clock in Unix epoch milliseconds
4. THE Windows_Sender SHALL process touch control messages (containing type, phase, x, y fields with coordinates normalized 0.0–1.0) and scroll control messages (containing type, dx, dy fields in pixel units) from the iOS_Receiver and translate them into Windows input events on the virtual display
5. WHEN the iOS_Receiver sends a keyframe request message (type "kf"), THE Windows_Sender SHALL encode and send the next video frame as an IDR frame prefixed with SPS and PPS parameter sets
6. IF the receiver's protocol version (from the hello message pv field, defaulting to 1 when absent) is below the sender's minimum supported peer version, THEN THE Windows_Sender SHALL send an updateRequired message containing the fields: type ("updateRequired"), target ("ios"), store (App Store URL string), and message (human-readable update prompt)
7. WHEN the iOS_Receiver sends a "sleeping" message, THE Windows_Sender SHALL tear down the virtual display and enter a reconnect-on-wake state
8. WHEN the iOS_Receiver sends a "closing" message, THE Windows_Sender SHALL end the session without attempting to reconnect
9. THE Windows_Sender SHALL send all JSON control messages on the video channel as payloads under 32 KB in size, starting with "{", and containing no NUL (0x00) bytes, so the iOS_Receiver can disambiguate them from H.264 video data
10. WHEN the Windows_Sender receives a control message with an unrecognized "type" field, THE Windows_Sender SHALL log the unknown type and continue processing subsequent messages without disconnecting

### Requirement 7: Touch and Scroll Input Injection on Windows

**User Story:** As a Windows 11 user, I want touch and scroll gestures on my iPad to control the Windows desktop on the virtual display, so that I can interact with windows placed on the second display.

#### Acceptance Criteria

1. WHEN the iOS_Receiver sends a touch message with phase "began", THE Windows_Sender SHALL inject a left mouse button down event at the absolute screen position computed by mapping the normalized coordinates to the virtual display's pixel bounds
2. WHEN the iOS_Receiver sends a touch message with phase "moved" and a left mouse button down event has previously been injected without a corresponding button up, THE Windows_Sender SHALL inject a mouse drag event (left button held) at the absolute screen position computed by mapping the normalized coordinates to the virtual display's pixel bounds
3. WHEN the iOS_Receiver sends a touch message with phase "ended" or "cancelled", THE Windows_Sender SHALL inject a left mouse button up event at the last known pointer position
4. IF the iOS_Receiver sends a touch message with phase "moved" and no left mouse button down event is currently active, THEN THE Windows_Sender SHALL discard the message without injecting any event
5. WHEN the iOS_Receiver sends a scroll message, THE Windows_Sender SHALL inject a scroll wheel event using the dx value for horizontal scrolling and the dy value for vertical scrolling, where dx and dy are expressed in display pixels with natural-scrolling sign convention
6. THE Windows_Sender SHALL map normalized touch coordinates ranging from 0.0 to 1.0 in video space (origin top-left) to absolute screen pixel coordinates within the virtual display's bounding rectangle, by computing x_screen = display_left + (normalized_x × display_width) and y_screen = display_top + (normalized_y × display_height)
7. IF the iOS_Receiver sends a touch message with normalized coordinates outside the range 0.0 to 1.0, THEN THE Windows_Sender SHALL clamp the coordinates to the range 0.0 to 1.0 before mapping to screen coordinates

### Requirement 8: Mac as Receiver for Windows (Network Mode)

**User Story:** As a Windows 11 user, I want to use a nearby Mac screen as a second display for my Windows PC, so that I can utilize my Mac's high-quality display as extended screen real estate.

#### Acceptance Criteria

1. THE Mac_Receiver SHALL listen for incoming connections on TCP port 9000 (configurable via user preferences) and advertise itself via Bonjour service type "_opensidecar._tcp" with a TXT record containing its install identity (field "id"), protocol version (field "pv"), and device type (field "device" set to "Mac")
2. WHEN a connection is accepted, THE Mac_Receiver SHALL send a hello message announcing its display panel dimensions in pixels (native resolution of the target screen), device scale factor, and device field set to "Mac"
3. THE Mac_Receiver SHALL decode incoming H.264 Annex B frames using VideoToolbox and render them full-screen on the designated display using the existing Metal renderer pipeline
4. THE Mac_Receiver SHALL forward mouse click and scroll input from the Mac's trackpad/mouse back to the Windows_Sender as control messages (touch type with began/moved/ended phases and scroll type with dx/dy) using the existing wire protocol format
5. WHEN the Mac_Receiver enters full-screen mode on a specific display, THE Mac_Receiver SHALL send an updated hello message with the native resolution of that display, and THE Windows_Sender SHALL resize the virtual display to match within 2 seconds
6. THE Windows_Sender SHALL discover Mac_Receiver instances using the same Bonjour service type used for iOS receivers, distinguishing them by the "device" field value of "Mac" in the TXT record and hello message
7. IF the Mac_Receiver loses the network connection to the Windows_Sender, THEN THE Mac_Receiver SHALL display a "Connection lost — waiting for sender" message and continue advertising via Bonjour for reconnection
8. IF the Mac_Receiver receives a malformed or undecodable H.264 frame, THEN THE Mac_Receiver SHALL send a keyframe request (type "kf") to the Windows_Sender and display the last successfully decoded frame until a new keyframe arrives

### Requirement 9: Windows Application Build System

**User Story:** As a developer, I want the Windows sender to build using standard Windows development tools, so that contributors can build and test it without specialized setup.

#### Acceptance Criteria

1. THE Windows_Sender SHALL build using CMake (version 3.21 or later) with Visual Studio 2022 or later on Windows 11
2. THE Windows_Sender SHALL compile as a native Windows desktop application (Win32/WinUI 3) without requiring a managed runtime (.NET, JVM, or similar)
3. THE Virtual_Display_Driver SHALL build as a separate UMDF driver project using the IddCx framework that can be test-signed for development and production-signed with an EV certificate for distribution
4. THE Windows_Sender SHALL declare its build dependencies in CMakeLists.txt including minimum Windows SDK version 10.0.22621.0, WDK version 10.0.22621.0, and Media Foundation headers
5. WHEN a developer runs the CMake build command (`cmake --build`), THE build system SHALL produce the Windows_Sender application executable and the Virtual_Display_Driver package (INF + SYS/DLL) in the build output directory
6. IF the required Windows SDK, WDK, or Visual Studio version is not detected during CMake configuration, THEN THE build system SHALL emit a descriptive error message identifying which prerequisite is missing and the minimum required version

### Requirement 10: Windows Session Lifecycle Management

**User Story:** As a Windows 11 user, I want the application to manage connections gracefully, so that I can plug in my iPad and start using it without manual configuration, and the system cleans up properly when I'm done.

#### Acceptance Criteria

1. WHEN the Windows_Sender launches, THE Windows_Sender SHALL enumerate attached USB iOS devices and discovered WiFi services within 3 seconds and present them in a connection picker UI
2. WHEN a USB device whose install identity matches a device the user has previously completed a successful session with is attached, THE Windows_Sender SHALL auto-connect to it within 5 seconds (unless the user has explicitly disabled auto-connect for that device)
3. WHILE a session is active, THE Windows_Sender SHALL send a ping message to the receiver every 2 seconds and expect a pong response within 5 seconds
4. IF no data is received from the receiver for 5 seconds, THEN THE Windows_Sender SHALL consider the connection dead and attempt reconnection using exponential backoff starting at 1 second up to 10 seconds, for a maximum of 3 attempts, ending the session if all attempts fail
5. WHEN the Windows_Sender application exits normally, THE Windows_Sender SHALL tear down all virtual displays, close all connections, and release all encoder resources within 3 seconds
6. THE Windows_Sender SHALL support running at least 4 simultaneous sessions to different receiver devices, each with its own virtual display and encoding pipeline
7. IF the Windows_Sender process terminates unexpectedly, THEN THE Virtual_Display_Driver SHALL remove all virtual displays it created within 5 seconds so that no orphaned displays remain visible in Windows Display Settings
