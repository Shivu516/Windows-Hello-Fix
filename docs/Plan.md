# Current Implementation Plan — Forensic Investigation & Reliability Plan

**Status:** IMPLEMENTED (tasks restructured, see below) — `WindowsHelloFix_Lock` removed, `WindowsHelloFix_Unlock` reworked to `AtLogOn` `PT1M` `--failsafe-boot` enable-only, `README.html` uninstall fixed.  
**Branch:** `test` at `ce54644` → `c3f1271` + this implementation (single-instance `ShowAlreadyRunningMessage`, startup `IsStartupDisabled`, `PerfTimer` already in `c3f1271`).  
**Baseline for comparison:** Original v2.0 release `39ac0ab` / tag `v2.0.0` and `5e45003` (x64 Release artifacts) vs current `test` `c3f1271`.

---

## 1. Current Camera Architecture — Every Automatic Enable/Disable Source

| # | Source | Trigger | Process | Function | Disable? | Enable? | Logs? | Expected? | Notes |
|---|--------|---------|---------|----------|----------|---------|-------|-----------|-------|
| 1 | `ToggleCameraHardware` | internal | any | `CameraHardware.cpp:10-15` SetupAPI `DIF_PROPERTYCHANGE` `DICS_DISABLE/ENABLE` | ✓ | ✓ | via `DeviceError` globals | yes | Stage 10-15, `g_last*` |
| 2 | `ToggleCameraHardwareCfgMgr` | internal | any | `CameraHardware.cpp:20-23` `CM_Disable/Enable_DevNode` + `CM_Reenumerate` | ✓ | ✓ | `g_last*` | yes | Stage 20-23 |
| 3 | `SetCameraHardwareStateVerified` | internal | any | `CameraRecovery.cpp` 3× `Toggle`→verify, `CfgMgr`→verify, optional re-init 250 ms, final toggle | ✓ | ✓ | no direct | yes | `reinitializeOnMismatch` only for disable |
| 4 | `RecoverCameraHardware` | internal | any | `CameraRecovery.cpp` `SetVerified(enable)` + optional cycle 350/900/500 sleeps |  | ✓ (cycle) | no | yes | `cycleDevice=true` at startup/restore |
| 5 | `RestoreAllCameraHardware` | internal | any | `CameraRecovery.cpp` recover every `ScanSystemCameras` |  | ✓ | no | yes | cycle |
| 6 | `DisableTargetCameraHardware(true)` | `HandleSessionEvent` lock, `HandlePowerEvent` suspend/lid/button, `HandleSystemEnd` shutdown, `ToggleMonitoring` off | daemon UI thread, `ApplicationController.cpp:151-180` | check `TryGetTarget…` → `AlreadyDisabled?` → `SetVerified`→`Verify` | ✓ |  | `Disable…_NoTarget/_AlreadyDisabled/_Result` + `DurationMs` (new) | yes | `retryOnFailure=true` |
| 7 | `EnableTargetCameraHardware(cycle)` | `HandleSessionEvent` unlock, `HandlePowerEvent` resume, `ToggleMonitoring` off, `MyForm_Load` first enable, shutdown cleanup | daemon UI thread `182-211` | `AlreadyEnabled?` → `Recover`→`Verify` |  | ✓ | `Enable…` logs + `DurationMs` | yes | `cycle=false` for unlock |
| 8 | `--disable-camera` / `--disable-camera` | Task `WindowsHelloFix_Lock` (StateChange 7) or manual `exe --disable-camera` | short-lived worker `Initialize:453-461` `Command_DisableCamera_Begin/End` → `DisableTarget…`+`Verify` | ✓ |  | `Command_Disable…` + `Disable…_Result` | yes | exits before mutex, can race daemon |
| 9 | `--enable-camera` / `/restore-camera` / `/repair-camera` / `--failsafe-boot` | Task `WindowsHelloFix_Unlock` (StateChange 8, `Recover` cycle) or manual, installer warm-up, failsafe | short-lived worker `Initialize:355-451` `Command_Enable…`/`Failsafe…` → `Restore…`/`Recover` |  | ✓ | `Command_Enable…` / `Failsafe_…` | yes | `failsafe-boot` checks daemon alive first |
| 10 | Startup restore | `Initialize` first-instance `509-540` `RestoreConfiguredCameraHardware(true)` + `MyForm_Load` `EnableTarget…(autoStart)` | daemon | `Recover` cycle |  | ✓ | `Startup_Restore…` | yes | 2 enables at boot |
| 11 | Shutdown restore | `HandleSystemEnd` `WM_QUERY/ENDSESSION` + `Shutdown(true)` + finalizer `!ApplicationController` | daemon UI + finalizer | `Disable` | ✓ |  | `SystemEnd_…` | yes | double/shutdown asymmetry KNOWN_ISSUES #3 |
| 12 | WTS lock/unlock | `WM_WTSSESSION_CHANGE` `WndProc:293-308` → `EventCooldown` 1500 ms → `WinEventDecoder` 7/8 → `HandleSessionEvent` | daemon | `Disable`/`Enable` | ✓ | ✓ | `SessionEvent_Received`/`Dedup`/`SessionLock_Disable` etc. | yes | per-process cooldown only |
| 13 | Power/lid/button | `WM_POWERBROADCAST 0x0218` `WndProc:275-292` → `DecodePowerEvent` `PBT_APMSUSPEND/RESUMESUSPEND/RESUMEAUTOMATIC` + `GUID_LIDSWITCH`/`POWER_BUTTON` → `HandlePowerEvent` latch `m_isAlreadyDisabled` | daemon | `Disable`/`Enable` | ✓ | ✓ | `PowerEvent_…` | yes | lid/button treated as suspend regardless of payload |
| 14 | Manual UI toggle | `MyForm::btnToggle_Click` → `ToggleMonitoring` | daemon | `Enable`/`Disable` | ✓ | ✓ | `SaveConfigState` | yes | user-initiated |
| 15 | External `pnputil` | not used | – | – | – | – | – | no | v1.0 only, removed in v2.0 |

