# Function Index

Grouped by source file. "Called by" lists the main callers in the current code.

## `src/core/MyForm_Camera.cpp`

| Function | Responsibility | Called by |
|---|---|---|
| `TrimTrailingChars` | Strip trailing CR/LF/space from wstring | `LoadConfigState` |
| `IsCurrentProcessElevatedNative` | Token elevation check (logging) | `Disable/EnableTargetCameraHardware`, `MyForm_Load` |
| `GetCurrentProcessIntegrityRid` | Token integrity RID (logging) | `Disable/EnableTargetCameraHardware`, `MyForm_Load` |
| `ScanSystemCameras` | Enumerate present Camera/Image devices | `TryGetTargetCameraInstanceId`, `RestoreAllCameraHardware`, `MyForm_Load` (ghost reset) |
| `ToggleCameraHardware` | SetupAPI enable/disable by instance ID | `SetCameraHardwareStateVerified` |
| `LocateCameraDevInst` | Resolve instance ID → DEVINST | `ToggleCameraHardwareCfgMgr` |
| `ToggleCameraHardwareCfgMgr` | CfgMgr enable/disable + reenumerate | `SetCameraHardwareStateVerified` |
| `GetCameraHardwareDisabledState` | True device disabled state (config flag or problem code) | `Disable/EnableTargetCameraHardware`, `VerifyCameraHardwareState`, both watchdogs (observe) |
| `VerifyCameraHardwareState` | Retry×3 check of disabled state (100 ms) | `SetCameraHardwareStateVerified`, `Disable/EnableTargetCameraHardware`, both watchdogs (confirm) |
| `TryEnterHardwareToggleCooldown` | Interlocked cooldown claim (currently unused in main flow) | (none in current flow) |
| `RecordHardwareToggleTime` | Stamp global toggle tick | `SetCameraHardwareStateVerified` |
| `SetCameraHardwareStateVerified` | Core verified set/retry/recover orchestrator | `Disable/EnableTargetCameraHardware`, `RecoverCameraHardware` |
| `RecoverCameraHardware` | Enable (+ optional cycle: 350/900/500 ms sleeps) | `RestoreConfiguredCameraHardware`, `RestoreAllCameraHardware`, ghost reset, both watchdogs (`cycle=false` only) |
| `RestoreAllCameraHardware` | Recover every scanned camera | `RestoreConfiguredCameraHardware` |
| `MyForm::DisableTargetCameraHardware` | Member: disable target w/ logging | `WndProc`, `MyForm_Load` (command), `~MyForm`, `!MyForm` |
| `MyForm::EnableTargetCameraHardware` | Member: enable target w/ logging | `WndProc`, `btnToggle_Click`, `~MyForm`, `!MyForm` |
| `MyForm::RestoreConfiguredCameraHardware` | Recover saved device or all | `MyForm_Load`, `~MyForm` (no selection branch) |

## `src/core/MyForm_Config.cpp`

| Function | Responsibility | Called by |
|---|---|---|
| `GetConfigFilePath` | `%APPDATA%\Windows Hello Fix\config.txt` | many |
| `GetDiagnosticLogFilePath` | `%APPDATA%\Windows Hello Fix\diagnostic.log` | `WriteDiagnosticLog` |
| `WriteDiagnosticLog` | Append timestamped diagnostic line (monitor-locked) | camera/event/system members |
| `WriteDiagnosticLogWithDevice` | Diagnostic line with device ID | `Disable/EnableTargetCameraHardware` |
| `SaveConfigState` | Write `monitoring=` + `device=` | `btnToggle_Click`, `~MyForm`, `MyForm_Load` (ghost reset) |
| `LoadConfigState` | Read config; out device ID (trimmed) | `TryGetTargetCameraInstanceId`, `MyForm_Load`, ghost reset |
| `EnsureConfigFileExists` | Create default config if missing | `MyForm_Load` |
| `TryGetTargetCameraInstanceId` | Resolve target camera (selection→config→MI_00→first) | `Disable/EnableTargetCameraHardware`, `MyForm_Load` (command verify) |

## `src/core/MyForm_Core.cpp`

| Function | Responsibility | Called by |
|---|---|---|
| `MyForm::MyForm` | Constructor; init all state; `InitializeComponent` | runtime (new MyForm) |
| `MyForm::~MyForm` | Destructor; final camera state + handle cleanup | runtime |
| `MyForm::!MyForm` | Finalizer; same cleanup minus SaveConfig | GC |
| `MyForm::InitializeComponent` | Build WinForms UI; wire events | ctor |
| `MyForm::MyForm_Load` | Full startup sequence | `Load` event |
| `MyForm::IsMonitoringActive` | Read-only: `isMonitoring` | both watchdogs (expected-state guard) |
| `MyForm::IsSystemEndingActive` | Read-only: `isSystemEnding` | both watchdogs (shutdown guard) |
| `MyForm::IsCameraExpectedEnabled` | Read-only: `!cameraExpectedDisabled` | both watchdogs (expected-state guard) |
| `MyForm::TryGetFailsafeTargetId` | Read-only: target via `TryGetTargetCameraInstanceId(..., true)` | both watchdogs (target resolution) |
| `MyForm::LogFailsafe` | Forward to `WriteDiagnosticLog` | both watchdogs (logging) |
| `MyForm::LogFailsafeWithDevice` | Forward to `WriteDiagnosticLogWithDevice` | both watchdogs (logging) |

## `src/core/MyForm_Events.cpp`

