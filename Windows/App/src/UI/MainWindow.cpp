// OpenDisplay Windows Sender — Main Window Implementation
// Hosts the connection picker, quality preset selector, session state indicator,
// and error message display using Win32 child controls.
//
// Validates: Requirements 10.1, 10.2

#include "MainWindow.h"

#include <algorithm>
#include <cassert>
#include <commctrl.h>

#pragma comment(lib, "comctl32.lib")

namespace OpenDisplay {

// Window class name for registration
static constexpr const wchar_t* kWindowClassName = L"OpenDisplayMainWindow";

// Custom message IDs for cross-thread UI updates
static constexpr UINT WM_SESSION_STATE_CHANGED = WM_USER + 1;
static constexpr UINT WM_SHOW_ERROR = WM_USER + 2;
static constexpr UINT WM_DEVICES_CHANGED = WM_USER + 3;

// Child control IDs
static constexpr int IDC_STATUS_LABEL = 101;
static constexpr int IDC_DEVICE_LIST = 102;
static constexpr int IDC_CONNECT_BTN = 104;
static constexpr int IDC_DISCONNECT_BTN = 105;
static constexpr int IDC_DEVICES_LABEL = 106;

MainWindow::MainWindow() = default;

MainWindow::~MainWindow() {
    if (m_hwnd) {
        DestroyWindow(m_hwnd);
        m_hwnd = nullptr;
    }
}

HRESULT MainWindow::Initialize(HINSTANCE hInstance) {
    m_hInstance = hInstance;

    // Enable visual styles (common controls v6)
    INITCOMMONCONTROLSEX icc = {};
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&icc);

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
        480, 430,
        nullptr,
        nullptr,
        hInstance,
        this  // Pass this pointer for WM_CREATE
    );

    if (!m_hwnd) {
        return HRESULT_FROM_WIN32(GetLastError());
    }

    // Set the window icon
    HICON hIcon = static_cast<HICON>(LoadImageW(
        hInstance, L"IDI_APPICON", IMAGE_ICON, 0, 0,
        LR_DEFAULTSIZE | LR_SHARED));
    if (hIcon) {
        SendMessage(m_hwnd, WM_SETICON, ICON_BIG, (LPARAM)hIcon);
        SendMessage(m_hwnd, WM_SETICON, ICON_SMALL, (LPARAM)hIcon);
    }

    // Get the default GUI font
    HFONT hFont = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));

    // --- Create child controls ---

    // App title
    HWND hTitle = CreateWindowExW(0, L"STATIC", L"OpenDisplay",
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        20, 12, 200, 22, m_hwnd,
        nullptr, hInstance, nullptr);
    HFONT hBoldFont = CreateFontW(18, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    SendMessage(hTitle, WM_SETFONT, (WPARAM)(hBoldFont ? hBoldFont : hFont), TRUE);

    // Status label
    HWND hStatus = CreateWindowExW(0, L"STATIC", L"Idle",
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        20, 36, 430, 18, m_hwnd,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_STATUS_LABEL)),
        hInstance, nullptr);
    SendMessage(hStatus, WM_SETFONT, (WPARAM)hFont, TRUE);

    // "Available Devices" label
    HWND hDevLabel = CreateWindowExW(0, L"STATIC", L"Available Devices",
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        20, 64, 430, 18, m_hwnd,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_DEVICES_LABEL)),
        hInstance, nullptr);
    SendMessage(hDevLabel, WM_SETFONT, (WPARAM)hFont, TRUE);

    // Device listbox
    HWND hList = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", L"",
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT,
        20, 84, 430, 250, m_hwnd,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_DEVICE_LIST)),
        hInstance, nullptr);
    SendMessage(hList, WM_SETFONT, (WPARAM)hFont, TRUE);

    // Connect button
    HWND hConnect = CreateWindowExW(0, L"BUTTON", L"Connect",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        20, 345, 210, 36, m_hwnd,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_CONNECT_BTN)),
        hInstance, nullptr);
    SendMessage(hConnect, WM_SETFONT, (WPARAM)hFont, TRUE);

    // Disconnect button
    HWND hDisconnect = CreateWindowExW(0, L"BUTTON", L"Disconnect",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_DISABLED,
        240, 345, 210, 36, m_hwnd,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_DISCONNECT_BTN)),
        hInstance, nullptr);
    SendMessage(hDisconnect, WM_SETFONT, (WPARAM)hFont, TRUE);

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
        // Trigger initial device list refresh
        PostMessage(m_hwnd, WM_DEVICES_CHANGED, 0, 0);
    }
}

