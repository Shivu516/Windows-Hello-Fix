#include "CameraFailsafe.h"
#include "../core/MyForm.h"
#include <msclr\marshal_cppstd.h>

namespace Windows_Hello_Fix_v2_0 {

    CameraFailsafe::CameraFailsafe(MyForm^ ownerForm)
        : owner(ownerForm)
        , state(WatchdogState::Idle)
        , consecutiveFailures(0)
        , lastRecoveryTick(0)
        , startupGraceUntilTick(0)
        , isArmed(false)
        , pendingOp(nullptr)
        , detectTick(0)
    {
        pollTimer = gcnew System::Windows::Forms::Timer();
        pollTimer->Interval = kIdleIntervalMs;
        pollTimer->Tick += gcnew System::EventHandler(this, &CameraFailsafe::OnPollTick);

        verifyTimer = gcnew System::Windows::Forms::Timer();
        verifyTimer->Interval = kVerifyDelayMs;
        verifyTimer->Tick += gcnew System::EventHandler(this, &CameraFailsafe::OnVerifyTick);
    }

    void CameraFailsafe::Arm()
    {
        if (isArmed) return;
        isArmed = true;
        state = WatchdogState::Idle;
        consecutiveFailures = 0;
        lastRecoveryTick = 0;
        startupGraceUntilTick = GetTickCount64() + kStartupGraceMs;

        owner->LogFailsafeEx(MyForm::DiagLevel::Info, L"FAILSAFE", L"Failsafe_Start",
            owner->NewOperationId(L"FAILSAFE"), L"", L"Enabled", true);

        pollTimer->Interval = kIdleIntervalMs;
        pollTimer->Start();
        verifyTimer->Stop();
    }

    void CameraFailsafe::Disarm()
    {
        if (!isArmed && pollTimer == nullptr && verifyTimer == nullptr) return;
        isArmed = false;
        state = WatchdogState::Idle;
        if (pollTimer != nullptr) pollTimer->Stop();
        if (verifyTimer != nullptr) verifyTimer->Stop();
    }

    bool CameraFailsafe::IsExpectedEnabled()
    {
        if (owner == nullptr) return false;
        if (owner->IsSystemEndingActive()) return false;
        if (!owner->IsMonitoringActive()) return false;
        return owner->IsCameraExpectedEnabled();
    }

    bool CameraFailsafe::TryGetTargetId(std::wstring& targetId)
    {
        if (owner == nullptr) return false;
        return owner->TryGetFailsafeTargetId(targetId);
    }

    void CameraFailsafe::OnPollTick(System::Object^ sender, System::EventArgs^ e)
    {
        if (!isArmed) return;

        ULONGLONG nowTick = GetTickCount64();

        // Startup grace period — do not compete with startup initialization.
        if (nowTick < startupGraceUntilTick) {
            return;
        }

        // Failsafe inactive when monitoring off, shutting down, or expected disabled (lock/suspend).
        if (owner->IsSystemEndingActive()) {
            return;
        }
        if (!owner->IsMonitoringActive()) {
            return;
        }
        if (!IsExpectedEnabled()) {
            return;
        }

        // Cooldown after a successful recovery — prevents immediate repeated recovery.
        if (lastRecoveryTick != 0 && (nowTick - lastRecoveryTick) < (ULONGLONG)kCooldownMs) {
            return;
        }

        // Already pending verification — wait for verify timer.
        if (state == WatchdogState::PendingVerification) {
            return;
        }
        if (state == WatchdogState::Recovering) {
            return;
        }

        std::wstring targetId;
        if (!TryGetTargetId(targetId) || targetId.empty()) {
            return;
        }

        bool isDisabled = false;
        if (!GetCameraHardwareDisabledState(targetId, isDisabled)) {
            return; // query failed — retry next poll
        }
        if (!isDisabled) {
            consecutiveFailures = 0;
            return; // already enabled — nothing to do
        }

        // Unexpected disabled detected → enter PendingVerification, wait ~10 s.
        state = WatchdogState::PendingVerification;
        pendingOp = owner->NewOperationId(L"FAILSAFE");
        detectTick = nowTick;
        owner->LogFailsafeExWithDevice(MyForm::DiagLevel::Info, L"FAILSAFE", L"Failsafe_DetectDisabled", pendingOp,
            L"", targetId, L"Disabled", false);

        verifyTimer->Interval = kVerifyDelayMs;
        verifyTimer->Start();
    }

