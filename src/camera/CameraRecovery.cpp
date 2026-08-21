#include "CameraRecovery.h"
#include "CameraHardware.h"
#include "CameraDevice.h"

#include <windows.h>
#include <setupapi.h>
#include <cfgmgr32.h>

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
            return true;
        }

        ToggleCameraHardwareCfgMgr(targetId, enable);
        if (VerifyCameraHardwareState(targetId, shouldBeDisabled)) {
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
