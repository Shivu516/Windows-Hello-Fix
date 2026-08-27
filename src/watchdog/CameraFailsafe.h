#pragma once

// Lightweight runtime camera-state failsafe.
// Lives outside src/core — observes ExpectedEnabled vs observed Disabled,
// waits for confirmation, then recovers through existing src/core camera pipeline.
// No SetupAPI/CfgMgr reimplementation, no GUI manipulation, no new threads.

#include <windows.h>
#include <string>

namespace Windows_Hello_Fix_v2_0 {

    // Forward-declare owning form to avoid circular include at header level.
    ref class MyForm;

    // CameraFailsafe — managed watchdog owned by MyForm, driven by WinForms timers
    // on the UI message pump. See docs/Plan.md for state/timing design.
    public ref class CameraFailsafe
    {
    private:
        MyForm^ owner;

        System::Windows::Forms::Timer^ pollTimer;
        System::Windows::Forms::Timer^ verifyTimer;

        enum class WatchdogState {
            Idle,
            PendingVerification,
            Recovering
        };

        WatchdogState state;
        int consecutiveFailures;
        ULONGLONG lastRecoveryTick;
        ULONGLONG startupGraceUntilTick;
        bool isArmed;

        // Timing constants — see docs/Plan.md §7-8, §13, §16-17
        static const int kIdleIntervalMs = 90000;        // 90 s idle poll
        static const int kVerifyDelayMs = 10000;         // 10 s confirmation
        static const int kCooldownMs = 30000;            // 30 s after success
        static const int kMaxRetries = 3;                // bounded retries
        static const ULONGLONG kStartupGraceMs = 45000;  // 45 s startup grace

        void OnPollTick(System::Object^ sender, System::EventArgs^ e);
        void OnVerifyTick(System::Object^ sender, System::EventArgs^ e);

        bool IsExpectedEnabled();
        bool TryGetTargetId(std::wstring& targetId);

    public:
        CameraFailsafe(MyForm^ ownerForm);

        void Arm();
        void Disarm();
    };

} // namespace Windows_Hello_Fix_v2_0
