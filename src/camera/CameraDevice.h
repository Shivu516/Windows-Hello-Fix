#pragma once

#include <string>
#include <vector>
#include <windows.h>

struct CameraDeviceInfo {
    std::wstring friendlyName;
    std::wstring instanceId;
};

std::vector<CameraDeviceInfo> ScanSystemCameras();