void MainWindow::NotifyDevicesChanged() {
    if (m_hwnd) {
        PostMessage(m_hwnd, WM_DEVICES_CHANGED, 0, 0);
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

        case WM_COMMAND: {
            int controlId = LOWORD(wParam);
            int notifyCode = HIWORD(wParam);

            if (controlId == IDC_CONNECT_BTN && notifyCode == BN_CLICKED) {
                // Get selected device from listbox
                HWND hList = GetDlgItem(m_hwnd, IDC_DEVICE_LIST);
                int sel = (int)SendMessage(hList, LB_GETCURSEL, 0, 0);
                if (sel == LB_ERR) {
                    MessageBoxW(m_hwnd, L"Select a device first.",
                               L"OpenDisplay", MB_OK | MB_ICONINFORMATION);
                    return 0;
                }

                // Get device from picker's display list
                auto devices = m_connectionPicker.GetDisplayList();
                if (sel >= 0 && sel < static_cast<int>(devices.size())) {
                    OnConnectRequested(devices[sel]);
                }
            }
            else if (controlId == IDC_DISCONNECT_BTN && notifyCode == BN_CLICKED) {
                OnDisconnectRequested();
            }
            return 0;
        }

        case WM_SESSION_STATE_CHANGED: {
            // Update status label
            HWND hStatus = GetDlgItem(m_hwnd, IDC_STATUS_LABEL);
            std::string text = StateToString(m_currentState);
            if (!m_currentStatus.empty() && m_currentState != SessionController::State::Idle) {
                text = m_currentStatus;
            }
            std::wstring wtext(text.begin(), text.end());
            SetWindowTextW(hStatus, wtext.c_str());

            // Enable/disable buttons based on state
            bool isActive = (m_currentState != SessionController::State::Idle &&
                             m_currentState != SessionController::State::Ended);
            EnableWindow(GetDlgItem(m_hwnd, IDC_CONNECT_BTN), !isActive);
            EnableWindow(GetDlgItem(m_hwnd, IDC_DISCONNECT_BTN), isActive);
            return 0;
        }

        case WM_SHOW_ERROR:
            if (m_errorVisible) {
                std::wstring title(m_errorTitle.begin(), m_errorTitle.end());
                std::wstring message(m_errorMessage.begin(), m_errorMessage.end());
                MessageBoxW(m_hwnd, message.c_str(), title.c_str(),
                           MB_OK | MB_ICONERROR);
            }
            return 0;

        case WM_DEVICES_CHANGED: {
            // Rebuild the listbox content from the connection picker
            HWND hList = GetDlgItem(m_hwnd, IDC_DEVICE_LIST);
            SendMessage(hList, LB_RESETCONTENT, 0, 0);
            auto devices = m_connectionPicker.GetDisplayList();
            for (const auto& dev : devices) {
                std::string label = dev.name;
                if (dev.isUSB) label += "  [USB]";
                else label += "  [WiFi]";
                std::wstring wlabel(label.begin(), label.end());
                SendMessageW(hList, LB_ADDSTRING, 0, (LPARAM)wlabel.c_str());
            }
            if (devices.empty()) {
                SendMessageW(hList, LB_ADDSTRING, 0,
                    (LPARAM)L"  (No devices found - connect iPad via USB or check WiFi)");
                EnableWindow(hList, FALSE);
            } else {
                EnableWindow(hList, TRUE);
            }
            return 0;
        }

        case WM_CLOSE:
            OnDisconnectRequested();
            DestroyWindow(m_hwnd);
            return 0;

        default:
            return DefWindowProc(m_hwnd, msg, wParam, lParam);
    }
}

} // namespace OpenDisplay
