#include "MyForm.h"

namespace Windows_Hello_Fix_v2_0 {

    String^ MyForm::GetConfigFilePath() {
        String^ dir = Path::Combine(
            Environment::GetFolderPath(Environment::SpecialFolder::ApplicationData),
            L"Windows Hello Fix"
        );

        Directory::CreateDirectory(dir);
        return Path::Combine(dir, L"config.txt");
    }

    String^ MyForm::GetDiagnosticLogFilePath() {
        String^ configPath = GetConfigFilePath();
        String^ configDirectory = Path::GetDirectoryName(configPath);

        if (String::IsNullOrEmpty(configDirectory)) {
            configDirectory = Path::Combine(
                Environment::GetFolderPath(Environment::SpecialFolder::ApplicationData),
                L"Windows Hello Fix"
            );
        }

        Directory::CreateDirectory(configDirectory);
        return Path::Combine(configDirectory, L"diagnostic.log");
    }

    void MyForm::WriteDiagnosticLog(String^ eventName, String^ targetState, bool verificationPass) {
        System::Threading::Monitor::Enter(diagnosticLogSync);
        try {
            String^ logPath = GetDiagnosticLogFilePath();
            StreamWriter^ sw = gcnew StreamWriter(logPath, true);
            String^ timestamp = DateTime::Now.ToString(L"yyyy-MM-dd HH:mm:ss.fff");
            sw->WriteLine(
                String::Format(
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
            System::Threading::Monitor::Exit(diagnosticLogSync);
        }
    }

    void MyForm::WriteDiagnosticLogWithDevice(String^ eventName, std::wstring targetInstanceId, String^ targetState, bool verificationPass) {
        String^ deviceId = msclr::interop::marshal_as<String^>(targetInstanceId);
        WriteDiagnosticLog(
            eventName + L" | Device=" + deviceId,
            targetState,
            verificationPass
        );
    }

    void MyForm::SaveConfigState(bool monitoring, String^ deviceInstanceId) {
        try {
            String^ path = GetConfigFilePath();
            StreamWriter^ sw = gcnew StreamWriter(path, false);
            sw->WriteLine(monitoring ? L"monitoring=1" : L"monitoring=0");
            sw->WriteLine(L"device=" + deviceInstanceId);
            sw->Close();
        }
        catch (...) {}
    }

    bool MyForm::LoadConfigState([System::Runtime::InteropServices::Out] String^% deviceInstanceId) {
        deviceInstanceId = L"";
        try {
            String^ path = GetConfigFilePath();
            if (!File::Exists(path)) {
                return false;
            }

            StreamReader^ sr = gcnew StreamReader(path);
            String^ line1 = sr->ReadLine();
            String^ line2 = sr->ReadLine();
            sr->Close();

            bool monitoringActive = (line1 != nullptr && line1->Trim() == L"monitoring=1");
            if (line2 != nullptr && line2->StartsWith(L"device=")) {
                // FIX: Trim prevents \r\n newline corruption in C++ string matching
                std::wstring rawPath = msclr::interop::marshal_as<std::wstring>(line2->Substring(7)->Trim());
                std::wstring sanitizedPath = TrimTrailingChars(rawPath);
                deviceInstanceId = msclr::interop::marshal_as<String^>(sanitizedPath);
            }
            return monitoringActive;
        }
        catch (...) {
            return false;
        }
    }

    void MyForm::EnsureConfigFileExists(String^ deviceInstanceId) {
        try {
            String^ path = GetConfigFilePath();
            if (!File::Exists(path)) {
                SaveConfigState(false, deviceInstanceId);
            }
        }
        catch (...) {}
    }

    bool MyForm::TryGetTargetCameraInstanceId(std::wstring& targetInstanceId, bool preferCurrentSelection) {
        targetInstanceId.clear();

        auto* pSelectedInstanceId = static_cast<std::wstring*>(selectedInstanceId);

        if (preferCurrentSelection && pSelectedInstanceId && !pSelectedInstanceId->empty()) {
            targetInstanceId = *pSelectedInstanceId;
            return true;
        }

        String^ savedDeviceInstance = L"";
        LoadConfigState(savedDeviceInstance);
        if (!String::IsNullOrEmpty(savedDeviceInstance)) {
            targetInstanceId = msclr::interop::marshal_as<std::wstring>(savedDeviceInstance);
            return true;
        }

        if (!preferCurrentSelection && pSelectedInstanceId && !pSelectedInstanceId->empty()) {
            targetInstanceId = *pSelectedInstanceId;
            return true;
        }

        std::vector<CameraDeviceInfo> cameras = ScanSystemCameras();
        for (size_t i = 0; i < cameras.size(); i++) {
            if (cameras[i].instanceId.find(L"MI_00") != std::wstring::npos) {
                targetInstanceId = cameras[i].instanceId;
                return true;
            }
        }

        if (!cameras.empty()) {
            targetInstanceId = cameras[0].instanceId;
            return true;
        }

        return false;
    }

}
