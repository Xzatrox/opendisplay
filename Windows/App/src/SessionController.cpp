#include "SessionController.h"

#include <algorithm>
#include <chrono>
#include <future>
#include <iostream>
#include <thread>
#include <vector>

#include <SetupAPI.h>
#include <winioctl.h>
#include <mferror.h>

#include <nlohmann/json.hpp>

#include "BonjourBrowser.h"
#include "ProtocolHandler.h"
#include "DriverInterface.h"
#include "Log.h"

// ─── Static member initialization ────────────────────────────────────────────

std::atomic<int> SessionController::s_activeSessionCount{0};
std::mutex SessionController::s_registryMutex;
std::vector<SessionController*> SessionController::s_activeSessions;

// ─── Constants ───────────────────────────────────────────────────────────────

/// Maximum pending encodes before dropping captured frames.
static constexpr int kMaxPendingEncodes = 1;

/// Maximum pending sends before dropping encoded frames.
static constexpr int kMaxPendingSends = 3;

/// Capture timeout in milliseconds (one frame interval at 60fps).
static constexpr uint32_t kCaptureTimeoutMs = 16;

/// Liveness check interval (how often we poll m_lastReceived).
static constexpr auto kLivenessCheckInterval = std::chrono::seconds(1);

/// Shutdown timeout: EndSession must complete within this duration.
static constexpr auto kShutdownTimeout = std::chrono::seconds(3);

/// Grace period for WiFi failover after USB detach.
static constexpr auto kWiFiFailoverGracePeriod = std::chrono::seconds(10);

// ─── Helper: Get current time in Unix epoch milliseconds ─────────────────────

static int64_t NowMs()
{
    auto now = std::chrono::system_clock::now().time_since_epoch();
    return std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
}

// ─── Helper: Compute virtual display resolution from receiver panel size ─────
// Divides by 2 (matching @2x HiDPI convention), rounds down to nearest even.
// Returns false if result is outside valid range [640,2732] x [480,2048].

static bool ComputeDisplayResolution(int pixelsWide, int pixelsHigh,
                                     uint32_t& outWidth, uint32_t& outHeight)
{
    if (pixelsWide <= 0 || pixelsHigh <= 0) {
        return false;
    }

    // Divide by 2, round down to nearest even number.
    int halfW = pixelsWide / 2;
    int halfH = pixelsHigh / 2;
    outWidth = static_cast<uint32_t>(halfW & ~1);  // Round down to even
    outHeight = static_cast<uint32_t>(halfH & ~1); // Round down to even

    // Validate range constraints
    if (outWidth < 640 || outWidth > 2732) return false;
    if (outHeight < 480 || outHeight > 2048) return false;

    return true;
}

// ─── Helper: Bitrate from StreamQuality preset ───────────────────────────────

static uint32_t BitrateFromQuality(StreamQuality quality)
{
    switch (quality) {
        case StreamQuality::Best:     return 18'000'000;
        case StreamQuality::Balanced: return 10'000'000;
        case StreamQuality::Fast:     return  6'000'000;
        default:                      return 10'000'000;
    }
}

// ─── Helper: Find a virtual display output via DXGI enumeration ──────────────
// Enumerates all DXGI adapters and outputs looking for the VDD virtual display.
// The VDD outputs typically have names like "\\.\DISPLAY15" and are small
// resolution (800x600 default). We return the first output that is NOT the
// primary display.

#include <dxgi1_2.h>
#include <d3d10.h>
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3d11.lib")

struct VirtualDisplayInfo {
    ComPtr<IDXGIOutput> output;
    ComPtr<ID3D11Device> device;
    uint32_t width;
    uint32_t height;
    std::wstring name;
};

