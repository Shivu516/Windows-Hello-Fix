#include "RecoveryLoopFailsafe.h"
#include "../core/MyForm.h"

namespace Windows_Hello_Fix_v2_0 {

    RecoveryLoopFailsafe::RecoveryLoopFailsafe(MyForm^ ownerForm)
        : owner(ownerForm)
        , state(RecoveryState::Idle)
        , consecutiveFailures(0)
        , lastRecoveryTick(0)
        , isArmed(false)
        , pendingOp(nullptr)
        , detectTick(0)
    {
        pollTimer = gcnew System::Windows::Forms::Timer();
        pollTimer->Interval = kPollIntervalMs;
        pollTimer->Tick += gcnew System::EventHandler(this, &RecoveryLoopFailsafe::OnPollTick);

        retryTimer = gcnew System::Windows::Forms::Timer();
        retryTimer->Interval = kRetryIntervalMs;
        retryTimer->Tick += gcnew System::EventHandler(this, &RecoveryLoopFailsafe::OnRetryTick);

        startupTimer = gcnew System::Windows::Forms::Timer();
        startupTimer->Interval = kStartupVerifyMs;
        startupTimer->Tick += gcnew System::EventHandler(this, &RecoveryLoopFailsafe::OnStartupTick);
    }

    RecoveryLoopFailsafe::~RecoveryLoopFailsafe()
    {
        Disarm();
        this->!RecoveryLoopFailsafe();
    }

    RecoveryLoopFailsafe::!RecoveryLoopFailsafe()
    {
        try { Disarm(); } catch (...) {}
    }

    void RecoveryLoopFailsafe::Arm()
    {
        if (isArmed) return;
        if (owner != nullptr && owner->IsSystemEndingActive()) return;

        isArmed = true;
        state = RecoveryState::Idle;
        consecutiveFailures = 0;
        lastRecoveryTick = 0;

        try { owner->LogFailsafeEx(MyForm::DiagLevel::Info, L"FAILSAFE", L"RecoveryLoop_Start", owner->NewOperationId(L"RECLOOP"), L"", L"Enabled", true); } catch (...) {}

        pollTimer->Interval = kPollIntervalMs;
        pollTimer->Start();
        retryTimer->Stop();
        startupTimer->Interval = kStartupVerifyMs;
        startupTimer->Start();
    }

    void RecoveryLoopFailsafe::Disarm()
    {
        isArmed = false;
        state = RecoveryState::Idle;
        try { if (pollTimer != nullptr) pollTimer->Stop(); } catch (...) {}
        try { if (retryTimer != nullptr) retryTimer->Stop(); } catch (...) {}
        try { if (startupTimer != nullptr) startupTimer->Stop(); } catch (...) {}
    }

    void RecoveryLoopFailsafe::OnOwnerLoad(System::Object^ /*sender*/, System::EventArgs^ /*e*/)
    {
        try { Arm(); } catch (...) {}
    }

    void RecoveryLoopFailsafe::OnOwnerClosing(System::Object^ /*sender*/, System::Windows::Forms::FormClosingEventArgs^ /*e*/)
    {
        try { Disarm(); } catch (...) {}
    }

    bool RecoveryLoopFailsafe::IsExpectedEnabled()
    {
        if (owner == nullptr) return false;
        if (owner->IsSystemEndingActive()) return false;
        if (!owner->IsMonitoringActive()) return false;
        return owner->IsCameraExpectedEnabled();
    }

    bool RecoveryLoopFailsafe::TryGetTargetId(std::wstring& targetId)
    {
        if (owner == nullptr) return false;
        return owner->TryGetFailsafeTargetId(targetId);
    }

    void RecoveryLoopFailsafe::RequestRecoveryCheck(const wchar_t* /*reason*/)
    {
        if (!isArmed) return;
        if (owner != nullptr && owner->IsSystemEndingActive()) return;
        if (owner != nullptr && !owner->IsMonitoringActive()) return;
        if (!IsExpectedEnabled()) return;
        if (state == RecoveryState::PendingVerification || state == RecoveryState::Recovering) return;

        ULONGLONG nowTick = GetTickCount64();
        if (lastRecoveryTick != 0 && (nowTick - lastRecoveryTick) < (ULONGLONG)kCooldownMs) return;

        std::wstring targetId;
        if (!TryGetTargetId(targetId) || targetId.empty()) return;

        bool isDisabled = false;
        if (!GetCameraHardwareDisabledState(targetId, isDisabled)) return;
        if (!isDisabled) {
            consecutiveFailures = 0;
            return;
        }

        state = RecoveryState::PendingVerification;
        pendingOp = owner->NewOperationId(L"RECLOOP");
        detectTick = nowTick;
        try { owner->LogFailsafeExWithDevice(MyForm::DiagLevel::Info, L"FAILSAFE", L"RecoveryLoop_DisabledDetected", pendingOp, L"", targetId, L"Disabled", false); } catch (...) {}

        retryTimer->Interval = kRetryIntervalMs;
        retryTimer->Start();
    }

    void RecoveryLoopFailsafe::OnStartupTick(System::Object^ /*sender*/, System::EventArgs^ /*e*/)
    {
        startupTimer->Stop();
        if (!isArmed) return;

        try { owner->LogFailsafeEx(MyForm::DiagLevel::Debug, L"FAILSAFE", L"RecoveryLoop_StartupVerification", L"", L"", L"NoChange", true); } catch (...) {}

        RequestRecoveryCheck(L"StartupVerification");
    }

