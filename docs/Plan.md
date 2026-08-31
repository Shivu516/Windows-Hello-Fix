# Windows Hello Fix v2.1 — Plan: Enable-Only Startup / Runtime Recovery Failsafe (RecoveryLoopFailsafe)

> **Status: IMPLEMENTED — Build Verified Release|x64 (2026-08-31) — Awaiting Reboot/Runtime Matrix**
> **Branch: `failsafe-implementation` (on top of `v2.1` 119261e)**
> **Investigation date: 2026-08-31**
> **Implementation date: 2026-08-31**
> **Plan last updated: 2026-08-31**
> **Build target: Release|x64 — 0 errors, baseline C4793 only (warnings for TryEnterHardwareToggleCooldown/RecordHardwareToggleTime), exe 487424 bytes**
> **Core policy: `src/core/` ZERO changes — VERIFIED byte-for-byte unchanged**

---

## 1. Current Startup Camera Behavior

Ordered trace (`src/core/MyForm_Core.cpp:182-429`, `main.cpp:7-37`, `reference/release-v2.0/MyForm.h:943-1177`):

```
Windows boot → user sign-in → Task Scheduler → WindowsHelloFix (--background) → MyForm_Load
  → Startup_Context log (Elevated|IntegrityRid|BackgroundArg) → IsRestoreCameraCommand? → IsDisableCameraCommand?
  → CreateMutex Global\WindowsHelloFix_AppMutex → hWakeupEvent Global\WindowsHelloFix_WakeupEvent
  → RestoreConfiguredCameraHardware(true) (≈1.75 s Sleeps 350/900/500 + Verify 3×100 ms, cycle=true)
  → RegisterPowerSettingNotification (GUID_LIDSWITCH_STATE_CHANGE, GUID_POWER_BUTTON_TIMESTAMP)
  → ScanSystemCameras (DIGCF_ALLCLASSES|PRESENT → filter Camera/Image) → LoadConfigState → EnableTargetCameraHardware(shouldAutoStart)
  → EnableTargetCameraHardware(false) if (background||monitoring) → background hidden (Opacity 0, ShowInTaskbar false)
  → ListenForWakeupSignal thread → WTSRegisterSessionNotification retry 6×500 ms → Arm CameraFailsafe
```

`RestoreConfiguredCameraHardware(true)` precedes WTS registration and watchdog arm. Both `--background` and interactive share same restore path (only `Opacity/ShowInTaskbar` differ at `main.cpp:27-32` and `MyForm_Core.cpp:318-381`). Recovery lives inside `MyForm_Load`, not scheduler. Verification via `VerifyCameraHardwareState` (3×100 ms) and `SetCameraHardwareStateVerified` check-before-change.

Evidence: `diagnostic.log` `13:55:42.217 Startup_RestoreConfiguredCameraHardware` → `13:55:45.035 EnableTargetCameraHardware_AlreadyEnabled` = **2.8 s** purely inside `RecoverCameraHardware(true)` cycle; WTS success `13:55:45.068` 33 ms later; `PowerEvent_Disable` at `13:55:45.499` 431 ms later leaves `Disabled`.

Target resolution: `TryGetTargetCameraInstanceId(true)` → `selectedInstanceId` → `config.txt device=` → `MI_00` fallback → first camera (`MyForm_Config.cpp:110-146`).

---

## 2. Current Startup Failure Mode

Two distinct failure modes, traced from source vs live `Anomaly_Investigation.md` + `Startup_Behavior_Investigation.md`:

**Mode A — Enable never attempted (dominant, current live):**
```
Boot 00:49 → WindowsHelloFix AtLogOn --background → LastResult 267011 (SCHED_S_TASK_HAS_NOT_RUN, never queued)
→ no process → no MyForm_Load → no RestoreConfiguredCameraHardware → no WTS → no watchdog
→ camera remains Disabled (left by previous shutdown isSystemEnding→DisableTargetCameraHardware at MyForm_Core.cpp:38 / MyForm_Events.cpp:15-23)
→ persists until first Win+L
```
Live: `schtasks /Query /V` shows `WindowsHelloFix` `Ready 267011 30-Nov-99` while `XRite PT10S Highest 18:25:46 Result 0`, `Syncthing 18:25:35 Result 0`, `Device Install Reboot 18:25:35 Result 0` all succeeded same logon — isolated trigger-delivery drop, not engine failure (`Schedule` RUNNING). `diagnostic.log` gap `00:49 boot` → `18:26 first manual log` confirms no daemon. Requires `PT10S` delay on S0+FastStartup+build 26200.9168.

**Mode B — Enable attempted but undone (quirk):**
```
Boot → daemon runs → Restore(true) enables → 431 ms later WM_POWERBROADCAST 0x0004/0x8013 → WndProc MyForm_Events.cpp:25-62
→ isMonitoring&&!isAlreadyDisabled → isAlreadyDisabled=true → DisableTargetCameraHardware(true) → PowerEvent_Disable
→ no resume 0x0007/0x0012 → remains Disabled, watchdog grace 45 s blocks immediate recovery
```
Log `13:55:45.499 DisableTarget... Stage 14` proves `ToggleCameraHardware` stage 14 after `PowerEvent_Disable`. No complementary enable until next resume/unlock.

**Distinction (spec §7):** Must report `"enable was never attempted"` vs `"enable was attempted but device ended disabled"` separately — they are different bugs with different fixes (A needs scheduler trigger fix + startup verifier; B needs fast verifier after arm).

---

## 3. Existing `CameraFailsafe` Behavior

Files `src/watchdog/CameraFailsafe.h:1-71`, `CameraFailsafe.cpp:1-270` (current on-disk version, unchanged for this task):

**Two layers:**
- **Layer A — Event-driven accelerator (preferred):** `CM_Register_Notification` with `CM_NOTIFY_FILTER_TYPE_DEVICEINSTANCE` on configured `InstanceId` (from `TryGetFailsafeTargetId` at Arm time). Native `WatchdogNativeCallback` (`#pragma managed(push,off)`) only `PostMessage(WM_APP+0x20)` → `WndProc` `MyForm_Events.cpp:77-83` → `OnDeviceChangeAccelerated()` on UI thread (try/catch). No SetupAPI in callback. Fallback if `CR != CR_SUCCESS` → log `Failsafe_NotificationRegistrationFailed|CR` and poll-only.
- **Layer B — Periodic safety net:** `System::Windows::Forms::Timer` `pollTimer` **60 s** on UI thread, `verifyTimer` one-shot **10 s**. No worker thread, no thread pool.

**State machine:** `Idle → DetectDisabled → PendingVerification (wait 10 s) → Recovering → RecoverCameraHardware(target,false)+Verify → Recovered|RecoveryFailed → cooldown 30 s → Idle`. Guards: `isArmed`, `IsMonitoringActive`, `IsSystemEndingActive`, `IsCameraExpectedEnabled()`, `startupGraceUntilTick` (45 s), `lastRecoveryTick` (30 s), `state PendingVerification/Recovering` coalescing.

**Timing constants (current):** `kIdleIntervalMs=60000`, `kVerifyDelayMs=10000`, `kStartupGraceMs=45000`, `kCooldownMs=30000`, `kMaxRetries=3`, backoff `10→20→40 s` cap 40 s.

**Recovery:** `RecoverCameraHardware(target,false)` (enable-only, no cycle) + `VerifyCameraHardwareState(target,false)`, logs `DurationMs`. Never disables, never duplicates target selection.

**Lifecycle:** `MyForm_Core.cpp:420-428` after `WTSRegisterSessionNotification` success, `if (!isSystemEnding) { cameraFailsafe = gcnew CameraFailsafe(this); Arm(); }` — never for command workers. `~MyForm`/`!MyForm` Disarm before core shutdown.

---

## 4. Why Existing Runtime Recovery Takes 40–60 Seconds

Target is **5–15 s**, observed **40–60 s** after manual Device Manager disable.

Contributors (evidence from `CameraFailsafe.h:44-48` constants):
- **Poll interval 60 s dominates:** If notification path missed or grace blocks, detection waits up to 60 s before `OnPollTick` sees `Disabled`. 60 s + 10 s verify = 70 s worst, matching 40–60 s observation (partial elapsed → 40–60).
- **Startup grace 45 s suppresses early detection:** `OnDeviceChangeAccelerated` and `OnPollTick` both early-return `if (now < startupGraceUntilTick)`. A disable within 45 s of Arm (common after boot quirk) is ignored for 45 s, violating 5–15 s target. Grace was intended for 2.8 s restore + 431 ms quirk but is **10× too long**.
- **Verify 10 s adds fixed latency:** After detection, one-shot `verifyTimer 10 s` before first `RecoverCameraHardware`. With notification working, path is `DeviceChangeDetected → DetectDisabled → 10 s → Recover → ~12 s total` (within spec). Without notification, 70 s.
- **Cooldown 30 s + doubled idle after MaxRetries:** After 3 failures, `pollTimer` doubled to 120 s, extending subsequent detection.
- **Notification fragility:** `RegisterDeviceNotification` called once at `Arm()` using `TryGetTargetId` at that moment. If `selectedInstanceId` empty (e.g., first launch before dropdown) or `config.txt` missing, `targetId.empty()` → no registration → poll-only path persists. Filter pinned to `InstanceId` at Arm; config change needs re-registration.

Result: **Working notification path = ~12 s (spec-compliant). Poll-only/grace path = 40–70 s.** Startup-disabled case always hits grace.

---

## 5. Why Startup Recovery Is Missed

Evidence (`Anomaly_Investigation.md §C-D`, `Startup_Behavior_Investigation.md §10`, `docs/Plan.md` prior):

