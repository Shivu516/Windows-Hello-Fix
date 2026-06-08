#include <windows.h>
#include <wtsapi32.h>
#include <setupapi.h>
#include <cfgmgr32.h>
#include <devguid.h>
#include <vector>
#include <string>
#include <fstream> // For saving/loading config state
#include <msclr\marshal_cppstd.h>
#include "resource.h"

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

static volatile LONG64 g_lastHardwareToggleTick = 0;
static volatile LONG g_lastSetupApiError = ERROR_SUCCESS;
static volatile LONG g_lastConfigManagerResult = CR_SUCCESS;
static volatile LONG g_lastHardwareToggleStage = 0;

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
        bool DisableTargetCameraHardware(bool retryOnFailure)
        {
            std::wstring targetId;
            if (!TryGetTargetCameraInstanceId(targetId, true)) {
                WriteDiagnosticLog(L"DisableTargetCameraHardware_NoTarget", L"Disabled", false);
                return false;
            }

            lastToggleTime = System::DateTime::Now;

            bool alreadyDisabled = false;
            if (GetCameraHardwareDisabledState(targetId, alreadyDisabled) && alreadyDisabled) {
                cameraExpectedDisabled = true;
                WriteDiagnosticLogWithDevice(L"DisableTargetCameraHardware_AlreadyDisabled", targetId, L"Disabled", true);
                return true;
            }

            bool result = SetCameraHardwareStateVerified(targetId, false, retryOnFailure);
            bool verified = VerifyCameraHardwareState(targetId, true);
            cameraExpectedDisabled = result;
            WriteDiagnosticLogWithDevice(
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

        bool EnableTargetCameraHardware(bool cycleDevice)
        {
            std::wstring targetId;
            if (!TryGetTargetCameraInstanceId(targetId, true)) {
                WriteDiagnosticLog(L"EnableTargetCameraHardware_NoTarget", L"Enabled", false);
                return false;
            }

            lastToggleTime = System::DateTime::Now;

            bool disabledNow = false;
            if (GetCameraHardwareDisabledState(targetId, disabledNow) && !disabledNow) {
                cameraExpectedDisabled = false;
                WriteDiagnosticLogWithDevice(L"EnableTargetCameraHardware_AlreadyEnabled", targetId, L"Enabled", true);
                return true;
            }

            bool result = RecoverCameraHardware(targetId, cycleDevice);
            bool verified = VerifyCameraHardwareState(targetId, false);
            cameraExpectedDisabled = !result;
            WriteDiagnosticLogWithDevice(
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
        bool IsRestoreCameraCommand(array<System::String^>^ args);
        bool IsDisableCameraCommand(array<System::String^>^ args);
        void RestoreConfiguredCameraHardware(bool cycleDevice);

        // Background thread listener loop
        void ListenForWakeupSignal();
        // Safe UI thread invoker
        void BringWindowToFrontDelegate();

    public:
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

// ====== NATIVE FUNCTION IMPLEMENTATIONS ======

std::wstring TrimTrailingChars(const std::wstring& str) {
    std::wstring sanitized = str;
    // Remove trailing carriage returns, newlines, or trailing spaces
    while (!sanitized.empty() && (sanitized.back() == L'\r' || sanitized.back() == L'\n' || sanitized.back() == L' ')) {
        sanitized.pop_back();
    }
    return sanitized;
}

bool IsCurrentProcessElevatedNative() {
    HANDLE token = NULL;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        return false;
    }

    TOKEN_ELEVATION elevation;
    DWORD returnLength = 0;
    BOOL ok = GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &returnLength);
    CloseHandle(token);

    return ok && elevation.TokenIsElevated != 0;
}

DWORD GetCurrentProcessIntegrityRid() {
    HANDLE token = NULL;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        return 0;
    }

    DWORD tokenInfoLength = 0;
    GetTokenInformation(token, TokenIntegrityLevel, NULL, 0, &tokenInfoLength);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || tokenInfoLength == 0) {
        CloseHandle(token);
        return 0;
    }

    PTOKEN_MANDATORY_LABEL tokenLabel = reinterpret_cast<PTOKEN_MANDATORY_LABEL>(LocalAlloc(LPTR, tokenInfoLength));
    if (tokenLabel == NULL) {
        CloseHandle(token);
        return 0;
    }

    DWORD integrityRid = 0;
    if (GetTokenInformation(token, TokenIntegrityLevel, tokenLabel, tokenInfoLength, &tokenInfoLength)) {
        DWORD subAuthorityCount = *GetSidSubAuthorityCount(tokenLabel->Label.Sid);
        integrityRid = *GetSidSubAuthority(tokenLabel->Label.Sid, subAuthorityCount - 1);
    }

    LocalFree(tokenLabel);
    CloseHandle(token);
    return integrityRid;
}

std::vector<CameraDeviceInfo> ScanSystemCameras() {
    std::vector<CameraDeviceInfo> list;
    HDEVINFO hDevInfo = SetupDiGetClassDevs(NULL, NULL, NULL, DIGCF_ALLCLASSES | DIGCF_PRESENT);

    if (hDevInfo == INVALID_HANDLE_VALUE) {
        return list;
    }

    SP_DEVINFO_DATA devInfoData;
    devInfoData.cbSize = sizeof(SP_DEVINFO_DATA);

    for (int i = 0; SetupDiEnumDeviceInfo(hDevInfo, i, &devInfoData); i++) {
        WCHAR classBuffer[256] = { 0 };

        if (SetupDiGetDeviceRegistryProperty(hDevInfo, &devInfoData, SPDRP_CLASS, NULL, (PBYTE)classBuffer, sizeof(classBuffer), NULL)) {
            std::wstring deviceClass(classBuffer);

            if (deviceClass == L"Camera" || deviceClass == L"Image") {
                WCHAR instancePath[MAX_DEVICE_ID_LEN];
                WCHAR desc[256] = { 0 };

                if (SetupDiGetDeviceInstanceId(hDevInfo, &devInfoData, instancePath, MAX_DEVICE_ID_LEN, NULL)) {
                    SetupDiGetDeviceRegistryProperty(hDevInfo, &devInfoData, SPDRP_DEVICEDESC, NULL, (PBYTE)desc, sizeof(desc), NULL);

                    CameraDeviceInfo info;
                    info.friendlyName = desc;
                    info.instanceId = instancePath;
                    list.push_back(info);
                }
            }
        }
    }
    SetupDiDestroyDeviceInfoList(hDevInfo);
    return list;
}

