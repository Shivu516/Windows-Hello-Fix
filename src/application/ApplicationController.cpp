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

#include <cstdlib>
#include <msclr\marshal_cppstd.h>

using namespace System;
using namespace System::Windows::Forms;
using namespace System::Drawing;

// ---------------------------------------------------------------------------
// Failsafe boot helpers (no persistent daemon, no second hardware impl)
// ---------------------------------------------------------------------------
static bool IsHelloFixDaemonAlive() {
    HANDLE hMutex = OpenMutex(SYNCHRONIZE, FALSE, L"Global\\WindowsHelloFix_AppMutex");
    if (hMutex) {
        CloseHandle(hMutex);
        return true;
    }
    return false;
}

static System::String^ GetFailsafeBootId() {
    ULONGLONG tickMs = GetTickCount64();
    System::TimeSpan up = System::TimeSpan::FromMilliseconds((double)tickMs);
    System::DateTime bootTime = System::DateTime::Now - up;
    // Boot session identifier — stable for one boot, changes each reboot
    return bootTime.ToString("yyyyMMdd_HHmmss");
}

static System::String^ GetFailsafeFlagPath(System::String^ bootId) {
    System::String^ dir = System::IO::Path::Combine(
        System::Environment::GetFolderPath(System::Environment::SpecialFolder::ApplicationData),
        L"Windows Hello Fix");
    System::IO::Directory::CreateDirectory(dir);
    return System::IO::Path::Combine(dir, L"failsafe_notified_" + bootId + L".flag");
}

static bool ShouldShowFailsafeNotificationThisBoot(System::String^ bootId) {
    try {
        System::String^ flagPath = GetFailsafeFlagPath(bootId);
        if (System::IO::File::Exists(flagPath)) {
            return false;
        }
        // Clean stale flags from previous boots (keep only current boot's flag)
        System::String^ dir = System::IO::Path::GetDirectoryName(flagPath);
        array<System::String^>^ oldFlags = System::IO::Directory::GetFiles(dir, L"failsafe_notified_*.flag");
        for each (System::String^ f in oldFlags) {
            try {
                if (!f->Equals(flagPath, System::StringComparison::OrdinalIgnoreCase)) {
                    System::IO::File::Delete(f);
                }
            } catch (...) {}
        }
        return true;
    } catch (...) {
        return true;
    }
}

static void MarkFailsafeNotificationShownThisBoot(System::String^ bootId) {
    try {
        System::String^ flagPath = GetFailsafeFlagPath(bootId);
        System::IO::File::WriteAllText(flagPath, System::DateTime::Now.ToString(L"yyyy-MM-dd HH:mm:ss"));
    } catch (...) {}
}

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
    std::wstring targetId;
    if (!TryGetTargetCameraInstanceId(targetId, true)) {
        ConfigStore::WriteDiagnosticLog(L"DisableTargetCameraHardware_NoTarget", L"Disabled", false);
        return false;
    }
    bool alreadyDisabled = false;
    if (GetCameraHardwareDisabledState(targetId, alreadyDisabled) && alreadyDisabled) {
        m_cameraExpectedDisabled = true;
        ConfigStore::WriteDiagnosticLogWithDevice(L"DisableTargetCameraHardware_AlreadyDisabled", targetId, L"Disabled", true);
        return true;
    }
    bool result = SetCameraHardwareStateVerified(targetId, false, retryOnFailure);
    bool verified = VerifyCameraHardwareState(targetId, true);
    m_cameraExpectedDisabled = result;
    ConfigStore::WriteDiagnosticLogWithDevice(
        String::Format(
            L"DisableTargetCameraHardware_Result | Elevated={0} | IntegrityRid={1} | SetupErr={2} | CfgMgr={3} | Stage={4}",
            IsCurrentProcessElevatedNative() ? L"1" : L"0",
            static_cast<Int32>(GetCurrentProcessIntegrityRid()),
            static_cast<Int32>(InterlockedCompareExchange(&g_lastSetupApiError, 0, 0)),
            static_cast<Int32>(InterlockedCompareExchange(&g_lastConfigManagerResult, 0, 0)),
            static_cast<Int32>(InterlockedCompareExchange(&g_lastHardwareToggleStage, 0, 0))
        ),
        targetId,
        L"Disabled",
        result && verified
    );
    return result && verified;
}

