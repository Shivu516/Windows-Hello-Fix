# Windows Hello Fix v2.1 — Plan: Runtime Camera-State Failsafe

> **Status: Investigation Complete, Failsafe Implemented — Static Verification Passed, Runtime Pending**
> **Implementation date: 2026-08-27**
> **Build: Release|x64 + Debug|x64 — 0 errors (warnings: baseline C4793 only)**
> **Author branch: `v2.1`**
> **Investigation date: 2026-08-27**
> **Plan last updated: 2026-08-27**

---

## 1. Context

Previous investigation (see report produced 2026-08-27) traced a recurring runtime behavior where, after the daemon is terminated (`End Task` / `taskkill /F`) and restarted, the camera toggles several times and can be left **Disabled** though steady-state after startup is **Enabled**.

Supplied diagnostic log:

```
13:55:42.213 Startup_Context BackgroundArg=0 Verify=PASS
13:55:42.217 Startup_RestoreConfiguredCameraHardware Enabled PASS
13:55:45.035 EnableTargetCameraHardware_AlreadyEnabled Enabled PASS
13:55:45.058 EnableTargetCameraHardware_AlreadyEnabled Enabled PASS
13:55:45.068 WTSRegisterSessionNotification_Success NoChange PASS
13:55:45.499 DisableTargetCameraHardware_Result Stage=14 Disabled PASS
13:55:45.500 PowerEvent_Disable Disabled PASS
```

Root cause classification: **Probable B (race) + D (Windows event quirk)**. Disable originates inside `WndProc` `src/core/MyForm_Events.cpp:25-62` via `WM_POWERBROADCAST 0x0004/0x8013` (suspend/lid/button path) 431 ms after WTS registration, after `isMonitoring` became `true`. No complementary resume/unlock recovery exists. Behaviour reproduced from current `src/core` code and identical in `release-v2.0/MyForm.h`; v2.1 extraction (`static`→`extern` globals, `src/core` split, `BackgroundSilentExit` reorder at `MyForm_Core.cpp:222-227`) is **not** the cause. Task Scheduler (`install_script.nsi:118-173` — `WindowsHelloFix --background`, `WindowsHelloFix_Lock --disable-camera` StateChange 7, `Unlock --enable-camera` StateChange 8) ruled out for this log because log shows `PowerEvent_Disable`, not `Command_DisableCamera`.

Camera hardware primitives (`ToggleCameraHardware` Stage 10-15, `ToggleCameraHardwareCfgMgr` Stage 20-23, `VerifyCameraHardwareState` 3×100 ms, `SetCameraHardwareStateVerified`, `RecoverCameraHardware` cycle Sleeps 350/900/500 at `src/core/MyForm_Camera.cpp`) are correct and must not be rewritten.

---

## 2. Goal

Add the **smallest independent runtime failsafe** that, while the background daemon is running, periodically verifies that the configured target camera is actually enabled during normal runtime and — if unexpectedly disabled — restores it after a short confirmation delay, without fighting legitimate lock/suspend/shutdown behavior.

The investigation established six principles applied here:

1. Startup already reaches `Enabled` (two `AlreadyEnabled` verifications).
2. A `PowerEvent_Disable` can arrive shortly after startup via existing `WndProc`.
3. The resulting `Disabled` can persist without recovery event.
4. Same fundamental behavior exists in `release-v2.0`.
5. Camera primitives are faithful to reference; do not rewrite.
6. Safest improvement is a small, independent watchdog.

---

## 3. Architecture Constraint

`src/core/` is a **stability boundary** per `AGENTS.md:2`. Do not restructure, split/merge files, move functions, change algorithms/timings, create a controller inside `src/core`, or duplicate camera-state authority. New failsafe lives **outside** `src/core`, ideally `src/watchdog/`.

```
src/
├── core/                     # protected — only read-only getters + lifecycle hooks
│   ├── MyForm.h
│   ├── MyForm_Camera.cpp     # DO NOT MODIFY BEHAVIOUR
│   ├── MyForm_Config.cpp
│   ├── MyForm_Core.cpp
│   ├── MyForm_Events.cpp
│   ├── MyForm_System.cpp
│   └── MyForm_UI.cpp
└── watchdog/                 # NEW — watchdog owns timers, not camera implementation
    ├── CameraFailsafe.h
    └── CameraFailsafe.cpp
```