// ====== TOGGLE CAMERA HARDWARE STATE ======

bool ToggleCameraHardware(std::wstring targetId, bool enable) {
    InterlockedExchange(&g_lastSetupApiError, ERROR_SUCCESS);
    InterlockedExchange(&g_lastHardwareToggleStage, 10);

    HDEVINFO hDevInfo = SetupDiGetClassDevs(NULL, NULL, NULL, DIGCF_ALLCLASSES);

    if (hDevInfo == INVALID_HANDLE_VALUE) {
        InterlockedExchange(&g_lastSetupApiError, static_cast<LONG>(GetLastError()));
        InterlockedExchange(&g_lastHardwareToggleStage, 11);
        return false;
    }

    SP_DEVINFO_DATA devInfoData;
    devInfoData.cbSize = sizeof(SP_DEVINFO_DATA);
    bool changed = false;

    for (int i = 0; SetupDiEnumDeviceInfo(hDevInfo, i, &devInfoData); i++) {
        WCHAR instancePath[MAX_DEVICE_ID_LEN];

        if (SetupDiGetDeviceInstanceId(hDevInfo, &devInfoData, instancePath, MAX_DEVICE_ID_LEN, NULL)) {

            // Keep the original working direct instance-ID behavior, with a case-insensitive fallback for saved config text.
            if (targetId == instancePath || _wcsicmp(targetId.c_str(), instancePath) == 0) {

                SP_PROPCHANGE_PARAMS params;
                ZeroMemory(&params, sizeof(SP_PROPCHANGE_PARAMS));

                params.ClassInstallHeader.cbSize = sizeof(SP_CLASSINSTALL_HEADER);
                params.ClassInstallHeader.InstallFunction = DIF_PROPERTYCHANGE;
                params.StateChange = enable ? DICS_ENABLE : DICS_DISABLE;
                params.Scope = DICS_FLAG_GLOBAL;
                params.HwProfile = 0;

                if (!SetupDiSetClassInstallParams(hDevInfo, &devInfoData, &params.ClassInstallHeader, sizeof(params))) {
                    InterlockedExchange(&g_lastSetupApiError, static_cast<LONG>(GetLastError()));
                    InterlockedExchange(&g_lastHardwareToggleStage, 12);
                    changed = false;
                    break;
                }

                if (!SetupDiCallClassInstaller(DIF_PROPERTYCHANGE, hDevInfo, &devInfoData)) {
                    InterlockedExchange(&g_lastSetupApiError, static_cast<LONG>(GetLastError()));
                    InterlockedExchange(&g_lastHardwareToggleStage, 13);
                    changed = false;
                    break;
                }

                InterlockedExchange(&g_lastSetupApiError, ERROR_SUCCESS);
                InterlockedExchange(&g_lastHardwareToggleStage, 14);
                changed = true;
                break;
            }
        }
    }
    SetupDiDestroyDeviceInfoList(hDevInfo);
    if (!changed && InterlockedCompareExchange(&g_lastHardwareToggleStage, 0, 0) == 10) {
        InterlockedExchange(&g_lastHardwareToggleStage, 15);
    }
    return changed;
}

bool LocateCameraDevInst(std::wstring targetId, DEVINST& devInst) {
    devInst = 0;
    HDEVINFO hDevInfo = SetupDiGetClassDevs(NULL, NULL, NULL, DIGCF_ALLCLASSES);

    if (hDevInfo == INVALID_HANDLE_VALUE) {
        return false;
    }

    SP_DEVINFO_DATA devInfoData;
    devInfoData.cbSize = sizeof(SP_DEVINFO_DATA);
    bool found = false;

    for (int i = 0; SetupDiEnumDeviceInfo(hDevInfo, i, &devInfoData); i++) {
        WCHAR instancePath[MAX_DEVICE_ID_LEN];
        if (SetupDiGetDeviceInstanceId(hDevInfo, &devInfoData, instancePath, MAX_DEVICE_ID_LEN, NULL)) {
            if (targetId == instancePath || _wcsicmp(targetId.c_str(), instancePath) == 0) {
                devInst = devInfoData.DevInst;
                found = true;
                break;
            }
        }
    }

    SetupDiDestroyDeviceInfoList(hDevInfo);
    return found;
}

bool ToggleCameraHardwareCfgMgr(std::wstring targetId, bool enable) {
    InterlockedExchange(&g_lastConfigManagerResult, CR_SUCCESS);
    InterlockedExchange(&g_lastHardwareToggleStage, 20);

    DEVINST devInst = 0;
    if (!LocateCameraDevInst(targetId, devInst)) {
        InterlockedExchange(&g_lastHardwareToggleStage, 21);
        return false;
    }

    CONFIGRET cr = enable ? CM_Enable_DevNode(devInst, 0) : CM_Disable_DevNode(devInst, CM_DISABLE_UI_NOT_OK);
    InterlockedExchange(&g_lastConfigManagerResult, static_cast<LONG>(cr));
    if (cr != CR_SUCCESS) {
        InterlockedExchange(&g_lastHardwareToggleStage, 22);
        return false;
    }

    CM_Reenumerate_DevNode(devInst, 0);
    InterlockedExchange(&g_lastHardwareToggleStage, 23);
    return true;
}

