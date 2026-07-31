// OpenDisplay Windows Sender — Main Window Implementation
// Hosts the connection picker, quality preset selector, session state indicator,
// and error message display.
//
// Validates: Requirements 10.1, 10.2

#include "MainWindow.h"

#include <algorithm>
#include <cassert>

namespace OpenDisplay {

// Window class name for registration
static constexpr const wchar_t* kWindowClassName = L"OpenDisplayMainWindow";

// Custom message IDs for cross-thread UI updates
static constexpr UINT WM_SESSION_STATE_CHANGED = WM_USER + 1;
static constexpr UINT WM_SHOW_ERROR = WM_USER + 2;

MainWindow::MainWindow() = default;

MainWindow::~MainWindow() {
    if (m_hwnd) {
        DestroyWindow(m_hwnd);
        m_hwnd = nullptr;
    }
}

HRESULT MainWindow::Initialize(HINSTANCE hInstance) {
    m_hInstance = hInstance;

    // Register window class
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(WNDCLASSEXW);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    wc.lpszClassName = kWindowClassName;

    if (!RegisterClassExW(&wc)) {
        DWORD err = GetLastError();
        if (err != ERROR_CLASS_ALREADY_EXISTS) {
            return HRESULT_FROM_WIN32(err);
        }
    }

    // Create the main window
    m_hwnd = CreateWindowExW(
        0,
        kWindowClassName,
        L"OpenDisplay",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT,
        480, 600,
        nullptr,
        nullptr,
        hInstance,
        this  // Pass this pointer for WM_CREATE
    );

    if (!m_hwnd) {
        return HRESULT_FROM_WIN32(GetLastError());
    }

    // Wire up the connection picker's connect callback
    m_connectionPicker.SetConnectCallback(
        [this](const DeviceInfo& device) {
            OnConnectRequested(device);
        });

    return S_OK;
}

void MainWindow::Show() {
    if (m_hwnd) {
        ShowWindow(m_hwnd, SW_SHOW);
        UpdateWindow(m_hwnd);
    }
}

void MainWindow::SetSessionController(std::shared_ptr<SessionController> controller) {
    m_sessionController = std::move(controller);

    if (m_sessionController) {
        // Register for state change callbacks
        m_sessionController->SetStateCallback(
            [this](SessionController::State state, const std::string& status) {
                UpdateSessionState(state, status);
            });
    }
}

void MainWindow::UpdateSessionState(SessionController::State state, const std::string& status) {
    m_currentState = state;
    m_currentStatus = status;

    // Post message to update UI on the main thread
    if (m_hwnd) {
        PostMessage(m_hwnd, WM_SESSION_STATE_CHANGED, 0, 0);
    }
}

void MainWindow::ShowError(const std::string& title, const std::string& message) {
    m_errorTitle = title;
    m_errorMessage = message;
    m_errorVisible = true;

    if (m_hwnd) {
        PostMessage(m_hwnd, WM_SHOW_ERROR, 0, 0);
    }
}

void MainWindow::ClearError() {
    m_errorVisible = false;
    m_errorTitle.clear();
    m_errorMessage.clear();

    if (m_hwnd) {
        InvalidateRect(m_hwnd, nullptr, TRUE);
    }
}

StreamQuality MainWindow::GetSelectedQuality() const {
    return m_selectedQuality;
}

void MainWindow::SetSelectedQuality(StreamQuality quality) {
    m_selectedQuality = quality;
}

void MainWindow::OnQualitySelectionChanged(int selectedIndex) {
    switch (selectedIndex) {
        case 0: m_selectedQuality = StreamQuality::Best; break;
        case 1: m_selectedQuality = StreamQuality::Balanced; break;
        case 2: m_selectedQuality = StreamQuality::Fast; break;
        default: m_selectedQuality = StreamQuality::Balanced; break;
    }
}

void MainWindow::OnConnectRequested(const DeviceInfo& device) {
    if (!m_sessionController) {
        ShowError("Not Ready", "Session controller not initialized.");
        return;
    }

    if (!SessionController::CanStartNewSession()) {
        ShowError("Session Limit", "Maximum number of concurrent sessions reached (4).");
        return;
    }

    ClearError();

    HRESULT hr = m_sessionController->StartSession(device, m_selectedQuality);
    if (FAILED(hr)) {
        ShowError("Connection Failed",
                  "Could not start session. Check that the device is running "
                  "the receiver app and is accessible.");
    }
}

void MainWindow::OnDisconnectRequested() {
    if (m_sessionController) {
        m_sessionController->EndSession();
    }
}

std::string MainWindow::StateToString(SessionController::State state) {
    switch (state) {
        case SessionController::State::Idle:           return "Idle";
        case SessionController::State::Connecting:     return "Connecting...";
        case SessionController::State::WaitingForHello: return "Waiting for device...";
        case SessionController::State::Streaming:      return "Streaming";
        case SessionController::State::Reconnecting:   return "Reconnecting...";
        case SessionController::State::Ended:          return "Session Ended";
        default: return "Unknown";
    }
}

LRESULT CALLBACK MainWindow::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    MainWindow* self = nullptr;

    if (msg == WM_CREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCT*>(lParam);
        self = static_cast<MainWindow*>(cs->lpCreateParams);
        SetWindowLongPtr(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<MainWindow*>(GetWindowLongPtr(hwnd, GWLP_USERDATA));
    }

    if (self) {
        return self->HandleMessage(msg, wParam, lParam);
    }

    return DefWindowProc(hwnd, msg, wParam, lParam);
}

LRESULT MainWindow::HandleMessage(UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;

        case WM_SESSION_STATE_CHANGED:
            // UI update for session state change — in a full WinUI 3 app this
            // would update the SessionStateText TextBlock. For the Win32 fallback,
            // we invalidate to trigger a repaint.
            InvalidateRect(m_hwnd, nullptr, TRUE);
            return 0;

        case WM_SHOW_ERROR:
            // In a full WinUI 3 app this would open the InfoBar.
            // For Win32 fallback, show a message box.
            if (m_errorVisible) {
                std::wstring title(m_errorTitle.begin(), m_errorTitle.end());
                std::wstring message(m_errorMessage.begin(), m_errorMessage.end());
                MessageBoxW(m_hwnd, message.c_str(), title.c_str(),
                           MB_OK | MB_ICONERROR);
            }
            return 0;

        case WM_CLOSE:
            // Initiate graceful shutdown
            OnDisconnectRequested();
            DestroyWindow(m_hwnd);
            return 0;

        default:
            return DefWindowProc(m_hwnd, msg, wParam, lParam);
    }
}

} // namespace OpenDisplay
