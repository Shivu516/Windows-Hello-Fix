#include "MyForm.h"
#include "../watchdog/CameraFailsafe.h"

namespace Windows_Hello_Fix_v2_0 {

    MyForm::MyForm(void) {
        components = nullptr;
        cachedCameras = new std::vector<CameraDeviceInfo>();
        selectedInstanceId = new std::wstring();
        isMonitoring = false;
        isBackgroundMode = false;
        isSystemEnding = false;
        cameraStateInitialized = false;
        cameraExpectedDisabled = false;
        restartQueuedByMismatch = false;
        lastCameraToggleTick = 0;
        hAppMutex = NULL;
        hWakeupEvent = NULL;
        keepListening = true;
        hLidNotification = NULL;
        hButtonNotification = NULL;
        diagnosticLogSync = gcnew Object();
        cameraFailsafe = nullptr;
        InitializeComponent();
    }

    // LAST THING THE APP DOES BEFORE SHUTTING DOWN ENTIRELY (Destructor)
    MyForm::~MyForm() {
        // Disarm auxiliary failsafe first — it must never outlive core shutdown logic.
        if (cameraFailsafe != nullptr) {
            try { cameraFailsafe->Disarm(); } catch (...) {}
            cameraFailsafe = nullptr;
        }
        keepListening = false;

        auto* pSelectedInstanceId = static_cast<std::wstring*>(selectedInstanceId);
        if (isSystemEnding) {
            DisableTargetCameraHardware(true);

            if (pSelectedInstanceId && pSelectedInstanceId->length() > 0) {
                String^ managedId = msclr::interop::marshal_as<String^>(*pSelectedInstanceId);
                SaveConfigState(true, managedId);
            }
        }
        else if (pSelectedInstanceId && pSelectedInstanceId->length() > 0) {
            // Last thing before shutting down: Ensure Camera is RE-ENABLED safely
            EnableTargetCameraHardware(false);

            // Last thing before shutting down: Revert config state to monitoring=1 with LIVE string
            String^ managedId = msclr::interop::marshal_as<String^>(*pSelectedInstanceId);
            SaveConfigState(true, managedId);
        }
        else {
            RestoreConfiguredCameraHardware(false);
        }

        if (hWakeupEvent) {
            SetEvent(hWakeupEvent);
            CloseHandle(hWakeupEvent);
        }
        if (hLidNotification) {
            UnregisterPowerSettingNotification(hLidNotification);
        }
        if (hButtonNotification) {
            UnregisterPowerSettingNotification(hButtonNotification);
        }
        if (cachedCameras != nullptr) {
            delete static_cast<std::vector<CameraDeviceInfo>*>(cachedCameras);
            cachedCameras = nullptr;
        }
        if (selectedInstanceId != nullptr) {
            delete static_cast<std::wstring*>(selectedInstanceId);
            selectedInstanceId = nullptr;
        }
        if (components) {
            delete components;
        }
        if (hAppMutex) {
            CloseHandle(hAppMutex);
        }
    }

