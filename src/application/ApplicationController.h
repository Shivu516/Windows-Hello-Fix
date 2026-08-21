#pragma once

#include <windows.h>
#include <string>
#include <vector>

#include "IUiSink.h"
#include "../events/SystemEvent.h"

struct CameraDeviceInfo;

public ref class ApplicationController {
public:
    ApplicationController(IUiSink^ sink);
    ~ApplicationController();
    !ApplicationController();

    bool Initialize(HWND hwnd, array<System::String^>^ args);
    void Shutdown(bool isSystemEnding);
    void HandleSystemEnd(HWND hwnd);
    void HandlePowerEvent(SystemEvent ev);
    void HandleSessionEvent(SystemEvent ev);

    bool ToggleMonitoring();
    bool TryGetTargetCameraInstanceId(std::wstring& outId, bool preferCurrentSelection);
    bool DisableTargetCameraHardware(bool retryOnFailure);
    bool EnableTargetCameraHardware(bool cycleDevice);
    void RestoreConfiguredCameraHardware(bool cycleDevice);

    property bool IsMonitoring { bool get() { return m_isMonitoring; } void set(bool v) { m_isMonitoring = v; } }
    property bool IsSystemEnding { bool get() { return m_isSystemEnding; } void set(bool v) { m_isSystemEnding = v; } }
    property bool IsAlreadyDisabled { bool get() { return m_isAlreadyDisabled; } void set(bool v) { m_isAlreadyDisabled = v; } }

    void SetHwnd(HWND hwnd) { m_hwnd = hwnd; }
    void OnWakeupSignal();
    void SetSelectedInstanceId(const std::wstring& id);
    std::wstring GetSelectedInstanceIdNative();
    System::String^ GetSelectedInstanceIdManaged();

private:
    IUiSink^ m_sink;
    HWND m_hwnd;

    bool m_isMonitoring;
    bool m_isSystemEnding;
    bool m_isAlreadyDisabled;
    bool m_cameraExpectedDisabled;

    std::wstring* m_selectedInstanceId;
    void* m_cachedCamerasPlaceholder;

    HANDLE m_hAppMutex;
    HANDLE m_hWakeupEvent;
    HPOWERNOTIFY m_hLidNotification;
    HPOWERNOTIFY m_hButtonNotification;

    System::Threading::Thread^ m_backgroundWorker;
    bool m_keepListening;

    void ListenForWakeupSignal();
    void RegisterNotifications();
    void UnregisterNotifications();
};
