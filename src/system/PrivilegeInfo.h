#pragma once

#include <windows.h>

bool IsCurrentProcessElevatedNative();
DWORD GetCurrentProcessIntegrityRid();
