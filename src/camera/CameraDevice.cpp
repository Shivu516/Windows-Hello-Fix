#include "CameraDevice.h"
#include "DeviceError.h"

#include <windows.h>
#include <setupapi.h>
#include <cfgmgr32.h>
#include <devguid.h>

#pragma comment(lib, "setupapi.lib")

volatile LONG g_lastSetupApiError = ERROR_SUCCESS;
volatile LONG g_lastConfigManagerResult = CR_SUCCESS;
volatile LONG g_lastHardwareToggleStage = 0;

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
