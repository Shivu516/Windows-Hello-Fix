#include "ApplicationController.h"
#include "IUiSink.h"
#include "CommandLine.h"

#include "../config/ConfigPaths.h"
#include "../config/ConfigStore.h"
#include "../camera/CameraDevice.h"
#include "../camera/CameraHardware.h"
#include "../camera/CameraRecovery.h"
#include "../camera/DeviceError.h"
#include "../system/PrivilegeInfo.h"
#include "../system/SingleInstance.h"
#include "../system/ProcessUtils.h"
#include "../events/NotificationRegistrar.h"
#include "../events/SystemEvent.h"
#include "../events/WinEventDecoder.h"
#include "../events/EventCooldown.h"
#include "../utilities/StringHelpers.h"
#include "../utilities/PerfTimer.h"

#include <msclr\marshal_cppstd.h>

using namespace System;
using namespace System::Windows::Forms;
using namespace System::Drawing;

ApplicationController::ApplicationController(IUiSink^ sink)
    : m_sink(sink), m_hwnd(NULL), m_isMonitoring(false), m_isSystemEnding(false), m_isAlreadyDisabled(false),
      m_cameraExpectedDisabled(false), m_selectedInstanceId(new std::wstring()), m_cachedCamerasPlaceholder(nullptr),
      m_hAppMutex(NULL), m_hWakeupEvent(NULL), m_hLidNotification(NULL), m_hButtonNotification(NULL),
      m_backgroundWorker(nullptr), m_keepListening(true) {
}

ApplicationController::~ApplicationController() {
    Shutdown(m_isSystemEnding);
    if (m_selectedInstanceId) {
        delete m_selectedInstanceId;
        m_selectedInstanceId = nullptr;
    }
}

ApplicationController::!ApplicationController() {
    m_keepListening = false;
    if (m_isSystemEnding) {
        DisableTargetCameraHardware(true);
    } else if (m_selectedInstanceId && !m_selectedInstanceId->empty()) {
        EnableTargetCameraHardware(false);
    }
    if (m_hWakeupEvent) {
        SingleInstance::SignalAndCloseWakeEvent(m_hWakeupEvent);
        m_hWakeupEvent = NULL;
    }
    if (m_hLidNotification || m_hButtonNotification) {
        NotificationRegistrar::UnregisterPowerNotifications(m_hLidNotification, m_hButtonNotification);
        m_hLidNotification = NULL;
        m_hButtonNotification = NULL;
    }
    if (m_hAppMutex) {
        SingleInstance::ReleaseMutex(m_hAppMutex);
        m_hAppMutex = NULL;
    }
}

bool ApplicationController::TryGetTargetCameraInstanceId(std::wstring& targetInstanceId, bool preferCurrentSelection) {
    targetInstanceId.clear();
    if (preferCurrentSelection && m_selectedInstanceId && !m_selectedInstanceId->empty()) {
        targetInstanceId = *m_selectedInstanceId;
        return true;
    }
    String^ savedDeviceInstance = L"";
    ConfigStore::LoadConfigState(savedDeviceInstance);
    if (!String::IsNullOrEmpty(savedDeviceInstance)) {
        targetInstanceId = msclr::interop::marshal_as<std::wstring>(savedDeviceInstance);
        return true;
    }
    if (!preferCurrentSelection && m_selectedInstanceId && !m_selectedInstanceId->empty()) {
        targetInstanceId = *m_selectedInstanceId;
        return true;
    }
    std::vector<CameraDeviceInfo> cameras = ScanSystemCameras();
    for (size_t i = 0; i < cameras.size(); i++) {
        if (cameras[i].instanceId.find(L"MI_00") != std::wstring::npos) {
            targetInstanceId = cameras[i].instanceId;
            return true;
        }
    }
    if (!cameras.empty()) {
        targetInstanceId = cameras[0].instanceId;
        return true;
    }
    return false;
}