static bool FindVirtualDisplayOutput(VirtualDisplayInfo& outInfo)
{
    ComPtr<IDXGIFactory1> factory;
    HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
    if (FAILED(hr)) return false;

    ComPtr<IDXGIAdapter1> adapter;
    for (UINT adapterIdx = 0;
         factory->EnumAdapters1(adapterIdx, adapter.ReleaseAndGetAddressOf()) != DXGI_ERROR_NOT_FOUND;
         ++adapterIdx)
    {
        DXGI_ADAPTER_DESC1 adapterDesc = {};
        adapter->GetDesc1(&adapterDesc);

        ComPtr<IDXGIOutput> output;
        for (UINT outputIdx = 0;
             adapter->EnumOutputs(outputIdx, output.ReleaseAndGetAddressOf()) != DXGI_ERROR_NOT_FOUND;
             ++outputIdx)
        {
            DXGI_OUTPUT_DESC desc = {};
            output->GetDesc(&desc);

            if (!desc.AttachedToDesktop) continue;

            std::wstring outputName = desc.DeviceName;
            int displayWidth = desc.DesktopCoordinates.right - desc.DesktopCoordinates.left;
            int displayHeight = desc.DesktopCoordinates.bottom - desc.DesktopCoordinates.top;

            std::wcerr << L"[FindVDD] Output: " << outputName
                       << L" (" << displayWidth << L"x" << displayHeight << L")"
                       << L" adapter=" << adapterDesc.Description << L"\n";

            // Heuristic: VDD displays are high-numbered (DISPLAY10+) or non-standard res
            bool isLikelyVDD = false;

            // Extract display number from name like "\\.\DISPLAY15"
            size_t dispPos = outputName.find(L"DISPLAY");
            if (dispPos != std::wstring::npos) {
                std::wstring numStr = outputName.substr(dispPos + 7);
                try {
                    int num = std::stoi(numStr);
                    if (num >= 10) isLikelyVDD = true;
                } catch (...) {}
            }

            // Also check: if it's not the primary (3440x1440) display
            if (displayWidth != 3440 && displayHeight != 1440 &&
                displayWidth > 0 && displayHeight > 0 &&
                displayWidth != displayHeight) {
                // Could be VDD if it's a non-standard small resolution
                if (displayWidth <= 800 && displayHeight <= 600) {
                    isLikelyVDD = true;
                }
                // Or if it matches our configured resolution
                if (displayWidth == 3024 && displayHeight == 1964) {
                    isLikelyVDD = true;
                }
                if (displayWidth == 1512 && displayHeight == 982) {
                    isLikelyVDD = true;
                }
            }

            if (isLikelyVDD) {
                std::wcerr << L"[FindVDD] → Selected as VDD target\n";

                // Create D3D11 device on THIS adapter (critical for Desktop Duplication)
                // Use BGRA support + multithread protection (required by MFT encoders)
                ComPtr<ID3D11Device> device;
                ComPtr<ID3D11DeviceContext> ctx;
                D3D_FEATURE_LEVEL featureLevel;
                D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
                hr = D3D11CreateDevice(
                    adapter.Get(),
                    D3D_DRIVER_TYPE_UNKNOWN,
                    nullptr,
                    D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT,
                    levels, _countof(levels),
                    D3D11_SDK_VERSION,
                    device.GetAddressOf(),
                    &featureLevel,
                    ctx.GetAddressOf());

                if (SUCCEEDED(hr)) {
                    // Enable multithread protection — required for MFT D3D11 usage
                    ComPtr<ID3D10Multithread> mt;
                    if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&mt)))) {
                        mt->SetMultithreadProtected(TRUE);
                    }

                    outInfo.output = output;
                    outInfo.device = device;
                    outInfo.width = static_cast<uint32_t>(displayWidth);
                    outInfo.height = static_cast<uint32_t>(displayHeight);
                    outInfo.name = outputName;
                    return true;
                } else {
                    std::cerr << "[FindVDD] D3D11CreateDevice failed: 0x"
                              << std::hex << hr << std::dec << "\n";
                }
            }
        }
    }

    return false;
}

// ─── Helper: Open a handle to the IDD virtual display driver ─────────────────

static HANDLE OpenDriverHandle()
{
    HDEVINFO devInfo = SetupDiGetClassDevs(
        &GUID_DEVINTERFACE_OPENDISPLAY_IDD,
        nullptr, nullptr,
        DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);

    if (devInfo == INVALID_HANDLE_VALUE) {
        return INVALID_HANDLE_VALUE;
    }

    SP_DEVICE_INTERFACE_DATA ifData = {};
    ifData.cbSize = sizeof(ifData);

    if (!SetupDiEnumDeviceInterfaces(devInfo, nullptr,
            &GUID_DEVINTERFACE_OPENDISPLAY_IDD, 0, &ifData)) {
        SetupDiDestroyDeviceInfoList(devInfo);
        return INVALID_HANDLE_VALUE;
    }

    // Get required size for detail buffer
    DWORD requiredSize = 0;
    SetupDiGetDeviceInterfaceDetail(devInfo, &ifData, nullptr, 0,
                                    &requiredSize, nullptr);

    std::vector<uint8_t> detailBuf(requiredSize);
    auto* detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA*>(
        detailBuf.data());
    detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA);

    if (!SetupDiGetDeviceInterfaceDetail(devInfo, &ifData, detail,
                                         requiredSize, nullptr, nullptr)) {
        SetupDiDestroyDeviceInfoList(devInfo);
        return INVALID_HANDLE_VALUE;
    }

    HANDLE handle = CreateFile(detail->DevicePath,
                               GENERIC_READ | GENERIC_WRITE,
                               0, nullptr, OPEN_EXISTING,
                               FILE_ATTRIBUTE_NORMAL, nullptr);

    SetupDiDestroyDeviceInfoList(devInfo);
    return handle;
}

// ─── Helper: Create virtual display via driver IOCTL ─────────────────────────

static HRESULT CreateVirtualDisplay(HANDLE driverHandle, uint32_t width,
                                    uint32_t height, uint32_t& outMonitorId)
{
    MonitorCreateParams params = {};
    params.widthPixels = width;
    params.heightPixels = height;
    params.refreshHz = 60;

    MonitorCreateResult result = {};
    DWORD bytesReturned = 0;

    BOOL ok = DeviceIoControl(driverHandle, IOCTL_CREATE_MONITOR,
                              &params, sizeof(params),
                              &result, sizeof(result),
                              &bytesReturned, nullptr);
    if (!ok) {
        return HRESULT_FROM_WIN32(GetLastError());
    }

    outMonitorId = result.monitorId;
    return S_OK;
}

static HRESULT ResizeVirtualDisplay(HANDLE driverHandle, uint32_t monitorId,
                                    uint32_t width, uint32_t height)
{
    MonitorResizeParams params = {};
    params.monitorId = monitorId;
    params.newWidthPixels = width;
    params.newHeightPixels = height;

    DWORD bytesReturned = 0;
    BOOL ok = DeviceIoControl(driverHandle, IOCTL_RESIZE_MONITOR,
                              &params, sizeof(params),
                              nullptr, 0,
                              &bytesReturned, nullptr);
    if (!ok) {
        return HRESULT_FROM_WIN32(GetLastError());
    }
    return S_OK;
}

