#pragma once

#include <windows.h>

namespace SingleInstance {

HANDLE CreateAppMutex(bool& outAlreadyExists);
bool TrySignalExistingInstance();
HANDLE CreateWakeupEvent();
void SignalAndCloseWakeEvent(HANDLE hEvent);
void ReleaseMutex(HANDLE hMutex);
void CloseHandleIfValid(HANDLE h);

}
