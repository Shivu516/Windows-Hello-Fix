#pragma once

#include <windows.h>
#include <cfgmgr32.h>
#include <string>

class ProductionLogger {
public:
    static void LogHardwareOperation(
        const std::wstring& operation,
        const std::wstring& deviceId,
        bool result,
        DWORD lastError,
        CONFIGRET configRet = CR_SUCCESS,
        ULONGLONG durationMs = 0
    );

    static void LogError(const std::wstring& context, const std::wstring& message);

    static void LogInfo(const std::wstring& context, const std::wstring& message);

private:
    static std::wstring GetTimestamp();
};