bool GetCameraHardwareDisabledState(std::wstring targetId, bool& isDisabled) {
    isDisabled = false;
    HDEVINFO hDevInfo = SetupDiGetClassDevs(NULL, NULL, NULL, DIGCF_ALLCLASSES);

    if (hDevInfo == INVALID_HANDLE_VALUE) {
        return false;
    }

    SP_DEVINFO_DATA devInfoData;
    devInfoData.cbSize = sizeof(SP_DEVINFO_DATA);
    bool found = false;

    for (int i = 0; SetupDiEnumDeviceInfo(hDevInfo, i, &devInfoData); i++) {
        WCHAR instancePath[MAX_DEVICE_ID_LEN];

        if (SetupDiGetDeviceInstanceId(hDevInfo, &devInfoData, instancePath, MAX_DEVICE_ID_LEN, NULL)) {
            if (targetId == instancePath || _wcsicmp(targetId.c_str(), instancePath) == 0) {
                ULONG status = 0;
                ULONG problem = 0;
                CONFIGRET statusResult = CM_Get_DevNode_Status(&status, &problem, devInfoData.DevInst, 0);

                DWORD propertyType = 0;
                DWORD configFlags = 0;
                BOOL hasConfigFlags = SetupDiGetDeviceRegistryProperty(
                    hDevInfo,
                    &devInfoData,
                    SPDRP_CONFIGFLAGS,
                    &propertyType,
                    reinterpret_cast<PBYTE>(&configFlags),
                    sizeof(configFlags),
                    NULL
                );

                bool disabledByConfig = (hasConfigFlags && ((configFlags & CONFIGFLAG_DISABLED) != 0));
                bool disabledByProblemCode = (statusResult == CR_SUCCESS && problem == CM_PROB_DISABLED);

                isDisabled = disabledByConfig || disabledByProblemCode;
                found = true;
                break;
            }
        }
    }

    SetupDiDestroyDeviceInfoList(hDevInfo);
    return found;
}

bool VerifyCameraHardwareState(std::wstring targetId, bool shouldBeDisabled) {
    bool isDisabled = false;

    for (int attempt = 0; attempt < 3; attempt++) {
        if (GetCameraHardwareDisabledState(targetId, isDisabled) && isDisabled == shouldBeDisabled) {
            return true;
        }
        ::Sleep(100);
    }

    return false;
}

bool TryEnterHardwareToggleCooldown(ULONGLONG cooldownMs) {
    for (int spin = 0; spin < 8; spin++) {
        LONG64 lastTick = InterlockedCompareExchange64(&g_lastHardwareToggleTick, 0, 0);
        ULONGLONG nowTick = GetTickCount64();

        if (lastTick != 0) {
            ULONGLONG elapsed = nowTick - static_cast<ULONGLONG>(lastTick);
            if (elapsed < cooldownMs) {
                return false;
            }
        }

        LONG64 previous = InterlockedCompareExchange64(&g_lastHardwareToggleTick, static_cast<LONG64>(nowTick), lastTick);
        if (previous == lastTick) {
            return true;
        }
    }

    return false;
}

void RecordHardwareToggleTime() {
    InterlockedExchange64(&g_lastHardwareToggleTick, static_cast<LONG64>(GetTickCount64()));
}

bool SetCameraHardwareStateVerified(std::wstring targetId, bool enable, bool reinitializeOnMismatch) {
    if (targetId.empty()) {
        return false;
    }

    bool shouldBeDisabled = !enable;

    // Check-before-change: if already in target state, skip hardware command churn.
    if (VerifyCameraHardwareState(targetId, shouldBeDisabled)) {
        return true;
    }

    for (int attempt = 0; attempt < 3; attempt++) {
        ToggleCameraHardware(targetId, enable);

        if (VerifyCameraHardwareState(targetId, shouldBeDisabled)) {
            RecordHardwareToggleTime();
            return true;
        }

        ToggleCameraHardwareCfgMgr(targetId, enable);
        if (VerifyCameraHardwareState(targetId, shouldBeDisabled)) {
            RecordHardwareToggleTime();
            return true;
        }

        if (reinitializeOnMismatch) {
            // Reinitialize the device node once Windows reports that the requested state did not stick.
            ToggleCameraHardware(targetId, !enable);
            ToggleCameraHardwareCfgMgr(targetId, !enable);
            ::Sleep(250);
        }

        ::Sleep(250);
    }

    ToggleCameraHardware(targetId, enable);
    bool verified = VerifyCameraHardwareState(targetId, shouldBeDisabled);
    if (verified) {
        RecordHardwareToggleTime();
    }
    return verified;
}

bool RecoverCameraHardware(std::wstring targetId, bool cycleDevice) {
    if (targetId.empty()) {
        return false;
    }

    bool restored = SetCameraHardwareStateVerified(targetId, true, false);

    if (cycleDevice) {
        ::Sleep(350);
        SetCameraHardwareStateVerified(targetId, false, false);
        ::Sleep(900);
        restored = SetCameraHardwareStateVerified(targetId, true, false) || restored;
        ::Sleep(500);
        restored = SetCameraHardwareStateVerified(targetId, true, false) || restored;
    }

    return restored;
}

void RestoreAllCameraHardware(bool cycleDevices) {
    std::vector<CameraDeviceInfo> cameras = ScanSystemCameras();

    for (size_t i = 0; i < cameras.size(); i++) {
        RecoverCameraHardware(cameras[i].instanceId, cycleDevices);
    }
}

// ====== MANAGED CLASS IMPLEMENTATION ======

namespace Windows_Hello_Fix_v2_0 {

    MyForm::MyForm(void) {
        components = nullptr;
        cachedCameras = new std::vector<CameraDeviceInfo>();
        selectedInstanceId = new std::wstring();
        isMonitoring = false;
        isBackgroundMode = false;
        isSystemEnding = false;
        cameraStateInitialized = false;
        cameraExpectedDisabled = false;
        restartQueuedByMismatch = false;
        lastCameraToggleTick = 0;
        hAppMutex = NULL;
        hWakeupEvent = NULL;
        keepListening = true;
        hLidNotification = NULL;
        hButtonNotification = NULL;
        diagnosticLogSync = gcnew Object();
        InitializeComponent();
    }