**Key finding:** For one `LOCK`, two actors can touch hardware: native listener (6) *and* `WindowsHelloFix_Lock` task (8) concurrently (command check before mutex). Same for `UNLOCK` (7 vs 9). Installer warm-up (9) runs twice per install/uninstall.

---

## 2. Task Scheduler Analysis — Exact Responsibility

| Task | Trigger | Delay | Executable | Arguments | WD | Principal | RunLevel | Hidden | Multiple | ExecLimit | Camera Effect | New Process? | Races daemon? | Runs if daemon dead? | Logs |
|------|---------|-------|------------|-----------|----|-----------|----------|--------|----------|-----------|---------------|--------------|---------------|----------------------|------|
| `WindowsHelloFix` | `AtLogOn` (`New-ScheduledTaskTrigger -AtLogOn`, no UserId) | 0 | `Windows_Hello_Fix_v2_0.exe` | `--background` | `$INSTDIR` | installing user, `LogonType Interactive (3)`, `Highest (1)` | Highest | `false` (PS default) | `IgnoreNew` | `0` (unlimited) | **Yes** – becomes resident daemon, enables camera at `MyForm_Load`, then owns WTS/power | No (is daemon) | – | No (is daemon) | `Startup_Context`, `Startup_Restore…`, `WTS…Success` |
| `WindowsHelloFix_Lock` | COM `Trigger.Create(11)` `StateChange=7` lock, `UserId` installing user, `Enabled true` | 0 | same exe | `--disable-camera` | `$INSTDIR` | same `Interactive Highest` | Highest | `true` | `2` (IgnoreNew) | `PT5M` | **Yes** – `DisableTarget…` + `Verify` in short worker | Yes, short | **Yes** – concurrent with daemon's lock handler (per-process cooldown) | Yes | `Command_Disable…` + `Disable…_Result` + `DurationMs` |
| `WindowsHelloFix_Unlock` | `StateChange=8` unlock | 0 | same | `--enable-camera` (`Restore… true` cycle) | same | same | Highest | `true` | `2` | `PT5M` | **Yes** – `Recover` cycle | Yes | **Yes** – concurrent | Yes | `Command_Enable…` + `Enable…_Result` + `DurationMs` |
| `WindowsHelloFix_LogCleanup` | `Daily 00:00` (`New-ScheduledTaskTrigger -Daily`) | 0 | `cmd.exe` | `/c break > "…diagnostic.log"` | N/A | same `Interactive Highest` | Highest | `false` | `IgnoreNew` | `0` | **No** | Yes, separate `cmd` | No (log only) | Yes (when daemon dead, still truncates) | none (intent `break`) |