static HRESULT DestroyVirtualDisplay(HANDLE driverHandle, uint32_t monitorId)
{
    MonitorDestroyParams params = {};
    params.monitorId = monitorId;

    DWORD bytesReturned = 0;
    BOOL ok = DeviceIoControl(driverHandle, IOCTL_DESTROY_MONITOR,
                              &params, sizeof(params),
                              nullptr, 0,
                              &bytesReturned, nullptr);
    if (!ok) {
        return HRESULT_FROM_WIN32(GetLastError());
    }
    return S_OK;
}

// ─── SessionController: Constructor / Destructor ─────────────────────────────

SessionController::SessionController()
{
    std::lock_guard<std::mutex> lock(s_registryMutex);
    s_activeSessions.push_back(this);
}

SessionController::~SessionController()
{
    // Ensure session is ended before destruction
    if (m_state.load() != State::Idle && m_state.load() != State::Ended) {
        EndSession();
    }

    std::lock_guard<std::mutex> lock(s_registryMutex);
    s_activeSessions.erase(
        std::remove(s_activeSessions.begin(), s_activeSessions.end(), this),
        s_activeSessions.end());
}

// ─── SessionController: Static App-Level Shutdown ────────────────────────────
// Shuts down ALL active sessions within 3 seconds total. Called on app exit.
// Each session's EndSession() is invoked concurrently. If the 3-second deadline
// expires, remaining sessions rely on the driver's handle-close callback
// (implemented in task 2.2, Req 10.7) to remove orphaned virtual displays
// when the process handle is closed by the OS.
//
// Validates: Requirements 1.4, 10.5

void SessionController::ShutdownAllSessions()
{
    // Snapshot the list under lock to avoid holding it during teardown
    std::vector<SessionController*> sessions;
    {
        std::lock_guard<std::mutex> lock(s_registryMutex);
        sessions = s_activeSessions;
    }

    if (sessions.empty()) {
        return;
    }

    // Launch all EndSession() calls concurrently using async futures
    std::vector<std::future<void>> futures;
    futures.reserve(sessions.size());

    for (auto* session : sessions) {
        State state = session->GetState();
        if (state != State::Idle && state != State::Ended) {
            futures.push_back(std::async(std::launch::async, [session]() {
                session->EndSession();
            }));
        }
    }

    // Wait for all to complete, with a total 3-second deadline
    auto deadline = std::chrono::steady_clock::now() + kShutdownTimeout;

    for (auto& fut : futures) {
        auto remaining = deadline - std::chrono::steady_clock::now();
        if (remaining <= std::chrono::milliseconds(0)) {
            // Deadline expired — remaining sessions will be cleaned up by
            // the driver's handle-close callback on process exit (Req 10.7).
            std::cerr << "[SessionController] ShutdownAllSessions deadline expired, "
                      << "driver handle-close will clean up remaining displays\n";
            break;
        }
        fut.wait_for(remaining);
    }
}

int SessionController::GetActiveSessionCount()
{
    return s_activeSessionCount.load();
}

bool SessionController::CanStartNewSession()
{
    return s_activeSessionCount.load() < kMaxSessions;
}

// ─── Helper: State to string for logging ─────────────────────────────────────
static std::string StateToString(SessionController::State s) {
    switch (s) {
        case SessionController::State::Idle: return "Idle";
        case SessionController::State::Connecting: return "Connecting";
        case SessionController::State::WaitingForHello: return "WaitingForHello";
        case SessionController::State::Streaming: return "Streaming";
        case SessionController::State::Reconnecting: return "Reconnecting";
        case SessionController::State::Ended: return "Ended";
        default: return "Unknown";
    }
}

// ─── SessionController: State Management ─────────────────────────────────────

void SessionController::SetState(State newState, const std::string& status)
{
    State prev = m_state.exchange(newState);
    Log::Info("State: " + StateToString(prev) + " -> " + StateToString(newState)
              + (status.empty() ? "" : " (" + status + ")"));

    // Track active session count transitions
    bool wasActive = (prev != State::Idle && prev != State::Ended);
    bool isActive = (newState != State::Idle && newState != State::Ended);
    if (!wasActive && isActive) {
        s_activeSessionCount.fetch_add(1);
    } else if (wasActive && !isActive) {
        s_activeSessionCount.fetch_sub(1);
    }

    StateCallback cb;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        cb = m_stateCallback;
    }
    if (cb) {
        cb(newState, status);
    }
}

SessionController::State SessionController::GetState() const
{
    return m_state.load();
}

void SessionController::SetStateCallback(StateCallback cb)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_stateCallback = std::move(cb);
}

void SessionController::EnableAutoConnect(const std::string& installId)
{
    std::lock_guard<std::mutex> lock(m_autoConnectMutex);
    m_autoConnectInstallIds.insert(installId);
}

void SessionController::DisableAutoConnect(const std::string& installId)
{
    std::lock_guard<std::mutex> lock(m_autoConnectMutex);
    m_autoConnectInstallIds.erase(installId);
}

void SessionController::SetBonjourBrowser(std::shared_ptr<BonjourBrowser> browser)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_bonjourBrowser = std::move(browser);
}

// ─── SessionController: StartSession ─────────────────────────────────────────

