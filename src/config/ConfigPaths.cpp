#include "ConfigPaths.h"

System::String^ ConfigPaths::GetConfigFilePath() {
    System::String^ dir = System::IO::Path::Combine(
        System::Environment::GetFolderPath(System::Environment::SpecialFolder::ApplicationData),
        L"Windows Hello Fix"
    );

    System::IO::Directory::CreateDirectory(dir);
    return System::IO::Path::Combine(dir, L"config.txt");
}

System::String^ ConfigPaths::GetDiagnosticLogFilePath() {
    System::String^ configPath = GetConfigFilePath();
    System::String^ configDirectory = System::IO::Path::GetDirectoryName(configPath);

    if (System::String::IsNullOrEmpty(configDirectory)) {
        configDirectory = System::IO::Path::Combine(
            System::Environment::GetFolderPath(System::Environment::SpecialFolder::ApplicationData),
            L"Windows Hello Fix"
        );
    }

    System::IO::Directory::CreateDirectory(configDirectory);
    return System::IO::Path::Combine(configDirectory, L"diagnostic.log");
}
