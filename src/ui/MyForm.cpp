#include "MyForm.h"

namespace Windows_Hello_Fix_v2_0 {

    MyForm::MyForm(void) {
        components = nullptr;
        cachedCameras = new std::vector<CameraDeviceInfo>();
        isBackgroundMode = false;
        cameraExpectedDisabled = false;
        m_controller = gcnew ApplicationController(this);
        InitializeComponent();
    }

    MyForm::~MyForm() {
        if (m_controller) {
            m_controller->Shutdown(m_controller->IsSystemEnding);
            delete m_controller;
            m_controller = nullptr;
        }
        if (cachedCameras != nullptr) {
            delete static_cast<std::vector<CameraDeviceInfo>*>(cachedCameras);
            cachedCameras = nullptr;
        }
        if (components) {
            delete components;
        }
    }

    MyForm::!MyForm() {
        if (m_controller) {
            m_controller->Shutdown(m_controller->IsSystemEnding);
        }
        if (cachedCameras != nullptr) {
            delete static_cast<std::vector<CameraDeviceInfo>*>(cachedCameras);
            cachedCameras = nullptr;
        }
    }

    void MyForm::AddDeviceToDropdown(String^ friendlyName) {
        this->deviceDrop->Items->Add(friendlyName);
    }
    void MyForm::ClearDeviceDropdown() {
        this->deviceDrop->Items->Clear();
    }
    void MyForm::SetSelectedDeviceIndex(int index) {
        this->deviceDrop->SelectedIndex = index;
    }
    int MyForm::GetDeviceCount() {
        return this->deviceDrop->Items->Count;
    }
    void MyForm::SetDeviceDropdownEnabled(bool enabled) {
        this->deviceDrop->Enabled = enabled;
    }
    void MyForm::SetToggleButtonText(String^ text) {
        this->btnToggle->Text = text;
    }
    void MyForm::SetStatusText(String^ text, System::Drawing::Color color) {
        this->lblStatus->Text = text;
        this->lblStatus->ForeColor = color;
    }
    void MyForm::SetWindowVisibleForBackground(bool isBackground) {
        if (isBackground) {
            this->Visible = false;
            this->ShowInTaskbar = false;
            this->WindowState = FormWindowState::Minimized;
        }
    }
    void MyForm::BringWindowToFront() {
        if (this->InvokeRequired) {
            this->Invoke(gcnew MethodInvoker(this, &MyForm::BringWindowToFrontDelegate));
        } else {
            BringWindowToFrontDelegate();
        }
    }
    bool MyForm::PromptGhostReset() {
        System::Windows::Forms::DialogResult result = MessageBox::Show(
            L"Windows Hello Fix is already running in the background.\n\nIf the application is frozen or not responding, would you like to force a reset and restart it?",
            L"Application Already Running",
            MessageBoxButtons::YesNo,
            MessageBoxIcon::Question
        );
        return result == System::Windows::Forms::DialogResult::Yes;
    }
    void MyForm::ShowNoDeviceSelectedMessage() {
        MessageBox::Show(L"Please select a camera device.", L"No Device Selected", MessageBoxButtons::OK, MessageBoxIcon::Warning);
    }
    void MyForm::ShowBackgroundNotice() {
        MessageBox::Show(
            L"The program is running in the background. To close it completely, click 'Stop Monitoring Service' or kill 'Windows_Hello_Fix_v2_0.exe' in Task Manager.",
            L"Background Service Active",
            MessageBoxButtons::OK,
            MessageBoxIcon::Information
        );
    }
    void MyForm::BringWindowToFrontDelegate() {
        this->Show();
        this->Visible = true;
        this->ShowInTaskbar = true;
        this->WindowState = FormWindowState::Normal;
        this->BringToFront();
        this->Activate();
        this->Refresh();
    }