    MyForm::!MyForm() {
        if (cameraFailsafe != nullptr) {
            try { cameraFailsafe->Disarm(); } catch (...) {}
            cameraFailsafe = nullptr;
        }
        keepListening = false;
        auto* pSelectedInstanceId = static_cast<std::wstring*>(selectedInstanceId);
        if (isSystemEnding) {
            DisableTargetCameraHardware(true);
        }
        else if (pSelectedInstanceId && pSelectedInstanceId->length() > 0) {
            EnableTargetCameraHardware(false);
        }
        if (hWakeupEvent) {
            SetEvent(hWakeupEvent);
            CloseHandle(hWakeupEvent);
        }
        if (hLidNotification) {
            UnregisterPowerSettingNotification(hLidNotification);
        }
        if (hButtonNotification) {
            UnregisterPowerSettingNotification(hButtonNotification);
        }
        if (cachedCameras != nullptr) {
            delete static_cast<std::vector<CameraDeviceInfo>*>(cachedCameras);
            cachedCameras = nullptr;
        }
        if (selectedInstanceId != nullptr) {
            delete static_cast<std::wstring*>(selectedInstanceId);
            selectedInstanceId = nullptr;
        }
        if (hAppMutex) {
            CloseHandle(hAppMutex);
        }
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
        array<System::String^>^ args = System::Environment::GetCommandLineArgs();
        bool launchRequestedBackground = false;

        for (int i = 0; i < args->Length; i++) {
            if (args[i]->Equals(L"/background", System::StringComparison::OrdinalIgnoreCase) ||
                args[i]->Equals(L"--background", System::StringComparison::OrdinalIgnoreCase)) {
                launchRequestedBackground = true;
                break;
            }
        }

        WriteDiagnosticLog(
            String::Format(
                L"Startup_Context | Elevated={0} | IntegrityRid={1} | BackgroundArg={2} | Exe={3} | Cwd={4} | Config={5}",
                IsCurrentProcessElevatedNative() ? L"1" : L"0",
                static_cast<Int32>(GetCurrentProcessIntegrityRid()),
                launchRequestedBackground ? L"1" : L"0",
                Application::ExecutablePath,
                Environment::CurrentDirectory,
                GetConfigFilePath()
            ),
            L"NoChange",
            IsCurrentProcessElevatedNative()
        );

        if (IsRestoreCameraCommand(args)) {
            this->ShowInTaskbar = false;
            this->Visible = false;
            WriteDiagnosticLog(L"Command_EnableCamera_Begin", L"Enabled", true);
            RestoreConfiguredCameraHardware(true);
            WriteDiagnosticLog(L"Command_EnableCamera_End", L"Enabled", true);
            Environment::Exit(0);
            return;
        }

        if (IsDisableCameraCommand(args)) {
            this->ShowInTaskbar = false;
            this->Visible = false;
            std::wstring commandTargetId;
            WriteDiagnosticLog(L"Command_DisableCamera_Begin", L"Disabled", true);
            bool commandDisableResult = DisableTargetCameraHardware(true);
            bool commandVerifyResult = TryGetTargetCameraInstanceId(commandTargetId, true) && VerifyCameraHardwareState(commandTargetId, true);
            WriteDiagnosticLog(L"Command_DisableCamera_End", L"Disabled", commandDisableResult && commandVerifyResult);
            Environment::Exit(0);
            return;
        }

        // ====== NATIVE INTER-PROCESS SIGNAL LAYER ======
        hAppMutex = CreateMutex(NULL, TRUE, L"Global\\WindowsHelloFix_AppMutex");
        if (GetLastError() == ERROR_ALREADY_EXISTS) {
            // Background/automatic launches must never wake the running daemon's GUI.
            if (launchRequestedBackground) {
                WriteDiagnosticLog(L"SingleInstance_BackgroundSilentExit", L"NoChange", true);
                Environment::Exit(0);
                return;
            }

            bool wakeSignalSent = false;
            HANDLE hOpenEvent = OpenEvent(EVENT_MODIFY_STATE, FALSE, L"Global\\WindowsHelloFix_WakeupEvent");
            if (hOpenEvent) {
                SetEvent(hOpenEvent);
                CloseHandle(hOpenEvent);
                ::Sleep(200);
                wakeSignalSent = true;
            }

            // Normal path: existing process receives wake signal and brings main window back.
            // Avoid showing scary duplicate-instance prompts on expected startup/manual-open races.
            if (wakeSignalSent) {
                WriteDiagnosticLog(L"SingleInstance_WakeSignalSent", L"NoChange", true);
                Environment::Exit(0);
                return;
            }

            // SELF-HEALING GHOST MUTEX PROTECTION WINDOW (only when wake signal path is unavailable)
            System::Windows::Forms::DialogResult result = MessageBox::Show(
                L"Windows Hello Fix is already running in the background.\n\nIf the application is frozen or not responding, would you like to force a reset and restart it?",
                L"Application Already Running",
                MessageBoxButtons::YesNo,
                MessageBoxIcon::Question
            );

            if (result == System::Windows::Forms::DialogResult::Yes) {
                WriteDiagnosticLog(L"SingleInstance_ForceResetRequested", L"NoChange", true);

                // FORCE REVERT CONFIG ON FORCED CLOSED LOOP BREAK
                String^ ghostDeviceInstance = L"";
                LoadConfigState(ghostDeviceInstance);

                // Dynamically fetch live ID if config string was damaged
                if (String::IsNullOrEmpty(ghostDeviceInstance)) {
                    auto cameras = ScanSystemCameras();
                    for (size_t i = 0; i < cameras.size(); i++) {
                        if (cameras[i].instanceId.find(L"MI_00") != std::wstring::npos) {
                            ghostDeviceInstance = msclr::interop::marshal_as<String^>(cameras[i].instanceId);
                            break;
                        }
                    }
                }

                if (!String::IsNullOrEmpty(ghostDeviceInstance)) {
                    // Forcibly wake camera back up via absolute hardware call first
                    std::wstring nativeGhostId = msclr::interop::marshal_as<std::wstring>(ghostDeviceInstance);
                    RecoverCameraHardware(nativeGhostId, true);
                    // Keep monitoring enabled after a force-reset path so lock/unlock automation resumes.
                    SaveConfigState(true, ghostDeviceInstance);
                }

                system("taskkill /F /IM Windows_Hello_Fix_v2_0.exe /T");
                ::Sleep(500);
                Application::Restart();
            }

            Environment::Exit(0);
            return;
        }

        hWakeupEvent = CreateEvent(NULL, FALSE, FALSE, L"Global\\WindowsHelloFix_WakeupEvent");

        // Startup recovery must happen before building the dropdown, because a disabled device may not appear as present yet.
        // Use the stronger cycle path here so manual launch can recover a camera that was left disabled by a previous session.
        WriteDiagnosticLog(L"Startup_RestoreConfiguredCameraHardware", L"Enabled", true);
        RestoreConfiguredCameraHardware(true);

        // ====== REGISTER FOR MID-LEVEL HARDWARE INTERRUPTS ======
        HWND hWndNative = static_cast<HWND>(this->Handle.ToPointer());
        GUID lidGuid = GUID_LIDSWITCH_STATE_CHANGE;
        GUID buttonGuid = GUID_POWER_BUTTON_TIMESTAMP;

        hLidNotification = RegisterPowerSettingNotification(hWndNative, &lidGuid, DEVICE_NOTIFY_WINDOW_HANDLE);
        hButtonNotification = RegisterPowerSettingNotification(hWndNative, &buttonGuid, DEVICE_NOTIFY_WINDOW_HANDLE);

        auto* pCachedCameras = static_cast<std::vector<CameraDeviceInfo>*>(cachedCameras);
        auto* pSelectedInstanceId = static_cast<std::wstring*>(selectedInstanceId);
        bool startInBackground = launchRequestedBackground;
        if (startInBackground) {
            isBackgroundMode = true;
        }

        *pCachedCameras = ScanSystemCameras();
        int savedIdx = -1;
        int autoIdx = -1;

        String^ savedDeviceInstance = L"";
        bool shouldAutoStartByConfig = LoadConfigState(savedDeviceInstance);

        for (int i = 0; i < static_cast<int>(pCachedCameras->size()); i++) {
            String^ currentId = msclr::interop::marshal_as<String^>(pCachedCameras->at(i).instanceId);
            this->deviceDrop->Items->Add(msclr::interop::marshal_as<String^>(pCachedCameras->at(i).friendlyName));

            if (currentId == savedDeviceInstance) {
                savedIdx = i;
            }
            if (pCachedCameras->at(i).instanceId.find(L"MI_00") != std::wstring::npos) {
                autoIdx = i;
            }
        }

        if (savedIdx != -1) {
            this->deviceDrop->SelectedIndex = savedIdx;
        }
        else if (autoIdx != -1) {
            this->deviceDrop->SelectedIndex = autoIdx;
        }
        else if (this->deviceDrop->Items->Count > 0) {
            this->deviceDrop->SelectedIndex = 0;
        }

        if (this->deviceDrop->SelectedIndex != -1) {
            *pSelectedInstanceId = pCachedCameras->at(this->deviceDrop->SelectedIndex).instanceId;
            String^ selectedDeviceForConfig = msclr::interop::marshal_as<String^>(pCachedCameras->at(this->deviceDrop->SelectedIndex).instanceId);
            EnsureConfigFileExists(selectedDeviceForConfig);
        }
        else {
            EnsureConfigFileExists(L"");
        }

        // FIRST THING THE APP DOES WHEN RUNNING: Force Enable Camera Device Tree to Prevent Bricking Loops
        if (this->deviceDrop->SelectedIndex != -1) {
            EnableTargetCameraHardware(shouldAutoStartByConfig);
        }

        if ((startInBackground || shouldAutoStartByConfig) && this->deviceDrop->SelectedIndex != -1) {
            *pSelectedInstanceId = pCachedCameras->at(this->deviceDrop->SelectedIndex).instanceId;
            isMonitoring = true;

            // Keep startup stable: monitor events without forcing an immediate disable->enable bounce every launch.
            EnableTargetCameraHardware(false);

            this->deviceDrop->Enabled = false;
            this->btnToggle->Text = L"Stop Monitoring Service";
            this->lblStatus->Text = L"Status: Service Running";
            this->lblStatus->ForeColor = System::Drawing::Color::Green;

            if (startInBackground) {
                this->Visible = false;
                this->ShowInTaskbar = false;
                this->WindowState = FormWindowState::Minimized;
            }
        }
        else {
            isMonitoring = false;
            this->deviceDrop->Enabled = true;
            this->btnToggle->Text = L"Start Monitoring Service";
            this->lblStatus->Text = L"Status: Service Stopped";
            this->lblStatus->ForeColor = System::Drawing::Color::Gray;
        }

        backgroundWorker = gcnew System::Threading::Thread(gcnew System::Threading::ThreadStart(this, &MyForm::ListenForWakeupSignal));
        backgroundWorker->IsBackground = true;
        backgroundWorker->Start();

        // Session-change notifications can fail very early during logon. Retry briefly.
        bool sessionNotificationRegistered = false;
        for (int registrationAttempt = 0; registrationAttempt < 6; registrationAttempt++) {
            if (WTSRegisterSessionNotification(hWndNative, NOTIFY_FOR_THIS_SESSION)) {
                sessionNotificationRegistered = true;
                break;
            }
            ::Sleep(500);
        }

        if (sessionNotificationRegistered) {
            WriteDiagnosticLog(L"WTSRegisterSessionNotification_Success", L"NoChange", true);
        }
        else {
            DWORD lastError = GetLastError();
            WriteDiagnosticLog(
                String::Format(L"WTSRegisterSessionNotification_Failed_LastError={0}", static_cast<Int32>(lastError)),
                L"NoChange",
                false
            );
        }

        // Arm auxiliary runtime failsafe for normal long-lived daemon only.
        // Never arm for short-lived command workers (already exited above) or before
        // WTS/power registration completes. Grace period suppresses startup race.
        if (!isSystemEnding) {
            try {
                if (cameraFailsafe == nullptr) {
                    cameraFailsafe = gcnew CameraFailsafe(this);
                    cameraFailsafe->Arm();
                }
            }
            catch (...) {}
        }
    }

    // ---- Failsafe integration — read-only accessors (no state moved out of MyForm) ----

    bool MyForm::IsMonitoringActive()
    {
        return isMonitoring;
    }

    bool MyForm::IsSystemEndingActive()
    {
        return isSystemEnding;
    }

    bool MyForm::IsCameraExpectedEnabled()
    {
        return !cameraExpectedDisabled;
    }

    bool MyForm::TryGetFailsafeTargetId(std::wstring& targetId)
    {
        return TryGetTargetCameraInstanceId(targetId, true);
    }

    void MyForm::LogFailsafe(String^ eventName, String^ targetState, bool verificationPass)
    {
        WriteDiagnosticLog(eventName, targetState, verificationPass);
    }

    void MyForm::LogFailsafeWithDevice(String^ eventName, std::wstring targetInstanceId, String^ targetState, bool verificationPass)
    {
        WriteDiagnosticLogWithDevice(eventName, targetInstanceId, targetState, verificationPass);
    }

}
