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

    // Per-process operation-id counter for log correlation (Op=PREFIX-000123).
    // 32-bit InterlockedIncrement: available on x86/x64/ARM64, including /clr.
    // (Native type: namespace-scope static is fine under /clr.)
    static volatile LONG s_nextOpId = 0;

    static String^ DiagLevelText(MyForm::DiagLevel level) {
        switch (level) {
        case MyForm::DiagLevel::Debug: return L"DEBUG";
        case MyForm::DiagLevel::Warn:  return L"WARN ";
        case MyForm::DiagLevel::Error: return L"ERROR";
        default:                       return L"INFO ";
        }
    }

    static String^ InferCategory(String^ eventName) {
        if (eventName->StartsWith(L"Startup_") || eventName->StartsWith(L"SingleInstance_") || eventName->StartsWith(L"WTS")) return L"STARTUP";
        if (eventName->StartsWith(L"Command_")) return L"COMMAND";
        if (eventName->StartsWith(L"SessionLock_") || eventName->StartsWith(L"SessionUnlock_")) return L"LOCK";
        if (eventName->StartsWith(L"Session")) return L"SESSION";
        if (eventName->StartsWith(L"Power")) return L"POWER";
        if (eventName->StartsWith(L"SystemEnd_")) return L"SYSTEM";
        if (eventName->StartsWith(L"Disable") || eventName->StartsWith(L"Enable")) return L"CAMERA";
        if (eventName->StartsWith(L"Failsafe_") || eventName->StartsWith(L"RecoveryLoop_")) return L"FAILSAFE";
        return L"SYSTEM";
    }

    static MyForm::DiagLevel InferLevel(String^ eventName) {
        if (eventName->IndexOf(L"Fail") >= 0 || eventName->IndexOf(L"NoTarget") >= 0 ||
            eventName->IndexOf(L"MaxRetries") >= 0 || eventName->IndexOf(L"MaxAttempts") >= 0) {
            return MyForm::DiagLevel::Error;
        }
        return MyForm::DiagLevel::Info;
    }

    void MyForm::WriteDiagnosticLogEx(DiagLevel level, String^ category, String^ eventName, String^ opId, String^ details, String^ targetState, bool verificationPass) {
        System::Threading::Monitor::Enter(diagnosticLogSync);
        try {
            if (cachedLogPid == nullptr) {
                cachedLogPid = System::Diagnostics::Process::GetCurrentProcess()->Id.ToString();
            }
            String^ cat = (category != nullptr && category->Length > 0) ? category : InferCategory(eventName);
            if (cat->Length < 8) cat = cat->PadRight(8);
            else if (cat->Length > 8) cat = cat->Substring(0, 8);
            String^ line = String::Format(
                L"[{0}] [{1}] [{2}] {3}",
                DateTime::Now.ToString(L"yyyy-MM-dd HH:mm:ss.fff"),
                DiagLevelText(level),
                cat,
                eventName
            );
            if (opId != nullptr && opId->Length > 0) line += L" | Op=" + opId;
            line += L" | Pid=" + cachedLogPid;
            if (details != nullptr && details->Length > 0) line += L" | " + details;
            line += String::Format(
                L" | Target={0} | Verify={1}",
                targetState,
                verificationPass ? L"PASS" : L"FAIL"
            );
            String^ logPath = GetDiagnosticLogFilePath();
            // Small open-retry: a sibling worker process may hold the file briefly.
            for (int attempt = 0; attempt < 3; attempt++) {
                try {
                    StreamWriter^ sw = gcnew StreamWriter(logPath, true);
                    sw->WriteLine(line);
                    sw->Close();
                    break;
                }
                catch (...) {
                    if (attempt == 2) break;
                    System::Threading::Thread::Sleep(50);
                }
            }
        }
        catch (...) {}
        finally {
            System::Threading::Monitor::Exit(diagnosticLogSync);
        }
    }

    void MyForm::WriteDiagnosticLogExWithDevice(DiagLevel level, String^ category, String^ eventName, String^ opId, String^ details, std::wstring targetInstanceId, String^ targetState, bool verificationPass) {
        String^ deviceId = msclr::interop::marshal_as<String^>(targetInstanceId);
        String^ fullDetails = (details != nullptr && details->Length > 0)
            ? (L"Device=" + deviceId + L" | " + details)
            : (L"Device=" + deviceId);
        WriteDiagnosticLogEx(level, category, eventName, opId, fullDetails, targetState, verificationPass);
    }

    String^ MyForm::NewOperationId(String^ prefix) {
        LONG id = ::InterlockedIncrement(&s_nextOpId);
        return String::Format(L"{0}-{1:D6}", prefix, id);
    }

    void MyForm::WriteDiagnosticLog(String^ eventName, String^ targetState, bool verificationPass) {
        WriteDiagnosticLogEx(InferLevel(eventName), InferCategory(eventName), eventName, L"", L"", targetState, verificationPass);
    }

    void MyForm::WriteDiagnosticLogWithDevice(String^ eventName, std::wstring targetInstanceId, String^ targetState, bool verificationPass) {
        WriteDiagnosticLogExWithDevice(InferLevel(eventName), InferCategory(eventName), eventName, L"", L"", targetInstanceId, targetState, verificationPass);
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