    // LAST THING THE APP DOES BEFORE SHUTTING DOWN ENTIRELY (Destructor)
    MyForm::~MyForm() {
        keepListening = false;

        auto* pSelectedInstanceId = static_cast<std::wstring*>(selectedInstanceId);
        if (isSystemEnding) {
            DisableTargetCameraHardware(true);

            if (pSelectedInstanceId && pSelectedInstanceId->length() > 0) {
                String^ managedId = msclr::interop::marshal_as<String^>(*pSelectedInstanceId);
                SaveConfigState(true, managedId);
            }
        }
        else if (pSelectedInstanceId && pSelectedInstanceId->length() > 0) {
            // Last thing before shutting down: Ensure Camera is RE-ENABLED safely
            EnableTargetCameraHardware(false);

            // Last thing before shutting down: Revert config state to monitoring=1 with LIVE string
            String^ managedId = msclr::interop::marshal_as<String^>(*pSelectedInstanceId);
            SaveConfigState(true, managedId);
        }
        else {
            RestoreConfiguredCameraHardware(false);
        }

        if (hWakeupEvent) {
            SetEvent(hWakeupEvent);
            CloseHandle(hWakeupEvent);
        }
        if (hLidNotification) {
            UnregisterPowerSettingNotification(hLidNotification);
        }
        if (hButtonNotification) {
            UnregisterPowerSettingNotification(hButtonNotification);
        }
        if (cachedCameras != nullptr) {
            delete static_cast<std::vector<CameraDeviceInfo>*>(cachedCameras);
            cachedCameras = nullptr;
        }
        if (selectedInstanceId != nullptr) {
            delete static_cast<std::wstring*>(selectedInstanceId);
            selectedInstanceId = nullptr;
        }
        if (components) {
            delete components;
        }
        if (hAppMutex) {
            CloseHandle(hAppMutex);
        }
    }

    MyForm::!MyForm() {
        keepListening = false;
        auto* pSelectedInstanceId = static_cast<std::wstring*>(selectedInstanceId);
        if (isSystemEnding) {
            DisableTargetCameraHardware(true);
        }
        else if (pSelectedInstanceId && pSelectedInstanceId->length() > 0) {
            EnableTargetCameraHardware(false);
        }
        if (hWakeupEvent) {
            SetEvent(hWakeupEvent);
            CloseHandle(hWakeupEvent);
        }
        if (hLidNotification) {
            UnregisterPowerSettingNotification(hLidNotification);
        }
        if (hButtonNotification) {
            UnregisterPowerSettingNotification(hButtonNotification);
        }
        if (cachedCameras != nullptr) {
            delete static_cast<std::vector<CameraDeviceInfo>*>(cachedCameras);
            cachedCameras = nullptr;
        }
        if (selectedInstanceId != nullptr) {
            delete static_cast<std::wstring*>(selectedInstanceId);
            selectedInstanceId = nullptr;
        }
        if (hAppMutex) {
            CloseHandle(hAppMutex);
        }
    }

    String^ MyForm::GetConfigFilePath() {
        String^ dir = Path::Combine(
            Environment::GetFolderPath(Environment::SpecialFolder::ApplicationData),
            L"Windows Hello Fix"
        );

        Directory::CreateDirectory(dir);
        return Path::Combine(dir, L"config.txt");
    }

    String^ MyForm::GetDiagnosticLogFilePath() {
        String^ configPath = GetConfigFilePath();
        String^ configDirectory = Path::GetDirectoryName(configPath);

        if (String::IsNullOrEmpty(configDirectory)) {
            configDirectory = Path::Combine(
                Environment::GetFolderPath(Environment::SpecialFolder::ApplicationData),
                L"Windows Hello Fix"
            );
        }

        Directory::CreateDirectory(configDirectory);
        return Path::Combine(configDirectory, L"diagnostic.log");
    }

    void MyForm::WriteDiagnosticLog(String^ eventName, String^ targetState, bool verificationPass) {
        System::Threading::Monitor::Enter(diagnosticLogSync);
        try {
            String^ logPath = GetDiagnosticLogFilePath();
            StreamWriter^ sw = gcnew StreamWriter(logPath, true);
            String^ timestamp = DateTime::Now.ToString(L"yyyy-MM-dd HH:mm:ss.fff");
            sw->WriteLine(
                String::Format(
                    L"{0} | Event={1} | Target={2} | Verify={3}",
                    timestamp,
                    eventName,
                    targetState,
                    verificationPass ? L"PASS" : L"FAIL"
                )
            );
            sw->Close();
        }
        catch (...) {}
        finally {
            System::Threading::Monitor::Exit(diagnosticLogSync);
        }
    }

    void MyForm::WriteDiagnosticLogWithDevice(String^ eventName, std::wstring targetInstanceId, String^ targetState, bool verificationPass) {
        String^ deviceId = msclr::interop::marshal_as<String^>(targetInstanceId);
        WriteDiagnosticLog(
            eventName + L" | Device=" + deviceId,
            targetState,
            verificationPass
        );
    }

    void MyForm::SaveConfigState(bool monitoring, String^ deviceInstanceId) {
        try {
            String^ path = GetConfigFilePath();
            StreamWriter^ sw = gcnew StreamWriter(path, false);
            sw->WriteLine(monitoring ? L"monitoring=1" : L"monitoring=0");
            sw->WriteLine(L"device=" + deviceInstanceId);
            sw->Close();
        }
        catch (...) {}
    }

    bool MyForm::LoadConfigState([System::Runtime::InteropServices::Out] String^% deviceInstanceId) {
        deviceInstanceId = L"";
        try {
            String^ path = GetConfigFilePath();
            if (!File::Exists(path)) {
                return false;
            }

            StreamReader^ sr = gcnew StreamReader(path);
            String^ line1 = sr->ReadLine();
            String^ line2 = sr->ReadLine();
            sr->Close();

            bool monitoringActive = (line1 != nullptr && line1->Trim() == L"monitoring=1");
            if (line2 != nullptr && line2->StartsWith(L"device=")) {
                // FIX: Trim prevents \r\n newline corruption in C++ string matching
                std::wstring rawPath = msclr::interop::marshal_as<std::wstring>(line2->Substring(7)->Trim());
                std::wstring sanitizedPath = TrimTrailingChars(rawPath);
                deviceInstanceId = msclr::interop::marshal_as<String^>(sanitizedPath);
            }
            return monitoringActive;
        }
        catch (...) {
            return false;
        }
    }