HRESULT SessionController::StartSession(const DeviceInfo& device, StreamQuality quality)
{
    if (!CanStartNewSession()) {
        return HRESULT_FROM_WIN32(ERROR_TOO_MANY_SESS);
    }

    State expected = State::Idle;
    if (!m_state.compare_exchange_strong(expected, State::Connecting)) {
        // Session already in progress
        return HRESULT_FROM_WIN32(ERROR_ALREADY_INITIALIZED);
    }

    m_device = device;
    m_quality = quality;

    SetState(State::Connecting, "Connecting to " + device.name);

    // Open driver handle for virtual display management.
    // Non-fatal if driver is not installed — session proceeds without
    // virtual display (useful for development/testing without WDK).
    m_driverHandle = OpenDriverHandle();
    if (m_driverHandle == INVALID_HANDLE_VALUE) {
        // Driver not installed — continue without virtual display
        // The session can still stream to the receiver; it just won't
        // create a virtual monitor on this machine.
    }

    // Establish transport connection
    m_transport = std::make_unique<WireTransport>();
    HRESULT hr;
    if (device.isUSB) {
        hr = m_transport->ConnectUSB(device.udid, device.port);
    } else {
        hr = m_transport->ConnectWiFi(device.host, device.port);
    }

    if (FAILED(hr)) {
        CloseHandle(m_driverHandle);
        m_driverHandle = INVALID_HANDLE_VALUE;
        m_transport.reset();
        SetState(State::Ended, "Connection failed");
        return hr;
    }

    // Set up protocol handler
    m_protocol = std::make_unique<ProtocolHandler>(
        [this](const std::string& json) {
            if (m_transport && m_transport->IsConnected()) {
                m_transport->SendControl(json);
            }
        });

    ProtocolCallbacks callbacks;
    callbacks.onHello = [this](const HelloInfo& info) { OnHelloReceived(info); };
    callbacks.onSleeping = [this]() { OnSleeping(); };
    callbacks.onClosing = [this]() { OnClosing(); };
    callbacks.onPong = [this](int64_t /*rttMs*/) {
        m_lastReceived = std::chrono::steady_clock::now();
    };
    m_protocol->SetCallbacks(std::move(callbacks));

    // Set transport control handler to route through ProtocolHandler
    m_transport->SetControlHandler([this](const nlohmann::json& msg) {
        m_lastReceived = std::chrono::steady_clock::now();
        OnControlMessage(msg);
    });

    // Input injector
    m_input = std::make_unique<WindowsInputInjector>();

    SetState(State::WaitingForHello, "Waiting for receiver hello");
    m_lastReceived = std::chrono::steady_clock::now();

    return S_OK;
}

// ─── SessionController: EndSession (Graceful Shutdown) ───────────────────────
// Tears down all pipeline components in order:
// 1. Stop liveness monitoring thread
// 2. Send closing message to receiver
// 3. Stop pipeline thread (capture loop)
// 4. Close TCP transport connection
// 5. Release MFT encoder
// 6. Stop desktop duplication capture
// 7. Destroy virtual display via IOCTL_DESTROY_MONITOR
// 8. Set state to Ended
//
// Defensive: handles cases where components may already be null or shut down.
//
// Validates: Requirements 1.4, 10.5, 10.7

void SessionController::EndSession()
{
    // Ensure we only run teardown once; allow re-entrancy from destructor
    State current = m_state.load();
    if (current == State::Idle || current == State::Ended) {
        return;
    }

    // Transition to Ended early to prevent re-entrant teardown
    // (e.g., callbacks triggering EndSession again)
    SetState(State::Ended, "Ending session");

    // 1. Stop liveness monitoring thread
    StopLivenessMonitor();

    // 2. Send "closing" message to the receiver (best-effort)
    if (m_transport && m_transport->IsConnected()) {
        try {
            nlohmann::json closingMsg;
            closingMsg["type"] = "closing";
            m_transport->SendControl(closingMsg.dump());
        } catch (...) {
            // Best-effort; don't let send failure block teardown
        }
    }

    // 3. Stop pipeline thread
    m_pipelineRunning.store(false);
    if (m_pipelineThread.joinable()) {
        m_pipelineThread.join();
    }

    // 4. Stop protocol handler ping timer
    if (m_protocol) {
        m_protocol->StopPingTimer();
        m_protocol.reset();
    }

    // 5. Close TCP transport connection
    if (m_transport) {
        m_transport->Disconnect();
        m_transport.reset();
    }

    // 6. Release encoder
    if (m_encoder) {
        m_encoder->Shutdown();
        m_encoder.reset();
    }
    if (m_amfEncoder) {
        m_amfEncoder->Shutdown();
        m_amfEncoder.reset();
    }

    // 7. Stop desktop duplication capture
    if (m_capture) {
        m_capture->Shutdown();
        m_capture.reset();
    }

    // 8. Destroy virtual display via IOCTL_DESTROY_MONITOR
    if (m_driverHandle != INVALID_HANDLE_VALUE && m_monitorId != 0) {
        DestroyVirtualDisplay(m_driverHandle, m_monitorId);
        m_monitorId = 0;
    }

    // 9. Close driver handle
    if (m_driverHandle != INVALID_HANDLE_VALUE) {
        CloseHandle(m_driverHandle);
        m_driverHandle = INVALID_HANDLE_VALUE;
    }

    // 10. Reset input injector
    m_input.reset();

    // Reset display dimensions
    m_displayWidth = 0;
    m_displayHeight = 0;

    // Reset state to Idle so a new session can be started
    SetState(State::Idle, "");
}

// ─── SessionController: Pipeline Loop ────────────────────────────────────────

