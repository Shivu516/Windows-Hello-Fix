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
volatile LONG g_lastAttemptCount = 0;
volatile LONG g_lastSuccessPath = 0;
volatile LONG g_lastPropChangeFlags = 0;
volatile LONG g_lastServiceState = 0;
volatile LONG g_lastServiceError = ERROR_SUCCESS;

// ====== NATIVE FUNCTION IMPLEMENTATIONS ======

std::wstring TrimTrailingChars(const std::wstring& str) {
    std::wstring sanitized = str;
    // Remove trailing carriage returns, newlines, or trailing spaces
    while (!sanitized.empty() && (sanitized.back() == L'\r' || sanitized.back() == L'\n' || sanitized.back() == L' ')) {
        sanitized.pop_back();
    }
    return sanitized;
}

// Human-readable text for a Win32 error code (log-only; never affects control flow).
// Returns L"Success" for ERROR_SUCCESS, L"Unknown error <n>" when FormatMessage
// has no string (e.g. CONFIGRET codes, which are not Win32 errors).
std::wstring GetLastWin32ErrorText(DWORD err) {
    if (err == ERROR_SUCCESS) {
        return L"Success";
    }
    LPWSTR buf = NULL;
    DWORD n = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        NULL, err, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), (LPWSTR)&buf, 0, NULL);
    std::wstring text;
    if (n > 0 && buf != NULL) {
        text = TrimTrailingChars(std::wstring(buf, n));
    }
    if (buf != NULL) {
        LocalFree(buf);
    }
    if (text.empty()) {
        wchar_t fallback[64];
        swprintf_s(fallback, L"Unknown error %lu", err);
        text = fallback;
    }
    return text;
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

    // Attribution for the managed Result log (log-only; no control-flow effect).
    // Attempt = 1-based loop iteration; Path: 1 SetupAPI, 2 CfgMgr32, 3 final pass.
    InterlockedExchange(&g_lastAttemptCount, 0);
    InterlockedExchange(&g_lastSuccessPath, 0);

    // Check-before-change: if already in target state, skip hardware command churn.
    if (VerifyCameraHardwareState(targetId, shouldBeDisabled)) {
        return true;
    }

    for (int attempt = 0; attempt < 3; attempt++) {
        InterlockedExchange(&g_lastAttemptCount, attempt + 1);
        ToggleCameraHardware(targetId, enable);

        if (VerifyCameraHardwareState(targetId, shouldBeDisabled)) {
            InterlockedExchange(&g_lastSuccessPath, 1);
            RecordHardwareToggleTime();
            return true;
        }

        ToggleCameraHardwareCfgMgr(targetId, enable);
        if (VerifyCameraHardwareState(targetId, shouldBeDisabled)) {
            InterlockedExchange(&g_lastSuccessPath, 2);
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
        InterlockedExchange(&g_lastSuccessPath, 3);
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

// ====== ISSUE #1 SAME-SESSION RECOVERY PRIMITIVES ======
// Additive operations for src/watchdog/CameraRecoveryFailsafe (Issue #1: the camera is
// PnP-enabled but the Media Foundation camera stack is unusable — e.g. MF_E_REBOOT_REQUIRED,
// Camera app 0xA00F4241(0xC00D7167)). Each primitive is a distinct documented mechanism;
// none duplicates the enable/disable toggle paths above. The watchdog decides WHEN these
// run; this file remains the single authority for HOW device/system state changes.
// Stage codes: 30-36 devnode restart, 40-50 Frame Server service, 60-65 parent
// re-enumeration (existing 10-15 SetupAPI toggle / 20-23 CfgMgr toggle unchanged).

// Rung 1 — documented device restart (what devcon restart / pnputil /restart-device do):
// DIF_PROPERTYCHANGE with DICS_PROPCHANGE stops and restarts the devnode's driver stack in
// place WITHOUT changing its enabled/disabled state — intentionally not a disable/enable
// toggle. DI_NEEDRESTART/DI_NEEDREBOOT in the resulting install params mean Windows
// deferred the restart (typically because handles are open); that is surfaced through
// g_lastPropChangeFlags instead of being reported as blind success.
bool RestartCameraHardware(std::wstring targetId) {
    InterlockedExchange(&g_lastSetupApiError, ERROR_SUCCESS);
    InterlockedExchange(&g_lastPropChangeFlags, 0);
    InterlockedExchange(&g_lastHardwareToggleStage, 30);

    if (targetId.empty()) {
        InterlockedExchange(&g_lastHardwareToggleStage, 31);
        return false;
    }

    HDEVINFO hDevInfo = SetupDiGetClassDevs(NULL, NULL, NULL, DIGCF_ALLCLASSES);

    if (hDevInfo == INVALID_HANDLE_VALUE) {
        InterlockedExchange(&g_lastSetupApiError, static_cast<LONG>(GetLastError()));
        InterlockedExchange(&g_lastHardwareToggleStage, 32);
        return false;
    }

    SP_DEVINFO_DATA devInfoData;
    devInfoData.cbSize = sizeof(SP_DEVINFO_DATA);
    bool restarted = false;

    for (int i = 0; SetupDiEnumDeviceInfo(hDevInfo, i, &devInfoData); i++) {
        WCHAR instancePath[MAX_DEVICE_ID_LEN];

        if (SetupDiGetDeviceInstanceId(hDevInfo, &devInfoData, instancePath, MAX_DEVICE_ID_LEN, NULL)) {
            if (targetId == instancePath || _wcsicmp(targetId.c_str(), instancePath) == 0) {

                SP_PROPCHANGE_PARAMS params;
                ZeroMemory(&params, sizeof(SP_PROPCHANGE_PARAMS));

                params.ClassInstallHeader.cbSize = sizeof(SP_CLASSINSTALL_HEADER);
                params.ClassInstallHeader.InstallFunction = DIF_PROPERTYCHANGE;
                params.StateChange = DICS_PROPCHANGE;
                // Scope is ignored for DICS_PROPCHANGE per the SetupAPI docs; CONFIGSPECIFIC
                // matches the devcon restart implementation.
                params.Scope = DICS_FLAG_CONFIGSPECIFIC;
                params.HwProfile = 0;

                if (!SetupDiSetClassInstallParams(hDevInfo, &devInfoData, &params.ClassInstallHeader, sizeof(params))) {
                    InterlockedExchange(&g_lastSetupApiError, static_cast<LONG>(GetLastError()));
                    InterlockedExchange(&g_lastHardwareToggleStage, 33);
                    break;
                }

                if (!SetupDiCallClassInstaller(DIF_PROPERTYCHANGE, hDevInfo, &devInfoData)) {
                    InterlockedExchange(&g_lastSetupApiError, static_cast<LONG>(GetLastError()));
                    InterlockedExchange(&g_lastHardwareToggleStage, 34);
                    break;
                }

                SP_DEVINSTALL_PARAMS devParams;
                ZeroMemory(&devParams, sizeof(devParams));
                devParams.cbSize = sizeof(devParams);
                if (SetupDiGetDeviceInstallParams(hDevInfo, &devInfoData, &devParams)) {
                    InterlockedExchange(&g_lastPropChangeFlags,
                        static_cast<LONG>(devParams.Flags & (DI_NEEDRESTART | DI_NEEDREBOOT)));
                }

                InterlockedExchange(&g_lastSetupApiError, ERROR_SUCCESS);
                InterlockedExchange(&g_lastHardwareToggleStage, 36);
                restarted = true;
                break;
            }
        }
    }
    SetupDiDestroyDeviceInfoList(hDevInfo);
    if (!restarted && InterlockedCompareExchange(&g_lastHardwareToggleStage, 0, 0) == 30) {
        InterlockedExchange(&g_lastHardwareToggleStage, 35); // target not found
    }
    return restarted;
}

// Bounded wait for a service to reach a desired state (never hangs; 250 ms cadence).
static bool WaitForServiceState(SC_HANDLE hSvc, DWORD desiredState, ULONG timeoutMs, DWORD& lastError) {
    int waitedMs = 0;
    while (waitedMs <= static_cast<int>(timeoutMs)) {
        SERVICE_STATUS status;
        if (!QueryServiceStatus(hSvc, &status)) {
            lastError = GetLastError();
            return false;
        }
        InterlockedExchange(&g_lastServiceState, static_cast<LONG>(status.dwCurrentState));
        if (status.dwCurrentState == desiredState) {
            return true;
        }
        if (desiredState == SERVICE_RUNNING && status.dwCurrentState == SERVICE_STOPPED) {
            // Start already failed back into stopped state — no point waiting further.
            return false;
        }
        ::Sleep(250);
        waitedMs += 250;
    }
    lastError = ERROR_TIMEOUT;
    return false;
}

// Rung 2 — bounded restart of the Windows Camera Frame Server service ("FrameServer").
// This resets the shared camera software stack where the MF_E_REBOOT_REQUIRED state is
// believed to live. It is stack-wide by nature (every camera client on the system
// momentarily loses its session), so it is only invoked after rung 1 failed. Never
// touches any other service; never implemented as an sc.exe/net.exe subprocess.
bool RestartCameraFrameServerService() {
    InterlockedExchange(&g_lastServiceError, ERROR_SUCCESS);
    InterlockedExchange(&g_lastServiceState, 0);
    InterlockedExchange(&g_lastHardwareToggleStage, 40);

    SC_HANDLE hScm = NULL;
    SC_HANDLE hSvc = NULL;
    DWORD err = ERROR_SUCCESS;
    bool ok = false;

    do {
        hScm = OpenSCManagerW(NULL, NULL, SC_MANAGER_CONNECT);
        if (hScm == NULL) {
            err = GetLastError();
            InterlockedExchange(&g_lastHardwareToggleStage, 41);
            break;
        }

        // Absence (ERROR_SERVICE_DOES_NOT_EXIST) is reported, not treated as a defect:
        // the service is not guaranteed on every Windows edition/version.
        hSvc = OpenServiceW(hScm, L"FrameServer", SERVICE_QUERY_STATUS | SERVICE_START | SERVICE_STOP);
        if (hSvc == NULL) {
            err = GetLastError();
            InterlockedExchange(&g_lastHardwareToggleStage, 42);
            break;
        }

        SERVICE_STATUS status;
        if (!QueryServiceStatus(hSvc, &status)) {
            err = GetLastError();
            InterlockedExchange(&g_lastHardwareToggleStage, 43);
            break;
        }
        InterlockedExchange(&g_lastServiceState, static_cast<LONG>(status.dwCurrentState));

        // Stop (skip when already stopped; tolerate service-not-active races).
        if (status.dwCurrentState != SERVICE_STOPPED) {
            if (!ControlService(hSvc, SERVICE_CONTROL_STOP, &status)) {
                DWORD stopErr = GetLastError();
                if (stopErr != ERROR_SERVICE_NOT_ACTIVE) {
                    err = stopErr;
                    InterlockedExchange(&g_lastHardwareToggleStage, 44);
                    break;
                }
            }
            if (!WaitForServiceState(hSvc, SERVICE_STOPPED, 10000, err)) {
                InterlockedExchange(&g_lastHardwareToggleStage, 46); // stop transition failed/timed out
                break;
            }
        }

        if (!StartServiceW(hSvc, 0, NULL)) {
            DWORD startErr = GetLastError();
            if (startErr != ERROR_SERVICE_ALREADY_RUNNING) {
                err = startErr;
                InterlockedExchange(&g_lastHardwareToggleStage, 47);
                break;
            }
        }

        if (!WaitForServiceState(hSvc, SERVICE_RUNNING, 10000, err)) {
            InterlockedExchange(&g_lastHardwareToggleStage, 49); // start transition failed/timed out
            break;
        }

        InterlockedExchange(&g_lastHardwareToggleStage, 50);
        ok = true;
    } while (0);

    InterlockedExchange(&g_lastServiceError, static_cast<LONG>(err));

    if (hSvc != NULL) CloseServiceHandle(hSvc);
    if (hScm != NULL) CloseServiceHandle(hScm);
    return ok;
}

// Read-only: "instanceId=status/problem" summary of a parent's children — the target and
// its sibling interfaces (e.g. the IR camera). Used to evidence rung 4's sibling impact.
static std::wstring BuildChildStatusSummary(DEVINST parentInst) {
    std::wstring summary;
    DEVINST child = 0;
    if (CM_Get_Child(&child, parentInst, 0) != CR_SUCCESS) {
        return L"none";
    }
    while (true) {
        WCHAR idBuf[MAX_DEVICE_ID_LEN] = { 0 };
        std::wstring id = L"?";
        if (CM_Get_Device_IDW(child, idBuf, MAX_DEVICE_ID_LEN, 0) == CR_SUCCESS) {
            id = idBuf;
        }
        ULONG status = 0;
        ULONG problem = 0;
        std::wstring stateText = L"?";
        if (CM_Get_DevNode_Status(&status, &problem, child, 0) == CR_SUCCESS) {
            WCHAR stateBuf[64];
            swprintf_s(stateBuf, L"0x%08X/%lu", status, problem);
            stateText = stateBuf;
        }
        if (!summary.empty()) {
            summary += L"; ";
        }
        summary += id + L"=" + stateText;
        DEVINST next = 0;
        if (CM_Get_Sibling(&next, child, 0) != CR_SUCCESS) {
            break;
        }
        child = next;
    }
    return summary;
}

// Rung 4 — re-enumerate the composite parent of a multi-interface (MI_*) camera child.
// This targets the correct tree level for genuine re-detection: CM_Reenumerate_DevNode
// re-enumerates a node's children, and the previous call in ToggleCameraHardwareCfgMgr
// aims it at the child leaf itself (a no-op). Refuses non-composite targets — their
// parent is a USB hub and re-enumerating it would disturb every device on that hub.
// Expected side effect: sibling interfaces of the same composite device (e.g. the IR
// camera) are re-detected too, so the caller must only reach this rung while the camera
// is expected-enabled; the returned before/after sibling summary evidences that impact.
bool ReenumerateCameraParent(std::wstring targetId, std::wstring& parentId, std::wstring& siblingStatusReport) {
    parentId.clear();
    siblingStatusReport.clear();
    InterlockedExchange(&g_lastConfigManagerResult, CR_SUCCESS);
    InterlockedExchange(&g_lastHardwareToggleStage, 60);

    if (targetId.empty() || targetId.find(L"&MI_") == std::wstring::npos) {
        InterlockedExchange(&g_lastHardwareToggleStage, 64); // not a composite child — refused
        return false;
    }

    DEVINST childInst = 0;
    if (!LocateCameraDevInst(targetId, childInst)) {
        InterlockedExchange(&g_lastHardwareToggleStage, 61);
        return false;
    }

    DEVINST parentInst = 0;
    CONFIGRET cr = CM_Get_Parent(&parentInst, childInst, 0);
    InterlockedExchange(&g_lastConfigManagerResult, static_cast<LONG>(cr));
    if (cr != CR_SUCCESS) {
        InterlockedExchange(&g_lastHardwareToggleStage, 62);
        return false;
    }

    WCHAR parentIdBuf[MAX_DEVICE_ID_LEN] = { 0 };
    if (CM_Get_Device_IDW(parentInst, parentIdBuf, MAX_DEVICE_ID_LEN, 0) == CR_SUCCESS) {
        parentId = parentIdBuf;
    }

    std::wstring before = BuildChildStatusSummary(parentInst);

    cr = CM_Reenumerate_DevNode(parentInst, CM_REENUMERATE_NORMAL);
    InterlockedExchange(&g_lastConfigManagerResult, static_cast<LONG>(cr));
    if (cr != CR_SUCCESS) {
        InterlockedExchange(&g_lastHardwareToggleStage, 63);
        return false;
    }

    // Synchronous completion does not guarantee children restarted — bounded settle
    // window before the after-side sibling snapshot.
    ::Sleep(1000);
    std::wstring after = BuildChildStatusSummary(parentInst);
    siblingStatusReport = L"Before{" + before + L"} After{" + after + L"}";

    InterlockedExchange(&g_lastHardwareToggleStage, 65);
    return true;
}

// ====== MyForm member camera operations (originally inline in header) ======

namespace Windows_Hello_Fix_v2_0 {

    static String^ SuccessPathText(LONG pathCode) {
        if (pathCode == 1) return L"SetupAPI";
        if (pathCode == 2) return L"CfgMgr";
        if (pathCode == 3) return L"Final";
        return L"None";
    }

    bool MyForm::DisableTargetCameraHardware(bool retryOnFailure)
    {
        return DisableTargetCameraHardware(retryOnFailure, nullptr);
    }

    bool MyForm::DisableTargetCameraHardware(bool retryOnFailure, String^ opId)
    {
        ULONGLONG opStart = GetTickCount64();
        std::wstring targetId;
        if (!TryGetTargetCameraInstanceId(targetId, true)) {
            WriteDiagnosticLogEx(DiagLevel::Error, L"CAMERA", L"DisableTargetCameraHardware_NoTarget", opId,
                String::Format(L"DurationMs={0}", (int)(GetTickCount64() - opStart)), L"Disabled", false);
            return false;
        }

        lastToggleTime = System::DateTime::Now;

        bool alreadyDisabled = false;
        if (GetCameraHardwareDisabledState(targetId, alreadyDisabled) && alreadyDisabled) {
            cameraExpectedDisabled = true;
            WriteDiagnosticLogExWithDevice(DiagLevel::Info, L"CAMERA", L"DisableTargetCameraHardware_AlreadyDisabled", opId,
                L"Path=Already", targetId, L"Disabled", true);
            return true;
        }

        bool result = SetCameraHardwareStateVerified(targetId, false, retryOnFailure);
        bool verified = VerifyCameraHardwareState(targetId, true);
        cameraExpectedDisabled = result;
        LONG setupErr = InterlockedCompareExchange(&g_lastSetupApiError, 0, 0);
        bool passed = result && verified;
        WriteDiagnosticLogExWithDevice(passed ? DiagLevel::Info : DiagLevel::Error, L"CAMERA",
            L"DisableTargetCameraHardware_Result", opId,
            String::Format(
                L"DurationMs={0} | Attempt={1} | Path={2} | Elevated={3} | IntegrityRid={4} | SetupErr={5} | CfgMgr={6} | Stage={7} | ErrText={8}",
                (int)(GetTickCount64() - opStart),
                static_cast<Int32>(InterlockedCompareExchange(&g_lastAttemptCount, 0, 0)),
                SuccessPathText(InterlockedCompareExchange(&g_lastSuccessPath, 0, 0)),
                IsCurrentProcessElevatedNative() ? L"1" : L"0",
                static_cast<Int32>(GetCurrentProcessIntegrityRid()),
                static_cast<Int32>(setupErr),
                static_cast<Int32>(InterlockedCompareExchange(&g_lastConfigManagerResult, 0, 0)),
                static_cast<Int32>(InterlockedCompareExchange(&g_lastHardwareToggleStage, 0, 0)),
                msclr::interop::marshal_as<String^>(GetLastWin32ErrorText(static_cast<DWORD>(setupErr)))
            ),
            targetId,
            L"Disabled",
            passed
        );
        return passed;
    }

    bool MyForm::EnableTargetCameraHardware(bool cycleDevice)
    {
        return EnableTargetCameraHardware(cycleDevice, nullptr);
    }

    bool MyForm::EnableTargetCameraHardware(bool cycleDevice, String^ opId)
    {
        ULONGLONG opStart = GetTickCount64();
        std::wstring targetId;
        if (!TryGetTargetCameraInstanceId(targetId, true)) {
            WriteDiagnosticLogEx(DiagLevel::Error, L"CAMERA", L"EnableTargetCameraHardware_NoTarget", opId,
                String::Format(L"DurationMs={0}", (int)(GetTickCount64() - opStart)), L"Enabled", false);
            return false;
        }

        lastToggleTime = System::DateTime::Now;

        bool disabledNow = false;
        if (GetCameraHardwareDisabledState(targetId, disabledNow) && !disabledNow) {
            cameraExpectedDisabled = false;
            WriteDiagnosticLogExWithDevice(DiagLevel::Info, L"CAMERA", L"EnableTargetCameraHardware_AlreadyEnabled", opId,
                L"Path=Already", targetId, L"Enabled", true);
            return true;
        }

        bool result = RecoverCameraHardware(targetId, cycleDevice);
        bool verified = VerifyCameraHardwareState(targetId, false);
        cameraExpectedDisabled = !result;
        LONG setupErr = InterlockedCompareExchange(&g_lastSetupApiError, 0, 0);
        bool passed = result && verified;
        WriteDiagnosticLogExWithDevice(passed ? DiagLevel::Info : DiagLevel::Error, L"CAMERA",
            L"EnableTargetCameraHardware_Result", opId,
            String::Format(
                L"DurationMs={0} | Attempt={1} | Path={2} | Elevated={3} | IntegrityRid={4} | SetupErr={5} | CfgMgr={6} | Stage={7} | ErrText={8}",
                (int)(GetTickCount64() - opStart),
                static_cast<Int32>(InterlockedCompareExchange(&g_lastAttemptCount, 0, 0)),
                SuccessPathText(InterlockedCompareExchange(&g_lastSuccessPath, 0, 0)),
                IsCurrentProcessElevatedNative() ? L"1" : L"0",
                static_cast<Int32>(GetCurrentProcessIntegrityRid()),
                static_cast<Int32>(setupErr),
                static_cast<Int32>(InterlockedCompareExchange(&g_lastConfigManagerResult, 0, 0)),
                static_cast<Int32>(InterlockedCompareExchange(&g_lastHardwareToggleStage, 0, 0)),
                msclr::interop::marshal_as<String^>(GetLastWin32ErrorText(static_cast<DWORD>(setupErr)))
            ),
            targetId,
            L"Enabled",
            passed
        );
        return passed;
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
