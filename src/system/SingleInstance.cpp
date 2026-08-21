#include "SingleInstance.h"

namespace SingleInstance {

HANDLE CreateAppMutex(bool& outAlreadyExists) {
    HANDLE hMutex = CreateMutex(NULL, TRUE, L"Global\\WindowsHelloFix_AppMutex");
    outAlreadyExists = (GetLastError() == ERROR_ALREADY_EXISTS);
    return hMutex;
}

bool TrySignalExistingInstance() {
    HANDLE hOpenEvent = OpenEvent(EVENT_MODIFY_STATE, FALSE, L"Global\\WindowsHelloFix_WakeupEvent");
    if (hOpenEvent) {
        SetEvent(hOpenEvent);
        CloseHandle(hOpenEvent);
        ::Sleep(200);
        return true;
    }
    return false;
}

HANDLE CreateWakeupEvent() {
    return CreateEvent(NULL, FALSE, FALSE, L"Global\\WindowsHelloFix_WakeupEvent");
}

void SignalAndCloseWakeEvent(HANDLE hEvent) {
    if (hEvent) {
        SetEvent(hEvent);
        CloseHandle(hEvent);
    }
}

void ReleaseMutex(HANDLE hMutex) {
    if (hMutex) {
        CloseHandle(hMutex);
    }
}

void CloseHandleIfValid(HANDLE h) {
    if (h) {
        CloseHandle(h);
    }
}

}
