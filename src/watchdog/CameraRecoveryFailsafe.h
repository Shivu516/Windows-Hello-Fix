#pragma once

// CameraRecoveryFailsafe — camera-stack health watchdog for Issue #1.
//
// Detects the failure state the PnP-only watchdogs cannot see:
//     PnP/device state = Enabled   BUT   Windows camera stack = unusable
// (Camera app 0xA00F4241 (0xC00D7167 = MF_E_REBOOT_REQUIRED); see
// docs/Issue1_Camera_Recovery_Research.md).
//
// Responsibilities (watchdog side): detection (Media Foundation health check),
// classification, recovery orchestration/escalation, retry/cooldown state, timers,
// logging. All actual device/system state changes are performed by src/core:
//     Rung 1  RestartCameraHardware()            (DICS_PROPCHANGE devnode restart)
//     Rung 2  RestartCameraFrameServerService()  (SCM restart of "FrameServer")
//     Rung 3  RecoverCameraHardware(target,true) (existing full-cycle pipeline)
//     Rung 4  ReenumerateCameraParent()          (composite-parent re-enumeration)
// The watchdog contains no SetupAPI/CfgMgr/SCM implementation of its own and never
// disables the camera on its own authority; it never runs while the camera is
// expected-disabled (lock/suspend/shutdown), never reboots, and never loops:
// bounded rungs per incident, then parking. See docs/CameraRecoveryFailsafe.md.

#include <windows.h>
#include <string>

namespace Windows_Hello_Fix_v2_0 {

    ref class MyForm;

    // Health-check verdict (native enum shared by the worker context and the state machine).
    enum class CameraHealthStatus {
        Healthy,        // first video frame obtained — camera stack works
        NotFound,       // configured target not enumerable by the camera stack
        PrivacyDenied,  // OS denies camera access — never a recovery candidate
        Busy,           // another client holds the camera — device responsive, never recovered over
        Wedged,         // init/stream failure consistent with Issue #1 — recovery candidate
        Timeout,        // health check exceeded its hard budget — recovery candidate
        Error           // camera-stack infrastructure failure — recovery candidate
    };

    public ref class CameraRecoveryFailsafe
    {
    private:
        MyForm^ owner;

        System::Windows::Forms::Timer^ transitionTimer;  // 10 s poll: expected/PnP enable transitions
        System::Windows::Forms::Timer^ backupTimer;      // 10 min periodic backup check
        System::Windows::Forms::Timer^ startupTimer;     // 15 s one-shot initial check after Arm
        System::Windows::Forms::Timer^ resultTimer;      // 500 ms worker-completion poll (while a worker is out)
        System::Windows::Forms::Timer^ gapTimer;         // one-shot settle / recheck gap

        enum class RecoveryState {
            Idle,
            Checking,     // health-check worker in flight
            Recovering    // recovery-rung worker in flight
        };

        RecoveryState state;
        int currentRung;                       // 0 = no incident; 1..4 = ladder position
        System::String^ incidentOp;            // CAMREC operation id for the current incident
        ULONGLONG incidentStartTick;
        ULONGLONG workerDeadlineTick;
        CameraHealthStatus lastHealthStatus;
        HRESULT lastHealthHr;
        ULONGLONG lastCheckTick;               // routine-check cooldown anchor
        ULONGLONG parkedUntilTick;             // exhaustion parking (cleared by next expected-disabled episode)
        bool prevExpectedDisabledObserved;     // enables the unlock/resume transition trigger
        bool notFoundRecheckQueued;            // one delayed re-check for NotFound, never more
        bool isArmed;

        enum class GapAction { None, PostRungCheck, NotFoundRecheck };
        GapAction pendingGapAction;

        // Native worker plumbing — raw void* like MyForm's cachedCameras/selectedInstanceId.
        // On budget exhaustion the context/event/handle are intentionally abandoned to the
        // worker (never killed mid-call): it self-cleans if it ever completes, otherwise
        // leaks by design. See docs/CameraRecoveryFailsafe.md §Threading.
        void* healthContext;                   // CameraHealthWorkContext*
        void* healthThread;                    // HANDLE
        void* rungContext;                     // CameraRungWorkContext*
        void* rungThread;                      // HANDLE

        // Timing constants — see docs/CameraRecoveryFailsafe.md §Timing.
        static const int kTransitionPollMs = 10000;        // 10 s enable-transition poll
        static const int kBackupPollMs = 600000;           // 10 min periodic backup
        static const int kStartupCheckMs = 15000;          // 15 s after Arm
        static const int kResultPollMs = 500;              // worker completion poll
        static const int kHealthBudgetMs = 10500;          // ~10 s hard budget (HLK-style)
        static const int kRung1BudgetMs = 20000;           // DICS_PROPCHANGE restart
        static const int kRung2BudgetMs = 40000;           // service restart (2×10 s bounded waits + slack)
        static const int kRung3BudgetMs = 25000;           // existing full cycle
        static const int kRung4BudgetMs = 25000;           // parent re-enumeration (incl. 1 s settle)
        static const int kGapMs = 5000;                    // settle between rung and follow-up check
        static const int kNotFoundRecheckMs = 15000;
        static const ULONGLONG kCheckCooldownMs = 60000;   // min spacing between routine checks
        static const ULONGLONG kParkMs = 1800000;          // 30 min parking after exhaustion

        void OnTransitionTick(System::Object^ sender, System::EventArgs^ e);
        void OnBackupTick(System::Object^ sender, System::EventArgs^ e);
        void OnStartupTick(System::Object^ sender, System::EventArgs^ e);
        void OnResultTick(System::Object^ sender, System::EventArgs^ e);
        void OnGapTick(System::Object^ sender, System::EventArgs^ e);

        bool IsExpectedEnabled();
        bool TryGetTargetId(std::wstring& targetId);
        void RequestHealthCheck(System::String^ trigger, bool bypassCooldown);
        void SpawnHealthWorker(std::wstring targetId, System::String^ trigger);
        void HandleHealthResult(CameraHealthStatus status, HRESULT hr);
        void HandleRungResult(bool ok);
        void AdvanceOrExhaust();
        void AdvanceToRung(int rung);
        void AbortIncident(System::String^ reason);
        void ParkAfterExhaustion();
        void OrphanHealthWorker();
        void OrphanRungWorker();

    public:
        CameraRecoveryFailsafe(MyForm^ ownerForm);
        ~CameraRecoveryFailsafe();
        !CameraRecoveryFailsafe();

        void Arm();
        void Disarm();

        // Hooks for main.cpp Load / FormClosing (same pattern as RecoveryLoopFailsafe).
        void OnOwnerLoad(System::Object^ sender, System::EventArgs^ e);
        void OnOwnerClosing(System::Object^ sender, System::Windows::Forms::FormClosingEventArgs^ e);
    };

} // namespace Windows_Hello_Fix_v2_0
