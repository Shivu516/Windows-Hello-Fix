#include "Logging.h"

#include <ctime>
#include <sstream>
#include <iomanip>

std::wstring ProductionLogger::GetTimestamp() {
    auto now = std::time(nullptr);
    struct tm timeinfo;
    localtime_s(&timeinfo, &now);

    std::wostringstream oss;
    oss << std::put_time(&timeinfo, L"%Y-%m-%d %H:%M:%S");
    return oss.str();
}

void ProductionLogger::LogHardwareOperation(
    const std::wstring& operation,
    const std::wstring& deviceId,
    bool result,
    DWORD lastError,
    CONFIGRET configRet,
    ULONGLONG durationMs
) {
    std::wostringstream log;
    log << L"[" << GetTimestamp() << L"] ";
    log << L"OP=" << operation << L" | ";
    log << L"DEVICE=" << deviceId << L" | ";
    log << L"RESULT=" << (result ? L"SUCCESS" : L"FAILED") << L" | ";

    if (lastError != 0) {
        log << L"WIN_ERROR=0x" << std::hex << lastError << std::dec << L" | ";
    }
    if (configRet != CR_SUCCESS) {
        log << L"CONFIGRET=" << configRet << L" | ";
    }
    if (durationMs > 0) {
        log << L"DURATION=" << durationMs << L"ms | ";
    }
    log << L"PID=" << GetCurrentProcessId() << L" | ";
    log << L"TID=" << GetCurrentThreadId();

    OutputDebugStringW(log.str().c_str());
    OutputDebugStringW(L"\r\n");
}

void ProductionLogger::LogError(const std::wstring& context, const std::wstring& message) {
    std::wostringstream log;
    log << L"[" << GetTimestamp() << L"] ";
    log << L"ERROR | " << context << L" | " << message;
    OutputDebugStringW(log.str().c_str());
    OutputDebugStringW(L"\r\n");
}

void ProductionLogger::LogInfo(const std::wstring& context, const std::wstring& message) {
    std::wostringstream log;
    log << L"[" << GetTimestamp() << L"] ";
    log << L"INFO | " << context << L" | " << message;
    OutputDebugStringW(log.str().c_str());
    OutputDebugStringW(L"\r\n");
}