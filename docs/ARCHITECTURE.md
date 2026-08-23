# Windows Hello Fix — Architecture (as-built)

> **Baseline:** this document describes the code as it exists on branch `test`,
> commit `acc37d8` ("Update ApplicationController.cpp"). It documents what the
> code **actually does**, not what it is supposed to do. Nothing here has been
> changed by the documentation effort.
>
> **Historical note:** `docs/architecture/*.md`, `docs/refactoring/*.md` and
> `src/README.md` describe an earlier state in which `src/` was unbuilt
> scaffolding and the whole application lived in a root-level monolithic
> `MyForm.h`. That migration has since been completed: `src/` is now the live,
> compiled implementation. Treat those older documents as history, not truth.

## 1. What the application is

Windows Hello Fix v2.0 is a C++/CLI (.NET 4.7.2, WinForms) desktop utility that
disables the RGB camera's device node when the workstation locks (or suspends /
shuts down) and re-enables it on unlock / resume, so that Windows Hello face
authentication uses the IR sensor instead of the RGB sensor.

It is a single executable (`Windows_Hello_Fix_v2_0.exe`) that always runs
elevated (`requireAdministrator` manifest + `UACExecutionLevel` in the vcxproj)
and can act as:

- an interactive GUI (device picker + start/stop button), or
- a hidden background daemon (started by Task Scheduler with `--background`),
  or
- a short-lived command-line worker (`--disable-camera`, `/enable-camera`,
  `/restore-camera`, `/repair-camera`).

## 2. Build shape

| Item | Value |
|---|---|
| Solution | `Windows_Hello_Fix_v2_0.sln` |
| Project | `Windows_Hello_Fix_v2_0.vcxproj` — Managed C++ (`CLRSupport=true`), toolset v143, Windows SDK 10.0.26100, .NET Framework v4.7.2 |
| Entry point | `main.cpp` → managed `main(array<String^>^)`, `[STAThread]`; `SubSystem=Windows`, `EntryPointSymbol=main` |
| Elevation | `app.manifest`: `requestedExecutionLevel level="requireAdministrator"`; linker `UACExecutionLevel=RequireAdministrator` |
| Linked libs | x64 configs: `setupapi.lib;user32.lib;wtsapi32.lib;advapi32.lib` (+ `#pragma comment(lib)` for cfgmgr32 etc.) |
| Resources | `Windows_Hello_Fix_v2_0_resources.rc` (compiled) includes `resource1.h` (`IDI_ICON1 = 114`). The uncompiled `Windows_Hello_Fix_v2_0.rc` pairs with `resource.h` (`IDI_ICON1 = 102`). `src/ui/MyForm.h` includes `resource.h` — see `docs/KNOWN_ISSUES.md` |
| Compiled sources | `main.cpp` plus every `.cpp` under `src/` (see vcxproj `ClCompile` items). Root `MyForm.h` is a 4-line compatibility shim that forwards to `src/ui/MyForm.h`. |

## 3. Component map and ownership

```
main.cpp                       process entry; WinForms bootstrap; initial hide
   │
   ▼
MyForm (src/ui)                WinForms Form; owns controls; WndProc dispatch;
   │  implements IUiSink       Load/FormClosing/button handlers
   │
   ▼
ApplicationController          orchestration: startup policy, monitoring state,
(src/application)              camera actions, notification lifetime,
                               single-instance/wake handling
   │
   ├──► CommandLine            argument classification (static)
   ├──► SingleInstance         mutex + wake-up named event (static)
   ├──► NotificationRegistrar  WTS session + power-setting registration (static)
   ├──► WinEventDecoder        raw message → SystemEvent decoding (static)
   ├──► EventCooldown          1500 ms per-channel dedup (static)
   ├──► ConfigStore            config.txt read/write + diagnostic.log writer
   ├──► ConfigPaths            %APPDATA% path resolution
   ├──► PrivilegeInfo          elevation/integrity queries
   ├──► ProcessUtils           taskkill wrapper (ghost reset)
   └──► Camera* (native)
        CameraDevice           ScanSystemCameras() discovery
        CameraHardware         SetupAPI / CfgMgr toggles + state query + verify
        CameraRecovery         retry/cycle/recover orchestration
        DeviceError.h          global diagnostic error slots
```

Ownership rules that are actually enforced by the code today:

- **`MyForm`** owns all WinForms controls and the only message pump. It never
  touches SetupAPI/CfgMgr directly; it calls `ApplicationController`.
- **`ApplicationController`** owns: the single-instance mutex handle, the wake
  event handle, power-notification handles, the selected camera instance id
  (a native `std::wstring*`), monitoring flags, and the wake-listener thread.
  All camera disable/enable decisions funnel through it.
- **Native camera functions** (`CameraHardware`, `CameraRecovery`,
  `CameraDevice`) are free functions with no managed dependencies. They do not
  log to `diagnostic.log` themselves; callers log.
- **`ConfigStore`** is the only writer of `%APPDATA%\Windows Hello Fix\`
  files (`config.txt`, `diagnostic.log`).
- **`WinEventDecoder` / `EventCooldown`** are pure/static helpers with no I/O.

Dependency direction is one-way: `ui → application → {camera, config, events,
system}` and everything may use `utilities`. No module depends back on `ui`
except through the `IUiSink` interface implemented by `MyForm`.

## 4. End-to-end flow

```
Process start (any path, see docs/STARTUP.md)
        ↓
main(): EnableVisualStyles → construct MyForm (creates ApplicationController,
        InitializeComponent) → if CommandLine::ShouldHideWindow(args):
        Opacity=0, ShowInTaskbar=false, WindowState=Minimized
        ↓