bool ApplicationController::EnableTargetCameraHardware(bool cycleDevice) {
    std::wstring targetId;
    if (!TryGetTargetCameraInstanceId(targetId, true)) {
        ConfigStore::WriteDiagnosticLog(L"EnableTargetCameraHardware_NoTarget", L"Enabled", false);
        return false;
    }
    bool disabledNow = false;
    if (GetCameraHardwareDisabledState(targetId, disabledNow) && !disabledNow) {
        m_cameraExpectedDisabled = false;
        ConfigStore::WriteDiagnosticLogWithDevice(L"EnableTargetCameraHardware_AlreadyEnabled", targetId, L"Enabled", true);
        return true;
    }
    bool result = RecoverCameraHardware(targetId, cycleDevice);
    bool verified = VerifyCameraHardwareState(targetId, false);
    m_cameraExpectedDisabled = !result;
    ConfigStore::WriteDiagnosticLogWithDevice(
        String::Format(
            L"EnableTargetCameraHardware_Result | Elevated={0} | IntegrityRid={1} | SetupErr={2} | CfgMgr={3} | Stage={4}",
            IsCurrentProcessElevatedNative() ? L"1" : L"0",
            static_cast<Int32>(GetCurrentProcessIntegrityRid()),
            static_cast<Int32>(InterlockedCompareExchange(&g_lastSetupApiError, 0, 0)),
            static_cast<Int32>(InterlockedCompareExchange(&g_lastConfigManagerResult, 0, 0)),
            static_cast<Int32>(InterlockedCompareExchange(&g_lastHardwareToggleStage, 0, 0))
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
        // Start monitoring - selectedInstanceId should already be set via UI
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

    if (CommandLine::IsFailsafeBootCommand(args)) {
        ConfigStore::WriteDiagnosticLog(L"Failsafe_Start", L"NoChange", true);

        // Safety gate: respect intentional Monitoring OFF
        System::String^ failsafeDevice = L"";
        bool failsafeMonitoring = ConfigStore::LoadConfigState(failsafeDevice);
        if (!failsafeMonitoring) {
            ConfigStore::WriteDiagnosticLog(L"Failsafe_Skipped_MonitoringOff", L"NoChange", true);
            Environment::Exit(0);
            return false;
        }

        // Check whether native daemon is already alive (mutex probe, no ownership)
        if (IsHelloFixDaemonAlive()) {
            ConfigStore::WriteDiagnosticLog(L"Failsafe_Healthy_DaemonRunning", L"NoChange", true);
            Environment::Exit(0);
            return false;
        }

        // Resolve target camera (persisted config → MI_00 heuristic)
        std::wstring failsafeTargetId;
        if (!TryGetTargetCameraInstanceId(failsafeTargetId, true)) {
            ConfigStore::WriteDiagnosticLog(L"Failsafe_Skipped_NoTarget", L"NoChange", false);
            Environment::Exit(0);
            return false;
        }

        // Idempotent pre-check: if already enabled, do nothing
        bool failsafeIsDisabled = false;
        bool failsafeFound = GetCameraHardwareDisabledState(failsafeTargetId, failsafeIsDisabled);
        if (!failsafeFound || !failsafeIsDisabled) {
            ConfigStore::WriteDiagnosticLogWithDevice(L"Failsafe_Skipped_AlreadyEnabled", failsafeTargetId, L"Enabled", true);
            Environment::Exit(0);
            return false;
        }

        // Final liveness re-check immediately before touching hardware (avoid race where daemon started between checks)
        if (IsHelloFixDaemonAlive()) {
            ConfigStore::WriteDiagnosticLog(L"Failsafe_Healthy_DaemonRunning", L"NoChange", true);
            Environment::Exit(0);
            return false;
        }

        // Recovery: strictly enable-only, reuses existing recovery (verification+retry)
        bool failsafeRecovered = RecoverCameraHardware(failsafeTargetId, true);
        bool failsafeVerified = VerifyCameraHardwareState(failsafeTargetId, false);
        bool failsafeSuccess = failsafeRecovered && failsafeVerified;

        ConfigStore::WriteDiagnosticLogWithDevice(
            System::String::Format(
                L"Failsafe_{0} | Elevated={1} | IntegrityRid={2} | SetupErr={3} | CfgMgr={4} | Stage={5}",
                failsafeSuccess ? L"Recovered" : L"Recover_Failed",
                IsCurrentProcessElevatedNative() ? L"1" : L"0",
                static_cast<System::Int32>(GetCurrentProcessIntegrityRid()),
                static_cast<System::Int32>(InterlockedCompareExchange(&g_lastSetupApiError, 0, 0)),
                static_cast<System::Int32>(InterlockedCompareExchange(&g_lastConfigManagerResult, 0, 0)),
                static_cast<System::Int32>(InterlockedCompareExchange(&g_lastHardwareToggleStage, 0, 0))
            ),
            failsafeTargetId,
            L"Enabled",
            failsafeSuccess
        );

        System::String^ failsafeBootId = GetFailsafeBootId();
        if (failsafeSuccess) {
            if (ShouldShowFailsafeNotificationThisBoot(failsafeBootId)) {
                MessageBox::Show(
                    L"Windows Hello Fix did not start correctly after startup. Your camera was automatically re-enabled as a safety measure. Please start or enable Windows Hello Fix if you want camera protection to remain active.",
                    L"Windows Hello Fix - Safety Recovery",
                    MessageBoxButtons::OK,
                    MessageBoxIcon::Information
                );
                MarkFailsafeNotificationShownThisBoot(failsafeBootId);
            }
        } else {
            if (ShouldShowFailsafeNotificationThisBoot(failsafeBootId)) {
                MessageBox::Show(
                    L"Windows Hello Fix did not start correctly after startup and the camera could not be automatically re-enabled. Please start Windows Hello Fix manually or check Device Manager.",
                    L"Windows Hello Fix - Recovery Failed",
                    MessageBoxButtons::OK,
                    MessageBoxIcon::Warning
                );
                MarkFailsafeNotificationShownThisBoot(failsafeBootId);
            }
        }

        Environment::Exit(0);
        return false;
    }

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

    // Startup Apps gate: if automatic background startup was disabled in Task Manager → Startup, honor it.
    // This respects the user's Startup Apps choice even though the elevated task remains registered.
    if (launchRequestedBackground && CommandLine::IsStartupDisabled()) {
        ConfigStore::WriteDiagnosticLog(L"Startup_DisabledByStartupApproved", L"NoChange", true);
        if (IsCurrentProcessElevatedNative()) {
            // Disable the scheduled task itself so next logon it does not launch at all (primary mechanism).
            _wsystem(L"schtasks /Change /TN \"WindowsHelloFix\" /DISABLE >nul 2>&1");
        }
        Environment::Exit(0);
        return false;
    }

    // Ensure the scheduled task is enabled when Startup is enabled (so re-enable via Startup Apps takes effect next logon).
    // This handles the case where the task was previously disabled via the gate above.
    if (launchRequestedBackground && IsCurrentProcessElevatedNative() && !CommandLine::IsStartupDisabled()) {
        _wsystem(L"schtasks /Change /TN \"WindowsHelloFix\" /ENABLE >nul 2>&1");
    }

    // Run stub gate: the visible Startup Apps Run entry launches the exe with RUNASINVOKER (non-elevated) so it does not prompt UAC.
    // That stub must not become the daemon; the scheduled task (RunLevel Highest) provides the elevated daemon.
    if (launchRequestedBackground && !IsCurrentProcessElevatedNative()) {
        ConfigStore::WriteDiagnosticLog(L"Startup_RunStubNonElevatedExit", L"NoChange", true);
        Environment::Exit(0);
        return false;
    }

    bool alreadyExists = false;
    m_hAppMutex = SingleInstance::CreateAppMutex(alreadyExists);
    if (alreadyExists) {
        // Background/automatic launches must never wake the running daemon's GUI.
        if (launchRequestedBackground) {
            ConfigStore::WriteDiagnosticLog(L"SingleInstance_BackgroundSilentExit", L"NoChange", true);
            Environment::Exit(0);
            return false;
        }
        bool wakeSignalSent = SingleInstance::TrySignalExistingInstance();
        if (wakeSignalSent) {
            ConfigStore::WriteDiagnosticLog(L"SingleInstance_WakeSignalSent", L"NoChange", true);
            if (m_sink) {
                try { m_sink->ShowAlreadyRunningMessage(); } catch (...) {}
            }
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
