#pragma once

// Windows Hello Fix — UI constants
// Genuine WinForms presentation constants only.
public ref class UiConstants abstract sealed {
public:
    literal System::String^ FormText = L"Windows Hello Fix v2.0";
    literal System::String^ FormName = L"MyForm";

    literal System::String^ LabelSelectSensor = L"Select Target RGB Sensor";
    literal System::String^ ButtonStart = L"Start Monitoring Service";
    literal System::String^ ButtonStop = L"Stop Monitoring Service";
    literal System::String^ StatusStopped = L"Status: Service Stopped";
    literal System::String^ StatusRunning = L"Status: Service Running";

    literal System::String^ MessageNoDeviceTitle = L"No Device Selected";
    literal System::String^ MessageNoDeviceText = L"Please select a camera device.";
    literal System::String^ MessageBackgroundTitle = L"Background Service Active";
    literal System::String^ MessageBackgroundText = L"The program is running in the background. To close it completely, click 'Stop Monitoring Service' or kill 'Windows_Hello_Fix_v2_0.exe' in Task Manager.";
    literal System::String^ MessageGhostTitle = L"Application Already Running";
    literal System::String^ MessageGhostText = L"Windows Hello Fix is already running in the background.\n\nIf the application is frozen or not responding, would you like to force a reset and restart it?";

    literal System::String^ NameDeviceDrop = L"deviceDrop";
    literal System::String^ NameToggleButton = L"btnToggle";

    literal int FormWidth = 430;
    literal int FormHeight = 240;
    literal int DeviceDropX = 25;
    literal int DeviceDropY = 75;
    literal int DeviceDropWidth = 380;
    literal int DeviceDropHeight = 31;
    literal int ToggleButtonX = 25;
    literal int ToggleButtonY = 130;
    literal int ToggleButtonWidth = 380;
    literal int ToggleButtonHeight = 45;
    literal int LabelTitleX = 20;
    literal int LabelTitleY = 25;
    literal int LabelStatusX = 25;
    literal int LabelStatusY = 195;
};