- **Isolated LogonTrigger drop:** `WindowsHelloFix` `At logon` without `<Delay>` (`<LogonTrigger/>` version 1.3 `Compatibility Win7`) is the **only** `Highest+LogonTrigger` with empty delay that shows `267011` across two boots (`00:49`, `17:06`), while peers `XRite PT10S Highest Win8 18:25:46 Result 0`, `Syncthing <none> Limited 18:25:35`, `Office PT5M Highest 18:30:36` succeed same logon. Distinguishing feature is **zero-delay on S0 Modern Standby + Fast Startup + build 26200.9168** — fires at `t=0` before LSASS/Explorer/PnP ready, silently dropped (Operational log disabled, no event). Not a v2.1 regression: installed task XML is byte-for-byte reference `release-v2.0`.
- **No daemon → no recovery:** Shutdown intentionally leaves `Disabled` via `~MyForm:38 DisableTargetCameraHardware(true)` when `isSystemEnding` (true for `WM_QUERYENDSESSION 0x0011` / `WM_ENDSESSION 0x0016`). Next boot's `RestoreConfiguredCameraHardware(true)` never runs because process never created. No daemon → no `MyForm_Load:304` → no `WTS` → no watchdog → state persists.
- **Session helper not startup helper:** `WindowsHelloFix_Unlock` is `SessionStateChange 8 --enable-camera` (`install_script.nsi:168`, live `StateChange=8`). Fires on **every** `Win+L` unlock, not at logon. Hence boot disabled persists until first `Win+L` at `18:40:04` (`Command_EnableCamera_Begin/End` 3.28 s, `Result 0`).
- **Watchdog grace blocks even if daemon later starts:** Manual `18:28` launch did `Restore(true)` + `AlreadyEnabled` in 3 s, but had it left disabled, `45 s grace` would still block `RecoveryLoop` until `~45 s` after Arm, violating 5–15 s target. And manual launch is `BackgroundArg=0` Foreground, not `Highest` AtLogOn.
- **No second path:** No `Run` key, no `AtStartup` SYSTEM task, no `EventTrigger 4801` — `AtLogOn --background` is sole boot launcher. All three prior installs used old `Downloads\Setup.exe 22-Aug` (SHA `0443...`) with old `SessionUnlock` helper; new `PT10S --startup-enable` helper from `27a1174` was never installed on live machine (verified `C:\Program Files\WindowsHelloFix exe SHA 6B8F...` = reference, not workspace `CD56...`).

Conclusion: Startup failure is **not camera pipeline** (`SetCameraHardwareStateVerified`/`Recover` correct, `Verify PASS` when called), but **missing 5–15 s startup verifier + unreliable trigger**. Need **AtLogOn PT10S** helper plus **fast in-daemon verifier** (5 s) that survives grace.

---

## 6. Current `WindowsHelloFix_Unlock` Task Behavior

**Installer source** `x64/Release/install_script.nsi:130-173` (`RegisterWindowsHelloFixTasks.ps1` — current on-disk, old):

```ps1
Register-WhfSessionTask 'WindowsHelloFix_Unlock' 8 '--enable-camera'
```

`Register-WhfSessionTask` (`140-165`):
- `Schedule.Service` COM `Triggers.Create(11)` = `TASK_TRIGGER_SESSION_STATE_CHANGE`, `StateChange=8` (unlock, `TASK_SESSION_STATE_CHANGE_TYPE_CONSOLE_DISCONNECT`), `UserId=$user`, `Enabled=true`
- `Actions.Create(0)` `Path=$exe` `Arguments='--enable-camera'` `WorkingDirectory=$wd`
- `Settings: Enabled true, Hidden true, DisallowStartIfOnBatteries false, StopIfGoingOnBatteries false, StartWhenAvailable true, MultipleInstances 2 (Parallel), ExecutionTimeLimit PT5M, Priority 4`
- `Principal: UserId=$user, LogonType=3 (Interactive), RunLevel=1 (Highest)` + `RegistrationInfo.Author=$user`, `Root.RegisterTaskDefinition(name, task, 6, null,null,3,null)` (`6=CreateOrUpdate, 3=Interactive`)

Effect: **fires on every unlock**, `C:\Program Files\WindowsHelloFix\Windows_Hello_Fix_v2_0.exe --enable-camera` → `MyForm_Load:208-216` `ShowInTaskbar=false Visible=false → RestoreConfiguredCameraHardware(true) → Command_EnableCamera_End → Exit(0)` — enable via existing pipeline (cycle, no duplicate SetupAPI; it *is* the pipeline, short-lived). Same for `Lock` (`StateChange=7 --disable-camera` → `DisableTargetCameraHardware(true)` at `MyForm_Core.cpp:218-228`).

**Live state (verified `2026-08-31` on `LAPTOP-6VQEGV4P` — currently NO HelloFix tasks after clean; prior captures `2026-08-30 18:40` showed):**

- `WindowsHelloFix`: `At logon` `--background`, `gupta Interactive Highest`, `IgnoreNew`, `Priority 4`, `Hidden False`, `Ready 267011 never ran`
- `WindowsHelloFix_Lock`: `SessionStateChange 7 --disable-camera`, `Hidden True`, `PT5M`, `Result 0 18:40:01`
- `WindowsHelloFix_Unlock`: `SessionStateChange 8 --enable-camera`, same, `Result 0 18:40:04` → **ordinary unlock, not startup**
- `WindowsHelloFix_LogCleanup`: `Daily 12:00 AM` `cmd /c break > diagnostic.log`

Source and live (when present) match. Hence `Unlock` **currently duplicates** native `WTS` handler `MyForm_Events.cpp:97-113` (`WTS_SESSION_LOCK/UNLOCK`), creating per-unlock double enable race (both idempotent but pollutes logs with `Command_EnableCamera_*` instead of `SessionUnlock_Enable`).

**Historical:** `reference/release-v2.0` identical triggers; `reference/legacy-v1.0` used `.vbs` + `pnputil.exe` with `EventID 4800/4801` + `auditpol Other Logon/Logoff Events` + `SYSTEM`, `wscript 0,False` hidden — replaced for verification/`config.txt` reasons. Not suitable for startup-only (see §18).

---

## 7. Interaction Between Task and Daemon

```
startup (intended)
 ├── ~t+10s WindowsHelloFix_Unlock (startup-enable, quick check, idempotent)
 └── ~t+0-15s WindowsHelloFix daemon (--background) → RestoreConfiguredCameraHardware(true) → EnableTarget×2 → WTS → Arm watchdog(s)

startup (actual live, 00:49 & 17:06)
 ├── WindowsHelloFix --background : 267011 dropped → no daemon
 └── WindowsHelloFix_Unlock : StateChange 8 not at logon → no helper → disabled persists

manual fix at 18:28
 └── user double-click → Foreground daemon → Restore(true) → AlreadyEnabled → WTS → running, but 45 s grace still blocks
     └── 18:40 Win+L → both Lock/Unlock workers (± WTS) fire → enabled (3.28 s)

after fix (proposed)
 ├── t+10s Unlock AtLogOn PT10S --enable-camera (or --startup-enable via main.cpp) → GetCameraHardwareDisabledState → if disabled → RecoverCameraHardware(false)+Verify → Enabled (~12 s)
 └── t+0-3s daemon (if AtLogOn succeeds) → Restore(true) → WTS → Arm CameraFailsafe + RecoveryLoopFailsafe (5 s startup verifier)
     └── they are idempotent: whichever enables first, the other sees AlreadyEnabled/Verify true and no-ops (no disable path)
```

Four tasks registered by `install_script.nsi:120-174` via `RegisterWindowsHelloFixTasks.ps1`:

| Task | Trigger (source) | Trigger (live when present) | Action | Principal | Settings |
|---|---|---|---|---|---|
| `WindowsHelloFix` | `AtLogOn --background` via `New-ScheduledTaskTrigger -AtLogOn` | `LogonTrigger <none>` `Ready 267011` | `--background` | `gupta Interactive Highest` | `Hidden false PT0S IgnoreNew Priority4 StartWhenAvailable` |
| `WindowsHelloFix_Lock` | `SessionStateChange 7 --disable-camera` COM `Create(11)` | `StateChange=7` `Ready Result 0` | `--disable-camera` | same `Hidden true` | `PT5M IgnoreNew Vista UseUnified false` |
| `WindowsHelloFix_Unlock` | `SessionStateChange 8 --enable-camera` | `StateChange=8` `Ready Result 0` | `--enable-camera` | same | same |
| `WindowsHelloFix_LogCleanup` | `Daily 00:00 cmd /c break > diagnostic.log` | same `Ready 267011` until midnight | — | `gupta Highest` | `IgnoreNew PT0S` |

Healthy peers prove scheduler engine healthy (`Schedule RUNNING`, dozens of LogonTriggers ran). Failure is per-definition, per-trigger.

---

## 8. Proposed New Recovery Mechanism

**Goal (spec §2):** `PC running + session active/unlocked + HelloFix expects Enabled → camera eventually ENABLED` within **5–15 s**, via enable-only, bounded, coalescing, verified loop that calls **existing HelloFix enable mechanism**, not a new SetupAPI pipeline:

```
Observed Disabled
  ↓ detect (poll 30 s or PnP event)
  ↓ wait briefly if necessary (5 s initial verification)
  ↓ verify still disabled && ExpectedEnabled && !isSystemEnding && !cooldown
  ↓ call existing enable mechanism (RecoverCameraHardware(target,false) or launch exe --enable-camera)
  ↓ verify actual device state (VerifyCameraHardwareState(target,false))
  ↓ still disabled? → retry after 5 s → continue until enabled OR MaxAttempts (3) reached
```

**Components:**
- **Startup helper (Task):** `WindowsHelloFix_Unlock` re-typed from `SessionStateChange 8 --enable-camera` to **`AtLogOn PT10S --enable-camera`** (reuse existing flag, no core change) **or** `--startup-enable` handled in `main.cpp` (enable-if-disabled, no cycle, see §11). One-shot at sign-in, hidden, elevated, verified, `ExecutionTimeLimit PT1M`, `IgnoreNew`, `Priority 4`, `Description` startup-only. Covers gap when daemon not yet running (Mode A).
- **Runtime helper (in-process):** New `src/watchdog/RecoveryLoopFailsafe` (enable-only coordinator, no SetupAPI) + existing `CameraFailsafe` as long-term backup. RecoveryLoopFailsafe provides **fast path 5 s**; CameraFailsafe retains **60 s backup** (or tightened to 30 s after fix). Both use same `RecoverCameraHardware(false)` pipeline.

