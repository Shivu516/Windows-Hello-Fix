#pragma once

/**
 * PRODUCTION UTILITIES HEADER
 * Provides:
 * - Thread-safe background operations (no WndProc blocking)
 * - Safe single-instance handling without Global namespace
 * - Enhanced error logging for all hardware operations
 * - Asynchronous hardware operations
 */

#include <string>
#include <queue>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <ctime>
#include <sstream>
#include <iomanip>

// ============================================================
// ENHANCED LOGGING SYSTEM
// ============================================================

class ProductionLogger {
private:
    static std::wstring GetTimestamp() {
        auto now = std::time(nullptr);
        struct tm timeinfo;
        localtime_s(&timeinfo, &now);

        std::wostringstream oss;
        oss << std::put_time(&timeinfo, L"%Y-%m-%d %H:%M:%S");
        return oss.str();
    }

public:
    static void LogHardwareOperation(
        const std::wstring& operation,
        const std::wstring& deviceId,
        bool result,
        DWORD lastError,
        CONFIGRET configRet = CR_SUCCESS,
        ULONGLONG durationMs = 0
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

    static void LogError(const std::wstring& context, const std::wstring& message) {
        std::wostringstream log;
        log << L"[" << GetTimestamp() << L"] ";
        log << L"ERROR | " << context << L" | " << message;
        OutputDebugStringW(log.str().c_str());
        OutputDebugStringW(L"\r\n");
    }

    static void LogInfo(const std::wstring& context, const std::wstring& message) {
        std::wostringstream log;
        log << L"[" << GetTimestamp() << L"] ";
        log << L"INFO | " << context << L" | " << message;
        OutputDebugStringW(log.str().c_str());
        OutputDebugStringW(L"\r\n");
    }
};

// ============================================================
// ASYNC HARDWARE OPERATION QUEUE
// ============================================================

struct HardwareOperation {
    enum OpType { DISABLE, ENABLE, VERIFY, RECOVER };

    OpType type;
    std::wstring deviceId;
    bool reinitializeOnMismatch;
    bool cycleDevice;
    ULONGLONG startTime;
};

class HardwareOperationQueue {
private:
    std::queue<HardwareOperation> operations;
    std::mutex queueMutex;
    std::condition_variable queueCV;
    volatile bool isShuttingDown = false;
    std::thread workerThread;

public:
    HardwareOperationQueue() {
        workerThread = std::thread(&HardwareOperationQueue::ProcessOperations, this);
    }

    ~HardwareOperationQueue() {
        Shutdown();
        if (workerThread.joinable()) {
            workerThread.join();
        }
    }

    void Shutdown() {
        std::unique_lock<std::mutex> lock(queueMutex);
        isShuttingDown = true;
        queueCV.notify_all();
    }

    void Enqueue(const HardwareOperation& op) {
        std::unique_lock<std::mutex> lock(queueMutex);
        operations.push(op);
        queueCV.notify_one();
    }

    bool IsProcessing() {
        std::unique_lock<std::mutex> lock(queueMutex);
        return !operations.empty();
    }

    // IMPORTANT: This function is defined in MyForm.h as a native function
    // Forward declaration here; implementation ties to native hardware functions
    void ProcessOperations();
};

// ============================================================
// SAFE SINGLE-INSTANCE HANDLER (No Global Namespace)
// ============================================================

class SingleInstanceManager {
private:
    HANDLE hMutex;
    static constexpr const wchar_t* MUTEX_NAME = L"Local\\WindowsHelloFix_AppMutex_v15";

public:
    SingleInstanceManager() : hMutex(NULL) {}

    ~SingleInstanceManager() {
        Release();
    }

