#pragma once

#include <windows.h>
#include <intrin.h> // Exposes _Interlocked*64 / Interlocked*64 on x86 (Win32); no-op on x64. Preserves LONG64 CAS semantics.
#include <wtsapi32.h>
#include <setupapi.h>
#include <cfgmgr32.h>
#include <devguid.h>
#include <vector>
#include <string>
#include <fstream> // For saving/loading config state
#include <msclr\marshal_cppstd.h>
#include "../../resource.h"

#pragma comment(lib, "wtsapi32.lib")
#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "cfgmgr32.lib")
#pragma comment(lib, "advapi32.lib")

// Define GUIDs manually if missing from standard headers
#ifndef GUID_LIDSWITCH_STATE_CHANGE
#define GUID_LIDSWITCH_STATE_CHANGE {0xBA3E0F4D, 0xB817, 0x4094, {0xA2, 0xD1, 0xD5, 0x63, 0x79, 0xE6, 0xA0, 0xF3}}
#endif

#ifndef GUID_POWER_BUTTON_TIMESTAMP
#define GUID_POWER_BUTTON_TIMESTAMP {0xA70AFB22, 0x3816, 0x4584, {0x9F, 0x24, 0x81, 0x0A, 0x4E, 0x27, 0x47, 0xFB}}
#endif

#ifndef CONFIGFLAG_DISABLED
#define CONFIGFLAG_DISABLED 0x00000001
#endif

#ifndef CM_PROB_DISABLED
#define CM_PROB_DISABLED 22
#endif

extern volatile LONG64 g_lastHardwareToggleTick;
extern volatile LONG g_lastSetupApiError;
extern volatile LONG g_lastConfigManagerResult;
extern volatile LONG g_lastHardwareToggleStage;
// Which SetCameraHardwareStateVerified attempt (1-based, 0 = none yet) and which
// path won (0 none, 1 SetupAPI, 2 CfgMgr32, 3 final pass, 4 already-correct).
// Written in the native retry loop, read by the managed Result log. Same
// InterlockedExchange pattern as the sibling g_last* globals.
extern volatile LONG g_lastAttemptCount;
extern volatile LONG g_lastSuccessPath;
// CameraRecoveryFailsafe (Issue #1) attribution globals — additive, same Interlocked pattern.
// g_lastPropChangeFlags: deferred-restart bits (DI_NEEDRESTART | DI_NEEDREBOOT) captured
//   after a DICS_PROPCHANGE restart; 0 = restart took effect immediately.
// g_lastServiceState / g_lastServiceError: last observed SERVICE_STATE and the Win32 error
//   of the Windows Camera Frame Server service restart (ERROR_SUCCESS on success).
extern volatile LONG g_lastPropChangeFlags;
extern volatile LONG g_lastServiceState;
extern volatile LONG g_lastServiceError;

// Forward-declare native helpers used by inline class methods to ensure
// they are visible at class parsing time.
bool SetCameraHardwareStateVerified(std::wstring targetId, bool enable, bool reinitializeOnMismatch);
bool RecoverCameraHardware(std::wstring targetId, bool cycleDevice);
bool GetCameraHardwareDisabledState(std::wstring targetId, bool& isDisabled);
bool VerifyCameraHardwareState(std::wstring targetId, bool shouldBeDisabled);
bool IsCurrentProcessElevatedNative();
DWORD GetCurrentProcessIntegrityRid();


namespace Windows_Hello_Fix_v2_0 {

    using namespace System;
    using namespace System::Windows::Forms;
    using namespace System::Drawing;
    using namespace System::ComponentModel;
    using namespace System::IO;
    using namespace System::Threading;

    ref class CameraFailsafe; // forward-declare watchdog (lives outside src/core)