    void RecoveryLoopFailsafe::OnPollTick(System::Object^ /*sender*/, System::EventArgs^ /*e*/)
    {
        if (!isArmed) return;
        if (owner != nullptr && owner->IsSystemEndingActive()) return;
        if (owner != nullptr && !owner->IsMonitoringActive()) return;
        if (!IsExpectedEnabled()) return;

        ULONGLONG nowTick = GetTickCount64();
        if (lastRecoveryTick != 0 && (nowTick - lastRecoveryTick) < (ULONGLONG)kCooldownMs) return;
        if (state == RecoveryState::PendingVerification || state == RecoveryState::Recovering) return;

        std::wstring targetId;
        if (!TryGetTargetId(targetId) || targetId.empty()) return;

        bool isDisabled = false;
        if (!GetCameraHardwareDisabledState(targetId, isDisabled)) return;
        if (!isDisabled) {
            consecutiveFailures = 0;
            return;
        }

        state = RecoveryState::PendingVerification;
        pendingOp = owner->NewOperationId(L"RECLOOP");
        detectTick = nowTick;
        try { owner->LogFailsafeExWithDevice(MyForm::DiagLevel::Info, L"FAILSAFE", L"RecoveryLoop_DisabledDetected", pendingOp, L"", targetId, L"Disabled", false); } catch (...) {}

        retryTimer->Interval = kRetryIntervalMs;
        retryTimer->Start();
    }

    void RecoveryLoopFailsafe::OnRetryTick(System::Object^ /*sender*/, System::EventArgs^ /*e*/)
    {
        retryTimer->Stop();

        if (!isArmed) { state = RecoveryState::Idle; return; }

        ULONGLONG nowTick = GetTickCount64();

        if (owner != nullptr && owner->IsSystemEndingActive()) {
            try { owner->LogFailsafeEx(MyForm::DiagLevel::Warn, L"FAILSAFE", L"RecoveryLoop_SkippedShutdown", pendingOp, L"", L"NoChange", true); } catch (...) {}
            state = RecoveryState::Idle;
            return;
        }
        if (owner != nullptr && !owner->IsMonitoringActive()) {
            try { owner->LogFailsafeEx(MyForm::DiagLevel::Warn, L"FAILSAFE", L"RecoveryLoop_SkippedMonitoringOff", pendingOp, L"", L"NoChange", true); } catch (...) {}
            state = RecoveryState::Idle;
            return;
        }
        if (!IsExpectedEnabled()) {
            try { owner->LogFailsafeEx(MyForm::DiagLevel::Warn, L"FAILSAFE", L"RecoveryLoop_SkippedExpectedDisabled", pendingOp, L"", L"NoChange", true); } catch (...) {}
            state = RecoveryState::Idle;
            return;
        }
        if (lastRecoveryTick != 0 && (nowTick - lastRecoveryTick) < (ULONGLONG)kCooldownMs) {
            state = RecoveryState::Idle;
            return;
        }

        std::wstring targetId;
        if (!TryGetTargetId(targetId) || targetId.empty()) { state = RecoveryState::Idle; return; }

        bool stillDisabled = false;
        if (!GetCameraHardwareDisabledState(targetId, stillDisabled) || !stillDisabled) {
            consecutiveFailures = 0;
            state = RecoveryState::Idle;
            return;
        }

        if (consecutiveFailures >= kMaxRetries) {
            try { owner->LogFailsafeExWithDevice(MyForm::DiagLevel::Error, L"FAILSAFE", L"RecoveryLoop_MaxAttempts", pendingOp, L"", targetId, L"Disabled", false); } catch (...) {}
            state = RecoveryState::Idle;
            consecutiveFailures = 0;
            return;
        }

        state = RecoveryState::Recovering;
        try { owner->LogFailsafeExWithDevice(MyForm::DiagLevel::Info, L"FAILSAFE", L"RecoveryLoop_EnableAttempt", pendingOp, L"", targetId, L"Enabled", true); } catch (...) {}

        ULONGLONG recoverStart = GetTickCount64();
        bool recoverResult = RecoverCameraHardware(targetId, false);
        bool verified = VerifyCameraHardwareState(targetId, false);
        ULONGLONG durationMs = GetTickCount64() - recoverStart;
        lastRecoveryTick = GetTickCount64();
        ULONGLONG detectToRecoverMs = (detectTick != 0 && recoverStart >= detectTick) ? (recoverStart - detectTick) : 0;

        if (recoverResult && verified) {
            try {
                owner->LogFailsafeExWithDevice(MyForm::DiagLevel::Info, L"FAILSAFE", L"RecoveryLoop_Recovered", pendingOp,
                    System::String::Format(L"DetectToRecoverMs={0} | DurationMs={1}", (int)detectToRecoverMs, (int)durationMs),
                    targetId, L"Enabled", true);
            } catch (...) {}
            consecutiveFailures = 0;
            state = RecoveryState::Idle;
        } else {
            consecutiveFailures++;
            try {
                owner->LogFailsafeExWithDevice(MyForm::DiagLevel::Error, L"FAILSAFE", L"RecoveryLoop_RecoveryFailed", pendingOp,
                    System::String::Format(L"DetectToRecoverMs={0} | DurationMs={1} | Attempt={2}", (int)detectToRecoverMs, (int)durationMs, consecutiveFailures),
                    targetId, L"Disabled", false);
            } catch (...) {}

            if (consecutiveFailures < kMaxRetries) {
                retryTimer->Interval = kRetryIntervalMs;
                retryTimer->Start();
                state = RecoveryState::PendingVerification;
            } else {
                try { owner->LogFailsafeExWithDevice(MyForm::DiagLevel::Error, L"FAILSAFE", L"RecoveryLoop_MaxAttempts", pendingOp, L"", targetId, L"Disabled", false); } catch (...) {}
                state = RecoveryState::Idle;
                consecutiveFailures = 0;
            }
        }
    }

} // namespace Windows_Hello_Fix_v2_0
