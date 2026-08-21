#pragma once

#include <string>
#include <windows.h>

bool SetCameraHardwareStateVerified(std::wstring targetId, bool enable, bool reinitializeOnMismatch);
bool RecoverCameraHardware(std::wstring targetId, bool cycleDevice);
void RestoreAllCameraHardware(bool cycleDevices);
