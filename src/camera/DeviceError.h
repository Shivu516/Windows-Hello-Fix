#pragma once

#include <windows.h>
#include <cfgmgr32.h>

extern volatile LONG g_lastSetupApiError;
extern volatile LONG g_lastConfigManagerResult;
extern volatile LONG g_lastHardwareToggleStage;

struct DeviceError {
    LONG setupErr;
    LONG configRet;
    LONG stage;
};
