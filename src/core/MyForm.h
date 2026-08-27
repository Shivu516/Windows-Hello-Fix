#pragma once

#include <windows.h>
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

        // Hardware Toggle Cooldown Tracking
        static System::DateTime lastToggleTime = System::DateTime::MinValue;
        static const int COOLDOWN_MILLISECONDS = 1500;

        // Configuration Helpers
        String^ GetConfigFilePath();
        String^ GetDiagnosticLogFilePath();
        void WriteDiagnosticLog(String^ eventName, String^ targetState, bool verificationPass);
        void WriteDiagnosticLogWithDevice(String^ eventName, std::wstring targetInstanceId, String^ targetState, bool verificationPass);
        void SaveConfigState(bool monitoring, String^ deviceInstanceId);
        bool LoadConfigState([System::Runtime::InteropServices::Out] String^% deviceInstanceId);
        void EnsureConfigFileExists(String^ deviceInstanceId);
        bool TryGetTargetCameraInstanceId(std::wstring& targetInstanceId, bool preferCurrentSelection);
        bool DisableTargetCameraHardware(bool retryOnFailure);
        bool EnableTargetCameraHardware(bool cycleDevice);
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

std::wstring TrimTrailingChars(const std::wstring& str);
