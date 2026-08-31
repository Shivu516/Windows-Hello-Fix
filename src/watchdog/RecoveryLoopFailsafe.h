#pragma once

// RecoveryLoopFailsafe — short-term enable-only recovery coordinator.
// Lives outside src/core (owned by main.cpp), observes ExpectedEnabled vs observed Disabled,
// verifies, then recovers through existing src/core pipeline with bounded coalesced retries.
// Enable-only: never disables camera. No SetupAPI/CfgMgr reimplementation.
// Driven by WinForms timers on UI message pump. PnP notification is optional polling fallback;
// current implementation uses timers only to avoid native/managed interop complexity while
// preserving the 5s startup + 30s poll + 5s retry contract (poll worst 30+5=35s; startup task covers gap).
// See docs/Plan.md §9, §11, §13-15, §18-22.

#include <windows.h>
#include <string>

namespace Windows_Hello_Fix_v2_0 {

    ref class MyForm;

    public ref class RecoveryLoopFailsafe
    {
    private:
        MyForm^ owner;

        System::Windows::Forms::Timer^ pollTimer;
        System::Windows::Forms::Timer^ retryTimer;
        System::Windows::Forms::Timer^ startupTimer;

        enum class RecoveryState {
            Idle,
            PendingVerification,
            Recovering
        };

        RecoveryState state;
        int consecutiveFailures;
        ULONGLONG lastRecoveryTick;
        bool isArmed;

        // Timing — per docs/Plan.md §12-14 (5s startup, 30s poll, 5s retry, 3 attempts, 30s cooldown)
        static const int kStartupVerifyMs = 5000;   // 5 s after Arm
        static const int kPollIntervalMs = 30000;   // 30 s periodic backup
        static const int kRetryIntervalMs = 5000;   // 5 s between retries
        static const int kCooldownMs = 30000;       // 30 s after success
        static const int kMaxRetries = 3;

        void OnPollTick(System::Object^ sender, System::EventArgs^ e);
        void OnRetryTick(System::Object^ sender, System::EventArgs^ e);
        void OnStartupTick(System::Object^ sender, System::EventArgs^ e);

        bool IsExpectedEnabled();
        bool TryGetTargetId(std::wstring& targetId);
        void RequestRecoveryCheck(const wchar_t* reason);

    public:
        RecoveryLoopFailsafe(MyForm^ ownerForm);
        ~RecoveryLoopFailsafe();
        !RecoveryLoopFailsafe();

        void Arm();
        void Disarm();

        // Hooks for main.cpp Load / FormClosing (avoids src/core edit)
        void OnOwnerLoad(System::Object^ sender, System::EventArgs^ e);
        void OnOwnerClosing(System::Object^ sender, System::Windows::Forms::FormClosingEventArgs^ e);
    };

} // namespace Windows_Hello_Fix_v2_0