// SEH wrapper — must have no C++ objects with destructors
static int FilterPipelineException(unsigned int code) {
    char buf[128];
    sprintf_s(buf, "Pipeline SEH exception 0x%08X", code);
    Log::Error(buf);
    return EXCEPTION_EXECUTE_HANDLER;
}

void SessionController::PipelineLoop()
{
    try {
        PipelineLoopInner();
    } catch (const std::exception& e) {
        Log::Error(std::string("Pipeline C++ exception: ") + e.what());
    } catch (...) {
        Log::Error("Pipeline unknown exception");
    }
}

void SessionController::PipelineLoopInner()
{
    Log::Info("Pipeline started: capture=" + std::string(m_capture ? "yes" : "NO")
              + " amfEncoder=" + std::string(m_amfEncoder ? "yes" : "NO")
              + " transport=" + std::string(m_transport ? "yes" : "NO"));

    if (!m_capture || !m_amfEncoder || !m_transport) {
        Log::Error("Pipeline missing component, exiting");
        return;
    }

    int frameCount = 0, sentFrames = 0, loopCount = 0;
    int captureErrors = 0, encodeErrors = 0, sendErrors = 0;

    while (m_pipelineRunning.load()) {
        loopCount++;

        // Get encoded output from AMF
        std::vector<uint8_t> annexBData;
        bool isKeyframe = false;
        HRESULT outHr = m_amfEncoder->GetOutput(annexBData, isKeyframe);
        if (outHr == S_OK && !annexBData.empty()) {
            if (m_transport && m_transport->IsConnected()) {
                HRESULT sendHr = m_transport->SendVideoFrame(annexBData, 0, NowMs());
                if (SUCCEEDED(sendHr)) {
                    sentFrames++;
                    // Successful send proves connection is alive
                    m_lastReceived = std::chrono::steady_clock::now();
                } else {
                    sendErrors++;
                    if (sendErrors <= 5) {
                        char buf[128];
                        sprintf_s(buf, "SendVideoFrame failed: 0x%08X (frame %d)", sendHr, sentFrames);
                        Log::Error(buf);
                    }
                    if (sendErrors > 10) {
                        Log::Error("Too many send errors, exiting pipeline");
                        break;
                    }
                }
            } else {
                Log::Info("Transport disconnected, exiting pipeline (sent=" + std::to_string(sentFrames) + ")");
                break;
            }
        }

        // Acquire frame from Desktop Duplication
        ID3D11Texture2D* texture = nullptr;
        DXGI_OUTDUPL_FRAME_INFO frameInfo = {};
        HRESULT hr = m_capture->AcquireFrame(kCaptureTimeoutMs, &texture, &frameInfo);
        if (hr == DXGI_ERROR_WAIT_TIMEOUT) continue;
        if (FAILED(hr)) {
            captureErrors++;
            if (captureErrors <= 5) {
                char buf[128];
                sprintf_s(buf, "AcquireFrame failed: 0x%08X (count %d)", hr, captureErrors);
                Log::Error(buf);
            }
            // After 3 consecutive failures, attempt reinitialize
            if (captureErrors == 3) {
                Log::Info("Attempting capture reinitialize...");
                HRESULT reinitHr = m_capture->Reinitialize();
                if (FAILED(reinitHr)) {
                    Log::Error("Reinitialize failed, will keep retrying");
                } else {
                    Log::Info("Reinitialize succeeded");
                    captureErrors = 0;
                }
            }
            // After many failures, wait longer before retrying
            if (captureErrors > 3 && captureErrors <= 300) {
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
            }
            // After 5 minutes of failures (300 * 500ms = 150s), give up
            if (captureErrors > 300) {
                Log::Error("Capture failed for too long, exiting pipeline");
                break;
            }
            continue;
        }
        captureErrors = 0; // Reset on success

        frameCount++;

        // Submit BGRA texture directly to AMF (GPU zero-copy)
        HRESULT submitHr = m_amfEncoder->SubmitFrame(texture, NowMs());
        m_capture->ReleaseFrame();
        if (FAILED(submitHr) && submitHr != 0x800401F0L) { // Not MF_E_NOTACCEPTING
            encodeErrors++;
            if (encodeErrors <= 5) {
                char buf[128];
                sprintf_s(buf, "SubmitFrame failed: 0x%08X (count %d)", submitHr, encodeErrors);
                Log::Error(buf);
            }
        }
    }

    if (!m_pipelineRunning.load()) {
        Log::Info("Pipeline stopped by flag (disconnect/shutdown)");
    }

    Log::Info("Pipeline exited: loops=" + std::to_string(loopCount)
              + " frames=" + std::to_string(frameCount)
              + " sent=" + std::to_string(sentFrames)
              + " captErr=" + std::to_string(captureErrors)
              + " encErr=" + std::to_string(encodeErrors)
              + " sendErr=" + std::to_string(sendErrors));
}

// ─── SessionController: Hello / Sleep / Close Handlers ───────────────────────