    bool TryAcquire() {
        // Use Local\ namespace instead of Global\ to avoid privilege escalation issues
        // Local\ is per-session and doesn't require SeCreateGlobalObjectsPrivilege
        hMutex = CreateMutexW(NULL, TRUE, MUTEX_NAME);

        if (hMutex == NULL) {
            ProductionLogger::LogError(L"SingleInstance", L"CreateMutex failed");
            return false;
        }

        DWORD lastError = GetLastError();
        if (lastError == ERROR_ALREADY_EXISTS) {
            CloseHandle(hMutex);
            hMutex = NULL;
            ProductionLogger::LogInfo(L"SingleInstance", L"Another instance already running");
            return false;
        }

        ProductionLogger::LogInfo(L"SingleInstance", L"Mutex acquired successfully");
        return true;
    }

    void Release() {
        if (hMutex != NULL) {
            CloseHandle(hMutex);
            hMutex = NULL;
        }
    }

    bool IsAcquired() const {
        return hMutex != NULL;
    }
};

// ============================================================
// ELEVATION VERIFICATION
// ============================================================

class ElevationChecker {
public:
    static bool IsRunningElevated() {
        BOOL isElevated = FALSE;
        HANDLE hToken = NULL;

        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &hToken)) {
            return false;
        }

        TOKEN_ELEVATION elevation;
        DWORD size = sizeof(TOKEN_ELEVATION);

        if (GetTokenInformation(hToken, TokenElevation, &elevation, sizeof(elevation), &size)) {
            isElevated = elevation.TokenIsElevated;
        }

        CloseHandle(hToken);
        return isElevated != FALSE;
    }

    static bool RequireElevation() {
        if (IsRunningElevated()) {
            ProductionLogger::LogInfo(L"Elevation", L"Running with administrator privileges");
            return true;
        }

        ProductionLogger::LogError(L"Elevation", L"NOT running elevated - hardware operations will fail!");
        return false;
    }
};

// ============================================================
// ENHANCED SETUPAPI WRAPPER
// ============================================================

class EnhancedSetupAPI {
public:
    static HDEVINFO GetCameraDevices() {
        // Use DIGCF_PRESENT to only get active devices
        // This prevents ghost/disabled devices from being enumerated
        HDEVINFO hDevInfo = SetupDiGetClassDevs(
            NULL,
            NULL,
            NULL,
            DIGCF_ALLCLASSES | DIGCF_PRESENT  // DIGCF_PRESENT is critical!
        );

        if (hDevInfo == INVALID_HANDLE_VALUE) {
            DWORD error = GetLastError();
            ProductionLogger::LogError(
                L"SetupAPI",
                std::wstring(L"SetupDiGetClassDevs failed: 0x") + 
                std::to_wstring(error)
            );
        }

        return hDevInfo;
    }

    static bool DisableDeviceWithFallback(
        HDEVINFO hDevInfo,
        SP_DEVINFO_DATA& devInfoData,
        std::wstring deviceId
    ) {
        // STRATEGY: Try SetupAPI first, fall back to CFGMGR32 if needed

        // Attempt 1: SetupAPI (preferred)
        SP_PROPCHANGE_PARAMS params = {};
        params.ClassInstallHeader.cbSize = sizeof(SP_CLASSINSTALL_HEADER);
        params.ClassInstallHeader.InstallFunction = DIF_PROPERTYCHANGE;
        params.Scope = DICS_FLAG_GLOBAL;
        params.StateChange = DICS_DISABLE;
        params.HwProfile = 0;

        ULONGLONG startTime = GetTickCount64();

        if (SetupDiSetClassInstallParams(hDevInfo, &devInfoData, 
                                        &params.ClassInstallHeader, sizeof(params))) {
            if (SetupDiCallClassInstaller(DIF_PROPERTYCHANGE, hDevInfo, &devInfoData)) {
                ULONGLONG duration = GetTickCount64() - startTime;
                ProductionLogger::LogHardwareOperation(
                    L"SetupAPI_Disable",
                    deviceId,
                    true,
                    0,
                    CR_SUCCESS,
                    duration
                );
                return true;
            }
        }

        DWORD setupApiError = GetLastError();
        ProductionLogger::LogHardwareOperation(
            L"SetupAPI_Disable",
            deviceId,
            false,
            setupApiError,
            CR_SUCCESS,
            GetTickCount64() - startTime
        );

        // Attempt 2: CFGMGR32 fallback
        DEVINST devInst = 0;
        CONFIGRET cr = CM_Locate_DevNodeW(&devInst, (PWSTR)deviceId.c_str(), 
                                          CM_LOCATE_DEVNODE_NORMAL);

        if (cr == CR_SUCCESS) {
            cr = CM_Disable_DevNode(devInst, CM_DISABLE_UI_NOT_OK);

            ULONGLONG duration = GetTickCount64() - startTime;
            ProductionLogger::LogHardwareOperation(
                L"CFGMGR32_Disable",
                deviceId,
                cr == CR_SUCCESS,
                0,
                cr,
                duration
            );

            if (cr == CR_SUCCESS) {
                CM_Reenumerate_DevNode(devInst, 0);
                return true;
            }
        } else {
            ProductionLogger::LogHardwareOperation(
                L"CFGMGR32_Locate",
                deviceId,
                false,
                0,
                cr,
                0
            );
        }

        return false;
    }

