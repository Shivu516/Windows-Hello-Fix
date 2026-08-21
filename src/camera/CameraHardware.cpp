#include "CameraHardware.h"
#include "DeviceError.h"
#include "CameraDevice.h"

#include <windows.h>
#include <setupapi.h>
#include <cfgmgr32.h>

#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "cfgmgr32.lib")

#ifndef CONFIGFLAG_DISABLED
#define CONFIGFLAG_DISABLED 0x00000001
#endif

#ifndef CM_PROB_DISABLED
#define CM_PROB_DISABLED 22
#endif

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