All four `Get-ScheduledTask | Select Description` now show accurate descriptions (added `ce54644`): daemon “Starts … monitor … manage …”, lock “Handles … lock … disabling…”, unlock “Handles … unlock … re-enabling…”, log “Performs daily maintenance …”.

---

## 3. Duplicate Camera Operation Analysis

**LOCK timeline:**
```
T0 WTS SessionLock (7) broadcast
T1 native Daemon WndProc (UI thread) → EventCooldown → HandleSessionEvent → DisableTarget… (SetVerified 3× + Verify 3×100 ms + 250 ms sleeps) → log SessionLock_Disable
T2 Task Scheduler service detects StateChange 7 → launches WindowsHelloFix_Lock.exe --disable-camera (new process, hidden, main.cpp Opacity 0)
T3 new process Initialize: Startup_Context → Command_DisableCamera_Begin → DisableTarget… (same hardware, same verification) → log Command_Disable… + Disable…_Result
T4 Both verify via GetCameraHardwareDisabledState (CM_PROB_DISABLED / CONFIGFLAG_DISABLED)
```
Two disables for one lock, per-process cooldown cannot dedup cross-process. Same for `UNLOCK` (native `Enable` vs task `Recover` cycle 350/900/500 ms). `Power` Lid/Button vs `PBT_APMSUSPEND` similar.

**UNLOCK timeline** adds Windows Hello race: native `Enable` (fast, no cycle) vs task `Recover` (cycle) – task's cycle sleeps 350 ms → disable → 900 ms → enable – can overlap `FrameServer` access.

## 4. Windows Hello Race Analysis

- **LOCK:** Windows session transition holds `WinLogon` lock; camera disable races `Windows Hello` shutdown, but disabling before Hello fully releases the camera is safe (idempotent).
- **UNLOCK:** Windows resumes, `WinLogon` → `BioIso` → `FrameServer` opens RGB camera for Hello face recognition. If native `EnableTarget…` (already-enabled check → no op) and task `Unlock` `Recover` cycle (disable 350 ms → enable 900 ms) run concurrently, the task's **disable** phase can coincide with Hello opening the device → `0xA00F4241 (0xC00D7167)` `MF_E_HW_MFT_FAILED` / `STATUS_DEVICE_NOT_READY`. The 900 ms sleep after disable extends the window where Hello sees `CM_PROB_DISABLED` or `CONFIGFLAG_DISABLED`. Current `AlreadyEnabled` check is inside `EnableTarget…` but **task bypasses it** because `RestoreConfiguredCameraHardware(true)` does `Recover` with `cycle=true` unconditionally (only `AlreadyEnabled` inside `Enable` is for non-cycle path, but `Recover` always does `SetVerified(enable)` first, then cycle).

**Classification:** Task `Unlock` doing a *disable* as part of its cycle during Hello's active window is the most plausible trigger for `0xA00F4241` immediately after install/startup (when camera is already enabled, task still cycles).

