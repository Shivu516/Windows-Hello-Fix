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

bool CommandLine::ShouldHideWindow(array<System::String^>^ args) {
    // Exact v2.0 main.cpp hide check: case-sensitive String::operator== for 8 literals
    for (int i = 0; i < args->Length; i++) {
        if (args[i] == L"--background" || args[i] == L"/background" ||
            args[i] == L"--disable-camera" || args[i] == L"/disable-camera" ||
            args[i] == L"--enable-camera" || args[i] == L"/enable-camera" ||
            args[i] == L"/restore-camera" || args[i] == L"/repair-camera") {
            return true;
        }
    }
    return false;
}