    public ref class MyForm : public System::Windows::Forms::Form
    {
    private:
        void* cachedCameras;
        // Will be cast to std::vector<CameraDeviceInfo>*
        void* selectedInstanceId;
        // Will be cast to std::wstring*
        bool isMonitoring;
        bool isBackgroundMode;
        // Track if running in /background mode
        bool isSystemEnding;
        // Prevent shutdown/logoff cleanup from re-enabling the camera we just disabled
        bool cameraStateInitialized;
        bool cameraExpectedDisabled;
        bool restartQueuedByMismatch;
        ULONGLONG lastCameraToggleTick;
        HANDLE hAppMutex;
        // Single instance tracking mutex
        HANDLE hWakeupEvent;
        // Named event for cross-process communication
        System::Threading::Thread^ backgroundWorker;
        bool keepListening;
        CameraFailsafe^ cameraFailsafe;
        // Auxiliary runtime failsafe — observes ExpectedEnabled vs observed Disabled;
        // never performs camera operations itself. Lives outside src/core.

        // Low-level hardware registration handles
        HPOWERNOTIFY hLidNotification;
        HPOWERNOTIFY hButtonNotification;

        System::Windows::Forms::ComboBox^ deviceDrop;
        System::Windows::Forms::Button^ btnToggle;
        System::Windows::Forms::Label^ lblTitle;
        System::Windows::Forms::Label^ lblStatus;
        System::ComponentModel::Container^ components;
        Object^ diagnosticLogSync;
        String^ cachedLogPid; // per-process pid string for log lines, set on first write

        // Hardware Toggle Cooldown Tracking
        static System::DateTime lastToggleTime = System::DateTime::MinValue;
        static const int COOLDOWN_MILLISECONDS = 1500;

        // Configuration Helpers
        String^ GetConfigFilePath();
        String^ GetDiagnosticLogFilePath();
        void WriteDiagnosticLog(String^ eventName, String^ targetState, bool verificationPass);
        void WriteDiagnosticLogWithDevice(String^ eventName, std::wstring targetInstanceId, String^ targetState, bool verificationPass);
    public:
        // Log severity for the professional diagnostic format.
        enum class DiagLevel { Debug, Info, Warn, Error };
    private:
        // Extended logger: [timestamp] [LEVEL] [CATEGORY] Event | Op=.. | Pid=.. | <details> | Target=.. | Verify=..
        // details is a pre-formatted "Key=Value | Key=Value" fragment (may be empty).
        // opId may be empty (standalone markers). deviceId empty = no Device field.
        void WriteDiagnosticLogEx(DiagLevel level, String^ category, String^ eventName, String^ opId, String^ details, String^ targetState, bool verificationPass);
        void WriteDiagnosticLogExWithDevice(DiagLevel level, String^ category, String^ eventName, String^ opId, String^ details, std::wstring targetInstanceId, String^ targetState, bool verificationPass);
        void SaveConfigState(bool monitoring, String^ deviceInstanceId);
        bool LoadConfigState([System::Runtime::InteropServices::Out] String^% deviceInstanceId);
        void EnsureConfigFileExists(String^ deviceInstanceId);
        bool TryGetTargetCameraInstanceId(std::wstring& targetInstanceId, bool preferCurrentSelection);
        bool DisableTargetCameraHardware(bool retryOnFailure);
        bool DisableTargetCameraHardware(bool retryOnFailure, String^ opId);
        bool EnableTargetCameraHardware(bool cycleDevice);
        bool EnableTargetCameraHardware(bool cycleDevice, String^ opId);
        bool IsRestoreCameraCommand(array<System::String^>^ args);
        bool IsDisableCameraCommand(array<System::String^>^ args);
        void RestoreConfiguredCameraHardware(bool cycleDevice);

        // Background thread listener loop
        void ListenForWakeupSignal();
        // Safe UI thread invoker
        void BringWindowToFrontDelegate();

    public:
        // Failsafe integration — read-only accessors; watchdog must not mutate core state
        bool IsMonitoringActive();
        bool IsSystemEndingActive();
        bool IsCameraExpectedEnabled();
        bool TryGetFailsafeTargetId(std::wstring& targetId);
        void LogFailsafe(String^ eventName, String^ targetState, bool verificationPass);
        void LogFailsafeWithDevice(String^ eventName, std::wstring targetInstanceId, String^ targetState, bool verificationPass);
        void LogFailsafeEx(DiagLevel level, String^ category, String^ eventName, String^ opId, String^ details, String^ targetState, bool verificationPass);
        void LogFailsafeExWithDevice(DiagLevel level, String^ category, String^ eventName, String^ opId, String^ details, std::wstring targetInstanceId, String^ targetState, bool verificationPass);
        // Allocates the next per-process operation id ("PREFIX-000123"). Thread-safe.
        // Public so watchdogs can correlate their detect→verify→recover lines.
        String^ NewOperationId(String^ prefix);

        MyForm(void);

    protected:
        ~MyForm();
        !MyForm();

    private:
        void InitializeComponent(void);
        System::Void MyForm_Load(System::Object^ sender, System::EventArgs^ e);
        System::Void MyForm_FormClosing(System::Object^ sender, System::Windows::Forms::FormClosingEventArgs^ e);
        System::Void btnToggle_Click(System::Object^ sender, System::EventArgs^ e);

    protected:
        virtual void WndProc(System::Windows::Forms::Message% m) override;
    };

} // end namespace Windows_Hello_Fix_v2_0