| Function | Responsibility | Called by |
|---|---|---|
| `MyForm::WndProc` | Dispatch shutdown/power/session messages | Windows message pump |

## `src/core/MyForm_System.cpp`

| Function | Responsibility | Called by |
|---|---|---|
| `MyForm::IsRestoreCameraCommand` | Detect enable/restore verbs | `MyForm_Load` |
| `MyForm::IsDisableCameraCommand` | Detect disable verb | `MyForm_Load` |
| `MyForm::ListenForWakeupSignal` | Block on wake event; raise window | `backgroundWorker` thread |
| `MyForm::BringWindowToFrontDelegate` | Show/activate window | `ListenForWakeupSignal` |

## `src/core/MyForm_UI.cpp`

| Function | Responsibility | Called by |
|---|---|---|
| `MyForm::MyForm_FormClosing` | Cancel close → minimize to background | `FormClosing` event |
| `MyForm::btnToggle_Click` | Start/stop monitoring; enable camera on stop | button `Click` event |

## `src/watchdog/CameraFailsafe.cpp` (owned by `MyForm`)

| Function | Responsibility | Called by |
|---|---|---|
| `CameraFailsafe::CameraFailsafe` | Bind owner; create poll (90 s) + verify (10 s) timers, stopped | `MyForm_Load` (`gcnew`) |
| `CameraFailsafe::Arm` | Reset state; 45 s grace stamp; log `Failsafe_Start`; start poll | `MyForm_Load` |
| `CameraFailsafe::Disarm` | Clear armed; stop both timers | `~MyForm`, `!MyForm` |
| `CameraFailsafe::IsExpectedEnabled` | monitoring && !systemEnding && !expectedDisabled | `OnPollTick`, `OnVerifyTick` |
| `CameraFailsafe::TryGetTargetId` | Target via `TryGetFailsafeTargetId` | `OnPollTick`, `OnVerifyTick` |
| `CameraFailsafe::OnPollTick` | 90 s detect → `PendingVerification` + 10 s verify timer | `pollTimer` |
| `CameraFailsafe::OnVerifyTick` | Re-check guards → `Recover(false)` + verify → log / backoff 10→20→40 s / max-retries | `verifyTimer` |

## `src/watchdog/RecoveryLoopFailsafe.cpp` (owned by `main.cpp`)

| Function | Responsibility | Called by |
|---|---|---|
| `RecoveryLoopFailsafe::RecoveryLoopFailsafe` | Bind owner; create startup (5 s) + poll (30 s) + retry (5 s) timers, stopped | `main.cpp` (`gcnew`) |
| `RecoveryLoopFailsafe::~RecoveryLoopFailsafe` | `Disarm()` + finalizer | CLR dispose |
| `RecoveryLoopFailsafe::!RecoveryLoopFailsafe` | Best-effort `Disarm()` | GC |
| `RecoveryLoopFailsafe::Arm` | Reset state (no grace); log `RecoveryLoop_Start`; start poll + startup timers | `OnOwnerLoad` |
| `RecoveryLoopFailsafe::Disarm` | Clear armed; stop all three timers | `OnOwnerClosing`, dtor/finalizer, `main.cpp` |
| `RecoveryLoopFailsafe::OnOwnerLoad` | `form.Load` hook → `Arm()` | WinForms `Load` event (wired in `main.cpp`) |
| `RecoveryLoopFailsafe::OnOwnerClosing` | `form.FormClosing` hook → `Disarm()` | WinForms `FormClosing` event (wired in `main.cpp`) |
| `RecoveryLoopFailsafe::IsExpectedEnabled` | monitoring && !systemEnding && !expectedDisabled | ticks + `RequestRecoveryCheck` |
| `RecoveryLoopFailsafe::TryGetTargetId` | Target via `TryGetFailsafeTargetId` | ticks + `RequestRecoveryCheck` |
| `RecoveryLoopFailsafe::RequestRecoveryCheck` | Shared detect → `PendingVerification` + 5 s retry timer | `OnStartupTick` |
| `RecoveryLoopFailsafe::OnStartupTick` | One-shot 5 s → log `StartupVerification` → `RequestRecoveryCheck` | `startupTimer` |
| `RecoveryLoopFailsafe::OnPollTick` | 30 s detect → `PendingVerification` + 5 s retry timer | `pollTimer` |
| `RecoveryLoopFailsafe::OnRetryTick` | Re-check guards → `Recover(false)` + verify → log / linear 5 s retry / max-attempts | `retryTimer` |

## `main.cpp` (outside `src/`)

| Function / block | Responsibility | Called by |
|---|---|---|
| `main(args)` `runHidden` scan | `--background`/`/background` + all camera verbs → `Opacity=0`, no taskbar, minimized | process start |
| `main(args)` `isCommandWorker` scan | camera verbs → skip watchdog creation (workers exit in `MyForm_Load`) | process start |
| `RecoveryLoopFailsafe` wiring | `gcnew` + `Load`/`FormClosing` subscriptions (daemon only) | process start |

## Counts

- **Source files documented:** 11 (7 under `src/core/`, 4 under `src/watchdog/`) + `main.cpp` wiring
- **Functions indexed:** 37 core + 6 failsafe accessors + 7 `CameraFailsafe` + 12 `RecoveryLoopFailsafe` + 3 `main.cpp` blocks = 65
- **Major Windows APIs explained:** ~25 (SetupAPI, CfgMgr, WTS, power, sync/IPC, token, WinForms)
- **Event paths documented:** shutdown, suspend, resume, lid/button, session lock, session unlock, wake signal, watchdog recovery
