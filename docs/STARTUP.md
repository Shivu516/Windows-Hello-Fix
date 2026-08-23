# Application Startup — all launch paths (as-built)

Baseline: branch `test`, commit `acc37d8`. This document traces every way
`Windows_Hello_Fix_v2_0.exe` can start and what each path actually does.

## 1. Entry point behavior common to ALL paths

`main.cpp` (`main(array<String^>^)`, `[STAThread]`):

1. `Application::EnableVisualStyles()`, `SetCompatibleTextRenderingDefault(false)`.
2. Construct `MyForm form;`
   - constructor creates `ApplicationController(this)` and builds controls.
3. `bool runHidden = CommandLine::ShouldHideWindow(args);`
   - true for **any** of: `/background`, `--background`, `/disable-camera`,
     `--disable-camera`, `/enable-camera`, `--enable-camera`, `/restore-camera`,
     `/repair-camera` (**case-sensitive** comparison).
4. If hidden: `Opacity = 0`, `ShowInTaskbar = false`,
   `WindowState = Minimized`. Nothing in `main.cpp` ever un-hides.
5. `Application::Run(%form)` — message pump; the form's `Load` event then runs
   `MyForm_Load`, which calls `m_controller->Initialize(hwnd, args)`.

Inside `Initialize` (src/application/ApplicationController.cpp):

1. Writes `Startup_Context | Elevated=… | IntegrityRid=… | BackgroundArg=… |
   Exe=… | Cwd=… | Config=…` to `diagnostic.log`.
2. **Command modes exit before single-instance handling:**
   - `/restore-camera`, `/enable-camera`, `--enable-camera`, `/repair-camera`
     → `RestoreConfiguredCameraHardware(true)` → log begin/end → `Exit(0)`.
   - `/disable-camera`, `--disable-camera`
     → `DisableTargetCameraHardware(true)` + verify → log begin/end → `Exit(0)`.
3. Single-instance mutex `Global\WindowsHelloFix_AppMutex`. If it already
   exists (a daemon is running):
   - with a background argument → log `SingleInstance_BackgroundSilentExit`
     → `Exit(0)` (never wakes the running GUI);
   - else signal `Global\WindowsHelloFix_WakeupEvent` → log
     `SingleInstance_WakeSignalSent` → `Exit(0)`;
   - if signaling fails → `IUiSink::PromptGhostReset()` Yes/No box → on Yes:
     recover camera for config device (or first `MI_00` scan hit), save config,
     `ProcessUtils::KillHelloFixProcess()` (`taskkill /F /IM
     Windows_Hello_Fix_v2_0.exe /T`), `Sleep(500)`, `Application::Restart()`.
4. First instance continues: create wake event, restore configured camera,
   register power notifications, start wake-listener thread, register WTS
   session notifications (6 × 500 ms retry), return true.
5. Back in `MyForm_Load`: scan cameras, populate dropdown, select saved /
   `MI_00` / first device, `EnsureConfigFileExists`,
   `EnableTargetCameraHardware(autoStart)` ("first enable"), then if
   `(isBackground || configAutoStart) && device selected` → monitoring ON,
   enable again, UI shows running state; background additionally hides the
   window via `SetWindowVisibleForBackground(true)`.

Elevation: the exe manifest is `requireAdministrator`; every launch either
runs elevated already (scheduled task RunLevel Highest, installer finish page,
child of elevated shell) or triggers one UAC prompt.

## 2. Startup path table