---

## 4. Responsibility Boundary

**Watchdog must NOT own camera implementation.** It only:

1. observes expected camera state (`ExpectedEnabled` vs `ExpectedDisabled`);
2. observes actual device state via `GetCameraHardwareDisabledState` / `VerifyCameraHardwareState`;
3. detects unexpected disabled;
4. waits for confirmation;
5. requests recovery through **existing** `RecoverCameraHardware(target,false)` / `SetCameraHardwareStateVerified`;
6. verifies recovery;
7. applies bounded retry/cooldown;
8. logs meaningful transitions via existing `WriteDiagnosticLog` path.

`src/core` remains authority for SetupAPI/CfgMgr/verification/retry/recovery.

---

## 5. Expected State Is Authority

Distinguish `EXPECTED_ENABLED` from `EXPECTED_DISABLED`, not “who disabled it” (PnP disabled state indistinguishable). Concept:

```
ExpectedEnabled + observed Disabled = unexpected → candidate for recovery
ExpectedDisabled + observed Disabled = correct → do nothing
```

Watchdog inactive when app intentionally expects disabled (lock/suspend/shutdown/command worker/monitoring off).

---

## 6. Minimum Core Integration

### `src/core/MyForm.h`
- Forward-declare `ref class CameraFailsafe;` inside `Windows_Hello_Fix_v2_0`.
- Add member `CameraFailsafe^ cameraFailsafe;` (handle; nil when disarmed).
- Add read-only accessors only:
  - `bool IsMonitoringActive();`
  - `bool IsSystemEndingActive();`
  - `bool IsCameraExpectedEnabled();` (inverse of `cameraExpectedDisabled` when initialized)
  - `bool TryGetFailsafeTargetId(std::wstring& out);` wrapper over `TryGetTargetCameraInstanceId(true)`
  - `void LogFailsafe(String^ eventName, String^ targetState, bool verify);`
  - `void LogFailsafeWithDevice(String^ eventName, std::wstring deviceId, String^ targetState, bool verify);`
- No mutable references, no state moved out of `MyForm`.

### `src/core/MyForm_Core.cpp`
- `#include "src/watchdog/CameraFailsafe.h"` in this TU only.
- After `WTSRegisterSessionNotification` success (`MyForm_Core.cpp:393-404`) in the normal long-lived daemon path: `cameraFailsafe = gcnew CameraFailsafe(this); cameraFailsafe->Arm();` — never in `--disable-camera`/`--enable-camera`/`/restore-camera` early-exit paths.
- In `~MyForm` and `!MyForm` (`MyForm_Core.cpp:26-105`) disarm before existing shutdown camera logic: `if (cameraFailsafe) { cameraFailsafe->Disarm(); cameraFailsafe = nullptr; }`
- Null-check `isSystemEnding` before arm; disarm order ensures `Disarm → existing Disable/Enable` cleanup remains authoritative.

No other `src/core` files modified initially. `MyForm_Events.cpp` unchanged (no `WM_DEVICECHANGE` yet). `MyForm_Camera.cpp` untouched.

---

## 7. Detection Strategy

**Idle poll** low-frequency `System::Windows::Forms::Timer` on UI thread (leverages existing `Application::Run` pump; no new thread).

- **Idle interval: 90 s** (spec `60-90 s`, recommend `90 s`). Poll via `GetCameraHardwareDisabledState` on target only (not full enumeration unless target lookup needs Scan).
- **No high-frequency polling**, no `SetupDi` every ms, no busy loops.

`WM_DEVICECHANGE` accelerator **not** in baseline — only if later runtime proves 90 s latency unacceptable.

---

## 8. Startup Grace Period

Suppress detection for **45 s** after arming. Covers startup `RestoreConfiguredCameraHardware(true)` cycle (Sleep 350/900/500 ≈1.75 s plus verify) plus the observed 431 ms post-WTS quirk window. Implemented as `startupGraceUntilTick = GetTickCount64()+45000` checked in poll.

---

## 9. Unexpected Disabled Sequence

When `monitoring==true && !isSystemEnding && ExpectedEnabled && target observed Disabled`:

```
Detect disabled → log Failsafe_DetectDisabled → PendingVerification → wait ~10 s
→ re-verify ExpectedEnabled still true && target still disabled → Recover
```

- **Confirmation delay: 10 s** (spec `5-15 s` range, initial `10 s`). Avoids fighting legitimate transitions.
- Uses one-shot `verifyTimer` (Forms::Timer, one-shot semantics via Stop after tick).

Before recovery re-checks: `monitoring==true`, `ExpectedEnabled`, `!isSystemEnding`, target exists, target still disabled. If any changed (lock→ExpectedDisabled, suspend, shutdown, unlock restored state) → cancel pending.

---

## 10. Manual Device Manager Disable

Treated as `ExpectedEnabled + observed Disabled` → same unexpected path → eventual recovery after 10 s verify. No attempt to identify “who” disabled.

---

## 11. Recovery Mechanism

Use existing proven path only:

```cpp
RecoverCameraHardware(targetId, false)  // enable-only, no cycle
VerifyCameraHardwareState(targetId, /*shouldBeDisabled*/ false)
```

Do **not** use `RecoverCameraHardware(target,true)` (cycle disables intentionally). Do not implement new recovery algorithm.

---

## 12. Verification & Logging

Success requires `recoverSucceeded && verifiedEnabled`. Log `Failsafe_Recovered` only after verification.

Events (via existing `diagnostic.log`):
- `Failsafe_Start` at `Arm()`
- `Failsafe_DetectDisabled` (PendingVerification entry)
- `Failsafe_RecoveryQueued` / `Failsafe_Recovered` (with `DurationMs` for recovery)
- `Failsafe_RecoveryFailed` (attempt failed)
- `Failsafe_Skipped_ExpectedDisabled / MonitoringOff / Shutdown / StartupGrace`
- `Failsafe_MaxRetries` at retry exhaustion
Do not log every idle poll.

DurationMs reuse: capture `GetTickCount64` around `RecoverCameraHardware`+`Verify` and log with `Failsafe_Recovered|DurationMs=...`.

---

## 13. Failure Handling & Cooldown

Bounded retries: **up to 3 attempts**, backoff `10 s → 20 s → 40 s` (cap `40 s`). After exhaustion → stop automatic recovery (double idle interval backoff). No infinite loop.

After successful recovery: **cooldown 30 s** (`kCooldownMs`) during which no new automatic recovery starts (unless ExpectedState flips). Prevents `detect→enable→disabled→enable` storm.

---

## 14. Device State Checks

Reuse `GetCameraHardwareDisabledState` / `VerifyCameraHardwareState` (`src/core/MyForm_Camera.cpp:217-275`). Do not duplicate SetupAPI/CfgMgr logic. Target remains authoritative via `TryGetTargetCameraInstanceId(true)` → `config.txt device=` → `MI_00` fallback → first camera (`MyForm_Config.cpp:110-146`).

---

## 15. Thread / Timer Model

- `System::Windows::Forms::Timer` for `pollTimer` and `verifyTimer` (UI thread).
- No worker thread, no thread pool, no async queue, no watchdog process/service.
- Poll handler does ≤3 ms `GetCameraHardwareDisabledState` on UI thread; recovery does blocking `RecoverCameraHardware(false)` (≈1-2 s) on UI thread — acceptable light duty; matches existing WndProc blocking model. If later proven to stall UI, migrate to background Timer without changing `src/core`.

---

## 16. Safety Invariants