    void MyForm::InitializeComponent(void) {
        this->deviceDrop = (gcnew System::Windows::Forms::ComboBox());
        this->btnToggle = (gcnew System::Windows::Forms::Button());
        this->lblTitle = (gcnew System::Windows::Forms::Label());
        this->lblStatus = (gcnew System::Windows::Forms::Label());
        this->components = (gcnew System::ComponentModel::Container());

        this->SuspendLayout();
        try {
            HICON hMainIcon = (HICON)LoadImage(GetModuleHandle(NULL), MAKEINTRESOURCE(IDI_ICON1), IMAGE_ICON, 0, 0, LR_DEFAULTSIZE);
            if (hMainIcon) {
                this->Icon = System::Drawing::Icon::FromHandle((IntPtr)hMainIcon);
            }
        }
        catch (...) {}

        // deviceDrop
        this->deviceDrop->DropDownStyle = System::Windows::Forms::ComboBoxStyle::DropDownList;
        this->deviceDrop->Font = (gcnew System::Drawing::Font(L"Segoe UI", 10));
        this->deviceDrop->Location = System::Drawing::Point(25, 75);
        this->deviceDrop->Name = L"deviceDrop";
        this->deviceDrop->Size = System::Drawing::Size(380, 31);
        this->deviceDrop->Enabled = true;

        // btnToggle
        this->btnToggle->Font = (gcnew System::Drawing::Font(L"Segoe UI", 10, System::Drawing::FontStyle::Bold));
        this->btnToggle->Location = System::Drawing::Point(25, 130);
        this->btnToggle->Name = L"btnToggle";
        this->btnToggle->Size = System::Drawing::Size(380, 45);
        this->btnToggle->Text = L"Start Monitoring Service";
        this->btnToggle->Click += gcnew System::EventHandler(this, &MyForm::btnToggle_Click);

        // lblTitle
        this->lblTitle->AutoSize = true;
        this->lblTitle->Font = (gcnew System::Drawing::Font(L"Segoe UI", 12, System::Drawing::FontStyle::Bold));
        this->lblTitle->Location = System::Drawing::Point(20, 25);
        this->lblTitle->Text = L"Select Target RGB Sensor";

        // lblStatus
        this->lblStatus->AutoSize = true;
        this->lblStatus->ForeColor = System::Drawing::Color::Gray;
        this->lblStatus->Location = System::Drawing::Point(25, 195);
        this->lblStatus->Text = L"Status: Service Stopped";

        // MyForm
        this->ClientSize = System::Drawing::Size(430, 240);
        this->Controls->Add(this->lblStatus);
        this->Controls->Add(this->lblTitle);
        this->Controls->Add(this->btnToggle);
        this->Controls->Add(this->deviceDrop);
        this->FormBorderStyle = System::Windows::Forms::FormBorderStyle::FixedDialog;
        this->MaximizeBox = false;
        this->MinimizeBox = false;
        this->Name = L"MyForm";
        this->StartPosition = System::Windows::Forms::FormStartPosition::CenterScreen;
        this->Text = L"Windows Hello Fix v2.0";
        this->FormClosing += gcnew System::Windows::Forms::FormClosingEventHandler(this, &MyForm::MyForm_FormClosing);
        this->Load += gcnew System::EventHandler(this, &MyForm::MyForm_Load);

        this->ResumeLayout(false);
        this->PerformLayout();
    }

    System::Void MyForm::MyForm_Load(System::Object^ sender, System::EventArgs^ e) {
        HWND hwnd = static_cast<HWND>(this->Handle.ToPointer());
        array<System::String^>^ args = System::Environment::GetCommandLineArgs();
        // Delegate startup orchestration to controller. Controller handles command line, single instance, ghost, config, camera, notifications.
        // MyForm handles UI population after controller's startup recovery and device enumeration.
        m_controller->SetHwnd(hwnd);
        bool shouldContinue = m_controller->Initialize(hwnd, args);
        if (!shouldContinue) {
            return;
        }

        // UI: populate dropdown from camera scan (controller already did Restore, but UI needs to show devices)
        auto* pCachedCameras = static_cast<std::vector<CameraDeviceInfo>*>(cachedCameras);
        *pCachedCameras = ScanSystemCameras();
        this->ClearDeviceDropdown();
        int savedIdx = -1;
        int autoIdx = -1;
        String^ savedDeviceInstance = L"";
        bool shouldAutoStartByConfig = ConfigStore::LoadConfigState(savedDeviceInstance);
        for (int i = 0; i < static_cast<int>(pCachedCameras->size()); i++) {
            String^ currentId = msclr::interop::marshal_as<String^>(pCachedCameras->at(i).instanceId);
            this->AddDeviceToDropdown(msclr::interop::marshal_as<String^>(pCachedCameras->at(i).friendlyName));
            if (currentId == savedDeviceInstance) savedIdx = i;
            if (pCachedCameras->at(i).instanceId.find(L"MI_00") != std::wstring::npos) autoIdx = i;
        }
        if (savedIdx != -1) this->SetSelectedDeviceIndex(savedIdx);
        else if (autoIdx != -1) this->SetSelectedDeviceIndex(autoIdx);
        else if (this->GetDeviceCount() > 0) this->SetSelectedDeviceIndex(0);

        if (this->deviceDrop->SelectedIndex != -1) {
            std::wstring selId = pCachedCameras->at(this->deviceDrop->SelectedIndex).instanceId;
            m_controller->SetSelectedInstanceId(selId);
            String^ selectedDeviceForConfig = msclr::interop::marshal_as<String^>(selId);
            ConfigStore::EnsureConfigFileExists(selectedDeviceForConfig);
        } else {
            ConfigStore::EnsureConfigFileExists(L"");
        }

        // Let controller handle monitoring UI decision
        bool isBackground = CommandLine::IsBackgroundLaunch(args);
        if (isBackground) isBackgroundMode = true;
        String^ savedForAuto = L"";
        bool autoStart = ConfigStore::LoadConfigState(savedForAuto);
        if (this->deviceDrop->SelectedIndex != -1) {
            // First enable to prevent bricking
            m_controller->EnableTargetCameraHardware(autoStart);
        }
        if ((isBackground || autoStart) && this->deviceDrop->SelectedIndex != -1) {
            m_controller->SetSelectedInstanceId(pCachedCameras->at(this->deviceDrop->SelectedIndex).instanceId);
            m_controller->IsMonitoring = true;
            m_controller->EnableTargetCameraHardware(false);
            this->SetDeviceDropdownEnabled(false);
            this->SetToggleButtonText(L"Stop Monitoring Service");
            this->SetStatusText(L"Status: Service Running", System::Drawing::Color::Green);
            if (isBackground) {
                this->SetWindowVisibleForBackground(true);
                isBackgroundMode = true;
            }
        } else {
            m_controller->IsMonitoring = false;
            this->SetDeviceDropdownEnabled(true);
            this->SetToggleButtonText(L"Start Monitoring Service");
            this->SetStatusText(L"Status: Service Stopped", System::Drawing::Color::Gray);
        }
    }