bool ApplicationController::DisableTargetCameraHardware(bool retryOnFailure) {
    PerfTimer timer;
    std::wstring targetId;
    if (!TryGetTargetCameraInstanceId(targetId, true)) {
        double dur = timer.ElapsedMs();
        ConfigStore::WriteDiagnosticLog(
            String::Format(L"DisableTargetCameraHardware_NoTarget | DurationMs={0:F2}", dur),
            L"Disabled",
            false
        );
        return false;
    }
    bool alreadyDisabled = false;
    if (GetCameraHardwareDisabledState(targetId, alreadyDisabled) && alreadyDisabled) {
        m_cameraExpectedDisabled = true;
        double dur = timer.ElapsedMs();
        ConfigStore::WriteDiagnosticLogWithDevice(
            String::Format(L"DisableTargetCameraHardware_AlreadyDisabled | DurationMs={0:F2}", dur),
            targetId,
            L"Disabled",
            true
        );
        return true;
    }
    bool result = SetCameraHardwareStateVerified(targetId, false, retryOnFailure);
    bool verified = VerifyCameraHardwareState(targetId, true);
    m_cameraExpectedDisabled = result;
    double dur = timer.ElapsedMs();
    ConfigStore::WriteDiagnosticLogWithDevice(
        String::Format(
            L"DisableTargetCameraHardware_Result | Elevated={0} | IntegrityRid={1} | SetupErr={2} | CfgMgr={3} | Stage={4} | DurationMs={5:F2}",
            IsCurrentProcessElevatedNative() ? L"1" : L"0",
            static_cast<Int32>(GetCurrentProcessIntegrityRid()),
            static_cast<Int32>(InterlockedCompareExchange(&g_lastSetupApiError, 0, 0)),
            static_cast<Int32>(InterlockedCompareExchange(&g_lastConfigManagerResult, 0, 0)),
            static_cast<Int32>(InterlockedCompareExchange(&g_lastHardwareToggleStage, 0, 0)),
            dur
        ),
        targetId,
        L"Disabled",
        result && verified
    );
    return result && verified;
}

bool ApplicationController::EnableTargetCameraHardware(bool cycleDevice) {
    PerfTimer timer;
    std::wstring targetId;
    if (!TryGetTargetCameraInstanceId(targetId, true)) {
        double dur = timer.ElapsedMs();
        ConfigStore::WriteDiagnosticLog(
            String::Format(L"EnableTargetCameraHardware_NoTarget | DurationMs={0:F2}", dur),
            L"Enabled",
            false
        );
        return false;
    }
    bool disabledNow = false;
    if (GetCameraHardwareDisabledState(targetId, disabledNow) && !disabledNow) {
        m_cameraExpectedDisabled = false;
        double dur = timer.ElapsedMs();
        ConfigStore::WriteDiagnosticLogWithDevice(
            String::Format(L"EnableTargetCameraHardware_AlreadyEnabled | DurationMs={0:F2}", dur),
            targetId,
            L"Enabled",
            true
        );
        return true;
    }
    bool result = RecoverCameraHardware(targetId, cycleDevice);
    bool verified = VerifyCameraHardwareState(targetId, false);
    m_cameraExpectedDisabled = !result;
    double dur = timer.ElapsedMs();
    ConfigStore::WriteDiagnosticLogWithDevice(
        String::Format(
            L"EnableTargetCameraHardware_Result | Elevated={0} | IntegrityRid={1} | SetupErr={2} | CfgMgr={3} | Stage={4} | DurationMs={5:F2}",
            IsCurrentProcessElevatedNative() ? L"1" : L"0",
            static_cast<Int32>(GetCurrentProcessIntegrityRid()),
            static_cast<Int32>(InterlockedCompareExchange(&g_lastSetupApiError, 0, 0)),
            static_cast<Int32>(InterlockedCompareExchange(&g_lastConfigManagerResult, 0, 0)),
            static_cast<Int32>(InterlockedCompareExchange(&g_lastHardwareToggleStage, 0, 0)),
            dur
        ),
        targetId,
        L"Enabled",
        result && verified
    );
    return result && verified;
}