void SessionController::OnHelloReceived(const HelloInfo& info)
{
    // Compute virtual display resolution from receiver panel size
    uint32_t width = 0, height = 0;
    if (!ComputeDisplayResolution(info.pixelsWide, info.pixelsHigh, width, height)) {
        SetState(State::Ended, "Invalid receiver display dimensions");
        EndSession();
        return;
    }

    // Find a virtual display to capture from (VDD third-party driver)
    VirtualDisplayInfo vddInfo;
    bool haveVDD = FindVirtualDisplayOutput(vddInfo);

    // If we have our custom driver, create/resize the display
    if (m_driverHandle != INVALID_HANDLE_VALUE) {
        HRESULT hr;
        if (m_monitorId == 0) {
            hr = CreateVirtualDisplay(m_driverHandle, width, height, m_monitorId);
        } else {
            hr = ResizeVirtualDisplay(m_driverHandle, m_monitorId, width, height);
        }
        if (FAILED(hr)) {
            m_monitorId = 0;
        }
    }

    m_displayWidth = width;
    m_displayHeight = height;

    // Set input injector display bounds
    if (m_input) {
        RECT bounds = {};
        bounds.left = 0;
        bounds.top = 0;
        bounds.right = static_cast<LONG>(width);
        bounds.bottom = static_cast<LONG>(height);
        m_input->SetDisplayBounds(bounds);
    }

    // Initialize capture from VDD virtual display (if available)
    if (haveVDD) {
        m_capture = std::make_unique<DesktopDuplicationCapture>();
        HRESULT capHr = m_capture->Initialize(vddInfo.output.Get(), vddInfo.device.Get());
        if (FAILED(capHr)) {
            std::cerr << "[SessionController] Capture init failed on VDD: 0x"
                      << std::hex << capHr << std::dec
                      << " - falling back to primary display\n";
            m_capture.reset();

            // Fallback: Desktop Duplication doesn't work on IDD outputs.
            // Capture from the primary display (DISPLAY1) instead.
            ComPtr<IDXGIFactory1> factory2;
            if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory2)))) {
                ComPtr<IDXGIAdapter1> primaryAdapter;
                if (factory2->EnumAdapters1(0, &primaryAdapter) != DXGI_ERROR_NOT_FOUND) {
                    ComPtr<IDXGIOutput> primaryOutput;
                    if (primaryAdapter->EnumOutputs(0, &primaryOutput) != DXGI_ERROR_NOT_FOUND) {
                        ComPtr<ID3D11Device> primaryDevice;
                        D3D_FEATURE_LEVEL fl;
                        HRESULT devHr = D3D11CreateDevice(
                            primaryAdapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                            D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                            nullptr, 0, D3D11_SDK_VERSION,
                            primaryDevice.GetAddressOf(), &fl, nullptr);
                        if (SUCCEEDED(devHr)) {
                            m_capture = std::make_unique<DesktopDuplicationCapture>();
                            capHr = m_capture->Initialize(primaryOutput.Get(), primaryDevice.Get());
                            if (SUCCEEDED(capHr)) {
                                DXGI_OUTPUT_DESC pDesc = {};
                                primaryOutput->GetDesc(&pDesc);
                                vddInfo.device = primaryDevice;
                                vddInfo.width = pDesc.DesktopCoordinates.right - pDesc.DesktopCoordinates.left;
                                vddInfo.height = pDesc.DesktopCoordinates.bottom - pDesc.DesktopCoordinates.top;
                                std::cerr << "[SessionController] Capturing PRIMARY display "
                                          << vddInfo.width << "x" << vddInfo.height << "\n";
                            } else {
                                std::cerr << "[SessionController] Primary also failed: 0x"
                                          << std::hex << capHr << std::dec << "\n";
                                m_capture.reset();
                            }
                        }
                    }
                }
            }
        } else {
            std::cerr << "[SessionController] Capturing VDD display "
                      << vddInfo.width << "x" << vddInfo.height << "\n";
        }
    }

    // Initialize AMF hardware encoder (AMD GPU)
    ID3D11Device* encDevice = haveVDD ? vddInfo.device.Get() : nullptr;
    uint32_t encWidth = haveVDD ? vddInfo.width : width;
    uint32_t encHeight = haveVDD ? vddInfo.height : height;
    // AMF accepts BGRA input directly — no need for resolution alignment tricks

    std::cerr << "[SessionController] Encoder resolution: "
              << encWidth << "x" << encHeight << "\n";

    m_amfEncoder = std::make_unique<AmfEncoder>();
    AmfEncoder::Config amfConfig;
    amfConfig.width = encWidth;
    amfConfig.height = encHeight;
    amfConfig.fps = 60;
    amfConfig.bitrate = BitrateFromQuality(m_quality);

    HRESULT encHr = m_amfEncoder->Initialize(amfConfig, encDevice);
    if (FAILED(encHr)) {
        std::cerr << "[SessionController] AMF encoder init failed: 0x"
                  << std::hex << encHr << std::dec << "\n";
        m_amfEncoder.reset();
    }

    // Start streaming
    SetState(State::Streaming, "Streaming to " + m_device.name);

    // Start pipeline thread
    m_pipelineRunning.store(true);
    m_pipelineThread = std::thread([this]() { PipelineLoop(); });

    // Start liveness monitoring
    StartLivenessMonitor();

    // Start ping timer
    if (m_protocol) {
        m_protocol->StartPingTimer();
    }
}

void SessionController::OnSleeping()
{
    // Tear down virtual display, keep connection alive for wake
    m_pipelineRunning.store(false);
    if (m_pipelineThread.joinable()) {
        m_pipelineThread.join();
    }

    if (m_encoder) {
        m_encoder->Shutdown();
        m_encoder.reset();
    }

    if (m_capture) {
        m_capture->Shutdown();
        m_capture.reset();
    }

    if (m_driverHandle != INVALID_HANDLE_VALUE && m_monitorId != 0) {
        DestroyVirtualDisplay(m_driverHandle, m_monitorId);
        m_monitorId = 0;
    }

    SetState(State::WaitingForHello, "Receiver sleeping, awaiting wake");
}