// ====== NATIVE STRUCT AND FUNCTIONS ======

struct CameraDeviceInfo {
    std::wstring friendlyName;
    std::wstring instanceId;
};

std::vector<CameraDeviceInfo> ScanSystemCameras();
bool ToggleCameraHardware(std::wstring targetId, bool enable);
bool LocateCameraDevInst(std::wstring targetId, DEVINST& devInst);
bool ToggleCameraHardwareCfgMgr(std::wstring targetId, bool enable);
bool GetCameraHardwareDisabledState(std::wstring targetId, bool& isDisabled);
bool VerifyCameraHardwareState(std::wstring targetId, bool shouldBeDisabled);
bool SetCameraHardwareStateVerified(std::wstring targetId, bool enable, bool reinitializeOnMismatch);
bool TryEnterHardwareToggleCooldown(ULONGLONG cooldownMs);
void RecordHardwareToggleTime();
bool RecoverCameraHardware(std::wstring targetId, bool cycleDevice);
void RestoreAllCameraHardware(bool cycleDevices);

// Issue #1 same-session recovery primitives (additive; called only by
// src/watchdog/CameraRecoveryFailsafe — the watchdog decides WHEN, these decide HOW).
// Rung 1: documented device restart — DIF_PROPERTYCHANGE + DICS_PROPCHANGE stops and
//   restarts the devnode's driver stack in place WITHOUT changing its enabled state.
//   Stages 30-36; sets g_lastSetupApiError + g_lastPropChangeFlags.
bool RestartCameraHardware(std::wstring targetId);
// Rung 2: bounded restart of the "FrameServer" (Windows Camera Frame Server) service via
//   the SCM. Reports absence (false + g_lastServiceError=ERROR_SERVICE_DOES_NOT_EXIST) on
//   systems without it. Never touches other services. Stages 40-50.
bool RestartCameraFrameServerService();
// Rung 4: re-enumeration of the composite parent of a multi-interface (MI_*) camera child
//   — the correct tree level for genuine re-detection. Refuses non-composite targets
//   (their parent would be a hub). Reports the parent instance ID and a before/after
//   sibling devnode status summary (IR-safety evidence). Stages 60-65.
bool ReenumerateCameraParent(std::wstring targetId, std::wstring& parentId, std::wstring& siblingStatusReport);
// Rung 5A/5B: deep device-instance removal — the documented "software unplug/replug".
//   DIF_REMOVE with DI_REMOVEDEVICE_GLOBAL deletes the devnode (NOT the driver package)
//   and, unlike DICS_PROPCHANGE/disable/query-remove, is NOT vetoed by open handles;
//   re-enumeration then reinstalls the same driver from the driver store into a fresh
//   devnode with the same instance ID. scope 0 (5A) removes the target RGB interface
//   devnode and re-enumerates its composite parent; scope 1 (5B) removes the composite
//   parent itself (only valid for &MI_ multi-interface targets) and re-enumerates its
//   hub ancestor. ok requires removal accepted, target gone (bounded), re-enumeration
//   issued, and EVERY expected sibling interface back and started (same instance IDs).
//   Stages 70-80; details (phases, elapsed, before/after topology) in the report string.
bool RemoveAndReenumerateCameraHardware(std::wstring targetId, int deepScope, std::wstring& report);
std::wstring GetLastWin32ErrorText(DWORD err);

std::wstring TrimTrailingChars(const std::wstring& str);