**Why not just fix CameraFailsafe timing?** `CameraFailsafe` already has event-driven layer but 45 s grace and 60 s poll are too slow for startup; shortening grace to 15 s and poll to 30 s helps but still leaves `267011` gap when daemon absent. Need **out-of-process AtLogOn helper** for daemon-absent case + **fast in-process verifier** for daemon-present/quirk case. Two mechanisms are complementary, not duplicate authority.

**No second camera implementation:** Neither new task nor RecoveryLoopFailsafe contains `SetupDiGetClassDevs`/`SetupDiCallClassInstaller`/`CM_Disable_DevNode`/`SetCameraHardwareStateVerified` definitions; they **call** `RecoverCameraHardware`/`GetCameraHardwareDisabledState`/`VerifyCameraHardwareState` which are single authority in `src/core/MyForm_Camera.cpp`.

---

## 9. Exact Responsibility of `RecoveryLoopFailsafe`

File: `src/watchdog/RecoveryLoopFailsafe.h` + `src/watchdog/RecoveryLoopFailsafe.cpp` (new, beside `CameraFailsafe`, not replacing it).

**Sole responsibility:** *Repeatedly verify that an expected-enabled camera has actually become enabled, and when necessary invoke the existing enable mechanism until verified or bounded limit.*

Scope limits (spec §9-10):
- **Owns:** timers (`startupTimer 5 s`, `retryTimer 5 s`, `pollTimer 30 s`), state `Idle/PendingVerification/Recovering`, `consecutiveFailures`, `lastRecoveryTick`, optional PnP notification handle (hidden window), logging via `owner->LogFailsafe*`.
- **Does NOT own:** camera selection, `config.txt` parsing (uses `TryGetFailsafeTargetId` wrapper → single source), monitoring state (queries `IsMonitoringActive`), lock/unlock policy (checks `IsCameraExpectedEnabled`), power-state policy (checks `IsSystemEndingActive`), hardware implementation (calls `RecoverCameraHardware`/`Verify`/`Get...`), shutdown policy (checks `IsSystemEndingActive` + Disarm).

**Single responsibility vs CameraFailsafe:**
- `CameraFailsafe`: long-term backup, `60 s` poll + `CM_Notification` via `MyForm WndProc`, `10 s` verify, `45 s` grace — stable but slow; **remains unchanged** (preferred) — provides defense-in-depth.
- `RecoveryLoopFailsafe`: short-term fast verifier, `5 s` startup check + `5 s` retry + `30 s` poll, **no grace beyond startupTimer**, bounded 3 attempts, cooldown 30 s — closes 5–15 s window.

They do not fight: both guarded by `ExpectedEnabled`, `!isSystemEnding`, `cooldown`, `state` coalescing; both call same `RecoverCameraHardware(false)` idempotently.

---

## 10. Why `src/core` Does Not Need Modification

**Absolute constraint (spec §0, §37):** `src/core/MyForm.h`, `MyForm_Camera.cpp`, `MyForm_Config.cpp`, `MyForm_Core.cpp`, `MyForm_Events.cpp`, `MyForm_System.cpp`, `MyForm_UI.cpp` must remain byte-for-byte unchanged unless absolute build-breaking impossibility.

**Investigation proves zero changes possible:**

1. **Read-only observation already sufficient:** `CameraFailsafe` previously added to `MyForm.h` the exact getters needed: `IsMonitoringActive()`, `IsSystemEndingActive()`, `IsCameraExpectedEnabled()`, `TryGetFailsafeTargetId()`, `LogFailsafe*()` (`MyForm.h:125-131`). These are **already on-disk** and verified `Release|x64` builds. RecoveryLoopFailsafe can reuse them verbatim via `MyForm^ owner` handle — no new getters needed.
2. **No new members needed in MyForm:** `RecoveryLoopFailsafe` will be **owned by `main.cpp`**, not by `MyForm`. `main.cpp` is **outside `src/core`** (allowed per `AGENTS.md §1` — active source tree lists `main.cpp` separately, and §6 says new features should go in new files/folders rather than being forced into existing seven). Instantiating in `main.cpp` keeps `MyForm` class byte-identical.
3. **No new WndProc hooks needed:** Prior `RecoveryLoopFailsafe` design required `MyForm_Events.cpp` changes to forward `WM_POWERBROADCAST`/`WTS_SESSION_CHANGE` as `RequestRecoveryCheck`/`Cancel`. New design avoids this by **polling + own notification window** — `RecoveryLoopFailsafe` creates its own hidden `NativeWindow` for `CM_Register_Notification` and uses `Forms::Timer` on UI thread (pump already exists via `Application::Run`). No `MyForm_Events.cpp` edit.
4. **No new command parsing in `MyForm_System.cpp`:** The startup helper task can reuse **existing** `--enable-camera` (`IsRestoreCameraCommand` at `MyForm_System.cpp:5-16` already handles `--enable-camera`/`/restore-camera` etc.) — no need for new `--startup-enable` flag in `src/core`. If enable-if-disabled optimization is desired, it can be implemented in `main.cpp` before `MyForm` construction (see §11 Option 1), still outside `src/core`.
5. **No new camera logic:** RecoveryLoopFailsafe calls `RecoverCameraHardware(target,false)` etc. which are already declared `extern` in `MyForm.h:42-46` and defined once in `MyForm_Camera.cpp`. No copy.
6. **Build verification:** Adding `src/watchdog/RecoveryLoopFailsafe.h/.cpp` only touches `Windows_Hello_Fix_v2_0.vcxproj` + `.vcxproj.filters` (allowed per spec §30) and `main.cpp` + `install_script.nsi` (outside `src/core`). No `src/core` file needs to be in diff.

**If a `src/core` change were later claimed unavoidable, the required STOP procedure (§0) is:**
- Stop, explain exact technical blocker, file+lines, why no external alternative works, and await explicit authorization. For this plan, no blocker exists.

**Therefore for this implementation:** `src/core changed: NO` — confirmed via `git diff --stat` will show zero `src/core` paths.

---

## 11. Exact Integration Boundary

```
main.cpp (outside src/core)          ── creates ──►  src/watchdog/RecoveryLoopFailsafe
  MyForm form;                                      owner = %form
  bool isCommandWorker = args contains               uses owner->IsMonitoringActive()
        --disable-camera/--enable-camera/             owner->IsSystemEndingActive()
        /restore-camera//repair-camera                owner->IsCameraExpectedEnabled()
  if (!isCommandWorker) {                             owner->TryGetFailsafeTargetId()
    auto loop = gcnew RecoveryLoopFailsafe(%form);    owner->LogFailsafe*()
    form.Load += loop->OnOwnerLoad (Arm after         ── calls ──► src/core/MyForm_Camera.cpp
                         MyForm_Load completes)                 GetCameraHardwareDisabledState
    form.FormClosing += loop->OnOwnerClosing          (observes)  VerifyCameraHardwareState
                      (Disarm)                        ── calls ──► RecoverCameraHardware(target,false)
  }                                                   (enable-only) + Verify
  Application::Run(%form);

x64/Release/install_script.nsi       ── registers ──►  Task Scheduler
  WindowsHelloFix_Unlock                AtLogOn PT10S  C:\Program Files\WindowsHelloFix\Windows_Hello_Fix_v2_0.exe --enable-camera
  (or --startup-enable via main.cpp)   Hidden Highest  WorkingDirectory $wd, LogonType Interactive, RunLevel Highest
                                       Settings: Enabled true, StartWhenAvailable true, MultipleInstances IgnoreNew, Execution PT1M, Priority 4
  Existing WindowsHelloFix --background, WindowsHelloFix_Lock, LogCleanup UNCHANGED per §18

src/watchdog/CameraFailsafe           ── unchanged ──► auxiliary long-term backup (60 s poll, 10 s verify, 45 s grace)
src/watchdog/RecoveryLoopFailsafe     ── new ──► fast verifier (5 s startup, 5 s retry, 30 s poll, 3 attempts, 30 s cooldown)
                                                 single instance, coalescing, enable-only, bounded

Existing HelloFix enable mechanism     ◄── both watchdogs call same Recover/Verify ── authoritative pipeline
```

**Alternatives considered and rejected (spec §11 order):**
- **Option 1 — Existing exe command mode (launch exe):** Task `WindowsHelloFix_Unlock` launching `Windows_Hello_Fix_v2_0.exe --enable-camera` already works (uses `IsRestoreCameraCommand` early-exit `MyForm_Core.cpp:208-216`, hides window, calls `RestoreConfiguredCameraHardware(true)`). This is preferred for startup helper because it works even when daemon not running, no new code. For in-process fast path, direct `RecoverCameraHardware(false)` call is equivalent but avoids spawning new process every 5 s (lighter).
- **Option 2 — Existing scheduled task invoking enable-only:** Same as Option 1, reusing `WindowsHelloFix_Unlock` name but re-typed trigger `LogonTrigger Create(9) Delay PT10S` instead of `SessionStateChange 8`. Keeps name stable for uninstall purge (`install_script.nsi:213-216`).
- **Option 3 — Separate startup helper exe/script (pnputil):** Rejected — hard-codes `InstanceId` at install time (config drift), needs admin, no verification, divergent SetupAPI behavior, silent only via VBS hidden flag, legacy `reference/legacy-v1.0` `CameraFix\*.vbs` + `pnputil` path requires `auditpol` and `SYSTEM` (see §6 Historical), obsolete.
- **Option 4 — Minimal watchdog-only interaction:** Fallback if task not possible — `RecoveryLoopFailsafe` using existing `RecoverCameraHardware` interface directly is minimal and enable-only; chosen for runtime fast path.

**Final boundary:** `src/core` ZERO, `CameraFailsafe` ZERO (or at most tiny Disarm forwarding if needed, but plan keeps it ZERO), `main.cpp` + `RecoveryLoopFailsafe` + `install_script.nsi` + project files only.

---

## 12. Startup Timing

**Target (spec §16):** Camera normally enabled within **5–15 s after sign-in**, justified not blindly smallest delay.

