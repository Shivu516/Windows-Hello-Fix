#include "CommandLine.h"
#include <windows.h>

bool CommandLine::IsBackgroundLaunch(array<System::String^>^ args) {
    for (int i = 0; i < args->Length; i++) {
        if (args[i]->Equals(L"/background", System::StringComparison::OrdinalIgnoreCase) ||
            args[i]->Equals(L"--background", System::StringComparison::OrdinalIgnoreCase)) {
            return true;
        }
    }
    return false;
}

bool CommandLine::IsRestoreCameraCommand(array<System::String^>^ args) {
    for (int i = 0; i < args->Length; i++) {
        if (args[i]->Equals(L"/restore-camera", System::StringComparison::OrdinalIgnoreCase) ||
            args[i]->Equals(L"/enable-camera", System::StringComparison::OrdinalIgnoreCase) ||
            args[i]->Equals(L"--enable-camera", System::StringComparison::OrdinalIgnoreCase) ||
            args[i]->Equals(L"/repair-camera", System::StringComparison::OrdinalIgnoreCase)) {
            return true;
        }
    }
    return false;
}

bool CommandLine::IsDisableCameraCommand(array<System::String^>^ args) {
    for (int i = 0; i < args->Length; i++) {
        if (args[i]->Equals(L"/disable-camera", System::StringComparison::OrdinalIgnoreCase) ||
            args[i]->Equals(L"--disable-camera", System::StringComparison::OrdinalIgnoreCase)) {
            return true;
        }
    }
    return false;
}

bool CommandLine::IsFailsafeBootCommand(array<System::String^>^ args) {
    for (int i = 0; i < args->Length; i++) {
        if (args[i]->Equals(L"/failsafe-boot", System::StringComparison::OrdinalIgnoreCase) ||
            args[i]->Equals(L"--failsafe-boot", System::StringComparison::OrdinalIgnoreCase)) {
            return true;
        }
    }
    return false;
}

bool CommandLine::ShouldHideWindow(array<System::String^>^ args) {
    for (int i = 0; i < args->Length; i++) {
        if (args[i] == L"--background" || args[i] == L"/background" ||
            args[i] == L"--disable-camera" || args[i] == L"/disable-camera" ||
            args[i] == L"--enable-camera" || args[i] == L"/enable-camera" ||
            args[i] == L"/restore-camera" || args[i] == L"/repair-camera" ||
            args[i] == L"--failsafe-boot" || args[i] == L"/failsafe-boot") {
            return true;
        }
    }
    // Case-insensitive fallback for failsafe-boot (covers --Failsafe-Boot etc.)
    return IsFailsafeBootCommand(args);
}

bool CommandLine::IsStartupDisabled() {
    const wchar_t* subKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\Run";
    const wchar_t* valueNames[] = { L"WindowsHelloFix", L"Windows Hello Fix" };
    BYTE data[64];
    DWORD type = 0;
    DWORD size = 0;
    HKEY hKey = NULL;

    // Helper lambda to check one hive (checks both spaced and unspaced names)
    auto checkHive = [&](HKEY root, REGSAM sam) -> bool {
        if (RegOpenKeyExW(root, subKey, 0, KEY_QUERY_VALUE | sam, &hKey) != ERROR_SUCCESS) return false;
        for (int n = 0; n < 2; n++) {
            size = sizeof(data);
            type = 0;
            LONG res = RegQueryValueExW(hKey, valueNames[n], NULL, &type, data, &size);
            if (res == ERROR_SUCCESS && type == REG_BINARY && size >= 1 && data[0] == 0x03) {
                RegCloseKey(hKey);
                return true; // 0x03 = disabled, 0x02 = enabled
            }
        }
        RegCloseKey(hKey);
        return false;
    };

    // Check HKLM 64-bit view (where installer writes Run)
    if (checkHive(HKEY_LOCAL_MACHINE, KEY_WOW64_64KEY)) return true;
    // Check HKLM 32-bit view (fallback)
    if (checkHive(HKEY_LOCAL_MACHINE, KEY_WOW64_32KEY)) return true;
    // Check HKCU (per-user StartupApproved)
    if (checkHive(HKEY_CURRENT_USER, 0)) return true;

    // Also check StartupFolder approved (in case Run was created as StartupFolder shortcut in some configs)
    const wchar_t* subKeyFolder = L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\StartupFolder";
    auto checkFolder = [&](HKEY root, REGSAM sam) -> bool {
        for (int n = 0; n < 2; n++) {
            if (RegOpenKeyExW(root, subKeyFolder, 0, KEY_QUERY_VALUE | sam, &hKey) == ERROR_SUCCESS) {
                size = sizeof(data);
                type = 0;
                LONG res = RegQueryValueExW(hKey, valueNames[n], NULL, &type, data, &size);
                RegCloseKey(hKey);
                if (res == ERROR_SUCCESS && type == REG_BINARY && size >= 1 && data[0] == 0x03) return true;
            }
        }
        // Also check wildcard scan for value containing WindowsHelloFix (folder shortcuts use file name)
        // Fall back to enumerating values
        if (RegOpenKeyExW(root, subKeyFolder, 0, KEY_QUERY_VALUE | sam, &hKey) == ERROR_SUCCESS) {
            DWORD index = 0;
            wchar_t valName[260];
            DWORD valNameLen = 260;
            while (RegEnumValueW(hKey, index, valName, &valNameLen, NULL, &type, data, &size) == ERROR_SUCCESS) {
                if (type == REG_BINARY && size >= 1 && data[0] == 0x03) {
                    // Check if value name contains WindowsHelloFix (with or without spaces)
                    if (wcsstr(valName, L"WindowsHelloFix") != nullptr || wcsstr(valName, L"Windows Hello Fix") != nullptr) {
                        RegCloseKey(hKey);
                        return true;
                    }
                }
                index++;
                valNameLen = 260;
                size = sizeof(data);
            }
            RegCloseKey(hKey);
        }
        return false;
    };
    if (checkFolder(HKEY_LOCAL_MACHINE, KEY_WOW64_64KEY)) return true;
    if (checkFolder(HKEY_CURRENT_USER, 0)) return true;

    return false;
}
