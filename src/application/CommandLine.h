#pragma once

public ref class CommandLine
{
public:
    static bool IsBackgroundLaunch(array<System::String^>^ args);
    static bool IsRestoreCameraCommand(array<System::String^>^ args);
    static bool IsDisableCameraCommand(array<System::String^>^ args);
    static bool ShouldHideWindow(array<System::String^>^ args);
};
