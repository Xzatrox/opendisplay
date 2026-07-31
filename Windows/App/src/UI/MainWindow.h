#pragma once

#include <memory>
#include <string>

#include <Windows.h>

#include "../SessionController.h"
#include "ConnectionPicker.h"

namespace OpenDisplay {

/// Main application window for the OpenDisplay Windows Sender.
/// Hosts the ConnectionPicker, quality preset selector, session state indicator,
/// and error message display.
///
/// Validates: Requirements 10.1, 10.2
class MainWindow {
public:
    MainWindow();
    ~MainWindow();

    /// Initialize the window and its child controls.
    /// @param hInstance The application instance handle.
    /// @return S_OK on success, or an error HRESULT.
    HRESULT Initialize(HINSTANCE hInstance);

    /// Show the window.
    void Show();

    /// Get the native window handle.
    HWND GetHandle() const { return m_hwnd; }

    /// Set the session controller for handling connect/disconnect actions.
    void SetSessionController(std::shared_ptr<SessionController> controller);

    /// Update the session state display.
    /// @param state The current session state.
    /// @param status Optional human-readable status string.
    void UpdateSessionState(SessionController::State state, const std::string& status = "");

    /// Display an error message to the user.
    /// @param title Brief error title.
    /// @param message Detailed error description.
    void ShowError(const std::string& title, const std::string& message);

    /// Clear any displayed error message.
    void ClearError();

    /// Get the currently selected quality preset.
    StreamQuality GetSelectedQuality() const;

    /// Set the quality preset selection.
    void SetSelectedQuality(StreamQuality quality);

    /// Get the connection picker control.
    ConnectionPicker& GetConnectionPicker() { return m_connectionPicker; }
    const ConnectionPicker& GetConnectionPicker() const { return m_connectionPicker; }

private:
    /// Window procedure callback.
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

    /// Handle window messages.
    LRESULT HandleMessage(UINT msg, WPARAM wParam, LPARAM lParam);

    /// Handle quality preset selection change.
    void OnQualitySelectionChanged(int selectedIndex);

    /// Handle connect request from the connection picker.
    void OnConnectRequested(const DeviceInfo& device);

    /// Handle disconnect request.
    void OnDisconnectRequested();

    /// Convert session state enum to display string.
    static std::string StateToString(SessionController::State state);

    HWND m_hwnd = nullptr;
    HINSTANCE m_hInstance = nullptr;

    ConnectionPicker m_connectionPicker;
    std::shared_ptr<SessionController> m_sessionController;
    StreamQuality m_selectedQuality = StreamQuality::Balanced;

    // Error state
    bool m_errorVisible = false;
    std::string m_errorTitle;
    std::string m_errorMessage;

    // Session state
    SessionController::State m_currentState = SessionController::State::Idle;
    std::string m_currentStatus;
};

} // namespace OpenDisplay
