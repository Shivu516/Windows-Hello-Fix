#pragma once

#include <string>

public ref class ConfigStore
{
private:
    static System::Object^ s_diagnosticLogSync = gcnew System::Object();

public:
    static void WriteDiagnosticLog(System::String^ eventName, System::String^ targetState, bool verificationPass);
    static void WriteDiagnosticLogWithDevice(System::String^ eventName, std::wstring targetInstanceId, System::String^ targetState, bool verificationPass);
    static void SaveConfigState(bool monitoring, System::String^ deviceInstanceId);
    static bool LoadConfigState([System::Runtime::InteropServices::Out] System::String^% deviceInstanceId);
    static void EnsureConfigFileExists(System::String^ deviceInstanceId);
};
