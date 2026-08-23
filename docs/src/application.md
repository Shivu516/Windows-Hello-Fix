# Module: `src/application/` — orchestration layer

C++/CLI module owning application lifecycle and policy. Depends on camera,
config, events, system modules; the UI reaches it directly and it reaches back
only through `IUiSink`.

---

## ApplicationController.h / ApplicationController.cpp

| Field | Content |
|---|---|
| Purpose | Central orchestrator: startup policy, command modes, single-instance handling, monitoring state machine, session/power/system-end reactions, camera enable/disable wrappers, wake-listener thread |
| Major type | `public ref class ApplicationController` (implements nothing; holds `IUiSink^`) |
| Major functions | See execution flows below |

**Important state**

| Member | Meaning |
|---|---|
| `m_sink` (`IUiSink^`) | UI callback interface (never null in practice — form passes `this`) |
| `m_hwnd` | Main window handle used for WTS/power registration |
| `m_isMonitoring` | Monitoring service on/off (drives event reactions) |
| `m_isSystemEnding` | Set by WM_ENDSESSION/WM_QUERYENDSESSION path; changes shutdown semantics |
| `m_isAlreadyDisabled` | Power-path latch preventing repeated disables per sleep cycle |
| `m_cameraExpectedDisabled` | Written after enable/disable attempts; **never read** (diagnostic placeholder) |
| `m_selectedInstanceId` (`std::wstring*`, raw pointer) | Current target device instance id; deleted only in destructor (leaks if finalizer-only teardown) |
| `m_cachedCamerasPlaceholder` (`void*`) | Unused placeholder |
| `m_hAppMutex`, `m_hWakeupEvent`, `m_hLidNotification`, `m_hButtonNotification` | Owned kernel handles |
| `m_backgroundWorker` (`Thread^`), `m_keepListening` | Wake-listener thread + loop flag |

**Execution flow: `Initialize(hwnd, args)`** — called from `MyForm_Load`:
1. Store hwnd; classify args via `CommandLine`.
2. Write `Startup_Context` log line (elevation, integrity RID, background arg,
   exe path, CWD, config path).
3. If restore-camera command → `RestoreConfiguredCameraHardware(true)` +
   begin/end logs → `Environment::Exit(0)` → return false.
4. If disable-camera command → `DisableTargetCameraHardware(true)` + verify +
   begin/end logs → `Exit(0)` → return false.
5. `SingleInstance::CreateAppMutex`; if already exists:
   background arg → silent exit log + `Exit(0)`; else
   `TrySignalExistingInstance` → wake log + `Exit(0)`; else
   `m_sink->PromptGhostReset()` → recover ghost device (config or first
   `MI_00`), save config, `ProcessUtils::KillHelloFixProcess()`,
   `Sleep(500)`, `Application::Restart()`. Any branch ends with `Exit(0)`.
6. First instance: create wakeup event; log + run
   `RestoreConfiguredCameraHardware(true)`; `RegisterNotifications()` (power);
   start `ListenForWakeupSignal` thread; `RegisterSessionNotificationWithRetry`
   with success/failure logging.
7. Return true (form continues UI population).

**Execution flow: `Shutdown(bool isSystemEnding)`**
1. `m_keepListening = false`.
2. system-ending → disable camera (retry) and save config(monitoring=1,
   selected device); non-ending with a selected id → enable camera(false) and
   save config; no selection → `RestoreConfiguredCameraHardware(false)`.
3. Signal+close wake event; unregister power notifications; close mutex.

Called from `~MyForm`, then again from `~ApplicationController` (double-run is
a documented inconsistency, KNOWN_ISSUES #3).

**Event handlers** (called from `MyForm::WndProc`):
- `HandleSystemEnd(hwnd)`: set flag, log `SystemEnd_Begin`, disable if
  monitoring (log outcome), unregister session notification.
- `HandlePowerEvent(ev)` / `HandleSessionEvent(ev)`: see
  SESSION_MONITORING.md §3/§2 verbatim.

**Camera wrappers**: `DisableTargetCameraHardware(retry)` and
`EnableTargetCameraHardware(cycleDevice)` resolve the target
(`TryGetTargetCameraInstanceId`), short-circuit when already in state, invoke
native recovery/verify, set `m_cameraExpectedDisabled`, and emit the
`*_Result` diagnostic lines embedding elevation/integrity/error/stage slots.

**Threading**: methods run on the UI thread except `ListenForWakeupSignal`
(blocked on wake event; marshals `BringWindowToFront` via sink).

**Error handling**: relies on return codes of native layer; catches nothing
except implicit managed exceptions from marshaling (none guarded). Exit paths
use `Environment::Exit(0)`.

**Dependencies**: everything under src/ except ui (via `IUiSink` inversion),
plus msclr marshaling.

---

## CommandLine.h / CommandLine.cpp

| Field | Content |
|---|---|
| Purpose | Static argument classification |
| API | `IsBackgroundLaunch` (`/background`,`--background`, OrdinalIgnoreCase); `IsRestoreCameraCommand` (`/restore-camera`,`/enable-camera`,`--enable-camera`,`/repair-camera`, ignore-case); `IsDisableCameraCommand` (`/disable-camera`,`--disable-camera`, ignore-case); `ShouldHideWindow` (all of the above, but **case-sensitive** `==`) |
| Inputs | `array<String^>^ args` (from `main` or `Environment::GetCommandLineArgs`) |
| Outputs | Booleans |
| Callers | `main.cpp` (ShouldHideWindow), `ApplicationController::Initialize` (the rest), `MyForm_Load` (IsBackgroundLaunch) |
| Side effects | None |
| Notes | Case-sensitivity mismatch between checks — KNOWN_ISSUES #4 |

---

## IUiSink.h

| Field | Content |
|---|---|
| Purpose | Managed interface decoupling controller from the Form |
| Methods | `AddDeviceToDropdown`, `ClearDeviceDropdown`, `SetSelectedDeviceIndex`, `GetDeviceCount`, `SetDeviceDropdownEnabled`, `SetToggleButtonText`, `SetStatusText`, `SetWindowVisibleForBackground(bool)`, `BringWindowToFront`, `PromptGhostReset`, `ShowNoDeviceSelectedMessage`, `ShowBackgroundNotice` |
| Implementor | `MyForm` (sealed overrides) |
| Threading | Only `BringWindowToFront` is guaranteed to be called off-UI-thread (listener thread); others are called during Load/dialog contexts |
