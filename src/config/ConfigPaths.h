#pragma once

public ref class ConfigPaths
{
public:
    static System::String^ GetConfigFilePath();
    static System::String^ GetDiagnosticLogFilePath();
};
