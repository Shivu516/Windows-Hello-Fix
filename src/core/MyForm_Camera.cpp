#include "MyForm.h"

using namespace System;
using namespace System::Windows::Forms;

// Global hardware toggle state — single definition, shared across all TUs.
// Previously `static` in the monolithic header (single TU); now `extern` in header,
// defined once here to preserve single authoritative instance.
volatile LONG64 g_lastHardwareToggleTick = 0;
volatile LONG g_lastSetupApiError = ERROR_SUCCESS;
volatile LONG g_lastConfigManagerResult = CR_SUCCESS;
volatile LONG g_lastHardwareToggleStage = 0;

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
    // NOTE: uses _InterlockedCompareExchange64 (compiler intrinsic from <intrin.h>)
    // rather than InterlockedCompareExchange64 (Win32 API alias). The SDK's x86
    // branch excludes the API alias under /clr (winnt.h guards it with
    // !defined(_MANAGED)), while the intrinsic is available on x86, x64 and
    // ARM64 alike. Same atomic CAS semantics on all architectures; x64 already
    // compiled to this intrinsic (baseline C4793).
    for (int spin = 0; spin < 8; spin++) {
        LONG64 lastTick = _InterlockedCompareExchange64(&g_lastHardwareToggleTick, 0, 0);
        ULONGLONG nowTick = GetTickCount64();

        if (lastTick != 0) {
            ULONGLONG elapsed = nowTick - static_cast<ULONGLONG>(lastTick);
            if (elapsed < cooldownMs) {
                return false;
            }
        }

        LONG64 previous = _InterlockedCompareExchange64(&g_lastHardwareToggleTick, static_cast<LONG64>(nowTick), lastTick);
        if (previous == lastTick) {
            return true;
        }
    }

    return false;
}

void RecordHardwareToggleTime() {
    // Atomic 64-bit stamp via CAS loop. _InterlockedExchange64 is not exposed to
    // managed (/clr) x86 code, so exchange is expressed with the available
    // _InterlockedCompareExchange64 intrinsic. Observably identical to an atomic
    // exchange here (return value unused): the tick ends up stamped exactly once.
    LONG64 newTick = static_cast<LONG64>(GetTickCount64());
    LONG64 observed = _InterlockedCompareExchange64(&g_lastHardwareToggleTick, 0, 0);
    while (_InterlockedCompareExchange64(&g_lastHardwareToggleTick, newTick, observed) != observed) {
        observed = _InterlockedCompareExchange64(&g_lastHardwareToggleTick, 0, 0);
    }
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

// ====== MyForm member camera operations (originally inline in header) ======

namespace Windows_Hello_Fix_v2_0 {

    bool MyForm::DisableTargetCameraHardware(bool retryOnFailure)
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

    bool MyForm::EnableTargetCameraHardware(bool cycleDevice)
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

}