    System::Void MyForm::MyForm_FormClosing(System::Object^ sender, System::Windows::Forms::FormClosingEventArgs^ e) {
        if (e->CloseReason == CloseReason::UserClosing) {
            e->Cancel = true;
            this->Hide();
            this->ShowInTaskbar = false;
            if (!isBackgroundMode) {
                this->ShowBackgroundNotice();
                isBackgroundMode = true;
            }
        }
    }

    System::Void MyForm::btnToggle_Click(System::Object^ sender, System::EventArgs^ e) {
        if (this->deviceDrop->SelectedIndex == -1) {
            this->ShowNoDeviceSelectedMessage();
            return;
        }
        auto* pCachedCameras = static_cast<std::vector<CameraDeviceInfo>*>(cachedCameras);
        // Sync selected device to controller before toggle
        m_controller->SetSelectedInstanceId(pCachedCameras->at(this->deviceDrop->SelectedIndex).instanceId);
        bool nowMonitoring = m_controller->ToggleMonitoring();
        if (nowMonitoring) {
            this->SetDeviceDropdownEnabled(false);
            this->SetToggleButtonText(L"Stop Monitoring Service");
            this->SetStatusText(L"Status: Service Running", System::Drawing::Color::Green);
        } else {
            this->SetDeviceDropdownEnabled(true);
            this->SetToggleButtonText(L"Start Monitoring Service");
            this->SetStatusText(L"Status: Service Stopped", System::Drawing::Color::Gray);
        }
    }

    void MyForm::WndProc(System::Windows::Forms::Message% m) {
        ULONGLONG nowTick = GetTickCount64();

        if (m.Msg == 0x0016 || m.Msg == 0x0011) {
            m_controller->IsSystemEnding = true;
            m_controller->HandleSystemEnd(static_cast<HWND>(this->Handle.ToPointer()));
            Form::WndProc(m);
            return;
        }
        else if (m.Msg == 0x0218) {
            int powerEvent = m.WParam.ToInt32();
            if (EventCooldown::ShouldSuppressPowerEvent(powerEvent, nowTick)) {
                ConfigStore::WriteDiagnosticLog(L"PowerEvent_DedupIgnored", L"NoChange", true);
                Form::WndProc(m);
                return;
            }
            SystemEvent powerDecoded = WinEventDecoder::DecodePowerEvent(powerEvent, (LPARAM)m.LParam.ToPointer());
            if (powerDecoded == SystemEvent::PowerSettingOther) {
                ConfigStore::WriteDiagnosticLog(L"PowerSetting_IrrelevantGuid", L"NoChange", true);
                Form::WndProc(m);
                return;
            }
            // Delegate power policy to controller
            m_controller->HandlePowerEvent(powerDecoded);
            Form::WndProc(m);
            return;
        }
        else if (m.Msg == WM_WTSSESSION_CHANGE) {
            int sessionEvent = m.WParam.ToInt32();
            ConfigStore::WriteDiagnosticLog(
                String::Format(L"SessionEvent_Received_Code={0}", sessionEvent),
                m_controller->IsMonitoring ? L"ActiveMonitoring" : L"MonitoringOff",
                true
            );
            if (EventCooldown::ShouldSuppressSessionEvent(sessionEvent, nowTick)) {
                ConfigStore::WriteDiagnosticLog(L"SessionEvent_DedupIgnored", L"NoChange", true);
                Form::WndProc(m);
                return;
            }
            SystemEvent sessEv = WinEventDecoder::DecodeSessionEvent(sessionEvent);
            m_controller->HandleSessionEvent(sessEv);
            Form::WndProc(m);
            return;
        }

        Form::WndProc(m);
    }

} // end namespace Windows_Hello_Fix_v2_0