void ApplicationController::RestoreConfiguredCameraHardware(bool cycleDevice) {
    bool restoredConfiguredDevice = false;
    String^ savedDeviceInstance = L"";
    try {
        ConfigStore::LoadConfigState(savedDeviceInstance);
        if (!String::IsNullOrEmpty(savedDeviceInstance)) {
            std::wstring nativeDeviceId = msclr::interop::marshal_as<std::wstring>(savedDeviceInstance);
            restoredConfiguredDevice = RecoverCameraHardware(nativeDeviceId, cycleDevice);
        }
    }
    catch (...) {
        restoredConfiguredDevice = false;
    }
    if (!restoredConfiguredDevice) {
        RestoreAllCameraHardware(cycleDevice);
    }
}

void ApplicationController::Shutdown(bool isSystemEnding) {
    m_keepListening = false;
    if (isSystemEnding) {
        DisableTargetCameraHardware(true);
        if (m_selectedInstanceId && !m_selectedInstanceId->empty()) {
            String^ managedId = msclr::interop::marshal_as<String^>(*m_selectedInstanceId);
            ConfigStore::SaveConfigState(true, managedId);
        }
    } else if (m_selectedInstanceId && !m_selectedInstanceId->empty()) {
        EnableTargetCameraHardware(false);
        String^ managedId = msclr::interop::marshal_as<String^>(*m_selectedInstanceId);
        ConfigStore::SaveConfigState(true, managedId);
    } else {
        RestoreConfiguredCameraHardware(false);
    }
    if (m_hWakeupEvent) {
        SingleInstance::SignalAndCloseWakeEvent(m_hWakeupEvent);
        m_hWakeupEvent = NULL;
    }
    if (m_hLidNotification || m_hButtonNotification) {
        NotificationRegistrar::UnregisterPowerNotifications(m_hLidNotification, m_hButtonNotification);
        m_hLidNotification = NULL;
        m_hButtonNotification = NULL;
    }
    if (m_hAppMutex) {
        SingleInstance::ReleaseMutex(m_hAppMutex);
        m_hAppMutex = NULL;
    }
}

void ApplicationController::HandleSystemEnd(HWND hwnd) {
    m_isSystemEnding = true;
    ConfigStore::WriteDiagnosticLog(L"SystemEnd_Begin", L"Disabled", true);
    if (m_isMonitoring) {
        bool shutdownDisableResult = DisableTargetCameraHardware(true);
        ConfigStore::WriteDiagnosticLog(L"SystemEnd_Disable", L"Disabled", shutdownDisableResult);
    }
    NotificationRegistrar::UnregisterSessionNotification(hwnd);
}

void ApplicationController::HandlePowerEvent(SystemEvent ev) {
    if (ev == SystemEvent::PowerSuspend || ev == SystemEvent::PowerSettingLid || ev == SystemEvent::PowerSettingButton) {
        if (m_isMonitoring && !m_isAlreadyDisabled) {
            m_isAlreadyDisabled = true;
            bool powerDisableResult = DisableTargetCameraHardware(true);
            ConfigStore::WriteDiagnosticLog(L"PowerEvent_Disable", L"Disabled", powerDisableResult);
            ::Sleep(500);
        }
    } else if (ev == SystemEvent::PowerResumeSuspend || ev == SystemEvent::PowerResumeAutomatic) {
        if (m_isMonitoring) {
            System::Threading::Thread::Sleep(1000);
            bool powerEnableResult = EnableTargetCameraHardware(false);
            ConfigStore::WriteDiagnosticLog(L"PowerEvent_Enable", L"Enabled", powerEnableResult);
            m_isAlreadyDisabled = false;
        }
    }
}

void ApplicationController::HandleSessionEvent(SystemEvent ev) {
    if (!m_isMonitoring) {
        ConfigStore::WriteDiagnosticLog(L"SessionEvent_Ignored_MonitoringOff", L"NoChange", true);
    } else if (ev == SystemEvent::SessionLock) {
        bool lockDisableResult = DisableTargetCameraHardware(true);
        ConfigStore::WriteDiagnosticLog(L"SessionLock_Disable", L"Disabled", lockDisableResult);
    } else if (ev == SystemEvent::SessionUnlock) {
        bool unlockEnableResult = EnableTargetCameraHardware(false);
        ConfigStore::WriteDiagnosticLog(L"SessionUnlock_Enable", L"Enabled", unlockEnableResult);
    }
}