    void MyForm::EnsureConfigFileExists(String^ deviceInstanceId) {
        try {
            String^ path = GetConfigFilePath();
            if (!File::Exists(path)) {
                SaveConfigState(false, deviceInstanceId);
            }
        }
        catch (...) {}
    }

    bool MyForm::TryGetTargetCameraInstanceId(std::wstring& targetInstanceId, bool preferCurrentSelection) {
        targetInstanceId.clear();

        auto* pSelectedInstanceId = static_cast<std::wstring*>(selectedInstanceId);

        if (preferCurrentSelection && pSelectedInstanceId && !pSelectedInstanceId->empty()) {
            targetInstanceId = *pSelectedInstanceId;
            return true;
        }

        String^ savedDeviceInstance = L"";
        LoadConfigState(savedDeviceInstance);
        if (!String::IsNullOrEmpty(savedDeviceInstance)) {
            targetInstanceId = msclr::interop::marshal_as<std::wstring>(savedDeviceInstance);
            return true;
        }

        if (!preferCurrentSelection && pSelectedInstanceId && !pSelectedInstanceId->empty()) {
            targetInstanceId = *pSelectedInstanceId;
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



    bool MyForm::IsRestoreCameraCommand(array<System::String^>^ args) {
        for (int i = 0; i < args->Length; i++) {
            if (args[i]->Equals(L"/restore-camera", System::StringComparison::OrdinalIgnoreCase) ||
                args[i]->Equals(L"/enable-camera", System::StringComparison::OrdinalIgnoreCase) ||
                args[i]->Equals(L"--enable-camera", System::StringComparison::OrdinalIgnoreCase) ||
                args[i]->Equals(L"/repair-camera", System::StringComparison::OrdinalIgnoreCase)) {
                return true;
            }
        }

        return false;
    }

    bool MyForm::IsDisableCameraCommand(array<System::String^>^ args) {
        for (int i = 0; i < args->Length; i++) {
            if (args[i]->Equals(L"/disable-camera", System::StringComparison::OrdinalIgnoreCase) ||
                args[i]->Equals(L"--disable-camera", System::StringComparison::OrdinalIgnoreCase)) {
                return true;
            }
        }

        return false;
    }

    void MyForm::RestoreConfiguredCameraHardware(bool cycleDevice) {
        bool restoredConfiguredDevice = false;
        String^ savedDeviceInstance = L"";

        try {
            LoadConfigState(savedDeviceInstance);

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

    void MyForm::InitializeComponent(void) {
        this->deviceDrop = (gcnew System::Windows::Forms::ComboBox());
        this->btnToggle = (gcnew System::Windows::Forms::Button());
        this->lblTitle = (gcnew System::Windows::Forms::Label());
        this->lblStatus = (gcnew System::Windows::Forms::Label());
        this->components = (gcnew System::ComponentModel::Container());

        this->SuspendLayout();
        try {
            HICON hMainIcon = (HICON)LoadImage(GetModuleHandle(NULL), MAKEINTRESOURCE(IDI_ICON1), IMAGE_ICON, 0, 0, LR_DEFAULTSIZE);
            if (hMainIcon) {
                this->Icon = System::Drawing::Icon::FromHandle((IntPtr)hMainIcon);
            }
        }
        catch (...) {}

        // deviceDrop
        this->deviceDrop->DropDownStyle = System::Windows::Forms::ComboBoxStyle::DropDownList;
        this->deviceDrop->Font = (gcnew System::Drawing::Font(L"Segoe UI", 10));
        this->deviceDrop->Location = System::Drawing::Point(25, 75);
        this->deviceDrop->Name = L"deviceDrop";
        this->deviceDrop->Size = System::Drawing::Size(380, 31);
        this->deviceDrop->Enabled = true;

        // btnToggle
        this->btnToggle->Font = (gcnew System::Drawing::Font(L"Segoe UI", 10, System::Drawing::FontStyle::Bold));
        this->btnToggle->Location = System::Drawing::Point(25, 130);
        this->btnToggle->Name = L"btnToggle";
        this->btnToggle->Size = System::Drawing::Size(380, 45);
        this->btnToggle->Text = L"Start Monitoring Service";
        this->btnToggle->Click += gcnew System::EventHandler(this, &MyForm::btnToggle_Click);

        // lblTitle
        this->lblTitle->AutoSize = true;
        this->lblTitle->Font = (gcnew System::Drawing::Font(L"Segoe UI", 12, System::Drawing::FontStyle::Bold));
        this->lblTitle->Location = System::Drawing::Point(20, 25);
        this->lblTitle->Text = L"Select Target RGB Sensor";

        // lblStatus
        this->lblStatus->AutoSize = true;
        this->lblStatus->ForeColor = System::Drawing::Color::Gray;
        this->lblStatus->Location = System::Drawing::Point(25, 195);
        this->lblStatus->Text = L"Status: Service Stopped";

        // MyForm
        this->ClientSize = System::Drawing::Size(430, 240);
        this->Controls->Add(this->lblStatus);
        this->Controls->Add(this->lblTitle);
        this->Controls->Add(this->btnToggle);
        this->Controls->Add(this->deviceDrop);
        this->FormBorderStyle = System::Windows::Forms::FormBorderStyle::FixedDialog;
        this->MaximizeBox = false;
        this->MinimizeBox = false;
        this->Name = L"MyForm";
        this->StartPosition = System::Windows::Forms::FormStartPosition::CenterScreen;
        this->Text = L"Windows Hello Fix v2.0";
        this->FormClosing += gcnew System::Windows::Forms::FormClosingEventHandler(this, &MyForm::MyForm_FormClosing);
        this->Load += gcnew System::EventHandler(this, &MyForm::MyForm_Load);

        this->ResumeLayout(false);
        this->PerformLayout();
    }

    System::Void MyForm::MyForm_Load(System::Object^ sender, System::EventArgs^ e) {
        array<System::String^>^ args = System::Environment::GetCommandLineArgs();
        bool launchRequestedBackground = false;

        for (int i = 0; i < args->Length; i++) {
            if (args[i]->Equals(L"/background", System::StringComparison::OrdinalIgnoreCase) ||
                args[i]->Equals(L"--background", System::StringComparison::OrdinalIgnoreCase)) {
                launchRequestedBackground = true;
                break;
            }
        }

        WriteDiagnosticLog(
            String::Format(
                L"Startup_Context | Elevated={0} | IntegrityRid={1} | BackgroundArg={2} | Exe={3} | Cwd={4} | Config={5}",
                IsCurrentProcessElevatedNative() ? L"1" : L"0",
                static_cast<Int32>(GetCurrentProcessIntegrityRid()),
                launchRequestedBackground ? L"1" : L"0",
                Application::ExecutablePath,
                Environment::CurrentDirectory,
                GetConfigFilePath()
            ),
            L"NoChange",
            IsCurrentProcessElevatedNative()
        );

        if (IsRestoreCameraCommand(args)) {
            this->ShowInTaskbar = false;
            this->Visible = false;
            WriteDiagnosticLog(L"Command_EnableCamera_Begin", L"Enabled", true);
            RestoreConfiguredCameraHardware(true);
            WriteDiagnosticLog(L"Command_EnableCamera_End", L"Enabled", true);
            Environment::Exit(0);
            return;
        }

        if (IsDisableCameraCommand(args)) {
            this->ShowInTaskbar = false;
            this->Visible = false;
            std::wstring commandTargetId;
            WriteDiagnosticLog(L"Command_DisableCamera_Begin", L"Disabled", true);
            bool commandDisableResult = DisableTargetCameraHardware(true);
            bool commandVerifyResult = TryGetTargetCameraInstanceId(commandTargetId, true) && VerifyCameraHardwareState(commandTargetId, true);
            WriteDiagnosticLog(L"Command_DisableCamera_End", L"Disabled", commandDisableResult && commandVerifyResult);
            Environment::Exit(0);
            return;
        }

        // ====== NATIVE INTER-PROCESS SIGNAL LAYER ======
        hAppMutex = CreateMutex(NULL, TRUE, L"Global\\WindowsHelloFix_AppMutex");
        if (GetLastError() == ERROR_ALREADY_EXISTS) {
            bool wakeSignalSent = false;
            HANDLE hOpenEvent = OpenEvent(EVENT_MODIFY_STATE, FALSE, L"Global\\WindowsHelloFix_WakeupEvent");
            if (hOpenEvent) {
                SetEvent(hOpenEvent);
                CloseHandle(hOpenEvent);
                ::Sleep(200);
                wakeSignalSent = true;
            }

            // Normal path: existing process receives wake signal and brings main window back.
            // Avoid showing scary duplicate-instance prompts on expected startup/manual-open races.
            if (wakeSignalSent) {
                WriteDiagnosticLog(L"SingleInstance_WakeSignalSent", L"NoChange", true);
                Environment::Exit(0);
                return;
            }

            // If wake event is unavailable and this is a background launch, fail quiet and do not block startup.
            if (launchRequestedBackground) {
                WriteDiagnosticLog(L"SingleInstance_BackgroundWakeEventMissing", L"NoChange", false);
                Environment::Exit(0);
                return;
            }

            // SELF-HEALING GHOST MUTEX PROTECTION WINDOW (only when wake signal path is unavailable)
            System::Windows::Forms::DialogResult result = MessageBox::Show(
                L"Windows Hello Fix is already running in the background.\n\nIf the application is frozen or not responding, would you like to force a reset and restart it?",
                L"Application Already Running",
                MessageBoxButtons::YesNo,
                MessageBoxIcon::Question
            );

            if (result == System::Windows::Forms::DialogResult::Yes) {
                WriteDiagnosticLog(L"SingleInstance_ForceResetRequested", L"NoChange", true);

                // FORCE REVERT CONFIG ON FORCED CLOSED LOOP BREAK
                String^ ghostDeviceInstance = L"";
                LoadConfigState(ghostDeviceInstance);

                // Dynamically fetch live ID if config string was damaged
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
                    // Forcibly wake camera back up via absolute hardware call first
                    std::wstring nativeGhostId = msclr::interop::marshal_as<std::wstring>(ghostDeviceInstance);
                    RecoverCameraHardware(nativeGhostId, true);
                    // Keep monitoring enabled after a force-reset path so lock/unlock automation resumes.
                    SaveConfigState(true, ghostDeviceInstance);
                }

                system("taskkill /F /IM Windows_Hello_Fix_v2_0.exe /T");
                ::Sleep(500);
                Application::Restart();
            }

            Environment::Exit(0);
            return;
        }

        hWakeupEvent = CreateEvent(NULL, FALSE, FALSE, L"Global\\WindowsHelloFix_WakeupEvent");

        // Startup recovery must happen before building the dropdown, because a disabled device may not appear as present yet.
        // Use the stronger cycle path here so manual launch can recover a camera that was left disabled by a previous session.
        WriteDiagnosticLog(L"Startup_RestoreConfiguredCameraHardware", L"Enabled", true);
        RestoreConfiguredCameraHardware(true);

        // ====== REGISTER FOR MID-LEVEL HARDWARE INTERRUPTS ======
        HWND hWndNative = static_cast<HWND>(this->Handle.ToPointer());
        GUID lidGuid = GUID_LIDSWITCH_STATE_CHANGE;
        GUID buttonGuid = GUID_POWER_BUTTON_TIMESTAMP;

        hLidNotification = RegisterPowerSettingNotification(hWndNative, &lidGuid, DEVICE_NOTIFY_WINDOW_HANDLE);
        hButtonNotification = RegisterPowerSettingNotification(hWndNative, &buttonGuid, DEVICE_NOTIFY_WINDOW_HANDLE);

        auto* pCachedCameras = static_cast<std::vector<CameraDeviceInfo>*>(cachedCameras);
        auto* pSelectedInstanceId = static_cast<std::wstring*>(selectedInstanceId);
        bool startInBackground = launchRequestedBackground;
        if (startInBackground) {
            isBackgroundMode = true;
        }

        *pCachedCameras = ScanSystemCameras();
        int savedIdx = -1;
        int autoIdx = -1;

        String^ savedDeviceInstance = L"";
        bool shouldAutoStartByConfig = LoadConfigState(savedDeviceInstance);

        for (int i = 0; i < static_cast<int>(pCachedCameras->size()); i++) {
            String^ currentId = msclr::interop::marshal_as<String^>(pCachedCameras->at(i).instanceId);
            this->deviceDrop->Items->Add(msclr::interop::marshal_as<String^>(pCachedCameras->at(i).friendlyName));

            if (currentId == savedDeviceInstance) {
                savedIdx = i;
            }
            if (pCachedCameras->at(i).instanceId.find(L"MI_00") != std::wstring::npos) {
                autoIdx = i;
            }
        }

        if (savedIdx != -1) {
            this->deviceDrop->SelectedIndex = savedIdx;
        }
        else if (autoIdx != -1) {
            this->deviceDrop->SelectedIndex = autoIdx;
        }
        else if (this->deviceDrop->Items->Count > 0) {
            this->deviceDrop->SelectedIndex = 0;
        }

        if (this->deviceDrop->SelectedIndex != -1) {
            *pSelectedInstanceId = pCachedCameras->at(this->deviceDrop->SelectedIndex).instanceId;
            String^ selectedDeviceForConfig = msclr::interop::marshal_as<String^>(pCachedCameras->at(this->deviceDrop->SelectedIndex).instanceId);
            EnsureConfigFileExists(selectedDeviceForConfig);
        }
        else {
            EnsureConfigFileExists(L"");
        }

        // FIRST THING THE APP DOES WHEN RUNNING: Force Enable Camera Device Tree to Prevent Bricking Loops
        if (this->deviceDrop->SelectedIndex != -1) {
            EnableTargetCameraHardware(shouldAutoStartByConfig);
        }

        if ((startInBackground || shouldAutoStartByConfig) && this->deviceDrop->SelectedIndex != -1) {
            *pSelectedInstanceId = pCachedCameras->at(this->deviceDrop->SelectedIndex).instanceId;
            isMonitoring = true;

            // Keep startup stable: monitor events without forcing an immediate disable->enable bounce every launch.
            EnableTargetCameraHardware(false);

            this->deviceDrop->Enabled = false;
            this->btnToggle->Text = L"Stop Monitoring Service";
            this->lblStatus->Text = L"Status: Service Running";
            this->lblStatus->ForeColor = System::Drawing::Color::Green;

            if (startInBackground) {
                this->Visible = false;
                this->ShowInTaskbar = false;
                this->WindowState = FormWindowState::Minimized;
            }
        }
        else {
            isMonitoring = false;
            this->deviceDrop->Enabled = true;
            this->btnToggle->Text = L"Start Monitoring Service";
            this->lblStatus->Text = L"Status: Service Stopped";
            this->lblStatus->ForeColor = System::Drawing::Color::Gray;
        }

        backgroundWorker = gcnew System::Threading::Thread(gcnew System::Threading::ThreadStart(this, &MyForm::ListenForWakeupSignal));
        backgroundWorker->IsBackground = true;
        backgroundWorker->Start();

        // Session-change notifications can fail very early during logon. Retry briefly.
        bool sessionNotificationRegistered = false;
        for (int registrationAttempt = 0; registrationAttempt < 6; registrationAttempt++) {
            if (WTSRegisterSessionNotification(hWndNative, NOTIFY_FOR_THIS_SESSION)) {
                sessionNotificationRegistered = true;
                break;
            }
            ::Sleep(500);
        }

        if (sessionNotificationRegistered) {
            WriteDiagnosticLog(L"WTSRegisterSessionNotification_Success", L"NoChange", true);
        }
        else {
            DWORD lastError = GetLastError();
            WriteDiagnosticLog(
                String::Format(L"WTSRegisterSessionNotification_Failed_LastError={0}", static_cast<Int32>(lastError)),
                L"NoChange",
                false
            );
        }
    }

    void MyForm::ListenForWakeupSignal() {
        while (keepListening && hWakeupEvent != NULL) {
            DWORD waitResult = WaitForSingleObject(hWakeupEvent, INFINITE);
            if (waitResult == WAIT_OBJECT_0 && keepListening) {
                if (this->InvokeRequired) {
                    this->Invoke(gcnew MethodInvoker(this, &MyForm::BringWindowToFrontDelegate));
                }
                else {
                    BringWindowToFrontDelegate();
                }
            }
        }
    }

    void MyForm::BringWindowToFrontDelegate() {
        this->Show();
        this->Visible = true;
        this->ShowInTaskbar = true;
        this->WindowState = FormWindowState::Normal;
        this->BringToFront();
        this->Activate();
        this->Refresh();
    }

    System::Void MyForm::MyForm_FormClosing(System::Object^ sender, System::Windows::Forms::FormClosingEventArgs^ e) {
        if (e->CloseReason == CloseReason::UserClosing) {
            e->Cancel = true;
            this->Hide();
            this->ShowInTaskbar = false;

            if (!isBackgroundMode) {
                MessageBox::Show(
                    L"The program is running in the background. To close it completely, click 'Stop Monitoring Service' or kill 'Windows_Hello_Fix_v2_0.exe' in Task Manager.",
                    L"Background Service Active",
                    MessageBoxButtons::OK,
                    MessageBoxIcon::Information
                );
                isBackgroundMode = true;
            }
        }
        // If closing because Windows is shutting down, let destructor handle config reset
    }

    System::Void MyForm::btnToggle_Click(System::Object^ sender, System::EventArgs^ e) {
        auto* pCachedCameras = static_cast<std::vector<CameraDeviceInfo>*>(cachedCameras);
        auto* pSelectedInstanceId = static_cast<std::wstring*>(selectedInstanceId);

        if (this->deviceDrop->SelectedIndex == -1) {
            MessageBox::Show(L"Please select a camera device.", L"No Device Selected", MessageBoxButtons::OK, MessageBoxIcon::Warning);
            return;
        }

        if (!isMonitoring) {
            *pSelectedInstanceId = pCachedCameras->at(this->deviceDrop->SelectedIndex).instanceId;
            isMonitoring = true;
            this->deviceDrop->Enabled = false;
            this->btnToggle->Text = L"Stop Monitoring Service";
            this->lblStatus->Text = L"Status: Service Running";
            this->lblStatus->ForeColor = System::Drawing::Color::Green;
            SaveConfigState(true, msclr::interop::marshal_as<String^>(*pSelectedInstanceId));
        }
        else {
            isMonitoring = false;
            EnableTargetCameraHardware(false);

            SaveConfigState(false, msclr::interop::marshal_as<String^>(*pSelectedInstanceId));

            pSelectedInstanceId->clear();
            this->deviceDrop->Enabled = true;
            this->btnToggle->Text = L"Start Monitoring Service";
            this->lblStatus->Text = L"Status: Service Stopped";
            this->lblStatus->ForeColor = System::Drawing::Color::Gray;
        }
    }

    void MyForm::WndProc(System::Windows::Forms::Message% m) {
        static bool isAlreadyDisabled = false; // Inter-process hardware state lock tracker
        static ULONGLONG lastSessionEventTick = 0;
        static int lastSessionEventCode = -1;
        static ULONGLONG lastPowerEventTick = 0;
        static int lastPowerEventCode = -1;
        ULONGLONG nowTick = GetTickCount64();

        // 1. System Shutdown / Logoff
        if (m.Msg == 0x0016 || m.Msg == 0x0011) {
            isSystemEnding = true;
            WriteDiagnosticLog(L"SystemEnd_Begin", L"Disabled", true);
            if (isMonitoring) {
                bool shutdownDisableResult = DisableTargetCameraHardware(true);
                WriteDiagnosticLog(L"SystemEnd_Disable", L"Disabled", shutdownDisableResult);
            }
            WTSUnRegisterSessionNotification(static_cast<HWND>(this->Handle.ToPointer()));
        }

        // 2. Power Broadcast Events (Sleep / Resume / Hardware Notifications)
        else if (m.Msg == 0x0218) { // WM_POWERBROADCAST
            int powerEvent = m.WParam.ToInt32();

            if (lastPowerEventCode == powerEvent && (nowTick - lastPowerEventTick) < 1500) {
                WriteDiagnosticLog(L"PowerEvent_DedupIgnored", L"NoChange", true);
                Form::WndProc(m);
                return;
            }
            lastPowerEventCode = powerEvent;
            lastPowerEventTick = nowTick;

            // Trap Sleep Broadcast or Low-Level Power Intercept Events
            if (powerEvent == 0x0004 || powerEvent == 0x8013) {
                if (isMonitoring && !isAlreadyDisabled) {

                    if (powerEvent == 0x8013) {
                        POWERBROADCAST_SETTING* pSetting = reinterpret_cast<POWERBROADCAST_SETTING*>(m.LParam.ToPointer());
                        if (pSetting != nullptr) {
                            GUID lidGuid = GUID_LIDSWITCH_STATE_CHANGE;
                            GUID buttonGuid = GUID_POWER_BUTTON_TIMESTAMP;

                            // Lid close event or physical Power button action caught instantly!
                            if (!IsEqualGUID(pSetting->PowerSetting, lidGuid) && !IsEqualGUID(pSetting->PowerSetting, buttonGuid)) {
                                WriteDiagnosticLog(L"PowerSetting_IrrelevantGuid", L"NoChange", true);
                                Form::WndProc(m);
                                return;
                            }
                        }
                    }

                    // Enforce structural hardware state lock
                    isAlreadyDisabled = true;
                    bool powerDisableResult = DisableTargetCameraHardware(true);
                    WriteDiagnosticLog(L"PowerEvent_Disable", L"Disabled", powerDisableResult);

                    // CRITICAL TIME WINDOW BYPASS (500ms safety window)
                    ::Sleep(500);
                }
            }
            // System Waking Up (PBT_APMRESUMESUSPEND = 0x0007 or PBT_APMRESUMEAUTOMATIC = 0x0012)
            else if (powerEvent == 0x0007 || powerEvent == 0x0012) {
                if (isMonitoring) {
                    // Force a brief delay to allow systemic device trees to rebuild
                    System::Threading::Thread::Sleep(1000);
                    bool powerEnableResult = EnableTargetCameraHardware(false);
                    WriteDiagnosticLog(L"PowerEvent_Enable", L"Enabled", powerEnableResult);
                    isAlreadyDisabled = false; // Release lock on verified wake
                }
            }
        }

        // 3. Session Lock / Unlock Events
        else if (m.Msg == WM_WTSSESSION_CHANGE) {
            int sessionEvent = m.WParam.ToInt32();

            WriteDiagnosticLog(
                String::Format(L"SessionEvent_Received_Code={0}", sessionEvent),
                isMonitoring ? L"ActiveMonitoring" : L"MonitoringOff",
                true
            );

            if (lastSessionEventCode == sessionEvent && (nowTick - lastSessionEventTick) < 1500) {
                WriteDiagnosticLog(L"SessionEvent_DedupIgnored", L"NoChange", true);
                Form::WndProc(m);
                return;
            }
            lastSessionEventCode = sessionEvent;
            lastSessionEventTick = nowTick;

            if (!isMonitoring) {
                WriteDiagnosticLog(L"SessionEvent_Ignored_MonitoringOff", L"NoChange", true);
            }
            else if (sessionEvent == WTS_SESSION_LOCK) {
                bool lockDisableResult = DisableTargetCameraHardware(true);
                WriteDiagnosticLog(L"SessionLock_Disable", L"Disabled", lockDisableResult);
            }
            else if (sessionEvent == WTS_SESSION_UNLOCK) {
                bool unlockEnableResult = EnableTargetCameraHardware(false);
                WriteDiagnosticLog(L"SessionUnlock_Enable", L"Enabled", unlockEnableResult);
            }
        }

        Form::WndProc(m);
    }

} // end namespace Windows_Hello_Fix_v2_0