    void CameraFailsafe::OnVerifyTick(System::Object^ sender, System::EventArgs^ e)
    {
        verifyTimer->Stop();

        if (!isArmed) {
            state = WatchdogState::Idle;
            return;
        }

        ULONGLONG nowTick = GetTickCount64();

        // Re-check all guard conditions — legitimate transition may have occurred during delay.
        if (owner->IsSystemEndingActive()) {
            state = WatchdogState::Idle;
            return;
        }
        if (!owner->IsMonitoringActive()) {
            state = WatchdogState::Idle;
            return;
        }
        if (!IsExpectedEnabled()) {
            owner->LogFailsafeEx(MyForm::DiagLevel::Warn, L"FAILSAFE", L"Failsafe_Skipped_ExpectedDisabled", pendingOp,
                L"", L"NoChange", true);
            state = WatchdogState::Idle;
            return;
        }
        if (nowTick < startupGraceUntilTick) {
            state = WatchdogState::Idle;
            return;
        }
        if (lastRecoveryTick != 0 && (nowTick - lastRecoveryTick) < (ULONGLONG)kCooldownMs) {
            state = WatchdogState::Idle;
            return;
        }

        std::wstring targetId;
        if (!TryGetTargetId(targetId) || targetId.empty()) {
            state = WatchdogState::Idle;
            return;
        }

        bool stillDisabled = false;
        if (!GetCameraHardwareDisabledState(targetId, stillDisabled) || !stillDisabled) {
            // No longer disabled (either query failed or device recovered) — cancel.
            state = WatchdogState::Idle;
            return;
        }

        if (consecutiveFailures >= kMaxRetries) {
            owner->LogFailsafeExWithDevice(MyForm::DiagLevel::Error, L"FAILSAFE", L"Failsafe_MaxRetries", pendingOp,
                L"", targetId, L"Disabled", false);
            // Back off by doubling idle interval temporarily; next poll will reset if recovered elsewhere.
            pollTimer->Interval = kIdleIntervalMs * 2;
            state = WatchdogState::Idle;
            consecutiveFailures = 0;
            return;
        }

        // Recovery: use existing proven pipeline, enable-only, no cycle.
        state = WatchdogState::Recovering;
        owner->LogFailsafeExWithDevice(MyForm::DiagLevel::Info, L"FAILSAFE", L"Failsafe_RecoveryQueued", pendingOp,
            L"", targetId, L"Enabled", true);

        ULONGLONG recoverStart = GetTickCount64();
        bool recoverResult = RecoverCameraHardware(targetId, false);
        bool verified = VerifyCameraHardwareState(targetId, false);
        ULONGLONG durationMs = GetTickCount64() - recoverStart;
        lastRecoveryTick = GetTickCount64();
        ULONGLONG detectToRecoverMs = (detectTick != 0 && recoverStart >= detectTick) ? (recoverStart - detectTick) : 0;

        if (recoverResult && verified) {
            owner->LogFailsafeExWithDevice(MyForm::DiagLevel::Info, L"FAILSAFE", L"Failsafe_Recovered", pendingOp,
                System::String::Format(L"DetectToRecoverMs={0} | DurationMs={1}", (int)detectToRecoverMs, (int)durationMs),
                targetId, L"Enabled", true);
            consecutiveFailures = 0;
            pollTimer->Interval = kIdleIntervalMs;
            state = WatchdogState::Idle;
        }
        else {
            consecutiveFailures++;
            owner->LogFailsafeExWithDevice(MyForm::DiagLevel::Error, L"FAILSAFE", L"Failsafe_RecoveryFailed", pendingOp,
                System::String::Format(L"DetectToRecoverMs={0} | DurationMs={1} | Attempt={2}", (int)detectToRecoverMs, (int)durationMs, consecutiveFailures),
                targetId, L"Disabled", false);

            if (consecutiveFailures < kMaxRetries) {
                // Bounded backoff: 10s, 20s, 40s
                int backoffMs = kVerifyDelayMs * (1 << (consecutiveFailures - 1));
                if (backoffMs > 40000) backoffMs = 40000;
                verifyTimer->Interval = backoffMs;
                verifyTimer->Start();
                state = WatchdogState::PendingVerification;
            }
            else {
                owner->LogFailsafeExWithDevice(MyForm::DiagLevel::Error, L"FAILSAFE", L"Failsafe_MaxRetries", pendingOp,
                    L"", targetId, L"Disabled", false);
                pollTimer->Interval = kIdleIntervalMs * 2;
                state = WatchdogState::Idle;
                consecutiveFailures = 0;
            }
        }
    }

} // namespace Windows_Hello_Fix_v2_0
