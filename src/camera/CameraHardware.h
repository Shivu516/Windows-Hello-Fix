#pragma once

#include <string>
#include <windows.h>
#include <cfgmgr32.h>

bool ToggleCameraHardware(std::wstring targetId, bool enable);
bool LocateCameraDevInst(std::wstring targetId, DEVINST& devInst);
bool ToggleCameraHardwareCfgMgr(std::wstring targetId, bool enable);
bool GetCameraHardwareDisabledState(std::wstring targetId, bool& isDisabled);
bool VerifyCameraHardwareState(std::wstring targetId, bool shouldBeDisabled);