bool ApplicationController::ToggleMonitoring() {
    if (!m_isMonitoring) {
        m_isMonitoring = true;
        if (m_selectedInstanceId && !m_selectedInstanceId->empty()) {
            ConfigStore::SaveConfigState(true, msclr::interop::marshal_as<String^>(*m_selectedInstanceId));
        }
        return true;
    } else {
        m_isMonitoring = false;
        EnableTargetCameraHardware(false);
        if (m_selectedInstanceId && !m_selectedInstanceId->empty()) {
            ConfigStore::SaveConfigState(false, msclr::interop::marshal_as<String^>(*m_selectedInstanceId));
        }
        m_selectedInstanceId->clear();
        return false;
    }
}

void ApplicationController::ListenForWakeupSignal() {
    while (m_keepListening && m_hWakeupEvent != NULL) {
        DWORD waitResult = WaitForSingleObject(m_hWakeupEvent, INFINITE);
        if (waitResult == WAIT_OBJECT_0 && m_keepListening) {
            if (m_sink) {
                m_sink->BringWindowToFront();
            }
        }
    }
}

void ApplicationController::OnWakeupSignal() {
    if (m_sink) {
        m_sink->BringWindowToFront();
    }
}

bool ApplicationController::Initialize(HWND hwnd, array<String^>^ args) {
    m_hwnd = hwnd;
    bool launchRequestedBackground = CommandLine::IsBackgroundLaunch(args);

    ConfigStore::WriteDiagnosticLog(
        String::Format(
            L"Startup_Context | Elevated={0} | IntegrityRid={1} | BackgroundArg={2} | Exe={3} | Cwd={4} | Config={5}",
            IsCurrentProcessElevatedNative() ? L"1" : L"0",
            static_cast<Int32>(GetCurrentProcessIntegrityRid()),
            launchRequestedBackground ? L"1" : L"0",
            Application::ExecutablePath,
            Environment::CurrentDirectory,
            ConfigPaths::GetConfigFilePath()
        ),
        L"NoChange",
        IsCurrentProcessElevatedNative()
    );

    // v2.0 order: command checks BEFORE mutex. RestoreCameraCommand first.
    if (CommandLine::IsRestoreCameraCommand(args)) {
        ConfigStore::WriteDiagnosticLog(L"Command_EnableCamera_Begin", L"Enabled", true);
        RestoreConfiguredCameraHardware(true);
        ConfigStore::WriteDiagnosticLog(L"Command_EnableCamera_End", L"Enabled", true);
        Environment::Exit(0);
        return false;
    }

    if (CommandLine::IsDisableCameraCommand(args)) {
        std::wstring commandTargetId;
        ConfigStore::WriteDiagnosticLog(L"Command_DisableCamera_Begin", L"Disabled", true);
        bool commandDisableResult = DisableTargetCameraHardware(true);
        bool commandVerifyResult = TryGetTargetCameraInstanceId(commandTargetId, true) && VerifyCameraHardwareState(commandTargetId, true);
        ConfigStore::WriteDiagnosticLog(L"Command_DisableCamera_End", L"Disabled", commandDisableResult && commandVerifyResult);
        Environment::Exit(0);
        return false;
    }

    // Native inter-process single-instance layer - exact v2.0 branching order:
    // Try wake first, then background-missing, then ghost prompt
    bool alreadyExists = false;
    m_hAppMutex = SingleInstance::CreateAppMutex(alreadyExists);
    if (alreadyExists) {
        bool wakeSignalSent = SingleInstance::TrySignalExistingInstance();
        if (wakeSignalSent) {
            ConfigStore::WriteDiagnosticLog(L"SingleInstance_WakeSignalSent", L"NoChange", true);
            Environment::Exit(0);
            return false;
        }

        if (launchRequestedBackground) {
            ConfigStore::WriteDiagnosticLog(L"SingleInstance_BackgroundWakeEventMissing", L"NoChange", false);
            Environment::Exit(0);
            return false;
        }

        bool doReset = false;
        if (m_sink) {
            doReset = m_sink->PromptGhostReset();
        }
        if (doReset) {
            ConfigStore::WriteDiagnosticLog(L"SingleInstance_ForceResetRequested", L"NoChange", true);
            String^ ghostDeviceInstance = L"";
            ConfigStore::LoadConfigState(ghostDeviceInstance);
            if (String::IsNullOrEmpty(ghostDeviceInstance)) {
                auto cameras = ScanSystemCameras();
                for (size_t i = 0; i < cameras.size(); i++) {
                    if (cameras[i].instanceId.find(L"MI_00") != std::wstring::npos) {
                        ghostDeviceInstance = msclr::interop::marshal_as<String^>(cameras[i].instanceId);
                        break;
                    }
                }
            }
            if (!String::IsNullOrEmpty(ghostDeviceInstance)) {
                std::wstring nativeGhostId = msclr::interop::marshal_as<std::wstring>(ghostDeviceInstance);
                RecoverCameraHardware(nativeGhostId, true);
                ConfigStore::SaveConfigState(true, ghostDeviceInstance);
            }
            ProcessUtils::KillHelloFixProcess();
            ::Sleep(500);
            Application::Restart();
        }
        Environment::Exit(0);
        return false;
    }

    m_hWakeupEvent = SingleInstance::CreateWakeupEvent();

    // Startup recovery must happen before building the dropdown, because a disabled device may not appear as present yet.
    // Use the stronger cycle path here so manual launch can recover a camera that was left disabled by a previous session.
    ConfigStore::WriteDiagnosticLog(L"Startup_RestoreConfiguredCameraHardware", L"Enabled", true);
    RestoreConfiguredCameraHardware(true);

    RegisterNotifications();

    // Background worker for wakeup signal
    m_keepListening = true;
    m_backgroundWorker = gcnew System::Threading::Thread(gcnew System::Threading::ThreadStart(this, &ApplicationController::ListenForWakeupSignal));
    m_backgroundWorker->IsBackground = true;
    m_backgroundWorker->Start();

    bool sessionRegistered = NotificationRegistrar::RegisterSessionNotificationWithRetry(m_hwnd);
    if (sessionRegistered) {
        ConfigStore::WriteDiagnosticLog(L"WTSRegisterSessionNotification_Success", L"NoChange", true);
    } else {
        DWORD lastError = GetLastError();
        ConfigStore::WriteDiagnosticLog(
            String::Format(L"WTSRegisterSessionNotification_Failed_LastError={0}", static_cast<Int32>(lastError)),
            L"NoChange",
            false
        );
    }

    return true;
}

void ApplicationController::RegisterNotifications() {
    HPOWERNOTIFY lid = nullptr, button = nullptr;
    NotificationRegistrar::RegisterPowerNotifications(m_hwnd, lid, button);
    m_hLidNotification = lid;
    m_hButtonNotification = button;
}

void ApplicationController::UnregisterNotifications() {
    if (m_hLidNotification || m_hButtonNotification) {
        NotificationRegistrar::UnregisterPowerNotifications(m_hLidNotification, m_hButtonNotification);
        m_hLidNotification = NULL;
        m_hButtonNotification = NULL;
    }
    if (m_hwnd) {
        NotificationRegistrar::UnregisterSessionNotification(m_hwnd);
    }
}

void ApplicationController::SetSelectedInstanceId(const std::wstring& id) {
    if (m_selectedInstanceId) *m_selectedInstanceId = id;
}

std::wstring ApplicationController::GetSelectedInstanceIdNative() {
    return m_selectedInstanceId ? *m_selectedInstanceId : L"";
}

System::String^ ApplicationController::GetSelectedInstanceIdManaged() {
    if (m_selectedInstanceId) return msclr::interop::marshal_as<System::String^>(*m_selectedInstanceId);
    return L"";
}