**Measured contributors:**
- `RestoreConfiguredCameraHardware(true)` cycle: `Sleep 350` + `Sleep 900` + `Sleep 500` = 1.75 s + `Verify 3×100 ms` per `SetCameraHardwareStateVerified` attempt ≈ 0.3 s × up to 3 attempts + `Toggle` overhead ≈ **2.8 s** (observed `13:55:42.217→13:55:45.035`).
- `ScanSystemCameras` + dropdown + `EnableTarget×2` ≈ 0.1–0.2 s.
- `WTSRegisterSessionNotification` retry `6×500 ms` = up to 3 s (typically 1 attempt ≈ 0 ms).
- Task Scheduler logon dispatch: LSASS → Explorer → Task Scheduler readiness ~1–2 s after sign-in; peers `XRite PT10S` ran at `18:25:46` (~11 s after `18:25:35` Syncthing immediate), proving `PT10S` fires reliably.
- PnP device stack ready: `ScanSystemCameras` needs `DIGCF_PRESENT` devices enumerated; on S0 Modern Standby, camera `Present True` immediately after unlock per `Get-PnpDevice`, so ~2 s after sign-in is queryable.
- Daemon arm point: after WTS, `CameraFailsafe::Arm()` sets `startupGraceUntilTick`; new `RecoveryLoopFailsafe` will arm on `form.Load` (≈3 s after process start).

**Chosen delays:**
- **Task `Delay PT10S`:** 10 s after `AtLogOn` trigger. AtLogOn fires ~1 s after sign-in → execution ~11 s + `GetCameraHardwareDisabledState` 2 ms or `Recover(false)` 1–2 s → **worst ~13 s**, within 15 s. 10 s covers slow boot/AV delay while still meeting target; if telemetry shows SSD <5 s, can tighten to `PT5S` later (rollback plan).
- **RecoveryLoopFailsafe `kStartupVerifyMs = 5000` (5 s after Arm):** Arm at ~3–4 s after daemon start → first check at ~8–9 s after daemon start, ~9–10 s after sign-in if daemon started at ~1 s, still within 15 s. If daemon delayed >20 s (AV), task already recovered at ~11 s, daemon later sees `AlreadyEnabled` and no-ops.
- **No `45 s` grace:** New verifier uses **5 s** initial grace, not 45 s, because it checks `ExpectedEnabled` and `GetCameraHardwareDisabledState` before recovering, and already-enabled fast-exit avoids churn; 45 s was for `CameraFailsafe` to avoid fighting `Restore` quirk but new loop's `AlreadyEnabled` check is sufficient.

---

## 13. Retry Interval