- Never alter `Global\WindowsHelloFix_AppMutex` / `Global\WindowsHelloFix_WakeupEvent` (`MyForm_Core.cpp:220,289`).
- Never manipulate `Opacity / Visible / ShowInTaskbar / WindowState / Show / Hide / Activate / BringToFront` (`main.cpp:27-32`, `MyForm_System.cpp:43-52`). Watchdog recovery must not make hidden daemon visible (Issue #2 fix `MyForm_Core.cpp:222-227` and `MyForm_System.cpp:44 Opacity=1.0` remain intact).
- `MyForm_Camera.cpp` no behavioural changes (sleeps/retries/stages preserved).
- `MyForm_Events.cpp` no behavioural changes (lock/unlock/suspend/resume/shutdown/dedup/`isAlreadyDisabled` at `5-73` preserved).
- Command workers (`--disable-camera`/`--restore-camera` early exits `MyForm_Core.cpp:197-217`) never arm watchdog.

---

## 17. Project Files

Add to `Windows_Hello_Fix_v2_0.vcxproj` / `.vcxproj.filters` using existing conventions:

```
ClInclude src\watchdog\CameraFailsafe.h  (Header Files; filter Source Files\src\watchdog)
ClCompile src\watchdog\CameraFailsafe.cpp (Source Files\src\watchdog)
New filter: Source Files\src\watchdog  {GUID}
```

No CLR/UAC/target-framework changes.

---

## 18. Documentation & AGENTS.md

- Update this `docs/Plan.md` after implementation: record failsafe implemented, new module, state model, timing, integration points, limitations.
- Inventory: if `docs/files/` or `docs/SOURCE_TREE.md` inventories `src/`, add `src/watchdog` entries only if required per doc rules.
- `AGENTS.md`: add short permanent rule: “The runtime camera failsafe must remain an auxiliary safety mechanism. It must never become a second camera-state authority or override an intentional ExpectedDisabled state.” Preserve all existing protections.

---

## 19. Build & Static Verification

- Build `Release|x64` (`MSBuild Windows_Hello_Fix_v2_0.vcxproj /p:Configuration=Release /p:Platform=x64 /t:Rebuild`) — expect 0 errors, baseline `C4793` for `TryEnterHardwareToggleCooldown`/`RecordHardwareToggleTime` only.
- Also attempt `Debug|x64` if practical.
- Static checks:
  - `grep -r SetupDi|cfgmgr|CM_* src/watchdog` → **no new camera implementation** in `CameraFailsafe.cpp` beyond calls to existing `MyForm_Camera.cpp` functions.
  - `grep -r Show|Hide|Activate|BringToFront|Opacity|ShowInTaskbar|WindowState src/watchdog` → none.
  - Timer intervals not millisecond-scale; confirm `90000` / `10000` / `30000`.
  - Retry loop bounded (`consecutiveFailures < 3`).

---

## 20. Runtime Validation (separate static vs runtime)

| Test | Expected |
|---|---|
| 1 Normal operation | Enabled, monitoring, no watchdog spam |
| 2 Manual disable (Device Manager while monitoring=ON, ExpectedEnabled) | Detect → ~10 s → Recovered → Enabled |
| 3 Lock | Lock disables, watchdog skips (ExpectedDisabled) |
| 4 Unlock | Unlock enables, watchdog quiet |
| 5 Suspend/resume | Power behaviour unchanged, no fight |
| 6 Startup | No interference during 45 s grace |
| 7 Failure reproduction (restart → possible PowerEvent_Disable) | If unexpectedly disabled, watchdog detects → 10 s → re-enables |
| 8 End Task | Process exits, watchdog exits with it |
| 9 Manual GUI second instance | Issue #2 remains fixed, single instance intact |

Separate `STATICALLY VERIFIED` from `RUNTIME TESTED` in report.

---

## 21. Implementation Phases

```
Investigation (done) → Design (this plan) → Implement watchdog outside src/core
→ minimal MyForm.h/Core integration → project-file registration
→ Build Release|x64 → Static verification → Controlled runtime tests (see §20)
→ lock/unlock/suspend/manual-disable tests → background/GUI regression → report
```

---

## 22. Assumption & Blocker Log

- Timing constants justified from `CAMERA_FLOW.md:12` cumulative Sleeps and observed 431 ms quirk.
- No assumption that Task Scheduler causes this log; evidence points to WndProc power path.
- Blocker if more than ~3 `src/core` files need edits → stop and report per spec.
- `release-v2.0/MyForm.h` remains reference; no contradiction found warranting redesign.

---

## 23. Required Final Report Contents (per task)

Implementation summary, architecture diagram, state model, timing, core integration list, camera/event code confirmations, build report, per-test results, representative logs, git diff, remaining risks, completion gate.

---

## 24. Implementation Record — 2026-08-27

### What was added

- **New module outside `src/core`**: `src/watchdog/CameraFailsafe.h` + `src/watchdog/CameraFailsafe.cpp` — auxiliary watchdog, no camera reimplementation.
- **Lifecycle integration**: `src/core/MyForm.h` forward-decl `CameraFailsafe`, member `CameraFailsafe^ cameraFailsafe`, public read-only accessors `IsMonitoringActive / IsSystemEndingActive / IsCameraExpectedEnabled / TryGetFailsafeTargetId / LogFailsafe*`. `src/core/MyForm_Core.cpp` `#include "../watchdog/CameraFailsafe.h"`, init `cameraFailsafe=nullptr`, `~MyForm`/`!MyForm` disarm before core shutdown, `MyForm_Load` arm after `WTSRegisterSessionNotification` (only for normal daemon, not command workers, with `try/catch` and `!isSystemEnding` guard).
- **Project files**: `Windows_Hello_Fix_v2_0.vcxproj` + `.vcxproj.filters` register new header/compile and new filter `Source Files\src\watchdog`.
- **No edits** to `src/core/MyForm_Camera.cpp` (camera pipeline unchanged), `src/core/MyForm_Events.cpp` (WndProc unchanged), `main.cpp`, `install_script.nsi`, `ProductionUtilities.h`/`WndProc_Redesign.txt`.

### Architecture delivered

```
src/core (protected)              ── minimal read-only getters ──►  src/watchdog/CameraFailsafe
  MyForm state owner                          owner handle            observes ExpectedEnabled
  SetupAPI/CfgMgr/verify/recover  ◄── RecoverCameraHardware(false)   poll 90 s + verify 10 s
                                                    Verify            startup grace 45 s, cooldown 30 s,
                                                                      bounded retries 3, backoff 10/20/40 s
```

See `src/watchdog/CameraFailsafe.h:8-42`, `CameraFailsafe.cpp:1-213` for timers (`System::Windows::Forms::Timer` on UI thread, no new threads), state `Idle/PendingVerification/Recovering`, `consecutiveFailures`, `lastRecoveryTick`.

### State model delivered

- `ExpectedEnabled` ⇔ `isMonitoring && !isSystemEnding && !cameraExpectedDisabled`
- `Observed Disabled` via `GetCameraHardwareDisabledState` on `TryGetFailsafeTargetId` target (`config.txt device=` → `MI_00` fallback).
- `Unexpected Disabled` = `ExpectedEnabled && observed Disabled` → `Failsafe_DetectDisabled` → wait 10 s → re-verify guards (monitoring, ExpectedEnabled, grace, cooldown, still disabled) → `RecoverCameraHardware(false)` + `Verify → Failsafe_Recovered|DurationMs` or `Failsafe_RecoveryFailed` with retries until `MaxRetries=3` → `Failsafe_MaxRetries` + doubled idle interval. `ExpectedDisabled` (lock/suspend) → `Failsafe_Skipped_ExpectedDisabled` (never recover).

### Timing delivered

- `kIdleIntervalMs = 90000`, `kVerifyDelayMs = 10000`, `kStartupGraceMs = 45000`, `kCooldownMs = 30000`, `kMaxRetries = 3`, backoff `kVerifyDelayMs * (1 << (n-1))` capped 40 s.

### Build verification

- `Release|x64` Rebuild: **0 errors, 3 warnings C4793** (`TryEnterHardwareToggleCooldown`/`RecordHardwareToggleTime` baseline) — `x64/Release/Windows_Hello_Fix_v2_0.exe`.
- `Debug|x64` Rebuild: **0 errors, same 3 warnings**.
- Static checks passed: no new `SetupDi/CfgMgr` definitions in `src/watchdog`, no `Show/Hide/Opacity/...` GUI manipulation, intervals not aggressive, retries bounded.

### Limitations / remaining runtime tests

See final implementation report §9 — tests 1-9 not yet hardware-executed; static verification only. Runtime validation pending for lock/unlock, suspend/resume, manual Device Manager disable, startup reproduction, background/GUI regression.

### Docs update

- This file updated from “Awaiting Implementation” to “Failsafe Implemented”.
- `AGENTS.md` will gain auxiliary-failsafe protection rule (next edit). No full doc-set rewrite; `src/watchdog` inventory may be added to `docs/SOURCE_TREE.md` if required by AGENTS §12.
