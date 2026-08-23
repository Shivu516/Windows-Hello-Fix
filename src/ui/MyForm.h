#pragma once

#include <windows.h>
#include <wtsapi32.h>
#include <setupapi.h>
#include <cfgmgr32.h>
#include <devguid.h>
#include <vector>
#include <string>
#include <fstream>
#include <msclr\marshal_cppstd.h>
#include "../../resource.h"
#include "../utilities/StringHelpers.h"
#include "../system/PrivilegeInfo.h"
#include "../camera/DeviceError.h"
#include "../camera/CameraDevice.h"
#include "../camera/CameraHardware.h"
#include "../camera/CameraRecovery.h"
#include "../config/ConfigPaths.h"
#include "../config/ConfigStore.h"
#include "../application/CommandLine.h"
#include "../events/SystemEvent.h"
#include "../events/WinEventDecoder.h"
#include "../events/EventCooldown.h"
#include "../events/NotificationRegistrar.h"
#include "../system/SingleInstance.h"
#include "../system/ProcessUtils.h"
#include "../application/IUiSink.h"
#include "../application/ApplicationController.h"
#include "UiConstants.h"

#pragma comment(lib, "wtsapi32.lib")
#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "cfgmgr32.lib")
#pragma comment(lib, "advapi32.lib")

#ifndef GUID_LIDSWITCH_STATE_CHANGE
#define GUID_LIDSWITCH_STATE_CHANGE {0xBA3E0F4D, 0xB817, 0x4094, {0xA2, 0xD1, 0xD5, 0x63, 0x79, 0xE6, 0xA0, 0xF3}}
#endif
#ifndef GUID_POWER_BUTTON_TIMESTAMP
#define GUID_POWER_BUTTON_TIMESTAMP {0xA70AFB22, 0x3816, 0x4584, {0x9F, 0x24, 0x81, 0x0A, 0x4E, 0x27, 0x47, 0xFB}}
#endif
#ifndef CONFIGFLAG_DISABLED
#define CONFIGFLAG_DISABLED 0x00000001
#endif
#ifndef CM_PROB_DISABLED
#define CM_PROB_DISABLED 22
#endif

namespace Windows_Hello_Fix_v2_0 {

    using namespace System;
    using namespace System::Windows::Forms;
    using namespace System::Drawing;
    using namespace System::ComponentModel;
    using namespace System::IO;
    using namespace System::Threading;

    public ref class MyForm : public System::Windows::Forms::Form, public IUiSink
    {
    private:
        ApplicationController^ m_controller;
        void* cachedCameras;
        bool isBackgroundMode;
        bool cameraExpectedDisabled;

        System::Windows::Forms::ComboBox^ deviceDrop;
        System::Windows::Forms::Button^ btnToggle;
        System::Windows::Forms::Label^ lblTitle;
        System::Windows::Forms::Label^ lblStatus;
        System::ComponentModel::Container^ components;

    public:
        MyForm(void);
        virtual void AddDeviceToDropdown(String^ friendlyName) sealed;
        virtual void ClearDeviceDropdown() sealed;
        virtual void SetSelectedDeviceIndex(int index) sealed;
        virtual int GetDeviceCount() sealed;
        virtual void SetDeviceDropdownEnabled(bool enabled) sealed;
        virtual void SetToggleButtonText(String^ text) sealed;
        virtual void SetStatusText(String^ text, System::Drawing::Color color) sealed;
        virtual void SetWindowVisibleForBackground(bool isBackground) sealed;
        virtual void BringWindowToFront() sealed;
        virtual bool PromptGhostReset() sealed;
        virtual void ShowNoDeviceSelectedMessage() sealed;
        virtual void ShowBackgroundNotice() sealed;
        virtual void ShowAlreadyRunningMessage() sealed;

    protected:
        ~MyForm();
        !MyForm();

    private:
        void InitializeComponent(void);
        System::Void MyForm_Load(System::Object^ sender, System::EventArgs^ e);
        System::Void MyForm_FormClosing(System::Object^ sender, System::Windows::Forms::FormClosingEventArgs^ e);
        System::Void btnToggle_Click(System::Object^ sender, System::EventArgs^ e);
        void BringWindowToFrontDelegate();

    protected:
        virtual void WndProc(System::Windows::Forms::Message% m) override;
    };

} // end namespace Windows_Hello_Fix_v2_0