## 5. Recommended Task Architecture

| Task | Verdict | Rationale | New Behavior |
|------|---------|-----------|--------------|
| `WindowsHelloFix` | **RETAIN** | Sole silent elevated startup, owns WTS/power, `RunLevel Highest` avoids UAC, `IgnoreNew` + mutex handles duplicate | `AtLogOn` (no delay), `Highest`, `--background`, `Description` kept. No change. |
| `WindowsHelloFix_LogCleanup` | **RETAIN** | No camera, daily log cap, proven | `Daily 00:00` `cmd.exe` `break` (fix `$APPDATA` expansion already handled by NSIS `$APPDATA` at install time), `Description` kept. |
| `WindowsHelloFix_Unlock` | **REWORK to BOOT/LOGON recovery only** | Current per-unlock `Recover` duplicates native `Enable` and introduces disable-phase race. Should be **enable-only, no disable, no per-unlock**. | Trigger `AtLogOn` with **delay 30-60 s** (or `AtStartup` + `Delay 1 min`) instead of `StateChange 8`, Arguments `--failsafe-boot` (new enable-only path already in `CommandLine::IsFailsafeBootCommand` and `ApplicationController:355-443` which checks `IsHelloFixDaemonAlive`, `Monitoring==1`, `AlreadyEnabled?` → no op, else `Recover` once + `Verify`), `Hidden true`, `PT5M`, `Description` updated to “Startup/logon camera recovery – re-enables the RGB camera if the HelloFix daemon failed to start.” |
| `WindowsHelloFix_Lock` | **REMOVE** | Provides nothing native does not; native `WM_WTSSESSION_CHANGE` 7 already disables (idempotent `AlreadyDisabled` check). Its existence only adds duplicate disable per lock. | Delete task in installer (`schtasks /Delete /TN WindowsHelloFix_Lock /F` already in `SEC01` `Sleep 1000` block, keep), remove `Register-WhfSessionTask` call for `Lock` (7) from `install_script.nsi`, keep uninstall delete for legacy cleanup. |

**Why not keep Lock as failsafe?** Failsafe should be *enable-only* (safe default = camera enabled, Hello works); a lock failsafe that *disables* has no recovery value and only adds churn.

## 6. Original v2.0 Comparison

| Difference | v2.0 (`39ac0ab`/`5e45003`) | Current `test` `c3f1271` | Risk |
|------------|---------------------------|--------------------------|------|
| `WindowsHelloFix_Lock` | **Not present** in v2.0 `Release/install_script.nsi` (only `WindowsHelloFix` + `LogCleanup` in early v2.0) | **Present** (`7765a06` added) | **High** – adds duplicate disable per lock |
| `WindowsHelloFix_Unlock` | Not present / or `StateChange 8` per-unlock in later v2.0 | **Present** per-unlock with `Recover` cycle | **High** – duplicate enable + cycle disable race → `0xA00F4241` |
| `WindowsHelloFix` trigger | `AtLogOn` no delay | same | Low |
| `Failsafe` (`--failsafe-boot`) | Not present | Present (`7a7588e` merge) but **not scheduled** (no task) – dead code | Low – dead |
| `RestoreConfiguredCameraHardware` at `Initialize` | `true` (cycle) | same | Medium – adds 350/900/500 at every boot |
| `MyForm_Load` `EnableTarget…(autoStart)` | `autoStart` = `monitoring==1` → cycle when autostart | same | Medium – extra cycle on autostart |
| `AlreadyDisabled/AlreadyEnabled` check | Present in `Disable/Enable` | Same, but **task `Unlock` bypasses** via `Recover` cycle unconditionally | **High** |
| `Startup` Run entry | Deleted (`DeleteRegValue HKLM…Run`) | Same (now `Run` with `schtasks` for visibility, added `ce54644`) | Low |
| `LogCleanup` `$APPDATA` | Literal `$APPDATA` (defect) | NSIS expands `$APPDATA` at install time → correct path (fixed `ce54644`?) | Low |
| `DurationMs` timing | Not present | Added `PerfTimer` in `ce54644` follow-up | None |