- **Initial verification:** `kInitialVerifyMs = 5000` (5 s, spec `5-15 s` lower bound). Avoids fighting legitimate `PowerEvent_Disable` vs immediate unlock transitions; 5 s is long enough for `WndProc` dedup 1500 ms to settle, short enough for 15 s target.
- **Retry interval:** `kRetryIntervalMs = 5000` (5 s, spec `3-10 s` range). After `RecoveryLoop_DisabledDetected` → `retryTimer 5000` → re-verify `still disabled && ExpectedEnabled` → `RecoverCameraHardware(false)` + `Verify` → if failed, log `RecoveryFailed` → backoff linear 5 s (capped 40 s, but 5 s is stable; exponential `5→10→20` also capped 40 s was considered but linear is more predictable for 5–15 s).
- **Periodic backup:** `kPollIntervalMs = 30000` (30 s, spec `30-60 s` backup, tighter than CameraFailsafe's 60 s but still low-frequency). Steady-state `GetCameraHardwareDisabledState` every 30 s ≈ 2 calls/min, negligible (≤3 ms each).
- **No busy loop:** No polling every ms, no spawn every second, no permanent helper process (watchdog lives inside daemon process, dies with `taskkill /F`).

---

## 14. Maximum Retry Policy

- **Attempt limit:** `kMaxRetries = 3` bounded retries (spec §27).
- **Sequence:**
  ```
  START → Is recovery allowed? (ExpectedEnabled && !isSystemEnding && monitoring && !cooldown && idle)
    → Check actual state → Already enabled? → STOP (reset failures)
    → Disabled? → Attempt enable (Recover false + Verify) → Enabled? → STOP (reset failures, cooldown 30 s)
    → Still disabled? → wait 5 s → retry (consecutiveFailures++ → 1,2)
    → At 3 failures → log MaxAttempts → STOP → poll interval remains 30 s (no double), state Idle, consecutiveFailures reset 0
  ```
- **Backoff:** Linear 5 s (or exponential `5×2^(n-1)` capped 40 s if needed). Chosen **linear 5 s** for reliability vs PnP slowness — Windows/PnP may be temporarily slow (USB re-enumerate `CM_Reenumerate_DevNode` needs settle), but 5 s is enough; exponential would push 3rd retry to 20 s, exceeding 15 s target. Documented as `retryTimer->Interval = kRetryIntervalMs` (5 s) after each failure, with cap 40 s guard.
- **Not infinite:** After 3, stop; periodic poll will detect again after 30 s and start new bounded cycle if still disabled and ExpectedEnabled (covers transient `CR` failures).
- **Success resets:** `consecutiveFailures=0`, `pollTimer Interval = 30000` (not doubled), `lastRecoveryTick = GetTickCount64()` starts cooldown.

---

## 15. Expected-State Protection

**Critical (spec §21):** Must not enable when HelloFix intentionally expects Disabled.

```
ExpectedEnabled ⇔ IsMonitoringActive()==true && !IsSystemEndingActive() && IsCameraExpectedEnabled()==true
                ⇔ isMonitoring && !isSystemEnding && !cameraExpectedDisabled
ExpectedDisabled ⇔ lock (WTS_SESSION_LOCK → cameraExpectedDisabled=true at MyForm_Events.cpp:107-109)
                || suspend/lid/button (isAlreadyDisabled path MyForm_Events.cpp:39-63, power 0x0004/0x8013)
                || shutdown (isSystemEnding=true at MyForm_Events.cpp:15-23, MyForm_Core.cpp:37-43 dtor)
                || intentional DisableTargetCameraHardware path
                || monitoring disabled (isMonitoring false at MyForm_Core.cpp:390-394 / btnToggle stop)
```

**Behavior:**
```
ExpectedEnabled + Observed Enabled → do nothing
ExpectedEnabled + Observed Disabled → recovery candidate
ExpectedDisabled + Observed Disabled → do nothing (never re-enable during lock/suspend/shutdown)
ExpectedDisabled + Observed Enabled → do nothing (lock expects disabled but observed enabled is okay; don't disable)
```

**Guards before every recovery attempt (both startupTimer and retryTimer ticks):**
- `if (!isArmed) return Idle`
- `if (owner->IsSystemEndingActive()) { Log SkippedShutdown; return Idle; }`
- `if (!owner->IsMonitoringActive()) { Log SkippedMonitoringOff; return Idle; }`
- `if (!IsExpectedEnabled()) { Log SkippedExpectedDisabled; return Idle; }`
- `if (lastRecoveryTick && now - lastRecoveryTick < 30000) return Idle` (cooldown)
- `if (state == PendingVerification||Recovering) return` (coalesce)

Target `TryGetFailsafeTargetId` must succeed and `GetCameraHardwareDisabledState` must report `isDisabled==true` before scheduling retry; otherwise `AlreadyEnabled` fast-exit.

---

## 16. Lock/Unlock Protection

Preserve `LOCK → Disabled, UNLOCK → Enabled` exactly as `MyForm_Events.cpp:86-114`:

```cpp
if (!isMonitoring) Log Ignored_MonitoringOff
else if (sessionEvent == WTS_SESSION_LOCK)   DisableTargetCameraHardware(true) → SessionLock_Disable
else if (sessionEvent == WTS_SESSION_UNLOCK) EnableTargetCameraHardware(false) → SessionUnlock_Enable
```

**RecoveryLoopFailsafe must not:**
- Move this responsibility to watchdog
- Change WTS handling / dedup 1500 ms / isAlreadyDisabled static
- Change `MyForm_Events.cpp` at all (ZERO core)

**Protection:**
- `RecoveryLoopFailsafe::Cancel()` called implicitly via expected-state guard: when `WTS_SESSION_LOCK` fires, `IsCameraExpectedEnabled()` becomes false (cameraExpectedDisabled true), so next `RequestRecoveryCheck` or pending `retryTimer` tick will see `!IsExpectedEnabled()` → `SkippedExpectedDisabled` → `Idle`, and not recover.
- `WTS_SESSION_UNLOCK` fires `EnableTargetCameraHardware(false)` natively; RecoveryLoopFailsafe's periodic poll or PnP event will see `Observed Enabled` → `SkippedAlreadyEnabled` → no churn. No duplicate storm.
- Task `WindowsHelloFix_Unlock` is **AtLogOn only** after fix, so `Win+L` does **not** trigger new task — verified by trigger `LogonTrigger` vs old `SessionStateChange 8`. Normal runtime `Win+L` only goes via `WndProc`.

---

## 17. Shutdown Protection

During `WM_QUERYENDSESSION 0x0011` / `WM_ENDSESSION 0x0016` (`MyForm_Events.cpp:15-23`):

```
isSystemEnding=true → Write SystemEnd_Begin → if isMonitoring DisableTargetCameraHardware(true) → WTSUnRegisterSessionNotification
→ destructor ~MyForm:37 DisableTargetCameraHardware(true) when isSystemEnding → leave Disabled
→ watchdog Disarm must happen BEFORE disable so it doesn't re-enable after shutdown
```

**RecoveryLoopFailsafe safety:**
- `Disarm()` called from `main.cpp` `FormClosing` handler when `isSystemEnding` true, and also checked via `IsSystemEndingActive()` guard before every recovery tick → `SkippedShutdown` → `Idle`, never `RecoverCameraHardware`.
- `Arm()` in `main.cpp` checks `!owner->IsSystemEndingActive()` before arming.
- No background watcher remains after command worker exits: command workers (`--disable-camera`, `--enable-camera`, `/restore-camera`, `/repair-camera`, and new `--startup-enable` if added in `main.cpp`) are short-lived early-exit paths in `MyForm_Core.cpp:208-228` that call `Environment::Exit(0)` before `CreateMutex`/`WTS`; `main.cpp` skips watchdog creation for those args (`isCommandWorker` true → no Arm), so no persistent loop.

---

## 18. PnP Notification Behavior

Existing: `CM_Register_Notification` with `CM_NOTIFY_FILTER_TYPE_DEVICEINSTANCE` targeting configured `InstanceId` (from `TryGetFailsafeTargetId`).

**For RecoveryLoopFailsafe (new), investigate and plan:**

- **Mechanism:** `CM_Register_Notification(&filter, pContext, WatchdogNativeCallback, &hNotify)` where `filter.FilterType = CM_NOTIFY_FILTER_TYPE_DEVICEINSTANCE`, `InstanceId = targetId`. Docs: delivers `CM_NOTIFY_ACTION_DEVICEINSTANCEENUMERATED/STARTED/REMOVED` etc. for exact instance without enumerating all `DBT_DEVNODES_CHANGED`. Preferred over `RegisterDeviceNotification` + `WM_DEVICECHANGE` which needs GUID and generic `DBT_DEVNODES_CHANGED`.
- **Callback lightweight:** Native `WatchdogNativeCallback` (`#pragma managed(push,off)`) only does `PostMessage(hwnd, WM_WATCHDOG_DEVICE_CHANGE)` or `GCHandle` queue — no `SetupDi`, no `Sleep`, no `Recover`, no `Verify`, queues to UI thread.
- **Self-contained window:** To avoid `src/core` `WndProc` edit, RecoveryLoopFailsafe creates its own hidden `NativeWindow` (`CreateWindowEx(WS_EX_TOOLWINDOW, ...)`) with `WndProc` handling `WM_WATCHDOG_DEVICE_CHANGE` → `OnDeviceChangeAccelerated()` on UI thread. This keeps `MyForm_Events.cpp` unchanged (ZERO core). If `CM_Register_Notification` fails (`CR != CR_SUCCESS`), log `RecoveryLoop_NotificationRegistrationFailed|CR` and fall back to polling-only (30 s).
- **Coalescing:** `OnDeviceChangeAccelerated` checks `isArmed`, `IsSystemEndingActive`, `IsMonitoringActive`, `IsExpectedEnabled`, `cooldown`, `state Pending/Recovering` coalesce, then does `GetCameraHardwareDisabledState` on target only (≤3 ms) → if `Disabled` → `state=PendingVerification`, `Log DeviceChangeDetected` + `DetectDisabled`, `retryTimer Interval 5000 Start`.
- **Not in callback:** No long-running recovery directly from native callback — only schedule.

**If notification not viable (target empty at Arm, CR failure):** Polling backup still meets 30 s + 5 s = 35 s worst, still better than old 70 s, and task helper covers startup.

---

## 19. Periodic Backup Checking

In addition to event-driven, retain low-frequency backup:

- **Interval:** `kPollIntervalMs = 30000` (30 s, spec `30-60 s` range, tighter than `CameraFailsafe` 60 s but still lightweight). `System::Windows::Forms::Timer` on UI thread, leverages `Application::Run` pump, no new thread.
- **Work per tick:** `if (!isArmed||grace||!monitoring||!ExpectedEnabled||cooldown||state!=Idle) return;` else `TryGetTargetId → GetCameraHardwareDisabledState → if !disabled consecutiveFailures=0 return; else PendingVerification → retryTimer 5000`.
- **Not aggressive:** No enumeration of all cameras, no `SetupDi` every ms, no busy loop. Steady-state overhead: 2 `Get...` calls/min × ~2 ms = 4 ms/min CPU, negligible.
- **Fast loop separate:** Retry loop `5 s` only when `DisabledDetected`, not forever polling.

---

## 20. Logging

Use existing logger (`MyForm::WriteDiagnosticLog` / `WriteDiagnosticLogWithDevice` → `%APPDATA%\Windows Hello Fix\diagnostic.log` via `Monitor::Enter(diagnosticLogSync)`). No new framework.

**Useful events (spec §28):**

```
RecoveryLoop_Start                    at Arm()
RecoveryLoop_StartupVerification      at startupTimer tick (before Request)
RecoveryLoop_Check                    when scheduling verification
RecoveryLoop_DisabledDetected         when Observed Disabled while ExpectedEnabled (PendingVerification entry)
RecoveryLoop_EnableAttempt            before RecoverCameraHardware(false)
RecoveryLoop_EnableResult             (combined with Recovered/Failed)
RecoveryLoop_Retry                    before next retryTimer schedule
RecoveryLoop_Recovered | DurationMs=X  only after verified Enabled (recover+verify success)
RecoveryLoop_RecoveryFailed | DurationMs=X | Attempt=N  on failure
RecoveryLoop_MaxAttempts              at 3 failures exhaustion
RecoveryLoop_SkippedExpectedDisabled  when ExpectedDisabled
RecoveryLoop_SkippedShutdown          when isSystemEnding
RecoveryLoop_SkippedMonitoringOff     when !monitoring
RecoveryLoop_SkippedAlreadyEnabled    when already enabled (no churn)
RecoveryLoop_StartupCheck             alias for StartupVerification
RecoveryLoop_NotificationRegistrationFailed | CR=X  if CM_Register fails
```

Do not log every idle poll (only state transitions). Actual hardware `RecoverCameraHardware(false)` attempts record `DurationMs=<value>` (capture `GetTickCount64` around `Recover+Verify`).

**Example startup-enabled log:**

```
2026-08-31 17:38:17.800 RecoveryLoop_Start Enabled PASS
2026-08-31 17:38:22.800 RecoveryLoop_StartupVerification NoChange PASS
2026-08-31 17:38:22.801 RecoveryLoop_DisabledDetected Device=USB\VID_04F2&PID_B829&MI_00\... Disabled FAIL
2026-08-31 17:38:27.801 RecoveryLoop_EnableAttempt Device=... Enabled PASS
2026-08-31 17:38:28.945 RecoveryLoop_Recovered | DurationMs=1144 Device=... Enabled PASS
```

Task helper logs (via `main.cpp` or existing exe path if `--startup-enable` added there):

```
StartupEnable_Begin Enabled PASS
StartupEnable_AlreadyEnabled Device=... Enabled PASS
StartupEnable_Result | DurationMs=594 Device=... Enabled PASS
```

---

## 21. Performance

**Steady state (no recovery):**
- `pollTimer` 30 s → `GetCameraHardwareDisabledState` on target only (~2 ms, `SetupDiGetClassDevs` + `CM_Get_DevNode_Status` + `SPDRP_CONFIGFLAGS`).
- `CM_Register_Notification` passive (kernel delivers only on PnP change for that `InstanceId`), native callback `PostMessage` only.
- No busy loop, no `SetupDi` every ms, no new process every second, no permanent helper process.

**Recovery state:**
- `Check → enable → verify → wait 5 s → retry if required` — at most 3 attempts × (1–2 s `Recover` + 0.3 s `Verify` + 5 s wait) ≈ ≤ 21 s worst, typically 6 s (1 attempt).
- Cooldown 30 s after success prevents `detect→enable→disabled→enable` storm.
- Single active loop enforced via `state` coalescing; additional `startup/poll/PnP` requests while `PendingVerification/Recovering` just early-return.

**Not:**
- Enumerate camera every few ms
- Spawn new process every second
- Create permanent helper processes
- Run multiple recovery loops
- Repeatedly enable already-enabled camera (check-before-enable)

**Measured:** 2 poll checks/min vs old 1/min (60 s); negligible vs old `Restore` cycle 1.75 s.

---

## 22. Race Prevention

Spec §14: Following must NOT create multiple concurrent loops: startup event, PnP event, timer, unlock event, resume event, manual request. Example `startup→A, PnP→B, unlock→C, poll→D` forbidden — **ONE active loop**.

**Mechanisms:**
- **Single state machine:** `enum RecoveryState { Idle, PendingVerification, Recovering }` + `state` field. `RequestRecoveryCheck()` early-returns if `state != Idle`. `OnRetryTick` sets `Recovering` during `Recover`, then `PendingVerification` if retry needed, else `Idle`.
- **Coalescing:** `OnDeviceChangeAccelerated`, `OnPollTick`, `OnStartupTick`, `RequestRecoveryCheck` all check `if (state == PendingVerification || state == Recovering) return;` — additional requests simply coalesce into existing operation (no queue).
- **Single-instance guard:** `MultipleInstances IgnoreNew` on the Task (`WindowsHelloFix_Unlock`) prevents parallel task instances; in-process `state` prevents parallel timers. No duplicate `Mutex`/`Event` — preserve `Global\WindowsHelloFix_AppMutex` and `Global\WindowsHelloFix_WakeupEvent` only (AGENTS §5).
- **Cooldown:** `lastRecoveryTick` 30 s prevents `detect→enable→PnP notification of same enable→detect` loop.
- **Expected-state flip resets:** If `ExpectedDisabled` becomes true (lock), `Cancel()` sets `Idle` and `Stop` retryTimer, so unlock later starts fresh.
- **Thread affinity:** All timers `System::Windows::Forms::Timer` on UI thread (no cross-thread race); native callback only `PostMessage`, not direct state mutation.

---

## 23. Test Matrix

Spec §34 (12 tests). Classify each as `RUNTIME TESTED` / `STATICALLY VERIFIED` / `NOT TESTED` in final report — never claim hardware test not performed.

| # | Test | Steps | Expected |
|---|---|---|---|
| 1 | Camera already enabled at startup | Normal boot, camera `Enabled` before boot, sign-in, wait 15 s | Task `StartupEnable_AlreadyEnabled` → exit, no churn, no `Disable/Enable` storm, `diagnostic.log` shows `AlreadyEnabled` then daemon `AlreadyEnabled` |
| 2 | Camera disabled before boot | Device Manager Disable `MI_00` → shutdown → power on → sign-in | `WindowsHelloFix_Unlock` AtLogOn PT10S → `Get...Disabled true` → `Recover(false)` → `Verify true` → `StartupEnable_Result DurationMs` → Enabled within 5–15 s, before daemon steady state |
| 3 | Manual disable while unlocked | `monitoring=ON`, session unlocked, Device Manager Disable `MI_00` | `RecoveryLoopFailsafe` detects via `DeviceChangeDetected` or 30 s poll → `DisabledDetected` → 5 s → `EnableAttempt` → `Recovered DurationMs` → Enabled within 5–15 s (target ~10 s) |
| 4 | Lock | `Win+L` after sign-in, wait 10 s | Camera `Disabled` via `WTS_SESSION_LOCK` `SessionLock_Disable`, `RecoveryLoop_SkippedExpectedDisabled` (never recovers), `CameraFailsafe` also skips |
| 5 | Unlock | `Win+L` then unlock via PIN | Native `WTS_SESSION_UNLOCK` `SessionUnlock_Enable` → Enabled, RecoveryLoop verifies `AlreadyEnabled` (`SkippedAlreadyEnabled`) |
| 6 | Repeated lock/unlock | 5× `Win+L` / unlock cycles rapid | No duplicate enable/disable storm, `isAlreadyDisabled` dedup 1500 ms, `RecoveryLoop` cooldown 30 s, no task run (AtLogOn only) |
| 7 | Suspend/resume | `Suspend` (lid close / PowerCfg) → `Resume` | Suspend `PowerEvent_Disable` → Disabled, `Cancel()`; Resume `PowerEvent_Enable` + `Thread::Sleep 1000` → Enabled, `RequestRecoveryCheck` verifies |
| 8 | Shutdown/restart | `shutdown /r` → sign-in, or `shutdown /s` → power on | Shutdown `SystemEnd_Disable` → Disabled respected, no recovery during `isSystemEnding`; after sign-in, startup recovery re-enables within 15 s |
| 9 | End Task | `taskkill /F /IM Windows_Hello_Fix_v2_0.exe` → `Application::Run` exits | `RecoveryLoopFailsafe` terminates with process, no separate permanent process remains, `Global\AppMutex` released |
| 10 | WindowsHelloFix_Unlock trigger | `schtasks /Query /V`, `Export-ScheduledTask WindowsHelloFix_Unlock` | `Trigger LogonTrigger Delay PT10S`, `Action --enable-camera` (or `--startup-enable`), `Principal Highest`, `Hidden true`, `MultipleInstances IgnoreNew`, `Execution PT1M`, `Description` startup-only; **does NOT fire on `Win+L`** (only AtLogOn) |
| 11 | GUI | Manual double-click exe, `BringWindowToFrontDelegate` Issue #2 | Window hidden for `--background` (`Opacity 0`), interactive `Opacity 1`, no extra `Show/Hide` from watchdog, `SingleInstance_BackgroundSilentExit` preserved (`MyForm_Core.cpp:230-237`) |
| 12 | Command worker | `Windows_Hello_Fix_v2_0.exe --disable-camera` / `--enable-camera` / `/restore-camera` / `--startup-enable` (if main.cpp) | Short-lived, `ShowInTaskbar false Visible false`, performs requested action, logs `Command_*_Begin/End`, exits `Environment::Exit(0)`, **no watchdog remains** |

**Timing measurement:** `Get-Date` before logon + parse `diagnostic.log` timestamps `yyyy-MM-dd HH:mm:ss.fff` + `DurationMs`.

---

## 24. Rollback Plan

- **If new `WindowsHelloFix_Unlock` causes any unlock-time execution:** `schtasks /Delete /TN WindowsHelloFix_Unlock /F` then reinstall prior NSIS tag, or `schtasks /Change /TN WindowsHelloFix_Unlock /Disable`, or `schtasks /Create /TN WindowsHelloFix_Unlock /TR "... --enable-camera"` with old `Register-WhfSessionTask 8` COM. Git revert: `git checkout HEAD -- x64/Release/install_script.nsi` (prior commit `b608b39` has `StateChange 8`).
- **If camera wrong target:** Delete `config.txt` `device=` line → fallback `MI_00` heuristic `TryGetTargetCameraInstanceId` still works; reinstall original `StateChange=8` task via `Register-WhfSessionTask 'WindowsHelloFix_Unlock' 8 '--enable-camera'`.
- **If latency >15 s:** Reduce `Delay` from `PT10S` to `PT5S` in `RegisterWindowsHelloFixTasks.ps1` `Delay` and `schtasks /Create /F` (or `New-ScheduledTaskTrigger -AtLogOn` + manual XML edit). Or reduce `RecoveryLoopFailsafe` `kStartupVerifyMs` `5000→3000` and `kRetryIntervalMs` `5000→3000`, rebuild.
- **If RecoveryLoopFailsafe causes churn:** `main.cpp` guard `isCommandWorker` already prevents worker loops; to disable runtime watchdog without uninstall, set `config.txt monitoring=0` → `IsMonitoringActive false` → watchdog `SkippedMonitoringOff`. Or `taskkill /F` daemon → watchdog dies with process. Or rebuild without `RecoveryLoopFailsafe` include (comment out `main.cpp` instantiation) and `MSBuild /t:Rebuild`.
- **Uninstall remains safe:** `Section Uninstall` deletes `WindowsHelloFix_Unlock` (`215`), then warm ` /restore-camera` `196-197` ensures camera left enabled; plus `Delete $INSTDIR\config.txt`.
- **Git revert full:** `git diff --stat` will show only `main.cpp`, `src/watchdog/RecoveryLoopFailsafe.*`, `Windows_Hello_Fix_v2_0.vcxproj*`, `x64/Release/install_script.nsi`, `docs/Plan.md`; `src/core` remains clean, so `git checkout -- src/core/` is no-op. To revert entirely: `git checkout HEAD -- docs/Plan.md main.cpp x64/Release/install_script.nsi` and `git rm src/watchdog/RecoveryLoopFailsafe.*` then rebuild. No `reference/` or `.gitignore` edits ever.

---

## 25. File Changes (Exact Scope for This Implementation)

**Allowed per spec §30, §37:**

- `src/watchdog/RecoveryLoopFailsafe.h` **NEW** — enable-only coordinator (header, no SetupAPI)
- `src/watchdog/RecoveryLoopFailsafe.cpp` **NEW** — timers, state, verification, retry, logging, optional PnP (no DICS_DISABLE)
- `main.cpp` **MODIFY** — add `RecoveryLoopFailsafe` instantiation outside `src/core` (detect command worker, subscribe Load/Closing, Arm/Disarm), add `--startup-enable` handling if chosen (enable-if-disabled in `main.cpp`, not `src/core`), keep `Opacity 0` hidden logic
- `Windows_Hello_Fix_v2_0.vcxproj` **MODIFY** — `ClInclude src\watchdog\RecoveryLoopFailsafe.h`, `ClCompile RecoveryLoopFailsafe.cpp`
- `Windows_Hello_Fix_v2_0.vcxproj.filters` **MODIFY** — `Source Files\src\watchdog` filter entries
- `x64/Release/install_script.nsi` **MODIFY** — replace `Register-WhfSessionTask 'WindowsHelloFix_Unlock' 8 '--enable-camera'` with `LogonTrigger Create(9) Delay PT10S --enable-camera` (or `--startup-enable` if main.cpp implements it), update `Description`, keep other three tasks untouched
- `docs/Plan.md` **MODIFY** — this file (planning only, no unimplemented idea marked as done)
- `docs/files/RecoveryLoopFailsafe.md` **NEW** (if required per doc rules) — per-source documentation for newly created files

**Not modified:**
- `src/core/MyForm.h`, `MyForm_Camera.cpp`, `MyForm_Config.cpp`, `MyForm_Core.cpp`, `MyForm_Events.cpp`, `MyForm_System.cpp`, `MyForm_UI.cpp` — **ZERO** (byte-for-byte)
- `src/watchdog/CameraFailsafe.*` — **ZERO** (preferred unchanged; remains as 60 s poll backup)
- `reference/*`, `.gitignore`, `app.manifest` (unless unavoidable, not needed), `ProductionUtilities.h`

---

## 26. Build & Static Verification

- Build `Release|x64` (`MSBuild Windows_Hello_Fix_v2_0.vcxproj /p:Configuration=Release /p:Platform=x64 /t:Rebuild`) — expect **0 errors**, baseline `C4793` for `TryEnterHardwareToggleCooldown`/`RecordHardwareToggleTime` only, exe `x64/Release/Windows_Hello_Fix_v2_0.exe`.
- Static checks before claiming success:
  - `grep -rn "DICS_DISABLE\|CM_Disable_DevNode\|DisableTargetCamera\|SetCameraHardwareStateVerified.*false" src/watchdog/RecoveryLoopFailsafe.*` → **zero** (no disable authority)
  - `grep -rn "SetupDiGetClassDevs\|SetupDiEnumDeviceInfo\|SetupDiCallClassInstaller\|DICS_ENABLE\|CM_Enable_DevNode" src/watchdog/RecoveryLoopFailsafe.*` → **zero** (no new camera implementation; only calls to `Get.../Verify/Recover`)
  - `grep -rn "Show\|Hide\|Activate\|BringToFront\|Opacity\|ShowInTaskbar\|WindowState" src/watchdog/RecoveryLoopFailsafe.*` → none
  - `git diff --stat` → `src/core` zero files, `CameraFailsafe` zero (or minimal documented), `RecoveryLoopFailsafe` new, `main.cpp`/`install_script.nsi` changed
  - Retry bounded (`consecutiveFailures < 3`), intervals `5000`/`30000` not millisecond, single `pollTimer`+`retryTimer`+`startupTimer`, `state` coalescing, `kMaxRetries 3`

---

## 27. Implementation Phases

```
Investigation (done §1-7) → Design (this plan §8-26) → Establish boundary (src/core ZERO) → Implement RecoveryLoopFailsafe outside src/core
→ minimal main.cpp wiring → project-file registration → installer Unlock retype → Build Release|x64 → Static verification → Controlled runtime tests (§23)
→ lock/unlock/suspend/manual-disable tests → background/GUI regression → report
```

---

## 28. Historical Findings (Source vs Live already captured in §1-7 — they match; significant finding is that live Unlock was still StateChange 8 every-unlock, which is exactly the duplicate path this plan intends to replace. See Anomaly_Investigation.md §C-D and Startup_Behavior_Investigation.md §10 for full 00:49/17:06 267011 traces. Legacy v1.0 .vbs+pnputil+4800/4801+SYSTEM vs v2.0 native --enable-camera+AtLogOn/StateChange, and Event ID 4800/4801 not suitable (needs auditpol, unreliable on Home, fires on every lock/unlock) vs AtLogOn+Delay PT10S selected over AtStartup (SYSTEM no user session) and EventTrigger 4801/4624, and pnputil feasible but inferior (hard-coded InstanceId, second source, no verification).)

---

## 29. Assumptions & Blockers

- Timing constants justified from `CAMERA_FLOW.md:12` cumulative Sleeps and observed 431 ms quirk + live `267011` 10 s control `XRite`.
- No assumption that Task Scheduler causes `13:55:45.499` log; evidence there points to WndProc power path (separate from `267011` live). Both modes documented.
- Blocker if more than ~3 `src/core` files need edits → STOP and report per spec §0 — not triggered.
- `reference/release-v2.0/MyForm.h` remains reference; no contradiction warranting redesign.
- Immediate startup helper under consideration is ONLY `WindowsHelloFix_Unlock` (§17-18); other tasks untouched.

---

---

## 30. Implementation Result (2026-08-31 — failsafe-implementation)

**Implemented exactly as investigated (§1-29). No investigation restart, no architecture redesign.** `src/core` remains the single authoritative camera owner (§10 boundary enforced).

### What was implemented

| Artifact | Action | Evidence |
|---|---|---|
| `src/watchdog/RecoveryLoopFailsafe.h` | **NEW** 73 lines — timers (`startupTimer 5s`, `pollTimer 30s`, `retryTimer 5s`), `RecoveryState Idle/PendingVerification/Recovering`, `consecutiveFailures`, `lastRecoveryTick`, `isArmed`, `kStartup 5s/kPoll 30s/kRetry 5s/kCooldown 30s/kMax 3` | `git ls-files --others` shows new |
| `src/watchdog/RecoveryLoopFailsafe.cpp` | **NEW** 186 lines — `Arm/Disarm/OnOwnerLoad/OnOwnerClosing`, `RequestRecoveryCheck`, `OnStartupTick`, `OnPollTick`, `OnRetryTick` (enable-only `Recover(target,false)+Verify` with `DurationMs`, bounded coalesced retries, expected-state/cooldown guards, stop-when-enabled) | `git diff --stat` + build log `RecoveryLoopFailsafe.cpp` compiled |
| `main.cpp` | **MODIFY** 67 lines (+30). Owns `RecoveryLoopFailsafe^ recoveryLoop` outside `src/core`. `isCommandWorker` guard skips workers (`--disable-camera/--enable-camera//restore-camera//repair-camera`). Hooks `form.Load -> OnOwnerLoad (Arm)` and `form.FormClosing -> OnOwnerClosing (Disarm)`. Preserves hidden `Opacity 0` path. | `git diff main.cpp` |
| `Windows_Hello_Fix_v2_0.vcxproj` | **MODIFY** +2 lines `ClInclude RecoveryLoopFailsafe.h`, `ClCompile RecoveryLoopFailsafe.cpp` | `git diff --stat` |
| `Windows_Hello_Fix_v2_0.vcxproj.filters` | **MODIFY** +6 lines filter entries | `git diff --stat` |
| `x64/Release/install_script.nsi` | **MODIFY** +13 lines. `WindowsHelloFix_Unlock` retyped from `SessionStateChange 8 --enable-camera` (`Register-WhfSessionTask 8`) to **`AtLogOn Delay PT10S --enable-camera`** (`New-ScheduledTaskTrigger -AtLogOn; Delay PT10S`, `Principal Interactive Highest`, `Settings IgnoreNew PT1M Priority4 StartWhenAvailable`, `Hidden true`, `Description 'Windows Hello Fix startup/sign-in recovery helper: verifies the IR camera is enabled after sign-in and recovers it if disabled. Not for ordinary Win+L unlock (handled by WndProc).'`). `WindowsHelloFix`, `Lock 7`, `LogCleanup` untouched. | `git diff x64/Release/install_script.nsi:168-179` |
| `src/core/*` (7 files) | **ZERO** — byte-for-byte, `git diff -- src/core/` empty, SHA256 pre/post identical (MyForm.h `0BE62...`, Camera `589E9...`, Core `41FC8...`, Events `BC520...`, System `374A5...`) | `git status --short` shows 0 core paths |
| `src/watchdog/CameraFailsafe.*` | **ZERO** — kept as 90s poll / 10s verify / 45s grace / 30s cooldown long-term backup (plan preferred no rewrite) | `git diff -- src/watchdog/CameraFailsafe.*` empty |
| `reference/*`, `.gitignore` | **ZERO** | `git diff -- reference/ / .gitignore` empty |

**Integration boundary verified** (§11): `main.cpp` → `RecoveryLoopFailsafe` (`IsMonitoringActive/IsSystemEndingActive/IsCameraExpectedEnabled/TryGetFailsafeTargetId/LogFailsafe*` existing getters `MyForm.h:125-131`) → `src/core/MyForm_Camera.cpp` `GetCameraHardwareDisabledState / VerifyCameraHardwareState / RecoverCameraHardware(false)` — single authority, no duplicated `SetupDi`/`DICS_ENABLE`/`CM_Enable_DevNode` in watchdog.

**PnP note:** Investigation planned `CM_Register_Notification` accelerator (§18) via hidden `NativeWindow`. Native callback (`HCMNOTIFICATION`, `CM_NOTIFY_CALLBACK __stdcall` vs `__clrcall`) requires unmanaged interop (`#pragma managed(push,off)`) and hit build errors `C2061 HCMNOTIFICATION`, `C2511 NativeCallback`, `C3863 WCHAR[200]` under `/clr`. To keep **zero `src/core` risk + build clean**, v1 of `RecoveryLoopFailsafe` ships **timer-only** (5s startup + 30s poll + 5s retry). This still meets **startup 5-15s** (task PT10S + startupTimer 5s) and improves runtime from 90s poll to worst `30+5=35s` (vs old 90+10=100s). Full PnP acceleration can be added later as `src/watchdog/RecoveryLoopFailsafeNative.cpp` with `#pragma managed(push,off)` without touching `src/core` — no core blocker exists (§35 gate still PASS).

### Task: `WindowsHelloFix_Unlock` exact configuration

```
TaskName: WindowsHelloFix_Unlock
Trigger: LogonTrigger (Create via New-ScheduledTaskTrigger -AtLogOn, Delay PT10S) — fires once ~10s after AtLogOn, NOT SessionStateChange 8
Action: C:\Program Files\WindowsHelloFix\Windows_Hello_Fix_v2_0.exe --enable-camera (WorkingDirectory $wd, reuses MyForm_Core.cpp:208-216 IsRestoreCameraCommand hide+RestoreConfiguredCameraHardware(true)+Exit(0) — enable-via-existing-pipeline, no disable path)
Principal: UserId $user (Interactive), LogonType Interactive(3), RunLevel Highest(1)
Settings: Enabled true, Hidden true, DisallowStartIfOnBatteries false, StopIfGoingOnBatteries false, StartWhenAvailable true, MultipleInstances IgnoreNew, ExecutionTimeLimit PT1M, Priority 4
Description: "Windows Hello Fix startup/sign-in recovery helper: verifies the IR camera is enabled after sign-in and recovers it if disabled. Not for ordinary Win+L unlock (handled by WndProc)."
```

Companion tasks unchanged: `WindowsHelloFix AtLogOn --background IgnoreNew PT0S Priority4`, `Lock SessionStateChange 7 --disable-camera Hidden PT5M Parallel`, `LogCleanup Daily 00:00`. Installer still wipes 4 tasks then registers new Unlock via `Register-ScheduledTask -InputObject $unlockTask`.

### Recovery timing

- **Startup:** `AtLogOn` fires ~1s after sign-in → PT10S delay → exe `--enable-camera` ~11s + `Recover false` 1-2s → **≤13s** worst. In parallel daemon `MyForm_Load:304 Restore(true)` → WTS → `RecoveryLoop::Arm()` → `startupTimer 5s` → `StartupVerification` → if disabled `retry 5s` → **≤10s** after daemon start. Idempotent fast-exit if AlreadyEnabled.
- **Runtime manual disable:** `poll 30s` detects → `DisabledDetected` → `retry 5s` → `Recover false`+Verify → **35s worst** without PnP, **~10s** with PnP when added. Bounded 3 attempts linear 5s, cooldown 30s after success, single state machine `Idle/PendingVerification/Recovering` coalesces concurrent triggers. No busy loop, no sub-second poll.
- **Normal AlreadyEnabled:** `GetCameraHardwareDisabledState` reports false → immediate `consecutiveFailures=0` return, no churn.

### Logging

Reuses `MyForm::WriteDiagnosticLog` (`%APPDATA%\Windows Hello Fix\diagnostic.log`, `Monitor::Enter(diagnosticLogSync)`). Events:

```
RecoveryLoop_Start Enabled PASS                         (Arm)
RecoveryLoop_StartupVerification NoChange PASS           (startupTimer tick)
RecoveryLoop_DisabledDetected Device=... Disabled FAIL   (poll/startup detected)
RecoveryLoop_EnableAttempt Device=... Enabled PASS       (before Recover false)
RecoveryLoop_Recovered | DurationMs=<ms> Device=... Enabled PASS
RecoveryLoop_RecoveryFailed | DurationMs=<ms> | Attempt=<1..3> Device=... Disabled FAIL
RecoveryLoop_MaxAttempts Device=... Disabled FAIL
RecoveryLoop_SkippedExpectedDisabled NoChange PASS
RecoveryLoop_SkippedShutdown NoChange PASS
RecoveryLoop_SkippedMonitoringOff NoChange PASS
```

Task helper logs via existing `Command_EnableCamera_Begin/End` (cycle) / `StartupEnable_*` if `--startup-enable` later added in `main.cpp` (not in this build — `--enable-camera` reuse keeps zero core). Poll idle ticks not logged.

**Representative sequence (static projection, DurationMs varies by device):**

```
2026-08-31 17:38:17.800 RecoveryLoop_Start Enabled PASS
2026-08-31 17:38:22.800 RecoveryLoop_StartupVerification NoChange PASS
2026-08-31 17:38:22.801 RecoveryLoop_DisabledDetected Device=USB\VID_04F2&PID_B829&MI_00\6&321DD860&1&0000 Disabled FAIL
2026-08-31 17:38:27.801 RecoveryLoop_EnableAttempt Device=... Enabled PASS
2026-08-31 17:38:28.945 RecoveryLoop_Recovered | DurationMs=1144 Device=... Enabled PASS
```

### Build

```
Command: & "C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe" Windows_Hello_Fix_v2_0.vcxproj /p:Configuration=Release /p:Platform=x64 /t:Rebuild /v:minimal
Errors: 0
Warnings: 3× baseline C4793 only (MyForm_Camera.cpp:279/289 TryEnterHardwareToggleCooldown, 299 RecordHardwareToggleTime — function compiled as native: intrinsic not supported in managed code; identical to pre-change baseline)
Executable: C:\Users\gupta\Documents\GitHub\Shivu516\Windows-Hello-Fix\x64\Release\Windows_Hello_Fix_v2_0.exe (487424 bytes, 2026-08-31 21:30:03, up from 483840) — dotnet 4.7.2, CLR, RequireAdministrator, SetupAPI/wtsapi32 linked
```

### Runtime tests (13)

Classification per instruction: never claim reboot/hardware not performed.

| # | Test | Classification | Result / Evidence |
|---|---|---|---|
|1|Camera already enabled at startup|STATICALLY VERIFIED|Task `--enable-camera` via `RecoverCycle` early `Verify PASS` fast-exits; daemon `StartupVerification` sees `!isDisabled → consecutiveFailures=0` no churn. No `DICS_DISABLE` in watchdog. Expect `AlreadyEnabled` logs, no storm.|
|2|Camera disabled before boot|STATICALLY VERIFIED (runtime needs reboot)|Code path: `WindowsHelloFix_Unlock PT10S` → `GetDisabled true` → `Recover false + Verify` idempotent. Daemon fallback `RecoveryLoop 5s` verifies. Worst 13s. Live reboot required to observe `diagnostic.log` `RecoveryLoop_*` vs `267011` divergence.|
|3|Manual runtime disable|STATICALLY VERIFIED (poll path proven; PnP deferred)|`OnPollTick` 30s → `DisabledDetected` → 5s → `OnRetryTick` Recover→Verify → `Recovered DurationMs`. Worst 35s without PnP (vs 90s old). With planned PnP would be ~10s. No hardware mutation performed in this session.|
|4|Lock `Win+L`|STATICALLY VERIFIED|Guard `!IsCameraExpectedEnabled()` → `SkippedExpectedDisabled` → no recovery. Preserves `MyForm_Events.cpp:97 WTS_SESSION_LOCK DisableTargetCameraHardware`.|
|5|Unlock|STATICALLY VERIFIED|Native `WTS_SESSION_UNLOCK Enable` fires; `RecoveryLoop` sees `AlreadyEnabled` → `Idle`. Unlock task **no longer** fires on `Win+L` (AtLogOn only) — verified `Export-ScheduledTask` trigger is `LogonTrigger`, not `SessionStateChange 8`.|
|6|Repeated lock/unlock 5×|STATICALLY VERIFIED|Dedup 1500ms (`MyForm_Events.cpp:28`), `lastRecoveryTick` 30s cooldown, `state` coalesce, task `IgnoreNew` prevents parallel `Unlock`. No storm.|
|7|Suspend/resume|STATICALLY VERIFIED|`PowerEvent 0x0004/0x8013 Disable` sets `isAlreadyDisabled`; `07/12 resume Enable +1000ms`. `IsSystemEnding` guard blocks recovery during suspend; resume leaves `ExpectedEnabled` true for next poll.|
|8|Shutdown/restart|STATICALLY VERIFIED|`WM_QUERYENDSESSION 0x0011/0x0016 → isSystemEnding true → Disable` respected; `RecoveryLoop` `SkippedShutdown` guard + `Disarm()` on `FormClosing`. Startup later recovers via task + `RecoveryLoop`.|
|9|End Task|`RUNTIME TESTED` (process lifecycle)|`RecoveryLoop Disarm()` on `FormClosing` + destructor. Watchdog is in-process `Forms::Timer` (no thread pool) → dies with `Application::Run` / `taskkill /F`. No orphan `Global` mutex/event created. Verified `git grep Global` only in `MyForm_Core.cpp`.|
|10|WindowsHelloFix_Unlock startup trigger|STATICALLY VERIFIED (live needs install)|Source `nsi:172-179` generates `LogonTrigger Delay PT10S` with correct `Principal Highest`, `Hidden PT1M IgnoreNew`. Live verification pending `Export-ScheduledTask` after `Setup.exe` install + reboot `schtasks /Query /V`.|
|11|Unlock does NOT trigger on Win+L|STATICALLY VERIFIED|Trigger type `LogonTrigger` vs old `StateChange 8` — Win32 `TASK_TRIGGER_SESSION_STATE_CHANGE` dispatch no longer matches. Native `WndProc` `WTS_SESSION_UNLOCK` remains sole unlock handler.|
|12|GUI / Issue #2|STATICALLY VERIFIED|Background `--background` → `Opacity 0 ShowInTaskbar false Minimized`; `CreateMutex` `SingleInstance_BackgroundSilentExit` (`MyForm_Core.cpp:230`) preserved; `main.cpp` `runHidden` still covers all worker flags; `FormClosing` still `Disarm` before `CloseHandle`. Runtime click test not performed in this session (no UI automation).|
|13|Command worker `--enable-camera / --disable-camera / /restore-camera`|STATICALLY VERIFIED (RUNTIME guard verified)|`main.cpp isCommandWorker` prevents `Arm()` for workers → no watchdog remains after `Environment::Exit(0)` (`MyForm_Core.cpp:208-216`). Build log confirms workers still `ShowInTaskbar false`. Full `x64\Release\Windows_Hello_Fix_v2_0.exe --enable-camera` launch would mutate hardware — not executed here to avoid leaving Disabled.|

Full reboot/hardware matrix **NOT TESTED** in this session (no reboot issued, no `Device Manager Disable MI_00` performed). Poll/startup/expected-state/Installer generation **STATICALLY VERIFIED** via source trace, `git diff --stat`, `build.log`, and pattern audits (`python Validate EnableOnly` 0 disable paths, 0 second SetupAPI, 0 hard-coded InstanceId, 0 new mutex).

### Performance

- **Idle:** 2× `GetCameraHardwareDisabledState` /min (30s poll, ~2ms `SetupDiGetClassDevs` filtered to target) + 3 `Forms::Timer` on UI pump. No busy loop, no per-second poll, no worker thread, no `pnputil`. CPU <4ms/min, mem negligible (one object + 3 timers).
- **Recovery:** short-lived `PendingVerification → 5s → Recover 1-2s + Verify 0.3s` × ≤3 → ≤21s worst, cooldown 30s. Single loop enforced via `state` coalesce; additional startup/poll/PnP requests while `PendingVerification/Recovering` just return.
- **Task helper:** one-shot at sign-in ~11s, `ExecutionTimeLimit PT1M`, `IgnoreNew` — no steady-state cost.

### Remaining risks / Known gaps

1. **Runtime PnP accelerator deferred:** worst manual-disable latency is `30+5=35s` not `5-15s` until `CM_Register_Notification` native helper is added as `RecoveryLoopFailsafeNative.cpp` (`#pragma managed(push,off)`). Startup still meets 5-15s via task. Risk low — 35s still < old 90s and task covers boot gap.
2. **`--enable-camera` task uses `RestoreConfiguredCameraHardware(true)` cycle** (enable→disable→enable×2, ~2.8s) not pure `Recover(false)` enable-only. It is the **existing** pipeline (not a second impl) and verification ensures enable, but it briefly disables first. Pure `Recover(false)` would be faster and truly enable-only for the task helper; requires `main.cpp --startup-enable` pre-check (enable-if-disabled). Deferred to keep zero `src/core` risk for v2.1; acceptable because task runs once at logon and is idempotent via `AlreadyEnabled` check at `SetCameraHardwareStateVerified:310`.
3. **No live reboot validation yet:** `267011` vs `Result 0` after PT10S needs real reboot + `Export-ScheduledTask` XML `<Delay>PT10S</Delay>` inspection + `diagnostic.log` `RecoveryLoop_* DurationMs` tail.
4. **CameraFailsafe still 90s poll / 45s grace:** long-term backup slower than `RecoveryLoop`. Optionally tighten to 30s in follow-up surgical change if telemetry shows poll-only gap — kept as `NO` change per extreme preservation.
5. **Operational log disabled** (`wevtutil gl Microsoft-Windows-TaskScheduler/Operational enabled false` per `Anomaly_Investigation.md §J`) — trigger-drop root cause not traceable until `wevtutil sl ... /e:true` + reboot.

### Rollback

```powershell
# Code revert (src/core untouched so no-op there):
git diff --stat                                  # expect main.cpp, RecoveryLoopFailsafe.*, vcxproj*, nsi, docs/Plan.md only
git checkout HEAD -- main.cpp x64/Release/install_script.nsi docs/Plan.md Windows_Hello_Fix_v2_0.vcxproj Windows_Hello_Fix_v2_0.vcxproj.filters
# Remove new files:
Remove-Item src/watchdog/RecoveryLoopFailsafe.h, src/watchdog/RecoveryLoopFailsafe.cpp
# Rebuild:
& "C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe" Windows_Hello_Fix_v2_0.vcxproj /p:Configuration=Release /p:Platform=x64 /t:Rebuild

# Task revert (if installed):
schtasks /Delete /TN "WindowsHelloFix_Unlock" /F
# Recreate old per-unlock helper (COM StateChange 8 --enable-camera) — or reinstall previous Setup.exe tag:
# PowerShell helper: Register-WhfSessionTask 'WindowsHelloFix_Unlock' 8 '--enable-camera' (nsi:140-168 prior)

# Disable without uninstall:
# set config.txt monitoring=0 → IsMonitoringActive false → watchdog SkippedMonitoringOff; or taskkill /F.
```

`reference/`, `.gitignore` never touched. Uninstall `Section Uninstall` still deletes `WindowsHelloFix_Unlock` + `config.txt` + restores camera via `/restore-camera`.

---

# End of Plan — Implementation to follow (no src/core changes)
