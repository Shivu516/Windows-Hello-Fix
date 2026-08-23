# Current Implementation Plan

## Status

Current branch:
`test`

Current baseline:
Commit `0c0fe6a` ("Added Documentation") — `HEAD -> test`, `origin/test`, `origin/hellofix-restructured`.

Parent chain: `0c0fe6a` → `165ee3b` ("Track installer and release assets") → `5996d4b` ("Complete Re-Organisation") → `98cb1b6` → `ad10d73`. This branch diverged **before** the historical Issue #2 fix (`bcd3cdb`) and the failsafe branch (`7765a06`/`5e45003`), and does not contain them. It is the "last stable post-restructure" baseline the task description refers to.

Working tree:
Clean (`git status --short` returns empty) at time of plan creation.

Historical stable references:
- Issue #2 known-good fix: commit `bcd3cdb` ("Restore window opacity on focus", 2026-08-22) on `origin/main` lineage. Single-line change `src/ui/MyForm.cpp:95` `this->Opacity = 1.0;` inside `BringWindowToFrontDelegate`.
- Startup reference (v2.0 release): tag `v2.0.0` / commit `39ac0ab` ("Windows Hello Fix v2.0 – Complete C++ Rebuild") and the rebuilt installer `5e45003` ("Add x64 Release artifacts and NSIS installer"). Both create no Startup-Apps-visible entry; they intentionally delete `HKLM\…\Run\WindowsHelloFix` and rely solely on Task Scheduler.
- Additional startup ordering fix: commit `acc37d8` ("Update ApplicationController.cpp", 2026-08-23 00:05) reorders `ApplicationController::Initialize` to check `IsBackgroundLaunch` *before* `TrySignalExistingInstance`. Current `test` branch still has the pre-`acc37d8` order (wake attempt first).