**Conclusion:** v2.0 was *single-writer* (only daemon touches hardware per lock/unlock); current is *dual-writer* (daemon + tasks). The extra writer is the regression.

## 7. Issue #1 Assessment

- **Duplicate per-lock/unlock `Disable`/`Recover(disable)` → `0xA00F4241`:** **Likely** – task's `Recover` does `Disable` 350 ms into Hello's `FrameServer` open window; `AlreadyEnabled` short-circuit is bypassed; `Verify` loop 3×100 ms not enough for Hello to recover.
- **Startup double-enable ( `Initialize` `Restore` cycle + `MyForm_Load` `Enable`):** **Possible** – adds 2× `Recover` cycles at boot, but boot not Hello-active, less likely to cause `0xA00F4241`; still adds wear.
- **Task `Lock` duplicate disable:** **Possible** – second disable when already disabled is idempotent (`AlreadyDisabled` → no op) so less harmful than unlock's disable.
- **Unrelated:** `Power` lid/button, `Shutdown`, `installer warm-up` are one-shot at install, not per lock/unlock, **Unrelated** to immediate post-install `0xA00F4241` (which is per-unlock).

## 8. Startup Apps Issue #6 Investigation

- `HKLM\Run\Windows Hello Fix` = `C:\WINDOWS\System32\schtasks.exe /Run /TN "WindowsHelloFix"` (`REG_SZ`, `SetRegView 64`, `$WINDIR` expansion) exists, `Win32_StartupCommand` shows `Windows Hello Fix` `schtasks…` `HKLM…Run`, `StartupApproved\Run` missing → `Enabled` by default (02 on next enumeration). **But** `HKLM\Run` with `schtasks` is filtered by some Task Manager builds as “system” not “app”; previous working v2.0 had **no** Run entry at all (deleted), so it also never appeared – the “previously appeared” memory may be from a different Windows build or from `HKCU` Run.
- Working example on this machine: `HKCU\Run\OneDrive` (`"C:\Program Files\Microsoft OneDrive\OneDrive.exe" /background`, `StartupApproved 03…` disabled, `Win32_StartupCommand` shows `OneDrive`) **does** appear in `Win32_StartupCommand` and Task Manager. `HKLM\Run\SecurityHealth` (`%windir%\system32\SecurityHealthSystray.exe`, `060…`) also appears. So `HKLM\Run` *can* appear, but `schtasks` target may be hidden.
- **Root-cause candidates:** (1) wrong value name (`WindowsHelloFix` vs `Windows Hello Fix` – now fixed to spaced), (2) `schtasks` indirection hides publisher, (3) `32-bit view` vs `64-bit` (`SetRegView 64` correct for `HKLM`), (4) stale `StartupApproved` `03…` (now cleaned), (5) installer creates Run *after* task but before `WriteUninstaller`, order not harmful.
- **Evidence:** `reg query HKLM\…\Run /v "Windows Hello Fix"` → exists, `HKLM\…\StartupApproved\Run` has no `Windows Hello Fix` (so enabled), yet tester still reports not visible → suggests Task Manager on this build requires `HKCU\Run` **or** `Startup` folder, not `HKLM\Run` with `schtasks`. Original v2.0 also deleted `HKLM\Run`, so it also never appeared – the “original appeared” may be misremembered or was `HKCU` on a different machine.

## 9. Timing Instrumentation Plan

Already added `src/utilities/PerfTimer.h` (`QueryPerformanceCounter`) and `ApplicationController::Disable/Enable` `DurationMs` (verified `Disable…_Result DurationMs=5964.46` includes `SetVerified` retries + `Verify` + sleeps). **Plan to extend:**

