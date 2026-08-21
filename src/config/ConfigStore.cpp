#include "ConfigStore.h"
#include "ConfigPaths.h"
#include "../utilities/StringHelpers.h"

#include <msclr\marshal_cppstd.h>

void ConfigStore::WriteDiagnosticLog(System::String^ eventName, System::String^ targetState, bool verificationPass) {
    System::Threading::Monitor::Enter(s_diagnosticLogSync);
    try {
        System::String^ logPath = ConfigPaths::GetDiagnosticLogFilePath();
        System::IO::StreamWriter^ sw = gcnew System::IO::StreamWriter(logPath, true);
        System::String^ timestamp = System::DateTime::Now.ToString(L"yyyy-MM-dd HH:mm:ss.fff");
        sw->WriteLine(
            System::String::Format(
                L"{0} | Event={1} | Target={2} | Verify={3}",
                timestamp,
                eventName,
                targetState,
                verificationPass ? L"PASS" : L"FAIL"
            )
        );
        sw->Close();
    }
    catch (...) {}
    finally {
        System::Threading::Monitor::Exit(s_diagnosticLogSync);
    }
}

void ConfigStore::WriteDiagnosticLogWithDevice(System::String^ eventName, std::wstring targetInstanceId, System::String^ targetState, bool verificationPass) {
    System::String^ deviceId = msclr::interop::marshal_as<System::String^>(targetInstanceId);
    WriteDiagnosticLog(
        eventName + L" | Device=" + deviceId,
        targetState,
        verificationPass
    );
}

void ConfigStore::SaveConfigState(bool monitoring, System::String^ deviceInstanceId) {
    try {
        System::String^ path = ConfigPaths::GetConfigFilePath();
        System::IO::StreamWriter^ sw = gcnew System::IO::StreamWriter(path, false);
        sw->WriteLine(monitoring ? L"monitoring=1" : L"monitoring=0");
        sw->WriteLine(L"device=" + deviceInstanceId);
        sw->Close();
    }
    catch (...) {}
}

bool ConfigStore::LoadConfigState([System::Runtime::InteropServices::Out] System::String^% deviceInstanceId) {
    deviceInstanceId = L"";
    try {
        System::String^ path = ConfigPaths::GetConfigFilePath();
        if (!System::IO::File::Exists(path)) {
            return false;
        }

        System::IO::StreamReader^ sr = gcnew System::IO::StreamReader(path);
        System::String^ line1 = sr->ReadLine();
        System::String^ line2 = sr->ReadLine();
        sr->Close();

        bool monitoringActive = (line1 != nullptr && line1->Trim() == L"monitoring=1");
        if (line2 != nullptr && line2->StartsWith(L"device=")) {
            // FIX: Trim prevents \r\n newline corruption in C++ string matching
            std::wstring rawPath = msclr::interop::marshal_as<std::wstring>(line2->Substring(7)->Trim());
            std::wstring sanitizedPath = TrimTrailingChars(rawPath);
            deviceInstanceId = msclr::interop::marshal_as<System::String^>(sanitizedPath);
        }
        return monitoringActive;
    }
    catch (...) {
        return false;
    }
}

void ConfigStore::EnsureConfigFileExists(System::String^ deviceInstanceId) {
    try {
        System::String^ path = ConfigPaths::GetConfigFilePath();
        if (!System::IO::File::Exists(path)) {
            SaveConfigState(false, deviceInstanceId);
        }
    }
    catch (...) {}
}
