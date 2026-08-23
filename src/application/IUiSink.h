#pragma once

public interface class IUiSink {
public:
    void AddDeviceToDropdown(System::String^ friendlyName);
    void ClearDeviceDropdown();
    void SetSelectedDeviceIndex(int index);
    int GetDeviceCount();
    void SetDeviceDropdownEnabled(bool enabled);
    void SetToggleButtonText(System::String^ text);
    void SetStatusText(System::String^ text, System::Drawing::Color color);
    void SetWindowVisibleForBackground(bool isBackground);
    void BringWindowToFront();
    bool PromptGhostReset();
    void ShowNoDeviceSelectedMessage();
    void ShowBackgroundNotice();
    void ShowAlreadyRunningMessage();
};
