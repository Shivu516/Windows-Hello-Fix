#include "PrivilegeInfo.h"

#pragma comment(lib, "advapi32.lib")

bool IsCurrentProcessElevatedNative() {
    HANDLE token = NULL;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        return false;
    }

    TOKEN_ELEVATION elevation;
    DWORD returnLength = 0;
    BOOL ok = GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &returnLength);
    CloseHandle(token);

    return ok && elevation.TokenIsElevated != 0;
}

DWORD GetCurrentProcessIntegrityRid() {
    HANDLE token = NULL;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        return 0;
    }

    DWORD tokenInfoLength = 0;
    GetTokenInformation(token, TokenIntegrityLevel, NULL, 0, &tokenInfoLength);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || tokenInfoLength == 0) {
        CloseHandle(token);
        return 0;
    }

    PTOKEN_MANDATORY_LABEL tokenLabel = reinterpret_cast<PTOKEN_MANDATORY_LABEL>(LocalAlloc(LPTR, tokenInfoLength));
    if (tokenLabel == NULL) {
        CloseHandle(token);
        return 0;
    }

    DWORD integrityRid = 0;
    if (GetTokenInformation(token, TokenIntegrityLevel, tokenLabel, tokenInfoLength, &tokenInfoLength)) {
        DWORD subAuthorityCount = *GetSidSubAuthorityCount(tokenLabel->Label.Sid);
        integrityRid = *GetSidSubAuthority(tokenLabel->Label.Sid, subAuthorityCount - 1);
    }

    LocalFree(tokenLabel);
    CloseHandle(token);
    return integrityRid;
}