    static bool EnableDeviceWithFallback(
        HDEVINFO hDevInfo,
        SP_DEVINFO_DATA& devInfoData,
        std::wstring deviceId
    ) {
        // STRATEGY: Try SetupAPI first, fall back to CFGMGR32 if needed

        SP_PROPCHANGE_PARAMS params = {};
        params.ClassInstallHeader.cbSize = sizeof(SP_CLASSINSTALL_HEADER);
        params.ClassInstallHeader.InstallFunction = DIF_PROPERTYCHANGE;
        params.Scope = DICS_FLAG_GLOBAL;
        params.StateChange = DICS_ENABLE;
        params.HwProfile = 0;

        ULONGLONG startTime = GetTickCount64();

        if (SetupDiSetClassInstallParams(hDevInfo, &devInfoData,
                                        &params.ClassInstallHeader, sizeof(params))) {
            if (SetupDiCallClassInstaller(DIF_PROPERTYCHANGE, hDevInfo, &devInfoData)) {
                ULONGLONG duration = GetTickCount64() - startTime;
                ProductionLogger::LogHardwareOperation(
                    L"SetupAPI_Enable",
                    deviceId,
                    true,
                    0,
                    CR_SUCCESS,
                    duration
                );
                return true;
            }
        }

        DWORD setupApiError = GetLastError();
        ProductionLogger::LogHardwareOperation(
            L"SetupAPI_Enable",
            deviceId,
            false,
            setupApiError,
            CR_SUCCESS,
            GetTickCount64() - startTime
        );

        // Attempt 2: CFGMGR32 fallback
        DEVINST devInst = 0;
        CONFIGRET cr = CM_Locate_DevNodeW(&devInst, (PWSTR)deviceId.c_str(),
                                          CM_LOCATE_DEVNODE_NORMAL);

        if (cr == CR_SUCCESS) {
            cr = CM_Enable_DevNode(devInst, 0);

            ULONGLONG duration = GetTickCount64() - startTime;
            ProductionLogger::LogHardwareOperation(
                L"CFGMGR32_Enable",
                deviceId,
                cr == CR_SUCCESS,
                0,
                cr,
                duration
            );

            if (cr == CR_SUCCESS) {
                CM_Reenumerate_DevNode(devInst, 0);
                return true;
            }
        } else {
            ProductionLogger::LogHardwareOperation(
                L"CFGMGR32_Locate",
                deviceId,
                false,
                0,
                cr,
                0
            );
        }

        return false;
    }
};

// ============================================================
// SHUTDOWN MANAGER - Ensures camera enable during shutdown
// ============================================================

class ShutdownManager {
private:
    static volatile bool isShuttingDown;

public:
    static bool IsShuttingDown() {
        return isShuttingDown;
    }

    static void NotifyShutdown() {
        isShuttingDown = true;
        ProductionLogger::LogInfo(L"Shutdown", L"Shutdown signal received");
    }

    static void ResetShutdownFlag() {
        isShuttingDown = false;
    }
};

volatile bool ShutdownManager::isShuttingDown = false;