| # | Source | Executable / arguments | Privilege | Expected GUI visibility | Expected monitoring state | Expected camera state |
|---|---|---|---|---|---|---|
| 1 | Manual launch — Start Menu shortcut `$SMPROGRAMS\Windows Hello Fix\Windows Hello Fix.lnk`, optional desktop shortcut, or direct exe run | exe, no args | UAC prompt (manifest requireAdministrator); none if parent already elevated | **Visible**, centered, normal window | Monitoring auto-starts only if `config.txt` says `monitoring=1` | Camera force-enabled once at Load ("prevent bricking"); left enabled unless monitoring starts |
| 2 | NSIS finish page (`MUI_FINISHPAGE_RUN`) | exe, no parameters defined | Inherits installer's admin token — no extra UAC prompt | **Visible** (interactive first-run experience) | Off unless config already says so (fresh install writes nothing) | Enabled at Load |
| 3 | Task Scheduler task `WindowsHelloFix` (at logon) | exe `--background` | Elevated (RunLevel Highest, interactive token) | **Hidden** — Opacity 0/minimized/no-taskbar from main.cpp, plus `SetWindowVisibleForBackground(true)` after autostart | **Always ON** when a device was selected (`isBackground \|\| autoStart` branch ignores config value) | Enabled at Load, then enabled again by monitoring branch; daemon manages lock/power from here |
| 4 | Task Scheduler task `WindowsHelloFix_Lock` (session lock trigger) | exe `--disable-camera` | Elevated (Highest, interactive token) | Hidden briefly (main.cpp hide), process exits within seconds | N/A — command mode exits before mutex/monitoring logic | Target RGB camera disabled + verified; logs `Command_DisableCamera_*` |
| 5 | Task Scheduler task `WindowsHelloFix_Unlock` (session unlock trigger) | exe `/enable-camera`-equivalent: `--enable-camera` | Elevated (Highest, interactive token) | Hidden briefly, exits | N/A | Configured camera restored (cycled) + verified; logs `Command_EnableCamera_*` |
| 6 | Installer sections (install & uninstall) | exe `/restore-camera` (run twice per phase) | Elevated (installer context) | Never visible (command mode exits immediately) | N/A | All/configured cameras recovered |
| 7 | Second manual launch while daemon runs | exe, no args, mutex exists, wake event works | UAC prompt first | Daemon GUI is **summoned** via wake event; new process exits | Unchanged (daemon keeps its own state) | Unchanged |
| 8 | Any launch with `--background` while daemon runs | exe `--background` | Elevated | No GUI change anywhere | Unchanged | Unchanged (silent exit, logged `SingleInstance_BackgroundSilentExit`) |
| 9 | Ghost-reset flow | second instance whose wake-signal fails → user clicks "Yes" on reset prompt | Elevated | Old daemon killed; `Application::Restart()` spawns fresh visible instance | Restarted instance follows path #1/#3 rules | Ghost device force-recovered before kill |
| 10 | HKLM/HKCU `Run` keys or Startup folder | **Not used.** Installer deletes legacy `HKLM …\Run\WindowsHelloFix` and creates no Run-key/startup-folder entries | n/a | n/a | n/a | n/a |

## 3. Duplicate-instance decision tree (exact code order)

```
Initialize(hwnd, args)
  ├─ command-mode arg?            → do camera work, Exit(0). NOTE: this happens
  │                                 BEFORE the mutex check, so --disable-camera /
  │                                 --enable-camera tasks run even while the daemon
  │                                 lives (intentional failsafe, but see KNOWN_ISSUES).
  ├─ CreateAppMutex already exists?
  │    ├─ background arg          → silent exit
  │    ├─ TrySignalExistingInstance (SetEvent wakeup) succeeded? → exit
  │    └─ PromptGhostReset?
  │         ├─ Yes → recover ghost device → SaveConfigState → KillHelloFixProcess
  │         │        (taskkill matches ALL processes with that image name,
  │         │         including the caller) → Sleep(500) → Application::Restart()
  │         └─ No  → Exit(0)
  └─ proceed as first instance
```

## 4. What each startup path leaves behind

- Every non-command path ends with WTS session notifications registered
  (unless the retry loop failed — logged as
  `WTSRegisterSessionNotification_Failed_LastError=N`) and lid/button power
  notifications registered.
- Command paths (#4–#6) never reach notification registration; they also skip
  the wake-listener thread and mutex creation.
- The wake event exists only while a first-instance GUI/daemon process lives;
  it is signaled-and-closed in `Shutdown`/finalizer.

## 5. Uncertainties (documented, not fixed)

- `CommandLine::IsBackgroundLaunch` compares case-insensitively while
  `ShouldHideWindow` in the same class compares case-sensitively; a mixed-case
  argument could theoretically start background monitoring without hiding the
  window.
- Path #9 kills by image name, which includes the calling process itself; the
  subsequent `Sleep`/`Restart()` sequence races with termination. It appears to
  work in practice but the ordering is not guaranteed.
- Whether the finish-page launch (#2) can occur before the install-time
  `/restore-camera` warm-up pass has finished is decided purely by the user
  clicking "Finish" (the script sleeps 2500 ms after that pass before writing
  the uninstaller, but the finish button is already live).