Application::Run(%form)   ← message pump starts; form handle created
        ↓
MyForm_Load (UI thread)
    ├─ controller->SetHwnd(hwnd)
    ├─ controller->Initialize(hwnd, args)
    │     ├─ write "Startup_Context" log line
    │     ├─ command modes? (/restore-camera|/enable-camera|/repair-camera or
    │     │   /disable-camera|--disable-camera) → do camera work → Exit(0)
    │     ├─ create Global\WindowsHelloFix_AppMutex; if already exists:
    │     │     ├─ --background arg → silent exit
    │     │     ├─ signal Global\WindowsHelloFix_WakeupEvent → exit
    │     │     └─ else ghost-reset prompt → taskkill + Application::Restart()
    │     ├─ create wakeup event (auto-reset)
    │     ├─ RestoreConfiguredCameraHardware(true)   (camera ON at startup)
    │     ├─ RegisterPowerNotifications (lid + button GUIDs)
    │     ├─ start wake-listener background thread
    │     └─ RegisterSessionNotificationWithRetry (WTS, 6 × 500 ms)
    ├─ scan cameras → populate dropdown → saved → MI_00 → first selection
    ├─ EnsureConfigFileExists(selected device)
    ├─ EnableTargetCameraHardware(autoStart)      ("first enable to prevent bricking")
    └─ if (isBackground || configAutoStart) && device selected:
          IsMonitoring = true; enable again; UI "Service Running";
          if isBackground → SetWindowVisibleForBackground(true)  (hides window)
       else UI "Service Stopped"
        ↓
Runtime (UI thread via WndProc)                    Runtime (listener thread)
  WM_QUERYENDSESSION / WM_ENDSESSION                 WaitForSingleObject(wakeup event)
      → HandleSystemEnd → disable if monitoring          → sink->BringWindowToFront()
  WM_POWERBROADCAST (suspend/lid/button/resume)
      → dedup → decode → HandlePowerEvent
  WM_WTSSESSION_CHANGE (lock/unlock)
      → dedup → decode → HandleSessionEvent
        ↓
Exit paths
  User close → FormClosing cancels & hides (app keeps running)
  Process teardown → MyForm dtor/finalizer → ApplicationController::Shutdown(bool)
      normal exit : enable camera, save config(monitoring=1, device)
      system end  : disable camera, save config
      then: signal+close wake event, unregister power notifications, release mutex
```

Full detail per stage: `STARTUP.md` (launch paths), `SESSION_MONITORING.md`
(lock/unlock/power/lid), `CAMERA_HARDWARE.md` (hardware path), `GUI.md`
(visibility), `LOGGING.md` (events).

## 5. Threading model

- **UI thread**: everything in `MyForm` (WndProc, Load, button handlers) and
  therefore every camera operation, including their blocking `Sleep()` calls
  (verify loops 100 ms ×3, reinit 250 ms, recovery cycle 350/900/500 ms, power
  handler 500/1000 ms, WTS registration retry up to 6 × 500 ms). The UI can
  freeze for seconds during hardware operations; there is no async queue
  (the old `WndProc_Redesign.txt` proposal was never integrated).
- **Wake listener thread**: one managed `System::Threading::Thread`
  (`IsBackground=true`) started in `Initialize`, blocked on
  `Global\WindowsHelloFix_WakeupEvent`; marshals `BringWindowToFront` onto the
  UI thread with `Control.Invoke`.
- **Logging**: `ConfigStore::WriteDiagnosticLog` serializes appends with a
  `Monitor` lock object.
- Scheduled-task instances of the exe are separate processes; cross-process
  coordination happens only via the mutex/wake-event names.

## 6. Persistent state

| Path | Writer | Format |
|---|---|---|
| `%APPDATA%\Windows Hello Fix\config.txt` | `ConfigStore::SaveConfigState` | Line 1: `monitoring=0\|1`; line 2: `device=<instance id>` |
| `%APPDATA%\Windows Hello Fix\diagnostic.log` | `ConfigStore::WriteDiagnosticLog(WithDevice)` | Append-only text, see `LOGGING.md` |

The installer pre-creates both the directory and an empty `diagnostic.log`;
its `WindowsHelloFix_LogCleanup` scheduled task attempts to truncate
`diagnostic.log` daily at 00:00 (see `TASK_SCHEDULER.md` for a caveat).

## 7. Named kernel objects

| Name | Type | Owner | Purpose |
|---|---|---|---|
| `Global\WindowsHelloFix_AppMutex` | Mutex (initial owner TRUE) | `SingleInstance::CreateAppMutex` | Single-instance detection (`ERROR_ALREADY_EXISTS`) |
| `Global\WindowsHelloFix_WakeupEvent` | Auto-reset event | `SingleInstance::CreateWakeupEvent` | Second-instance → running-daemon GUI summoning |
| `WindowsHelloFixSetup_Mutex` | Mutex (no `Global\` prefix) | NSIS installer `.onInit` | Installer single-instance (per-session scope) |

## 8. Where to go next

- Launch/elevation/GUI-visibility matrix: `docs/STARTUP.md`
- Lock/unlock/power/lid event trace: `docs/SESSION_MONITORING.md`
- Camera discovery/toggle/verification: `docs/CAMERA_HARDWARE.md`
- GUI visibility inventory: `docs/GUI.md`
- Installer behavior: `docs/INSTALLER.md`
- Scheduled tasks: `docs/TASK_SCHEDULER.md`
- Diagnostic events catalog: `docs/LOGGING.md`
- Per-file reference for every file under `src/`: `docs/src/*.md`
- Documented-but-unfixed oddities: `docs/KNOWN_ISSUES.md`