void SessionController::OnClosing()
{
    EndSession();
}

// ─── SessionController: Liveness Monitoring ──────────────────────────────────
// Validates: Requirements 10.3, 10.4

void SessionController::StartLivenessMonitor()
{
    if (m_livenessRunning.load()) {
        return;
    }

    m_lastReceived = std::chrono::steady_clock::now();
    m_livenessRunning.store(true);
    m_livenessThread = std::thread(&SessionController::LivenessLoop, this);
}

void SessionController::StopLivenessMonitor()
{
    m_livenessRunning.store(false);
    if (m_livenessThread.joinable()) {
        m_livenessThread.join();
    }
}

void SessionController::LivenessLoop()
{
    try {
    auto nextPingTime = std::chrono::steady_clock::now() + kPingInterval;

    while (m_livenessRunning.load()) {
        std::this_thread::sleep_for(kLivenessCheckInterval);

        if (!m_livenessRunning.load()) {
            break;
        }

        State currentState = m_state.load();
        if (currentState != State::Streaming) {
            continue;
        }

        auto now = std::chrono::steady_clock::now();

        // Send ping every kPingInterval (2 seconds)
        if (now >= nextPingTime) {
            nlohmann::json pingMsg;
            pingMsg["type"] = "ping";
            pingMsg["t"] = NowMs();

            if (m_transport && m_transport->IsConnected()) {
                m_transport->SendControl(pingMsg.dump());
            }

            nextPingTime = now + kPingInterval;
        }

        // Check pong timeout (5 seconds without any data received)
        auto elapsed = now - m_lastReceived;
        if (elapsed >= kPongTimeout) {
            auto secs = std::chrono::duration_cast<std::chrono::seconds>(elapsed).count();
            Log::Error("Liveness timeout: no data for " + std::to_string(secs) + "s");

            // Transition to Reconnecting state and attempt reconnection
            SetState(State::Reconnecting, "Connection lost, attempting to reconnect...");
            m_livenessRunning.store(false);

            // Attempt reconnection on this thread (liveness thread becomes reconnect thread)
            if (AttemptReconnect()) {
                Log::Info("Reconnected, restarting pipeline");
                SetState(State::Streaming, "Reconnected successfully");
                // Wait for old pipeline thread to finish
                m_pipelineRunning.store(false);
                if (m_pipelineThread.joinable()) m_pipelineThread.join();
                // Restart pipeline thread
                m_pipelineRunning.store(true);
                m_pipelineThread = std::thread([this]() { PipelineLoop(); });
                // Restart liveness monitoring in a fresh state
                StartLivenessMonitor();
            } else {
                SetState(State::Ended, "Reconnection failed after maximum attempts");
            }
            return;
        }
    }
    } catch (...) {
        Log::Error("Exception in liveness thread");
    }
}

// ─── SessionController: Reconnection Logic ───────────────────────────────────
// Validates: Requirements 10.4

uint32_t SessionController::ComputeReconnectBackoffMs(int attempt)
{
    if (attempt <= 0) {
        return 1000;
    }

    // delay = min(2^(attempt-1) seconds, cap)
    uint32_t delaySec = 1;
    for (int i = 1; i < attempt; ++i) {
        delaySec *= 2;
        if (delaySec >= static_cast<uint32_t>(kReconnectBackoffCapSec)) {
            delaySec = static_cast<uint32_t>(kReconnectBackoffCapSec);
            break;
        }
    }

    return std::min(delaySec, static_cast<uint32_t>(kReconnectBackoffCapSec)) * 1000;
}