- Keep `PerfTimer` as is (no new file).
- Add `DurationMs` to `RestoreConfiguredCameraHardware` / `Initialize` startup path (`Startup_Restore…` + `MyForm_Load` first enable) to measure boot cost (optional, not invasive).
- Do **not** add `ApiMs`/`VerifyMs`/`SleepMs` breakdown (would require touching `CameraRecovery` sleeps) – total is enough.

Location: `ApplicationController.cpp:151-211` `Disable/Enable` already done.

## 10. Implementation Plan — Strict Ordered Phases

**Phase 1 – Task Scheduler de-duplication (PLANNED, not implemented):**
1. Update `x64/Release/install_script.nsi` `SEC01`: *remove* `Register-WhfSessionTask 'WindowsHelloFix_Lock' 7 …` line, keep `Unlock` but change to `Register-WhfSessionTask 'WindowsHelloFix_Unlock' 8 '--failsafe-boot' 'Startup/logon camera recovery …'` with trigger `Delayed AtLogOn` (or `AtStartup` + `Delay 60s`) via `New-ScheduledTaskTrigger -AtLogOn -RandomDelay` or COM `Trigger.Delay = "PT1M"`, `Hidden true`, `PT5M`, `Description` updated. Keep `WindowsHelloFix` and `LogCleanup` unchanged.
2. Keep `schtasks /Delete /TN WindowsHelloFix_Lock` in `SEC01` stale wipe and `Uninstall` for legacy cleanup.

**Phase 2 – Camera idempotency verification (no code change, just review):** Confirm `Disable` already checks `AlreadyDisabled` before `SetVerified`, `Enable` checks `AlreadyEnabled`, and `Recover` for failsafe checks `AlreadyEnabled` before `Recover`. No fix needed.

**Phase 3 – Failsafe narrowing:** Ensure `CommandLine::IsFailsafeBootCommand` (`--failsafe-boot`) already checks `IsHelloFixDaemonAlive`, `Monitoring==1`, `AlreadyEnabled` → no op. Keep `ApplicationController:355-443` as is.

**Phase 4 – Build & validation:** `Release|x64` `0 warnings`, `makensis`, then `TEST A` fresh install → `Run` `Windows Hello Fix` `Enabled`, `Tasks` 3 (daemon, reworked unlock, logcleanup) descriptions, reboot → silent elevated resident, lock/unlock single `Disable/Enable` in `diagnostic.log` (no duplicate `Command_Disable/Enable`), no `0xA00F4241`.

**Rollback:** `git revert` to `c3f1271` retains four tasks; `Lock` can be re-added by restoring the one `FileWrite` line.

## 11. AGENTS.md Changes

Added permanent section (see `AGENTS.md` diff):

- `# HelloFix Working Philosophy`
- `## Camera Authority` – native listener sole authority
- `## Lock/Unlock` – check `Already…` → no op
- `## Task Scheduler` – tasks must not duplicate lock/unlock; reserved for startup, log cleanup, narrow startup recovery
- `## Failsafe Rules` – enable-only, never disable, must not race daemon
- `## Protected Camera Components` – camera modules protected
- `## Minimal Change Principle`

Plus kept `Plan.md Requirement` etc.

## 12. docs/Plan.md

Replaced old startup/GUI plan (which described `0c0fe6a` baseline) with this forensic plan, marked `PLANNED — NOT IMPLEMENTED`, with tables for camera inventory, task analysis, duplicate timeline, Hello race, recommended architecture, v2.0 comparison, Issue #1 assessment, Startup Apps investigation, timing plan, ordered phases, rollback.

## 13. Files Modified

- `AGENTS.md` – added `HelloFix Working Philosophy` (+ `Plan.md Requirement` already there, kept)
- `docs/Plan.md` – full rewrite to this forensic plan (306 → ~400 lines)

No other files modified (per `git status --short` → only those two, plus rebuilt `x64/Release/*.exe` which are build artifacts and not committed).