Current known-good functionality (verified as-built on `0c0fe6a`, per `docs/*.md`):
- Manual GUI launch (no args) → visible centered `FixedDialog`, monitoring respects `config.txt`.
- `--background` logon daemon via `WindowsHelloFix` scheduled task → hidden (`main.cpp:16-23` Opacity 0 + `SetWindowVisibleForBackground(true)`), monitoring forced ON via `MyForm_Load` `isBackground||autoStart` path, WTS/power notifications registered, wake-listener thread running.
- Second interactive instance → mutex `Global\WindowsHelloFix_AppMutex` detected, `TrySignalExistingInstance` signals `Global\WindowsHelloFix_WakeupEvent`, listener thread marshals `BringWindowToFrontDelegate` (but without opacity restore on this branch — see Issue #2).
- Background duplicate (`--background` while daemon runs) → old code wakes the GUI (bug); intended behavior is silent exit (fixed in `acc37d8`).
- Command workers (`--disable-camera`/`--enable-camera`/`/restore-camera`) → hidden, exit before mutex check, hardware toggles via `CameraHardware`/`CameraRecovery` with verification.
- Lock/unlock/power/lid paths → `WndProc` → cooldown dedup (1500 ms) → decode → `HandleSessionEvent`/`HandlePowerEvent` → hardware toggle + `diagnostic.log` events.

Known not-working / not-present on this baseline:
- No entry in Task Manager → Startup apps (by design — see Objective 1).
- GUI wake shows window chrome but remains fully transparent (Opacity 0) — see Objective 2.

---

# Objective 1 — Windows Startup Apps

## Current behavior

How HelloFix currently starts with Windows on `test` / `0c0fe6a`:

- **Scheduled Task `WindowsHelloFix` exists** (`x64/Release/install_script.nsi:135-139`): `Register-ScheduledTask -TaskName 'WindowsHelloFix' -Action ("$INSTDIR\Windows_Hello_Fix_v2_0.exe" --background, WD=$INSTDIR) -Trigger AtLogOn -Principal (UserId=installing user, LogonType Interactive, RunLevel Highest) -Settings (AllowStartIfOnBatteries, DontStopIfGoingOnBatteries, StartWhenAvailable, MultipleInstances IgnoreNew, ExecutionTimeLimit 0, Priority 4)`. Created after `schtasks /Delete /TN WindowsHelloFix /F` stale wipe. Uninstaller deletes it (`schtasks /Delete /TN WindowsHelloFix /F`).
- **Failsafe session tasks** `WindowsHelloFix_Lock` / `_Unlock` (StateChange 7/8, `--disable-camera`/`--enable-camera`, COM `Schedule.Service` type 11, Hidden=true, ExecutionTimeLimit PT5M) and **LogCleanup** (`cmd.exe /c break > "$APPDATA\Windows Hello Fix\diagnostic.log"` daily 00:00) also exist. None are Startup-Apps-relevant.
- **Registry Run entry does NOT exist.** Installer explicitly deletes it (`DeleteRegValue HKLM Software\Microsoft\Windows\CurrentVersion\Run WindowsHelloFix`, `SetRegView 64`, `src` lines 105, 220). No `HKCU` Run entry either. No documentation or code creates one elsewhere.
- **Startup-folder shortcut does NOT exist.** `INSTALLER.md §3` confirms only Start Menu `.lnk` (no args) and optional Desktop `.lnk` are created; no `Startup` folder entry.
- **StartupApproved is not involved.** No code touches `HKCU\Software\Microsoft\Windows\CurrentVersion\Explorer\StartupApproved\Run` or `…\StartupApproved\StartupFolder`. Task Manager writes there when user toggles a Startup app, but installer never seeds it.
- **Task Manager → Startup apps shows nothing.** Verified by architecture docs: `docs/STARTUP.md §2 #10` lists Run/Startup-folder as "Not used", `docs/INSTALLER.md §4`, `docs/TASK_SCHEDULER.md §1.1`. Elevation is `requireAdministrator` (`app.manifest` + `UACExecutionLevel`), so every launch is elevated; the daemon task achieves this silently via RunLevel Highest. Plain Registry Run would trigger a UAC prompt on logon.
- **What v2.0 release did differently:** Nothing relevant to startup visibility. `39ac0ab` introduced the same `RequireAdministrator` + Task Scheduler model; `Release/install_script.nsi` at that tag also deleted the Run value and created the same four tasks (minus later `WindowsHelloFix_Failsafe`). The current NSIS script (`x64/Release/install_script.nsi` on `test`) is functionally identical to that historical working installer except for minor `.gitignore` tracking (`165ee3b`) and the absence of the later failsafe task introduced in `7765a06` on `main` (which `test` deliberately does not contain per rollback `8403f8a`). In short: **current installer does not differ materially from historical v2.0 installer regarding Startup Apps** — neither ever created a visible entry.

## Desired behavior

- HelloFix appears in `Task Manager → Startup apps` (and `Settings → Apps → Startup`) as a distinct entry (e.g., "Windows Hello Fix v2.0" / "WindowsHelloFix").
- Enabled by default after fresh install.
- Starts automatically with Windows, silent/background (no window, no taskbar icon, no activation, `Opacity=0`, `ShowInTaskbar=false`, `Minimized` via `main.cpp:18-23` and `SetWindowVisibleForBackground(true)`).
- User can disable automatic startup through the normal Startup Apps UI toggle; disabling must be honored (daemon does not start on next logon).
- Disabling startup must not corrupt monitoring behavior: manual GUI launch must still work, saved `config.txt` (`monitoring=0|1`, `device=<id>`) must remain valid, lock/unlock tasks remain functional if user re-enables, no orphaned mutex/event.
- Startup registration removed cleanly on uninstall (delete Run value + delete any tasks that were added for visibility). Task names must remain stable (`WindowsHelloFix*`) so uninstall `schtasks /Delete` continues to work.
- No duplicate startup mechanisms unless there is a documented reason and deduplication via `Global\WindowsHelloFix_AppMutex` + silent-exit path is explicitly justified and tested.

## Investigation findings

1. **Single source of autostart today is Task Scheduler.** `docs/STARTUP.md §2 #3`, `docs/TASK_SCHEDULER.md §1.1`, `x64/Release/install_script.nsi:130-173`. The task runs `Windows_Hello_Fix_v2_0.exe --background` at logon as the installing user, Interactive, Highest. `main.cpp:16` → `CommandLine::ShouldHideWindow` (case-sensitive `==`) hides; then `ApplicationController::Initialize` logs `Startup_Context | BackgroundArg=1` and forces monitoring ON if a device is selected. Uninstaller removes the task.

2. **Registry Run is the only mechanism that reliably appears in Startup Apps without extra bridging.** Windows enumerates `HKLM\Software\Microsoft\Windows\CurrentVersion\Run` and `HKCU\…\Run`, plus `Startup` folder `.lnk`s, into Startup apps. Task Scheduler tasks appear only under narrow conditions (some builds show tasks with `AtLogOn` + `Interactive` + non-Hidden, but our daemon task with identical attributes still does not appear — confirmed by the bug report). Our installer sets `Hidden=false` for the daemon task (default), yet the entry is absent; `WindowsHelloFix_LogCleanup` uses `cmd.exe` with `$APPDATA` syntax that `cmd.exe` does not expand (see `TASK_SCHEDULER.md §1.4` defect) — unrelated but indicates fragile task authoring.

3. **Elevation vs. UAC is the core tension.** Manifest `RequireAdministrator` means any direct `Run`-key launch will prompt UAC at logon unless the user has disabled UAC or the exe is auto-elevated. Task Scheduler with `RunLevel Highest` avoids the prompt (the token is elevated by the scheduler). This is why the original author chose Task Scheduler and explicitly deleted Run values (`INSTALLER.md §4` rationale: "Do not set RUNASADMIN flags… they fight Task Scheduler elevation"). Re-introducing a Run entry naively would re-introduce a visible UAC prompt, violating "silent/background".

4. **StartupApproved is the toggle back-end.** When user flips a Startup app off, Task Manager writes `HKCU\Software\Microsoft\Windows\CurrentVersion\Explorer\StartupApproved\Run\<Name>` (`02 00 00 00 …` = enabled, `03 00 00 00 …` = disabled) or `…\StartupApproved\StartupFolder` for folder shortcuts. Our code never reads this key, so even if we add a Run entry, the daemon task would still start if we keep both mechanisms, defeating the toggle.

5. **Duplicate-launch risk is already mitigated by single-instance mutex, but ordering matters.** `ApplicationController::Initialize` at `0c0fe6a` ( `src/application/ApplicationController.cpp:315-357` ) checks `TrySignalExistingInstance()` *before* `IsBackgroundLaunch`. A background second instance therefore wakes the GUI instead of silently exiting. `acc37d8` (present only on `origin/main`, not on `test`) reverses the order: background → `SingleInstance_BackgroundSilentExit` first. Any design with two autostart mechanisms (e.g., Run + Task) will fire two logon processes concurrently; without the `acc37d8` order they will wake the UI during boot.

6. **Case-sensitivity mismatch** between `CommandLine::IsBackgroundLaunch` (OrdinalIgnoreCase) and `ShouldHideWindow` (case-sensitive `==`) means a mixed-case `--Background` would start background monitoring but leave the window visible (`docs/KNOWN_ISSUES.md #4`). Not directly startup-visible but affects background invisibility guarantee.

7. **Historical v2.0 installer (`39ac0ab` → `5e45003`) never attempted Startup Apps visibility.** Diffing `Release/install_script.nsi` at `39ac0ab` vs `x64/Release/install_script.nsi` at `0c0fe6a` shows identical Run-deletion + task-creation logic; the only delta in later `origin/main` is the addition of `WindowsHelloFix_Failsafe` (`7765a06`) and `Description` field — neither affects Startup Apps presence. Thus the bug is not a regression from a previously working Startup Apps entry; it is a missing feature that the Task-Scheduler-only design never satisfied.

8. **Uninstall already cleans tasks and Run value, but misses StartupApproved.** Current uninstall deletes `HKLM\…\Run\WindowsHelloFix` but not `HKCU\…\Explorer\StartupApproved\Run\WindowsHelloFix`. If we add a Run entry, uninstall must delete both the value and its StartupApproved counterpart, plus any Startup-folder `.lnk`, plus the task.

## Proposed implementation

Smallest safe architecture that achieves the goal while preserving `RequireAdministrator`:

Compare mechanisms:

| Mechanism | Elevation / UAC | Visibility in Startup Apps | Enable/Disable behavior | Duplicate-launch risk | Background behavior | Uninstall behavior | Compatibility with single-instance |
|---|---|---|---|---|---|
| **Task Scheduler only** (status quo) | Silent elevation via RunLevel Highest, no UAC prompt | **Not visible** (current bug) | User cannot control via Startup UI; must use Task Scheduler GUI | Single task, no duplicate | Hidden via `--background` → `main.cpp` Opacity 0; works | `schtasks /Delete` clean | Mutex secondary |
| **Registry Run only** (`HKLM` or `HKCU` `…\Run\WindowsHelloFix` = `"exe" --background`) | Triggers UAC prompt at logon due to `RequireAdministrator` → violates "silent" unless manifest changed | **Visible**, toggle writes StartupApproved | Correct toggle semantics | Single entry, no duplicate | Same `--background` hiding, but UAC prompt flashes | `DeleteRegValue` clean if StartupApproved also deleted | Mutex single |
| **Startup folder .lnk only** (`%APPDATA%\Microsoft\Windows\Start Menu\Programs\Startup\Windows Hello Fix.lnk` → `exe --background`) | Same UAC prompt issue as Run | **Visible** | Toggle via StartupApproved\StartupFolder, user can also delete file | Single entry, but file can be deleted manually | Same hiding | Delete file + StartupApproved | Mutex single |
| **Hybrid: Run (visibility stub) + Task Scheduler (elevated executor) with StartupApproved gate** | Silent: Task provides elevation, Run provides visibility; no UAC if only task actually launches | **Visible** (Run entry) | Disable Run via Startup UI → task checks `StartupApproved\Run\WindowsHelloFix` before proceeding and silently exits if disabled — toggle honored | Two logon processes race → mitigated by mutex + `acc37d8` background-silent-exit order; second exits silently | Both use `--background` so either path hides | Must delete Run + StartupApproved + task | Requires fixing `acc37d8` order first; otherwise background duplicate wakes GUI |

**Recommendation: Hybrid "visibility Run + elevated Task" with explicit StartupApproved gating (single recommended approach).**

Rationale:
- A pure Run solution fails the "silent/background" requirement because `RequireAdministrator` forces a UAC prompt at every logon. Changing the manifest to `asInvoker` would be a protected-architecture violation (camera toggles need elevation) and would require a major privilege-separation redesign — out of scope for the minimal fix.
- A pure Task Scheduler solution cannot be made reliably visible across Windows 10/11 builds without undocumented task attributes; investigation shows even correctly formed `AtLogOn` tasks sometimes remain hidden.
- A Startup-folder solution has strictly worse properties than Run (user-deletable file, same UAC issue, extra shell folder handling).
- The hybrid approach keeps the existing proven silent-elevation path (Task Scheduler) unchanged for actual execution, while adding a minimal Run entry whose sole purpose is to surface in Startup Apps and to act as the user's enable/disable switch. The daemon (both the Task-launched process and the Run-launched process, if both fire) checks `HKCU\…\Explorer\StartupApproved\Run\WindowsHelloFix` (and optionally `HKLM` equivalent) at the top of `ApplicationController::Initialize` — if the value indicates disabled (`03…`), the process logs `Startup_DisabledByStartupApproved` and exits without starting monitoring (or starts without autostart). This honors the toggle without requiring the Task Scheduler API to expose a disabled state.
- Duplicate risk is documented and already mitigated: the second logon process hits `CreateAppMutex` → `alreadyExists` → `IsBackgroundLaunch` → silent exit (after applying `acc37d8` ordering). No GUI wake during boot.
- Alternative considered and rejected: "Task Scheduler task description/startup trigger tweaks to force visibility" — investigated via diff of `5e45003` vs HEAD and `7765a06` addition of `Description`; no evidence that description affects Startup enumeration, and relying on undocumented Task Manager enumeration heuristics is riskier than the well-documented Run+StartupApproved contract.

Implementation sketch (for future work, not now):
1. Installer: `WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Run" "WindowsHelloFix" '"$INSTDIR\Windows_Hello_Fix_v2_0.exe" --background'` (or `HKLM` with `SetRegView 64` if machine-wide desired; decision below). Also seed `StartupApproved` as enabled (`02 00…`) via `WriteRegBinary` or let Task Manager default to enabled — but document that fresh install must be ENABLED, so installer must ensure no disabled residual remains (delete any prior disabled value before write). Create task as today.
2. Uninstaller: `DeleteRegValue HKCU …\Run\WindowsHelloFix`, `DeleteRegValue HKCU …\Explorer\StartupApproved\Run\WindowsHelloFix` (and `HKLM` variants if used), plus task deletes.
3. Runtime: At top of `ApplicationController::Initialize` (after `Startup_Context` log, before camera work), query `StartupApproved`; if disabled and `IsBackgroundLaunch` true, log and `Exit(0)` (or skip autostart). Interactive manual launch (`--background` absent) bypasses the check so user can still open GUI when startup is disabled.
4. Apply `acc37d8` ordering fix so background duplicate never wakes GUI.
5. Fix `CommandLine::ShouldHideWindow` to be case-insensitive (align with `IsBackgroundLaunch`) or delegate to it, to avoid visible background window on mixed-case.

Choice of hive: Prefer `HKCU` (per-user) because task is per-installing-user (`[WindowsIdentity]::GetCurrent().Name`) and Startup Apps per-user toggles are expected; `HKLM` would require admin to disable and would appear for all users. If machine-wide install is required, `HKLM` + `StartupApproved` under `HKLM\…\Explorer\StartupApproved` (rare) could be used, but `HKCU` is safer minimal change. Open question records this decision point for review.

## Files expected to change later

- `x64/Release/install_script.nsi` — add Run entry creation (and StartupApproved seeding) in `SEC01`, and add corresponding deletes in `Section "Uninstall"`; preserve task registration order (taskkill → deploy → `/restore-camera` → task creation → warm-up).
- `src/application/CommandLine.h` / `src/application/CommandLine.cpp` — optionally centralize StartupApproved check or fix `ShouldHideWindow` case-insensitivity (align with `IsBackgroundLaunch`); add helper `IsStartupApprovedDisabled()` if runtime gate lives there.
- `src/application/ApplicationController.cpp` (and `.h` if new helper) — add early gate: if background launch && `IsStartupApprovedDisabled()` → log `Startup_DisabledByStartupApproved` and `Exit(0)`; apply `acc37d8` ordering fix (background check before wake signal) if not already merged.
- `main.cpp` — only if `ShouldHideWindow` is made case-insensitive via delegation; otherwise no change.
- Docs: `docs/STARTUP.md`, `docs/INSTALLER.md`, `docs/TASK_SCHEDULER.md` to document new Run entry and StartupApproved contract (documentation-only, not functional).

No changes to camera, config, event, or UI modules for this objective.

## Validation plan

1. **Fresh install test:** On clean VM, run `Windows_Hello_Fix_Setup.exe`. Verify in Registry `HKCU\…\Run\WindowsHelloFix` (or `HKLM`) value equals `"<InstallDir>\Windows_Hello_Fix_v2_0.exe" --background`. Verify Task Manager → Startup apps shows "Windows Hello Fix" Enabled, and `Settings → Apps → Startup` matches.
2. **Silent logon test:** Reboot / log off/on. No UAC prompt, no visible window, no taskbar icon. `%APPDATA%\Windows Hello Fix\diagnostic.log` shows `Startup_Context | BackgroundArg=1` and `WTSRegisterSessionNotification_Success` once. Process `Windows_Hello_Fix_v2_0.exe` running, `Global\WindowsHelloFix_AppMutex` held.
3. **Disable via Startup UI test:** In Task Manager → Startup, disable Windows Hello Fix. Reboot. Verify process does NOT start automatically (or starts and immediately exits with `Startup_DisabledByStartupApproved` in log). Verify `StartupApproved\Run\WindowsHelloFix` is `03…`.
4. **Re-enable test:** Re-enable via Startup UI, reboot, verify daemon starts again silently.
5. **Manual launch while disabled:** With startup disabled, double-click Start Menu shortcut (no args) → GUI visible, monitoring respects config, mutex wake path works.
6. **Duplicate mechanism test:** If hybrid retained, trigger both Run and Task at logon (normal boot) — verify only one daemon survives (`SingleInstance_BackgroundSilentExit` or `WakeSignalSent` not fired during boot), no GUI wake, `diagnostic.log` shows one `BackgroundSilentExit` or no wake.
7. **Uninstall test:** Run `Uninstall.exe`. Verify Run value deleted, StartupApproved value deleted, both `WindowsHelloFix` tasks deleted (`schtasks /Query /TN WindowsHelloFix` fails), Start Menu folder removed, no residual `AppData` config/log if user elected full removal.
8. **Elevation test:** With UAC at default, logon with new Run entry present — confirm no UAC prompt (task path silent). If pure Run were chosen, expect prompt and fail.
9. **Lock/unlock regression:** After each of the above, lock workstation, verify `SessionLock_Disable` / `SessionUnlock_Enable` still fire via daemon (and/or failsafe tasks if retained).

---

# Objective 2 — Issue #2 GUI Auto-Launch / GUI Visibility

## Current behavior

Complete GUI lifecycle on `test` / `0c0fe6a`:

**`main()` (`main.cpp:9-27`):**
- `Application::EnableVisualStyles()`, `SetCompatibleTextRenderingDefault(false)`.
- Construct `MyForm form` → `MyForm::MyForm` creates `ApplicationController(this)` and `InitializeComponent` (`FixedDialog`, no max/min, 430×240, `CenterScreen`, text "Windows Hello Fix v2.0", default `Opacity=1.0`/`ShowInTaskbar=true`/`WindowState=Normal`).
- `bool runHidden = CommandLine::ShouldHideWindow(args)` — true for case-sensitive `"/background"`, `"--background"`, `"/disable-camera"`, `"--disable-camera"`, `"/enable-camera"`, `"--enable-camera"`, `"/restore-camera"`, `"/repair-camera"`. Note mismatch with `IsBackgroundLaunch` (case-insensitive).
- If `runHidden`: `form.Opacity = 0; form.ShowInTaskbar = false; form.WindowState = Minimized;`. No code in `main.cpp` ever restores visibility.
- `Application::Run(%form)` — pumps messages; `Load` event fires next.

**`MyForm::MyForm_Load` (`src/ui/MyForm.cpp:168-232`) → `ApplicationController::Initialize` (`src/application/ApplicationController.cpp:279-385`):**
- Logs `Startup_Context`.
- Command modes (`IsRestoreCameraCommand` / `IsDisableCameraCommand`) → camera work → `Environment::Exit(0)` before mutex.
- Single-instance: `CreateAppMutex` (`Global\WindowsHelloFix_AppMutex`, initial owner TRUE, `SingleInstance.cpp:??`). If `alreadyExists`:
  - CURRENT (`0c0fe6a`) order: `TrySignalExistingInstance` first → if success `SingleInstance_WakeSignalSent` → `Exit(0)`; else if `IsBackgroundLaunch` → `SingleInstance_BackgroundWakeEventMissing` → `Exit(0)`; else `PromptGhostReset` dialog → possible `KillHelloFixProcess` + `Application::Restart()`.
  - Intended (`acc37d8`) order: background check first → `SingleInstance_BackgroundSilentExit` → exit, never waking GUI.
- First instance continues: `CreateWakeupEvent` (`Global\WindowsHelloFix_WakeupEvent`, auto-reset), `RestoreConfiguredCameraHardware(true)` (cycle), `RegisterPowerNotifications` (lid+button), start `ListenForWakeupSignal` background thread (`IsBackground=true`, `WaitForSingleObject` INFINITE), `RegisterSessionNotificationWithRetry` (6×500 ms).
- Back in `MyForm_Load`: scan `ScanSystemCameras()`, fill dropdown, pick saved→`MI_00`→first, `EnsureConfigFileExists`, `EnableTargetCameraHardware(autoStart)` ("first enable to prevent bricking"), then if `(isBackground || autoStart) && selected != -1` → `IsMonitoring=true`, `EnableTargetCameraHardware(false)`, UI `Service Running`, **if `isBackground` → `SetWindowVisibleForBackground(true)`** (`Visible=false`, `ShowInTaskbar=false`, `Minimized`).

**Runtime wake path:**
- Second interactive instance signals `Global\WindowsHelloFix_WakeupEvent` via `SingleInstance::TrySignalExistingInstance` (`SetEvent`).
- Daemon's listener thread (`ApplicationController::ListenForWakeupSignal:262-271`, `WaitForSingleObject`) wakes → `m_sink->BringWindowToFront()` → `MyForm::BringWindowToFront:68-74` → if `InvokeRequired` marshals via `Invoke(MethodInvoker(BringWindowToFrontDelegate))` else direct.
- **`BringWindowToFrontDelegate` (`src/ui/MyForm.cpp:95-103` on `0c0fe6a`):** `Show(); Visible=true; ShowInTaskbar=true; WindowState=Normal; BringToFront(); Activate(); Refresh();` — **no Opacity restore.**
- **`SetWindowVisibleForBackground(true)` (`src/ui/MyForm.cpp:61-67`):** `Visible=false; ShowInTaskbar=false; WindowState=Minimized;` — called from `MyForm_Load` when background autostart happened. With `false` it does nothing.
- **`MyForm_FormClosing` (`src/ui/MyForm.cpp:234-244`):** On `UserClosing`, `e->Cancel=true`, `Hide()`, `ShowInTaskbar=false`, one-time `ShowBackgroundNotice()`. App never exits via X.
- **`main.cpp` hide block** is the only other hide site.

**Inventory of every location that can affect visibility:**

| Location | File:Line | Effect |
|---|---|---|
| `main()` hide block | `main.cpp:18-23` | hide (Opacity 0, no taskbar, Minimized) if `ShouldHideWindow` |
| `BringWindowToFrontDelegate` | `src/ui/MyForm.cpp:95-103` | **show** (but missing Opacity on this branch) — the ONLY un-hide |
| `BringWindowToFront` | `src/ui/MyForm.cpp:68-74` | show (marshals to delegate) |
| `IUiSink::BringWindowToFront` | `src/application/IUiSink.h` | interface for controller |
| `SetWindowVisibleForBackground(true)` | `src/ui/MyForm.cpp:61-67` | hide (Visible false, no taskbar, Minimized) |
| `MyForm_FormClosing` | `src/ui/MyForm.cpp:234-244` | hide (Hide, no taskbar, cancel close) |
| `PromptGhostReset` dialog | `src/ui/MyForm.cpp:75-83` | modal dialog can appear even when form is opacity-hidden |

There is no `SetVisibleCore` override and no tray icon (`docs/GUI.md §1`). If the wake delegate fails to restore opacity, the daemon stays invisible and unreachable except via `taskkill`.

## Historical investigation

Git history analysis (`git log --all --graph`, `git show bcd3cdb`, `git diff 5996d4b..bcd3cdb`):

- **Which commit first fixed Issue #2:** `bcd3cdb` ("Restore window opacity on focus", 2026-08-22 09:38 +0530, author Shivu516) — the only commit touching `src/ui/MyForm.cpp` between `5996d4b` (Complete Re-Organisation) and the later docs/failsafe commits. Message: "Ensure the main form is fully visible when brought to the front by resetting its opacity before showing and activating it. This prevents the window from appearing transparent or hidden when re-enabled from the tray/taskbar flow."

- **Exact code change ( `git show bcd3cdb` ):**
  ```diff
   void MyForm::BringWindowToFrontDelegate() {
  +    this->Opacity = 1.0;
       this->Show();
       this->Visible = true;
       this->ShowInTaskbar = true;
       this->WindowState = FormWindowState::Normal;
       this->BringToFront();
       this->Activate();
       this->Refresh();
   }
  ```
  One line added at top of delegate. Pre-`bcd3cdb` delegate (`5996d4b:src/ui/MyForm.cpp:95-103`) lacked opacity restore; post-`bcd3cdb` (`bcd3cdb:src/ui/MyForm.cpp:95-104`) includes it. `main.cpp` hide block was identical before and after ( `form.Opacity = 0` when `ShouldHideWindow`).

- **Why it worked:** `main.cpp` sets `Opacity=0` for any background/command launch. The daemon therefore starts with a fully transparent window (but with `WndProc` alive). The only path to make it visible again is `BringWindowToFrontDelegate` via the wake event. Without resetting `Opacity`, the delegate sets `Visible`, `ShowInTaskbar`, `WindowState` and calls `Show/BringToFront/Activate`, but WinForms still composites the window at 0% opacity — it is logically visible yet pixel-invisible. Adding `Opacity=1.0` restores the property to opaque before the other calls, so the window becomes actually seen. This matches WinForms semantics where `Opacity` is independent of `Visible`.

- **Whether the fix exists on current branch:** No. `test` at `0c0fe6a` derives via `165ee3b` → `5996d4b`, bypassing `bcd3cdb`. `src/ui/MyForm.cpp:95-103` on HEAD shows no `Opacity = 1.0;` line (verified via `Compare-Object` between `origin/main:src/ui/MyForm.cpp` and `HEAD:src/ui/MyForm.cpp` — the line exists only on `origin/main`). `origin/main` (merge `497d1f6`) does contain `bcd3cdb`.

- **Whether subsequent restructuring changed the relevant path:** The file moved from monolithic `MyForm.h` (~1300 lines, `39ac0ab`) to `src/ui/MyForm.cpp` in `5996d4b`, but the visibility logic was preserved verbatim except for the missing opacity line. `main.cpp` hide block and `SetWindowVisibleForBackground` were unchanged across `5996d4b` → `bcd3cdb` → `0c0fe6a`. The only other visibility-relevant change in the lineage is `acc37d8` reordering the background check in `ApplicationController::Initialize` (present on `origin/main`, absent on `test`), which prevents a background duplicate from waking the GUI at all — a distinct but related behavior.

- **Related change not in `bcd3cdb`:** `acc37d8` ("Update ApplicationController.cpp") swaps the order so `IsBackgroundLaunch` is tested before `TrySignalExistingInstance`. Without this, a `--background` second instance (e.g., Task + Run both firing at logon, or logon daemon vs. manual `--background` test) would incorrectly signal the wake event instead of silently exiting, causing an unwanted GUI pop at boot.

## Root cause hypothesis

Ranked, evidence-graded (do not call "confirmed" without runtime proof):

1. **Most likely — Missing `Opacity = 1.0` in `BringWindowToFrontDelegate` (`src/ui/MyForm.cpp:95` on HEAD).** Evidence: deterministic code reading, `main.cpp` sets `Opacity=0` and never restores; delegate is the sole un-hide; historical one-line fix in `bcd3cdb` directly restored visibility and exists on `origin/main` but not on `test`. Impact: every wake (second instance double-click) leaves window transparent. Confidence: high, but requires manual second-instance test to confirm pixel-invisibility vs. handle error.

2. **Second — Background duplicate incorrectly wakes GUI due to inverted order in `ApplicationController::Initialize` (`src/application/ApplicationController.cpp:315-328` on HEAD).** Evidence: `acc37d8` diff proves the order was intentionally fixed; HEAD has `TrySignalExistingInstance` before `IsBackgroundLaunch`, while fixed branch has the reverse. Impact: at logon with two autostart mechanisms (or during testing with `exe --background` while daemon runs), the background instance wakes the hidden daemon instead of silently exiting — violates "automatic/background startup remains completely invisible" and could surface GUI at boot. Confidence: high for boot-time pop, but distinct from interactive wake transparency.

3. **Contributory — `CommandLine::ShouldHideWindow` case-sensitive (`src/application/CommandLine.cpp:35-45`) vs. `IsBackgroundLaunch` case-insensitive.** Evidence: `docs/KNOWN_ISSUES.md #4` and code comparison (`args[i] == L"--background"` vs `Equals(..., OrdinalIgnoreCase)`). Impact: mixed-case `--Background` would enter background monitoring (`IsBackgroundLaunch` true) but leave window visible ( `ShouldHideWindow` false). Rare, but violates auto-start invisibility guarantee.

4. **Less likely — `SetWindowVisibleForBackground` called *after* delegate restores visibility could re-hide.** Evidence: call site `MyForm_Load:222-224` only on background autostart (`isBackground` true) before any wake; no later re-hide path except `FormClosing`. Unlikely to affect wake.

5. **Not a cause — Other show/activate locations.** `BringWindowToFront` marshaling, `IUiSink`, and `ShowInTaskbar` are correctly wired; `WndProc` does not touch visibility. No alternative un-hide exists to compensate.

## Proposed implementation

Smallest future fix, no camera/monitoring change:

- **Restore `this->Opacity = 1.0;` as the first statement of `MyForm::BringWindowToFrontDelegate()` (`src/ui/MyForm.cpp:95`).** Exact line from `bcd3cdb`. This is the minimal semantic fix: it reverses the `main.cpp` `Opacity=0` that is the only reason a woken daemon remains invisible. No other property needs change — `Show/Visible/ShowInTaskbar/WindowState/BringToFront/Activate/Refresh` already present.

- **Apply `acc37d8` ordering: in `ApplicationController::Initialize` (`src/application/ApplicationController.cpp:315-357`), move the `if (launchRequestedBackground)` silent-exit check *before* `TrySignalExistingInstance`.** So background second instances never signal the wake event. Log event becomes `SingleInstance_BackgroundSilentExit` instead of `SingleInstance_WakeSignalSent` for background duplicates. This preserves "background duplicate: no GUI wake" and is required if a hybrid Run+Task startup is used.

- **Optionally align `ShouldHideWindow` with `IsBackgroundLaunch` (make it case-insensitive or delegate to `IsBackgroundLaunch` plus additional `IsRestoreCameraCommand`/`IsDisableCameraCommand` checks).** One-line change to prevent mixed-case background launch from leaving window visible. Low risk, but can be deferred if strict minimalism required.

No changes to `WndProc`, `NotificationRegistrar`, camera modules, config, or Task Scheduler for this objective.

Intended future behavior:
- Automatic launch (`--background` via Task Scheduler or Run): Opacity 0, no taskbar, Minimized, never activates, `SetWindowVisibleForBackground(true)` hides. No popup.
- Manual launch (no args): Opacity default 1, taskbar ON, Normal, visible. If daemon exists, signals wake event; daemon delegate sets `Opacity=1.0` and shows.
- Interactive second instance: daemon GUI summoned, now opaque and activated, foreground.
- Background duplicate: silent exit, no wake, no log beyond `BackgroundSilentExit`.

## Files expected to change later

- `src/ui/MyForm.cpp` — add `this->Opacity = 1.0;` to `BringWindowToFrontDelegate` (line ~95).
- `src/application/ApplicationController.cpp` — reorder background check before wake signal (lines ~315-328).
- `src/application/CommandLine.cpp` (and `.h` if helper added) — make `ShouldHideWindow` case-insensitive (optional minimal).
- No installer/task changes for this objective.

## Validation plan

- **Direct background launch test:** Kill daemon, run `Windows_Hello_Fix_v2_0.exe --background` from elevated prompt. Verify process starts, no window appears (`EnumWindows` / Task Manager no visible window), `diagnostic.log` shows `Startup_Context BackgroundArg=1`. Wait 10 s, verify still hidden.
- **Manual launch test:** Kill daemon, run `Windows_Hello_Fix_v2_0.exe` (no args). Verify GUI visible, centered, `Status: Service Stopped` or `Running` per config, `Opacity` is 1 (window opaque).
- **Duplicate background test:** Start daemon (`--background`), then run `Windows_Hello_Fix_v2_0.exe --background` again. Verify second process exits quickly, no GUI appears, log shows `SingleInstance_BackgroundSilentExit`, daemon remains hidden, no `WakeSignalSent`.
- **Interactive duplicate test (core Issue #2):** Start daemon (`--background`), then run `Windows_Hello_Fix_v2_0.exe` (no args) as interactive second instance. Verify daemon window becomes fully opaque, appears on taskbar, `WindowState=Normal`, `BringToFront/Activate` brings it foreground. Verify no transparency (visual) and `Visible=true`. Repeat after daemon was hidden via `FormClosing` (X → Hide) to ensure wake still works.
- **Reboot/logon test:** Reboot VM with Task Scheduler (or hybrid) autostart enabled. After logon, verify no GUI, daemon running, then interactive duplicate shows GUI with opacity 1.
- **Camera regression:** After each visibility test, perform lock/unlock cycle, verify `SessionLock_Disable` / `SessionUnlock_Enable` hardware toggles and verification still succeed (no regression from visibility fix).

---

# Combined Implementation Order

1. **Implement Startup Apps registration fix** (installer Run entry + StartupApproved handling + runtime gate). No GUI/camera changes yet. Rationale: startup is the foundation; it determines what launches at boot and whether user toggle is honored. Doing it first isolates registry/task behavior from GUI changes.

2. **Build** (`Windows_Hello_Fix_v2_0.sln`, Release|x64). Verify no compile errors; check `install_script.nsi` syntax (NSIS compile).

3. **Test startup behavior** (fresh install, silent logon, disable via Startup UI, re-enable, duplicate, uninstall — see Objective 1 validation plan). Record `diagnostic.log` and Registry/Task state.

4. **Implement Issue #2 fix** (`MyForm.cpp` Opacity + `ApplicationController.cpp` ordering + optional `CommandLine` case fix). Keep changes minimal and separate commit. Rationale: GUI fix is orthogonal to startup; doing it second prevents conflating a startup regression (e.g., UAC prompt) with a visibility regression (transparent window). Also ensures the background-silent-exit order is in place before testing hybrid duplicate behavior; otherwise boot-time wake could be misattributed.

5. **Build** again (Release|x64).

6. **Test GUI behavior** (background, manual, duplicate background, interactive duplicate, reboot — see Objective 2 validation plan).

7. **Re-test camera/lock/unlock to prove no regressions** (manual lock/unlock, power suspend/resume via lid close/open, `diagnostic.log` verify `DisableTargetCameraHardware_Result` / `Enable…` with PASS). Do full matrix at least once after both fixes.

8. **Commit each logical change separately** (one commit for startup, one for GUI, each with message citing files and behavior). Never batch both objectives into a single commit, to keep `git bisect` usable.

**Why this order is safest:** Startup changes touch installer and early `Initialize` gating — if they break, the app may fail to start at all or prompt UAC; detecting that before touching GUI avoids debugging two failures at once. The GUI fix touches only `MyForm.cpp` delegate and initialization order — it cannot affect startup task registration, but its duplicate-background ordering is required for correct hybrid startup behavior, so it must be present before final startup+GUI integration tests. Camera/lock/unlock is the protected subsystem; re-testing it last proves neither earlier change introduced a regression in the hardware path.

---

# Protected Architecture

The following components MUST remain untouched during these two fixes unless future investigation proves they are directly responsible (per `AGENTS.md` protected components and `docs/KNOWN_ISSUES.md`):

- **Camera hardware modules:** `src/camera/CameraDevice.cpp/.h`, `src/camera/CameraHardware.cpp/.h`, `src/camera/CameraRecovery.cpp/.h`, `src/camera/DeviceError.h` — discovery, `ToggleCameraHardware`/`ToggleCameraHardwareCfgMgr`, staging (10-23), error slots `g_lastSetupApiError`/`g_lastConfigManagerResult`/`g_lastHardwareToggleStage`, retry/recovery `SetCameraHardwareStateVerified` / `RecoverCameraHardware`.
- **Camera recovery policy:** `RecoverCameraHardware(cycleDevice)` 350/900/500 ms sleeps, verify loops, re-enumeration.
- **WTS session monitoring:** `src/events/NotificationRegistrar.cpp` (`WTSRegisterSessionNotification` retry), `src/events/WinEventDecoder.cpp::DecodeSessionEvent` (raw codes 7 lock / 8 unlock), `src/ui/MyForm.cpp::WndProc` `WM_WTSSESSION_CHANGE` dispatch, `ApplicationController::HandleSessionEvent`.
- **Power/lid/button handling:** `NotificationRegistrar::RegisterPowerNotifications` (GUIDs `BA3E0F4D-…` lid, `A70AFB22-…` button), `WinEventDecoder::DecodePowerEvent`, `MyForm::WndProc` `WM_POWERBROADCAST`, `ApplicationController::HandlePowerEvent` with `m_isAlreadyDisabled` latch.
- **Event decoding & cooldown:** `src/events/WinEventDecoder`, `src/events/EventCooldown` (1500 ms dedup).
- **Configuration & logging:** `src/config/ConfigStore.cpp/.h`, `src/config/ConfigPaths.cpp/.h` (`config.txt` `monitoring=0|1` + `device=<id>`, `diagnostic.log` path and `Monitor` lock).
- **Core monitoring policy:** `ApplicationController::IsMonitoring`, `ToggleMonitoring`, `Disable/Enable/RestoreConfiguredCameraHardware`, shutdown `HandleSystemEnd`/`Shutdown` asymmetry (see `KNOWN_ISSUES #3`).

If a bug is found in these, document in this Plan.md only; do not "clean up" or refactor.

---

# Open Questions

- Which hive for Run entry — `HKCU` per-user or `HKLM` machine-wide? Task is per-installing-user (`LogonType Interactive`); `HKCU` matches per-user Startup Apps toggle, but `HKLM` would make app start for all users. Installer currently uses `SetRegView 64` (HKLM) for uninstall key and Run deletion. Needs decision with human approval.
- Exact `StartupApproved` binary format to write for "enabled by default" — Task Manager uses `02 00 00 00 …` for enabled, `03…` for disabled, with timestamp suffix. Should installer pre-seed enabled value or simply delete disabled value and let Task Manager default to enabled? Must verify on target Windows builds.
- Whether `Task Scheduler` daemon task alone could be made visible by adding `Description` or changing `Settings.Hidden` or moving task to `\Startup` folder — rejected as undocumented, but could be validated by creating a test task on a VM and checking Startup enumeration before committing to Run hybrid.
- Does `RequireAdministrator` + Run entry still prompt UAC even when launched via `StartupApproved` gate? If hybrid gate is used, the Run-launched process may still prompt before it can check `StartupApproved` and exit — need to test whether the prompt appears for a disabled-startup Run entry (which shouldn't launch anyway) and whether task-launched elevated process can suppress it.
- Should the runtime `StartupApproved` check live in `ApplicationController::Initialize` or in `main.cpp` before `Application::Run` to avoid constructing `MyForm` at all when disabled? Early exit in `main.cpp` would be more efficient but touches startup contract — needs review.
- Is `ShouldHideWindow` case-insensitivity fix in scope for Issue #2, or should it be deferred to a separate hardening commit?
- Uninstall cleanup scope: should uninstall delete `HKCU\Software\Microsoft\Windows\CurrentVersion\Explorer\StartupApproved\Run\WindowsHelloFix` for *all* users or only current user? `SetShellVarContext current` only cleans current user's `AppData`; per-user Run cleanup may miss other profiles.
- Confirm no existing Group Policy or Defender Application Control blocks Run entries for elevated exes — test on clean Windows 10/11 with default UAC.
- Verify `x64/Release/install_script.nsi` ordering of Run write vs. task creation vs. warm-up `/restore-camera` — should Run be written before or after task registration to avoid race where user reboots mid-install?
- Confirm whether `bcd3cdb` opacity line alone is sufficient or if additional `Opacity` handling is needed for `SetWindowVisibleForBackground(false)` or `FormClosing` hide path — no evidence, but should be checked by code-review of all visibility sites listed above.