bool SessionController::AttemptReconnect()
{
    for (int attempt = 1; attempt <= kMaxReconnectAttempts; ++attempt) {
        uint32_t backoffMs = ComputeReconnectBackoffMs(attempt);

        std::cerr << "[SessionController] Reconnect attempt " << attempt
                  << "/" << kMaxReconnectAttempts
                  << ", backoff " << backoffMs << "ms\n";

        // Wait for the backoff period, but allow early exit if session is ending
        auto deadline = std::chrono::steady_clock::now()
            + std::chrono::milliseconds(backoffMs);

        while (std::chrono::steady_clock::now() < deadline) {
            if (m_state.load() == State::Ended) {
                return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        if (m_state.load() == State::Ended) {
            return false;
        }

        // Disconnect old transport
        if (m_transport) {
            m_transport->Disconnect();
        }

        // Attempt to reconnect using the same device info
        HRESULT hr = E_FAIL;
        if (m_transport) {
            if (m_device.isUSB) {
                hr = m_transport->ConnectUSB(m_device.udid, m_device.port);
            } else {
                hr = m_transport->ConnectWiFi(m_device.host, m_device.port);
            }
        }

        if (SUCCEEDED(hr) && m_transport && m_transport->IsConnected()) {
            std::cerr << "[SessionController] Reconnect succeeded on attempt "
                      << attempt << "\n";
            m_lastReceived = std::chrono::steady_clock::now();
            return true;
        }

        std::cerr << "[SessionController] Reconnect attempt " << attempt
                  << " failed (hr=0x" << std::hex << hr << std::dec << ")\n";
    }

    std::cerr << "[SessionController] All " << kMaxReconnectAttempts
              << " reconnect attempts exhausted\n";
    return false;
}

// ─── SessionController: WiFi Failover ────────────────────────────────────────
// Validates: Requirements 4.8, 10.6

bool SessionController::AttemptWiFiFailover()
{
    std::shared_ptr<BonjourBrowser> browser;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        browser = m_bonjourBrowser;
    }

    if (!browser) {
        std::cerr << "[SessionController] WiFi failover: no Bonjour browser available\n";
        return false;
    }

    // Look for the device's WiFi service using its install ID
    std::string targetInstallId = m_device.udid; // installId matches udid for paired devices

    auto deadline = std::chrono::steady_clock::now() + kWiFiFailoverGracePeriod;

    std::cerr << "[SessionController] WiFi failover: searching for device "
              << targetInstallId << " (10s grace period)\n";

    while (std::chrono::steady_clock::now() < deadline) {
        if (m_state.load() == State::Ended) {
            return false;
        }

        auto services = browser->GetServices();
        for (const auto& svc : services) {
            if (svc.installId == targetInstallId) {
                std::cerr << "[SessionController] WiFi failover: found device at "
                          << svc.host << ":" << svc.port << "\n";

                // Disconnect old USB transport
                if (m_transport) {
                    m_transport->Disconnect();
                }

                // Attempt WiFi connection
                HRESULT hr = E_FAIL;
                if (m_transport) {
                    hr = m_transport->ConnectWiFi(svc.host, svc.port);
                }

                if (SUCCEEDED(hr) && m_transport && m_transport->IsConnected()) {
                    // Update device info to reflect WiFi connection
                    m_device.isUSB = false;
                    m_device.host = svc.host;
                    m_device.port = svc.port;
                    m_lastReceived = std::chrono::steady_clock::now();

                    std::cerr << "[SessionController] WiFi failover succeeded\n";
                    return true;
                }

                std::cerr << "[SessionController] WiFi failover connection attempt failed\n";
            }
        }

        // Poll every 500ms during the grace period
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

    std::cerr << "[SessionController] WiFi failover grace period expired\n";
    return false;
}

// ─── SessionController: USB Device Events ────────────────────────────────────
// Validates: Requirements 4.8, 10.2

void SessionController::OnUSBDeviceDetached(const std::string& udid)
{
    // Only handle if this session is actively using this USB device
    if (!m_device.isUSB || m_device.udid != udid) {
        return;
    }

    State currentState = m_state.load();
    if (currentState != State::Streaming && currentState != State::WaitingForHello) {
        return;
    }

    std::cerr << "[SessionController] USB device detached: " << udid
              << ", attempting WiFi failover\n";

    // Stop current liveness monitoring
    StopLivenessMonitor();

    SetState(State::Reconnecting, "USB disconnected, attempting WiFi failover...");

    // Attempt WiFi failover (blocks up to 10 seconds)
    if (AttemptWiFiFailover()) {
        SetState(State::Streaming, "Switched to WiFi connection");
        StartLivenessMonitor();
    } else {
        // WiFi failover failed — try normal reconnection as last resort
        std::cerr << "[SessionController] WiFi failover failed, attempting reconnect\n";
        if (AttemptReconnect()) {
            SetState(State::Streaming, "Reconnected successfully");
            StartLivenessMonitor();
        } else {
            SetState(State::Ended, "Connection lost: USB detached, no WiFi available");
        }
    }
}

void SessionController::OnUSBDeviceAttached(const DeviceInfo& device)
{
    // Check if auto-connect is enabled for this device's install ID
    bool shouldAutoConnect = false;
    {
        std::lock_guard<std::mutex> lock(m_autoConnectMutex);
        shouldAutoConnect = m_autoConnectInstallIds.count(device.udid) > 0;
    }

    if (!shouldAutoConnect) {
        return;
    }

    // Only auto-connect if this controller is idle and we haven't reached session limit
    if (m_state.load() != State::Idle) {
        return;
    }

    if (!CanStartNewSession()) {
        std::cerr << "[SessionController] Auto-connect skipped: maximum sessions ("
                  << kMaxSessions << ") reached\n";
        return;
    }

    std::cerr << "[SessionController] Auto-connect: starting session for device "
              << device.name << " (" << device.udid << ")\n";

    // Start session with balanced quality by default for auto-connect
    HRESULT hr = StartSession(device, StreamQuality::Balanced);
    if (FAILED(hr)) {
        std::cerr << "[SessionController] Auto-connect session start failed (hr=0x"
                  << std::hex << hr << std::dec << ")\n";
    }
}

// ─── SessionController: Control Message Handling ─────────────────────────────
// Validates: Requirements 10.3

void SessionController::OnControlMessage(const nlohmann::json& msg)
{
    // Update liveness timestamp on any received data
    m_lastReceived = std::chrono::steady_clock::now();

    // Delegate to ProtocolHandler for message routing (hello, pong, sleeping, closing)
    if (m_protocol) {
        m_protocol->HandleMessage(msg);
    }

    // Handle touch/scroll/kf messages that go directly to components
    std::string type;
    try {
        type = msg.value("type", "");
    } catch (...) {
        return;
    }

    if (type == "touch" && m_input) {
        std::string phase = msg.value("phase", "");
        double x = msg.value("x", 0.0);
        double y = msg.value("y", 0.0);
        m_input->HandleTouch(phase, x, y);
    } else if (type == "scroll" && m_input) {
        double dx = msg.value("dx", 0.0);
        double dy = msg.value("dy", 0.0);
        m_input->HandleScroll(dx, dy);
    } else if (type == "kf" && m_encoder) {
        m_encoder->RequestKeyframe();
    }
}
