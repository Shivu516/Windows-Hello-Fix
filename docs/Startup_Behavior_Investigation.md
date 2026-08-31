# HelloFix v2.0 Startup Camera Behavior — Read-Only Investigation

> **Type:** Static source investigation — no source, installer, registry, Task Scheduler, or camera hardware was modified.  
> **Date:** 2026-08-31  
> **Primary control:** `reference/release-v2.0` (`MyForm.h:1-1362`, `main.cpp:1-37`, `Release/install_script.nsi:1-257`)  
> **Current comparison:** `main.cpp:1-38`, `src/core/MyForm.h:1-171`, `src/core/MyForm_Core.cpp:1-499`, `src/core/MyForm_Camera.cpp:1-470`, `src/core/MyForm_Events.cpp:1-119`, `src/core/MyForm_System.cpp:1-64`, `x64/Release/install_script.nsi:1-283`  
> **Branch:** `test` (contains `27a1174` not yet deployed to this machine — see live vs workspace hash notes in `docs/Anomaly_Investigation.md`)  
> **Scope compliance:** Only this file was written; `reference/`, `src/`, `main.cpp`, `install_script.nsi`, registry, tasks, and PnP state were read only.

---

## 1. Executive Summary

**Question:** Does the existing HelloFix v2.0 program still enable/recover the configured RGB camera whenever the daemon is first launched, including at Windows startup?

**Short answer:**

| Question | Answer | One-line reason |
|---|---|---|
| **A. Does v2.0 enable/recover the camera when the program first launches?** | **YES** | `MyForm_Load` at `reference/release-v2.0/MyForm.h:943` unconditionally calls `RestoreConfiguredCameraHardware(true)` at `1066` before any UI or monitoring decision, and later `EnableTargetCameraHardware(shouldAutoStartByConfig)` at `1122` / `EnableTargetCameraHardware(false)` at `1131`. |
| **B. Does `--background` retain that behavior?** | **YES** | `launchRequestedBackground` is detected at `948-952` but the recovery at `1066` executes before the `startInBackground` branch at `1126-1142`; background vs foreground share the same restore path (only UI visibility differs). |
| **C. Is camera recovery fundamentally dependent on Task Scheduler?** | **NO** | Task Scheduler only **launches** the exe (`--background`); the recovery logic lives entirely inside `MyForm_Load` / `MyForm_Camera.cpp`. Any launch mechanism that reaches `MyForm_Load` triggers the same recovery. |
| **D. What does v2.0 register for Windows startup?** | One `At logon` task (`WindowsHelloFix --background`) + two `SessionStateChange` helpers + one daily cleanup; **no** `HKLM/HKCU …\Run` entry (the installer **deletes** it at `install_script.nsi:105`). | See §7/§6. |
| **E. Is the Task Manager Startup Apps entry required?** | **NO** | v2.0 by design does not use `Run`; recovery is via the `At logon` scheduled task, which Task Manager does not surface in Startup Apps. |
| **F. Could “disabled until Win+L” occur because HelloFix never launched at boot?** | **YES** — and that is the only hypothesis consistent with the traced code and the live `267011` never-ran evidence. | See §10. |

**Confidence:** A–C, E **High** (direct static trace); D **High**; F **High** (code + live `267011` divergence from §J of previous investigation — scheduler health vs isolated task failure). The only non-static uncertainty is whether this machine's post-launch disable-by-power-event path (documented `docs/Plan.md` 431 ms quirk) ever contributes on *this* boot — no diagnostic log exists for `00:49`, so §10 calls it *unlikely* here.

---

## 2. v2.0 Startup Call Graph

### 2.1 Entry point — `reference/release-v2.0/main.cpp:1-37`

```cpp
main.cpp:7  [STAThread] int main(array<String^>^ args)
main.cpp:10   Application::EnableVisualStyles()
main.cpp:11   Application::SetCompatibleTextRenderingDefault(false)
main.cpp:13   MyForm form;                          // ctor
main.cpp:16-24  runHidden = args contains --background/--disable-camera/--enable-camera//restore-camera//repair-camera
main.cpp:27-32  if (runHidden) { form.Opacity=0; form.ShowInTaskbar=false; form.WindowState=Minimized; }
main.cpp:34   Application::Run(%form);              // pumps messages → triggers MyForm_Load → WndProc
```

`runHidden` is a *presentation* flag only. `Opacity=0` + `ShowInTaskbar=false` + `Minimized` keeps `WndProc` alive while hidden, exactly as commented at `main.cpp:28`.

### 2.2 Construction — `reference/release-v2.0/MyForm.h:587-605` (`MyForm::MyForm`)

```
MyForm.h:587  components=nullptr
              cachedCameras = new vector<CameraDeviceInfo>()
              selectedInstanceId = new wstring()
              isMonitoring=false, isBackgroundMode=false, isSystemEnding=false
              cameraStateInitialized=false, cameraExpectedDisabled=false
              hAppMutex=NULL, hWakeupEvent=NULL, keepListening=true
              hLidNotification=NULL, hButtonNotification=NULL
              diagnosticLogSync = gcnew Object()
MyForm.h:888  InitializeComponent()   // creates deviceDrop/btnToggle/lblTitle/lblStatus, loads Icon IDI_ICON1,
                                    // wired: FormClosing → MyForm_FormClosing 937, Load → MyForm_Load 937
```

No camera touched in the ctor. All hardware work is deferred to `MyForm_Load`.

### 2.3 `MyForm_Load` — the startup spine — `reference/release-v2.0/MyForm.h:943-1177`

Ordered exactly as executed (early exits guard short-lived workers):

```
943  System::Void MyForm::MyForm_Load(sender,e)
944    args = Environment::GetCommandLineArgs()
947-953  launchRequestedBackground = args contains --background/--background
955-967  WriteDiagnosticLog  Startup_Context | Elevated | IntegrityRid | BackgroundArg | Exe | Cwd | Config
         (logs to %APPDATA%\Windows Hello Fix\diagnostic.log via MyForm.h:714-735)
969-977  if (IsRestoreCameraCommand(args)) { ShowInTaskbar=false Visible=false
          RestoreConfiguredCameraHardware(true) → Write Command_EnableCamera_End → Environment::Exit(0) }
         // matches /restore-camera /enable-camera --enable-camera /repair-camera at MyForm.h:835-842
979-989  if (IsDisableCameraCommand(args)) { ShowInTaskbar=false Visible=false
          DisableTargetCameraHardware(true) → Verify → Write Command_DisableCamera_End → Exit(0) }
         // matches /disable-camera --disable-camera at MyForm.h:848-854

         // === single-instance layer ===
992    hAppMutex = CreateMutex(NULL,TRUE,"Global\\WindowsHelloFix_AppMutex")
994    if (GetLastError()==ERROR_ALREADY_EXISTS) {
           // background re-launches (the At-logon task firing into a running daemon) must NOT wake the GUI:
           // v2.0 first tries wake event, then background-fail-quiet, then ghost-mutex MessageBox
993-1059   if (wakeSignalSent) → SingleInstance_WakeSignalSent → Exit(0)
            if (launchRequestedBackground) → SingleInstance_BackgroundWakeEventMissing → Exit(0)  [1031]
            else MessageBox Yes/No → RecoverCameraHardware(nativeGhostId,true) → taskkill /F → Restart
         }
1061   hWakeupEvent = CreateEvent(NULL,FALSE,FALSE,"Global\\WindowsHelloFix_WakeupEvent")

         // === STARTUP CAMERA RECOVERY (the critical line) ===
1065   WriteDiagnosticLog Startup_RestoreConfiguredCameraHardware Enabled PASS
1066   RestoreConfiguredCameraHardware(true)   // cycle=true, ~1.75 s Sleeps 350/900/500 + verify

         // === hardware notification registration ===
1069-1074  hLidNotification  = RegisterPowerSettingNotification(hWnd,GUID_LIDSWITCH_STATE_CHANGE)
          hButtonNotification = RegisterPowerSettingNotification(hWnd,GUID_POWER_BUTTON_TIMESTAMP)

         // === device enumeration & UI population ===
1083   *pCachedCameras = ScanSystemCameras()    // SetupDiGetClassDevs DIGCF_ALLCLASSES|PRESENT → filter Camera/Image
1088   shouldAutoStartByConfig = LoadConfigState(savedDeviceInstance) // %APPDATA%\…\config.txt: monitoring=1 + device=
1090-1119  populate deviceDrop Items, resolve savedIdx/autoIdx(MI_00)/0, SelectedIndex
1113   *pSelectedInstanceId = pCachedCameras[SelectedIndex].instanceId
1115   EnsureConfigFileExists(selectedDeviceForConfig)

         // === second recovery layer — guard against bricking loops ===
1122   if (SelectedIndex!=-1) EnableTargetCameraHardware(shouldAutoStartByConfig) // cycle == monitoring flag

1126   if ((startInBackground || shouldAutoStartByConfig) && SelectedIndex!=-1) {
1127     *pSelectedInstanceId = cached SelectedIndex.instanceId
1128     isMonitoring=true
1131     EnableTargetCameraHardware(false)    // stable: no bounce every launch
1133-1141  deviceDrop.Enabled=false; btnToggle=Stop Monitoring; lblStatus Service Running Green;
           if (startInBackground) { Visible=false; ShowInTaskbar=false; WindowState=Minimized }
         } else {
1144     isMonitoring=false; deviceDrop.Enabled=true; btnToggle=Start Monitoring; lblStatus Stopped Gray
         }

1152-1154  backgroundWorker = new Thread(ListenForWakeupSignal) → IsBackground=true → Start()
          // ListenForWakeupSignal MyForm.h:1179-1191: WaitForSingleObject(hWakeupEvent,INFINITE) → Invoke BringWindowToFrontDelegate
1193-1201  BringWindowToFrontDelegate: Show Visible ShowInTaskbar WindowState Normal BringToFront Activate

1158-1176  WTSRegisterSessionNotification retry loop 6×500 ms
          if success → Write WTSRegisterSessionNotification_Success else Write Failed LastError

         // (no watchdog in v2.0 — src/watchdog is v2.1 addition)
```

**Key ordering observation:** `RestoreConfiguredCameraHardware(true)` at `1066` precedes the `startInBackground` conditional at `1126`, the `WTSRegisterSessionNotification` loop at `1159`, and the watchdog (which does not exist in v2.0). Therefore **every** daemon launch—manual, `--background`, or scheduler-driven—that passes the two early-exit command checks reaches the same recovery.

---

## 3. Camera Initialization / Recovery Call Chain

### 3.1 Target resolution — `reference/release-v2.0/MyForm.h:795-831` `TryGetTargetCameraInstanceId`

```
TryGetTargetCameraInstanceId(target, preferCurrentSelection)
  if (preferCurrentSelection && selectedInstanceId not empty) → target = *selectedInstanceId          // 798-803
  else LoadConfigState(savedDeviceInstance) → target = savedDeviceInstance trimmed+sanitized           // 806-810
  else if (!preferCurrentSelection && selectedInstanceId not empty) → target = *selectedInstanceId    // 812-815
  else ScanSystemCameras() → first MI_00 else first camera                                            // 818-827
```

`LoadConfigState` at `758-783` reads `%APPDATA%\Windows Hello Fix\config.txt` (`monitoring=1`, `device=USB\…&MI_00\…`), trims `\r\n`, and the live file on this machine matches that format.

### 3.2 Hardware state detection — `reference/release-v2.0/MyForm.h:427-472` `GetCameraHardwareDisabledState` + `474-485` `VerifyCameraHardwareState`

```
GetCameraHardwareDisabledState(target, isDisabled)
  SetupDiGetClassDevs(DIGCF_ALLCLASSES) → enum SetupDiGetDeviceInstanceId
  for matching instanceId (== or _wcsicmp):
    CM_Get_DevNode_Status(status,problem,DevInst,0)
    SetupDiGetDeviceRegistryProperty SPDRP_CONFIGFLAGS → configFlags
    disabledByConfig       = hasConfigFlags && (configFlags & CONFIGFLAG_DISABLED(1))
    disabledByProblemCode  = statusResult==CR_SUCCESS && problem==CM_PROB_DISABLED(22)
    isDisabled = disabledByConfig || disabledByProblemCode          // 460-463

VerifyCameraHardwareState(target, shouldBeDisabled)
  for 3 attempts: if (Get... == shouldBeDisabled) return true; Sleep(100)
```

Both primitives are shared by every caller.

### 3.3 Verified state change — `reference/release-v2.0/MyForm.h:512-554` `SetCameraHardwareStateVerified`

```
SetCameraHardwareStateVerified(target, enable, reinitializeOnMismatch)
  if (target empty) return false
  shouldBeDisabled = !enable
  if (Verify(target, shouldBeDisabled)) return true      // check-before-change, skip churn 519-522
  for 3 attempts:
    ToggleCameraHardware(target,enable)            // SetupDi DIF_PROPERTYCHANGE 317-376 stage 10-15
    if (Verify(...)) → RecordHardwareToggleTime → return true
    ToggleCameraHardwareCfgMgr(target,enable)      // LocateCameraDevInst → CM_Enable/Disable_DevNode → CM_Reenumerate 405-425 stage 20-23
    if (Verify(...)) → RecordHardwareToggleTime → return true
    if (reinitializeOnMismatch) { Toggle(!enable) both paths; Sleep(250) }
    Sleep(250)
  ToggleCameraHardware(target,enable); return Verify(...)
```

`ToggleCameraHardwareCfgMgr` and `ToggleCameraHardware` each set `g_lastSetupApiError / g_lastConfigManagerResult / g_lastHardwareToggleStage` (`35-38`) for diagnostics; `RecoverCameraHardware` log lines later surface them as `SetupErr/CfgMgr/Stage`.

### 3.4 Recovery wrappers

**`RecoverCameraHardware`** — `reference/release-v2.0/MyForm.h:556-573`

```
RecoverCameraHardware(target, cycleDevice)
  restored = SetCameraHardwareStateVerified(target, true, false)            // enable
  if (cycleDevice) { Sleep(350); SetVerified(false); Sleep(900);
                     restored = SetVerified(true)||restored; Sleep(500);
                     restored = SetVerified(true)||restored }               // full cycle ≈1.75 s
```

**`RestoreConfiguredCameraHardware`** — `reference/release-v2.0/MyForm.h:859-878`

```
RestoreConfiguredCameraHardware(cycleDevice)
  LoadConfigState(savedDeviceInstance)
  if (savedDeviceInstance not empty) restored = RecoverCameraHardware(nativeDeviceId, cycleDevice)
  if (!restored) RestoreAllCameraHardware(cycleDevice) // ScanSystemCameras → Recover each, 576-581
```

**`EnableTargetCameraHardware`** / **`DisableTargetCameraHardware`** — `reference/release-v2.0/MyForm.h:106-140` / `142-176` (current split at `src/core/MyForm_Camera.cpp:377-447`)

```
DisableTargetCameraHardware(retryOnFailure)
  TryGetTargetCameraInstanceId(true) → if not found log NoTarget
  GetCameraHardwareDisabledState → if alreadyDisabled → cameraExpectedDisabled=true AlreadyDisabled PASS
  else result=SetCameraHardwareStateVerified(target,false,retryOnFailure); verified=Verify(true)
       cameraExpectedDisabled=result; log DisableTargetCameraHardware_Result|Elevated|IntegrityRid|SetupErr|CfgMgr|Stage

EnableTargetCameraHardware(cycleDevice)
  symmetric: AlreadyEnabled fast-exit, else RecoverCameraHardware(target,cycleDevice) + Verify,
  cameraExpectedDisabled = !result
```

**Therefore the answer to “what operation at startup”** is **not** `disable→wait→enable` and not `enable only when monitoring starts`. The first operation in `MyForm_Load` is:

```
RestoreConfiguredCameraHardware(true)  // reference MyForm.h:1066
  → RecoverCameraHardware(configuredDevice, true)   // 556 with cycle=true
    → SetCameraHardwareStateVerified(target, true)  + verify, then if cycle:
      disable → 900 ms → enable → 500 ms → enable   // the "restore" cycle
```

followed later by:

```
EnableTargetCameraHardware(shouldAutoStartByConfig) // 1122  (cycle = monitoring flag)
EnableTargetCameraHardware(false)                   // 1131  when (background || shouldAutoStart)
```

The `SetCameraHardwareStateVerified` check-before-change means an already-enabled camera short-circuits out immediately (`AlreadyEnabled` log lines). Only a disabled target incurs hardware churn.

---

## 4. Normal Launch Behavior (no arguments)

User double-clicks `Windows_Hello_Fix_v2_0.exe`. `main.cpp:16-24` leaves `runHidden=false`, so `Opacity=1` (default) and `ShowInTaskbar=true`. `MyForm_Load` follows the full spine at §2.3 with `launchRequestedBackground=false`:

- Early command checks at `969`/`979` do not fire.
- Mutex is fresh (`CreateMutex` succeeds), so the single-instance MessageBox path is skipped.
- `RestoreConfiguredCameraHardware(true)` **does** fire at `1066` (~1.75 s).
- `1122` enables again conditioned on `monitoring=1`; if the last session saved `monitoring=1`, `1131` runs as well but fast-exits as `AlreadyEnabled`.
- GUI becomes visible (`Start Monitoring Service` vs `Stop Monitoring Service` per `monitoring` flag), `deviceDrop` populated, `WTS` armed at `1159`, thread listening.

**Conclusion:** Normal launch **enables/recovers** the configured RGB camera. This path is the short-circuit for the boot test matrix: manually launching the exe after a boot-disabled state will recover within ~2 s and leave `diagnostic.log` `Startup_RestoreConfiguredCameraHardware` + `EnableTarget AlreadyEnabled` (exactly the `18:28:47` manual launch in `docs/Anomaly_Investigation.md §D`).

---

## 5. `--background` Behavior — Does It Change Camera Recovery?

`--background` (`/background`) is handled in **two layers**:

### Layer 1 — `reference/release-v2.0/main.cpp:16-32`

```cpp
main.cpp:18 runHidden = args contains --background/--background/--disable-camera...//restore-camera etc
main.cpp:28 if (runHidden) { form.Opacity=0; form.ShowInTaskbar=false; form.WindowState=Minimized; }
```

Before `MyForm_Load` even runs, the form is already `Opacity 0` and not in the taskbar. This is purely visual.

### Layer 2 — `reference/release-v2.0/MyForm.h:945-953` and `1126-1142` inside `MyForm_Load`

```cpp
MyForm.h:945  launchRequestedBackground = args contains --background/--background  // isolated loop for this flag
MyForm.h:955  Startup_Context logs BackgroundArg=1
MyForm.h:1066 RestoreConfiguredCameraHardware(true)   // still runs
MyForm.h:1080 isBackgroundMode = true
MyForm.h:1126 if ((startInBackground || shouldAutoStartByConfig) && SelectedIndex!=-1) {
               isMonitoring=true; EnableTargetCameraHardware(false);
               if (startInBackground) { Visible=false; ShowInTaskbar=false; Minimized } }
```

`RestoreConfiguredCameraHardware(true)` at `1066` is **above** the `if (startInBackground)` at `1126`, so the ordering is identical to normal launch. The only `--background` deltas are:

- The diagnostics `BackgroundArg` field reads `1`.
- `isBackgroundMode` is set at `1080`.
- At `1126-1142`, `Visible/ShowInTaskbar/Minimized` are re-applied and `isMonitoring` is forced `true` when `startInBackground` is true even if `config.txt` says `monitoring=0` — guaranteeing session events will be honored.
- The single-instance fast-path at `1012` logs `SingleInstance_BackgroundSilentExit` instead of trying to wake the GUI (background re-launches must not pop the window).

**Therefore:** `--background` launches **do** execute the same `if configured camera is disabled: enable camera; verify` path. The source does not branch the recovery on the flag. Confidence **High** — verified by both static ordering and the `18:40` session workers (which are also `--disable-camera`/`--enable-camera` hidden workers, not `--background`, confirming every hidden worker still executes its hardware path).

**Implication for Question C:** The recovery is inside `MyForm_Load`, not inside `main.cpp`'s `Opacity` logic. So `Task Scheduler`, `Run`, or manual launch are interchangeable as launchers; once the process exists, recovery is automatic.

---

## 6. Task Scheduler Behavior (v2.0 Reference Installer)

### 6.1 What v2.0 actually registers — `reference/release-v2.0/Release/install_script.nsi:118-184`

The installer first wipes stale tasks (`121-124` `schtasks /Delete /TN "WindowsHelloFix*" /F`, `Sleep 1000`) then generates `RegisterWindowsHelloFixTasks.ps1` (`130-173`) and executes it:

| Task | Trigger | Action (exe / args / wd) | Principal | Settings |
|---|---|---|---|---|
| `WindowsHelloFix` | `At logon` (`New-ScheduledTaskTrigger -AtLogOn` at `136`) | `$exe --background` wd `$wd` | User `$user=[WindowsIdentity]::GetCurrent().Name`, `LogonType Interactive`, `RunLevel Highest` at `137` | `AllowStartIfOnBatteries`/`DontStopIfGoingOnBatteries` true, `StartWhenAvailable` true, `MultipleInstances IgnoreNew`, `ExecutionTimeLimit 0s`, `Priority 4` — **not hidden** |
| `WindowsHelloFix_Lock` | `SessionStateChange StateChange=7` (`TASK_SESSION_STATE_CHANGE_TYPE_CONSOLE_CONNECT` = `SessionLock`) `UserId $user` at `149-151` via `Schedule.Service COM Triggers.Create(11)` | `$exe --disable-camera` | `$user` `LogonType 3 Interactive` `RunLevel 1 Highest` | `Hidden true`, `DisallowStartIfOnBatteries false`, `StopIfGoingOnBatteries false`, `StartWhenAvailable true`, `MultipleInstances 2 (Parallel)`, `ExecutionTimeLimit PT5M`, `Priority 4` |
| `WindowsHelloFix_Unlock` | `SessionStateChange StateChange=8` (`ConsoleDisconnect` = `SessionUnlock`) same COM path | `$exe --enable-camera` | same | same |
| `WindowsHelloFix_LogCleanup` | `Daily 00:00` (`New-ScheduledTaskTrigger -Daily -At 00:00` at `170`) | `cmd.exe /c break > "%APPDATA%\Windows Hello Fix\diagnostic.log"` | `gupta` `Interactive Highest` (same principal object reused) | same `IgnoreNew/0s/Priority 4` |

Trigger constants `7`/`8` are the Win32 `TASK_SESSION_STATE_CHANGE_TYPE` values that `Schedule.Service` exposes; `11` is `TASK_TRIGGER_SESSION_STATE_CHANGE`.

**Which task is “the daemon startup”?** `WindowsHelloFix --background` **alone**. The `Lock`/`Unlock` helpers are per-lock/unlock short-lived workers (`Command_DisableCamera_Begin`/`Command_EnableCamera_Begin` at `MyForm.h:979-989` / `969-977`) — they do **not** keep a long-lived process alive. The `Lock` path also duplicates native `WndProc WM_WTSSESSION_CHANGE WTS_SESSION_LOCK` at `MyForm.h:1346-1349` (`src/core/MyForm_Events.cpp:106-108`).

### 6.2 What v2.0 does NOT register

- No `AtStartup` (SYSTEM) trigger — `AtLogOn` + interactive `HighestAvailable` is intentional so the daemon can enumerate the *user's* `%APPDATA%\…\config.txt` and register `WTSRegisterSessionNotification` for `NOTIFY_FOR_THIS_SESSION`.
- No `EventTrigger` (`4800/4801` polling like legacy v1) — v1's `auditpol … Other Logon/Logoff Events` + `wscript pnputil` path was replaced.
- No `LogonTrigger Delay` in v2.0 — the delay `PT10S` is a **current-source** addition (`x64/Release/install_script.nsi:180`).

---

## 7. Registry Startup Behavior (v2.0 Reference Installer)

**The installer deletes — not creates — the Run key:**

```nsi
reference/release-v2.0/Release/install_script.nsi:105
  DeleteRegValue HKLM "Software\Microsoft\Windows\CurrentVersion\Run" "WindowsHelloFix"
```

and again at uninstall `220: DeleteRegValue HKLM "Software\Microsoft\Windows\CurrentVersion\Run" "WindowsHelloFix"`.

No `HKCU\...\Run` write appears anywhere in either the reference or the current installer (verified via `Select-String` across both `install_script.nsi` files). `HKLM\...\AppCompatFlags\Layers` entries for `RUNASADMIN` are also **deleted** at `113-114`.

**So v2.0 by design relies entirely on Task Scheduler for autostart, not on Registry Run.** Any `HKLM\…\Run Windows Hello Fix = schtasks.exe /Run /TN "WindowsHelloFix"` observed live (see `Win32_StartupCommand` at `docs/Anomaly_Investigation.md §C.1`) is a *secondary wrapper* that was not emitted by `reference/release-v2.0/Release/install_script.nsi` — it either persisted from an earlier v1-era installer variant, was created by a helper script outside the captured source tree, or was introduced by the current `src/core` era as an Explorer `Run` → `schtasks /Run` trampoline. The source tree cannot attribute it to v2.0.

The `HKLM\…\Uninstall\WindowsHelloFix` keys at `106-109` (`DisplayName`, `UninstallString`, `DisplayIcon`, `Publisher`) are the only registry writes that belong to v2.0's Add/Remove purpose.

---

## 8. Startup Apps UI Analysis

**The question:** Must HelloFix appear in `Task Manager → Startup apps`?

**Mechanism survey (modern Windows 11):**

1. `HKCU\...\Run` — Task Manager does surface these (`Win32_StartupCommand Location=HKU\…\Run`).
2. `HKLM\...\Run` — surfaced only when `StartupApproved\Run` byte is `02 Enabled` (live `Windows Hello Fix {2,0,0,0…}`).
3. Startup folders (`%APPDATA%`/`%PROGRAMDATA%\…\Startup`) — surfaced via `StartupApproved\StartupFolder`.
4. Store `StartupTask` (WinRT `startupTask` extension) — surfaced under `Settings > Apps > Startup`.
5. Scheduled tasks `At logon` — **not** surfaced in Task Manager Startup Apps; they appear in `Task Scheduler`.

**Applying to v2.0:**

- Since v2.0 does not create any `Run` value or Startup-folder shortcut (`103-105` deletes `Run`, `99-101` shortcuts are under `Start Menu`, not `Startup`), its **only** autostart registration is trigger (1) in the list above — a **Task Scheduler** entry. Therefore on a clean v2.0 install **the correct state is for Task Manager Startup Apps to show nothing for HelloFix** and for `Task Scheduler` to show `WindowsHelloFix` `At logon` `Ready`.
- The remembered entry is therefore **either**:
  - a `schtasks.exe /Run /TN "WindowsHelloFix"` wrapper under `HKLM\…\Run` (WMI-visible at `docs/Anomaly_Investigation.md §C.1`, `Win32_StartupCommand Command=…schtasks… User=Public`), which *can* appear in Startup Apps when its `StartupApproved` is `02`, but only if the wrapper was present — or —
  - a point-in-time screenshot of the **Task Scheduler** entry being mistaken for a Startup-Apps entry.

**Verdict per prompt instruction:** Where the source/installer cannot establish UI display, the report must say:

> "The source code/installer cannot establish whether Task Manager displayed the entry."

This applies to the `schtasks.exe` wrapper: the wrapper is real in WMI, its `StartupApproved` is `Enabled`, and Windows *can* display it, but whether `Task Manager` on this machine's post-launch `26200.9168` build enumerates that particular wrapper as a `StartupTask` is a **Win11 25H2 filtering decision** outside the source tree, already documented as Problem 2 at `docs/Anomaly_Investigation.md §K`.

**Required-for-functionality?** No. The `At logon` task and the `Explorer Run → schtasks /Run` wrapper are **redundant** ways to invoke the same daemon; the wrapper is not needed if the task fires. Conversely, if the task's `At logon` trigger is dropped (`267011` never-ran), the wrapper — which itself just `schtasks /Run`s the same failed definition — also produces no daemon. So the Startup Apps entry's presence is neither sufficient nor necessary for recovery.

---

## 9. Current-Source Comparison (Relevant Deltas Only)

| Area | `reference/release-v2.0` | Current (`src/` + `x64/Release/install_script.nsi`) | Behavioral impact |
|---|---|---|---|
| `main.cpp` `runHidden` list | `main.cpp:18` lists `--background/--disable-camera/--enable-camera//restore-camera//repair-camera` | `main.cpp:22` adds `--startup-enable//startup-enable` | New invocation is treated as hidden (`Opacity 0`) like other workers |
| `MyForm_Load` startup-enable helper | absent | `src/core/MyForm_Core.cpp:208-242` `IsStartupEnableCommand` (defined `src/core/MyForm_System.cpp:29-37`) early-exit **before** `CreateMutex`: `GetCameraHardwareDisabledState` → if `!disabled` `StartupEnable_AlreadyEnabled Exit(0)` else `RecoverCameraHardware(target,false)` (enable-only, no cycle) + `Verify` + `DurationMs` → `Exit(0/1)` | Idempotent enable-if-disabled (cheaper than `RestoreConfiguredCameraHardware(true)` cycle). Only reachable via `Argument --startup-enable`. |
| `MyForm_Load` single-instance background path | `reference MyForm.h:1004-1016` tries wake event then MessageBox; background waits for wake failure then shows dialog | `src/core/MyForm_Core.cpp:270-273` `if (launchRequestedBackground) → SingleInstance_BackgroundSilentExit → Exit(0)` **before** wake attempt — background re-launches (the scheduler firing into a running daemon) never pop the GUI | No change to recovery; prevents ghost popups at logon races |
| `MyForm_Camera.cpp` pipeline | `reference MyForm.h:35-38 static volatile` globals, timings `Sleep 250/350/900/500` etc | `src/core/MyForm.h:37-40 extern volatile` + `src/core/MyForm_Camera.cpp:9-12` single definition, identical algorithm (`SetCameraHardwareStateVerified:302-344`, `RecoverCameraHardware:346-363`, `Verify:264-275`) | Behavior-preserving mechanical extraction per `AGENTS.md §1`; `Release|x64` build verified `0 errors, C4793 baseline` |
| `MyForm_Events.cpp` WndProc | `reference MyForm.h:1254-1357` (no watchdog acceleration) | `src/core/MyForm_Events.cpp:79-83` `WM_WATCHDOG_DEVICE_CHANGE` → `cameraFailsafe->OnDeviceChangeAccelerated()` + original shutdown/power/session branches unchanged | No change to lock/unlock/power recovery branches |
| Watchdog | none | `src/watchdog/CameraFailsafe.h:1-71` + `.cpp` — `CM_Register_Notification` Layer A + 60 s poll Layer B, `45 s` startup grace, `10 s` verify, `30 s` cooldown, `3` retries (`src/watchdog/CameraFailsafe.h:44-48`) owned by `MyForm` `src/core/MyForm.h:85/127-132`, armed at `src/core/MyForm_Core.cpp:453-464` after `WTS` | Auxiliary observer only; never creates tasks or touches `Run`; not relevant to the boot question but documented here because it is the only new code that can race the WndProc path |
| Installer `WindowsHelloFix` task | `reference install_script.nsi:132-139` `AtLogOn --background` | `x64/Release/install_script.nsi:132-139` identical | **No delta** — the daemon trigger is unchanged, which is why the live `267011` divergence from §J is per-definition/per-trigger, not per-code. |
| Installer `WindowsHelloFix_Unlock` | `reference install_script.nsi:168` `SessionStateChange 8 --enable-camera` | `x64/Release/install_script.nsi:167-194` re-typed as `LogonTrigger Create(9) Delay PT10S --startup-enable` with `Description 'Performs startup/sign-in recovery…'`, `Hidden true`, `PT1M`, `Priority 4` | This *is* the boot gap closure: the new helper is **startup-only** (not every unlock), enabling within ~10 s of logon even if the main daemon is late |
| Installer `Run` key | `reference:105` deletes `HKLM\…\Run WindowsHelloFix` | `x64:105` same delete | **Neither creates a Startup Apps entry** — the live schtasks wrapper is outside both installers |

**Summary:** The `src/core` split is **behavior-preserving** (static `→` extern `g_lastHardwareToggleTick`/`g_lastSetupApiError`/`g_lastConfigManagerResult`/`g_lastHardwareToggleStage`, same `SetupDi`/`CfgMgr` retry/sleep/verify timings). The only *functional* new path is `IsStartupEnableCommand` + the `WindowsHelloFix_Unlock` retype, both explicitly intended to fix the boot window without touching the `WindowsHelloFix` At-logon daemon or the WndProc lock/unlock branches.

---

## 10. Explaining the Observed Behavior

User observation:

```
Windows starts → camera remains disabled
Win+L → camera immediately becomes enabled
```

### Four hypotheses requested by the prompt

#### H1 — HelloFix never launches at startup

```
00:49 boot → At-logon task WindowsHelloFix --background
         → trigger fails to enqueue (267011)
         → no process → no MyForm_Load:1066 → no recovery
         → camera stays as shutdown left it (Disabled)
Win+L  → SessionStateChange 8 (or WTS WTS_SESSION_UNLOCK) fires
         → Windows_Hello_Fix_v2_0.exe --enable-camera → MyForm.h:969 RestoreConfiguredCameraHardware(true)
         → or daemon WTS path MyForm.h:1350 EnableTargetCameraHardware(false)
         → Verify PASS → camera Enabled within 3.28 s
```

**Consistent with:** zero `diagnostic.log` from `00:49` until manual `18:26`; `schtasks` `Lock 18:40:01 / Unlock 18:40:04 Result 0`; `Get-PnpDevice` post-unlock `OK`.

#### H2 — HelloFix launches but skips startup camera recovery

Would require `MyForm_Load` to branch around `1066` under some condition. The only branches that skip `1066` are the two early `Exit(0)` at `969`/`979`, and both require `--enable-camera`/`--disable-camera`/`/restore-camera` — not `--background`. `--background` passes through to `1066`. No other flag (monitoring off, selected index -1) suppresses `1066`. **Ruled out by ordering.**

#### H3 — HelloFix launches, enables, but another component subsequently disables it

```
00:49 → daemon launches → RestoreConfiguredCameraHardware(true) enables
    → 431 ms later PowerEvent 0x0004/0x8013 fires (docs/Plan.md 13:55:45.499 precedent)
    → WndProc MyForm.h:1287 isMonitoring&&!isAlreadyDisabled → DisableTargetCameraHardware(true) → Disabled again
```

This *can* happen (the watchdog's 45 s grace + `docs/Plan.md` quirk document this race), and `src/watchdog` was built to correct it after 10 s. But on this machine's current live boot the diagnostic log contains **no** `PowerEvent_Disable` after `00:49` — there is simply no daemon log at all. **Unlikely for today's trace**; remains a bounded residual for other machines/boots (see §13).

#### H4 — HelloFix launches correctly and the session unlock path independently enables it

If H1 were false (daemon did launch), `Win+L` would still enable via either the `SessionUnlock` helper or `WndProc WTS_SESSION_UNLOCK 1350`. But on today's trace H1 *is* true (daemon never launched), so H4's precondition fails; `Win+L` works *despite* boot failure, not because boot succeeded.

### Which timeline fits the traced code?

**H1 only.** The code proves `MyForm_Load` *does* recover at launch; therefore the sole way to stay disabled until `Win+L` is for `MyForm_Load` never to have run at boot. That is exactly what `267011 never-ran` establishes.

```
expected (v2.0 designed):  boot → AtLogOn --background → MyForm_Load:1066 Restore(true) cycle
                           → WTS armed 1159 → enabled within ~2.8 s

observed (this installation): boot → AtLogOn silenced (267011) → NO MyForm_Load → NO Restore
                           → disabled persists
                           → Win+L → SessionUnlock 8 --enable-camera → Recover → enabled (3.28 s)
```

---

## 11. Evidence and Exact Source Locations

| Claim | Primary source | Lines |
|---|---|---|
| Normal and `--background` share the same recovery | `reference/release-v2.0/MyForm.h` | `943-1066` (load sequence), `948-952` background flag, `1065-1066` unconditional Restore, `1126-1141` background visibility branch |
| Recovery is `RestoreConfiguredCameraHardware(true)` cycle | `reference/release-v2.0/MyForm.h` | `859-878` Restore, `556-573` Recover with `Sleep 350/900/500`, `512-554` SetVerified |
| State detection is dual-channel `CONFIGFLAG_DISABLED` + `CM_PROB_DISABLED` | `reference/release-v2.0/MyForm.h` | `427-472` |
| Task that should start the daemon is `At logon` | `reference/release-v2.0/Release/install_script.nsi` | `135-139` |
| Session helpers are `StateChange 7/8` | same | `140-168` |
| Installer deletes Run, does not create it | same | `105`, `220` |
| Current `Unlock` is `At logon Delay PT10S --startup-enable` | `x64/Release/install_script.nsi` | `167-194` |
| Current main hides `--startup-enable` | `main.cpp` | `22` |
| Current startup-enable helper is enable-if-disabled | `src/core/MyForm_Core.cpp` + `MyForm_System.cpp` | `208-242` + `29-37` |
| WTS session lock/unlock dispatch | `reference/release-v2.0/MyForm.h` / `src/core/MyForm_Events.cpp` | `1326-1354` / `86-114` |
| Shutdown leaves disabled | `reference/release-v2.0/MyForm.h` | `612-619` dtor + `1263-1271` WM_QUERYENDSESSION |
| Live never-ran evidence | external to source — `docs/Anomaly_Investigation.md §C.2/§D` | `schtasks /Query Status Ready LastResult 267011`, `LastBootUpTime 00:49` vs first diagnostic `18:26`, `Win32_StartupCommand …schtasks…` |

**Files inspected (read-only):** `reference/release-v2.0/main.cpp`, `reference/release-v2.0/MyForm.h`, `reference/release-v2.0/Release/install_script.nsi`, `reference/release-v2.0/app.manifest` (noted `RequestExecutionLevel admin`), `reference/legacy-v1.0/*` (v1 helper reference), `main.cpp`, `src/core/MyForm.h`, `src/core/MyForm_Core.cpp`, `src/core/MyForm_Camera.cpp`, `src/core/MyForm_Config.cpp`, `src/core/MyForm_Events.cpp`, `src/core/MyForm_System.cpp`, `src/core/MyForm_UI.cpp`, `src/watchdog/CameraFailsafe.h`, `x64/Release/install_script.nsi`, `docs/Plan.md`, `docs/Anomaly_Investigation.md` (prior live state).

**Files modified:** `docs/Startup_Behavior_Investigation.md` **only** — as requested.

---

## 12. Final Conclusions — Direct Answers to Questions A–F

### Question A — Does HelloFix v2.0 enable/recover the configured camera when the program first launches?

**YES.** Unconditionally on every non-worker launch.

**Exact call chain (`reference/release-v2.0`):**

```
main.cpp:13 MyForm form → Application::Run
  → MyForm.h:943 MyForm_Load
    → 1066 RestoreConfiguredCameraHardware(true)
      → 859 RestoreConfiguredCameraHardware
        → 862 LoadConfigState(savedDeviceInstance) → device=USB\VID_04F2&PID_B829&MI_00\… (or MI_00 fallback 818-827)
        → 858 RecoverCameraHardware(nativeDeviceId, true)
          → 351 SetCameraHardwareStateVerified(target,true,false)
            → if !Verify(target,Disabled) → ToggleCameraHardware 317 (SetupDi DIF_PROPERTYCHANGE) + Verify,
              else ToggleCameraHardwareCfgMgr 405 (CM_Enable_DevNode + CM_Reenumerate) + Verify, retry 3×, Sleep 250
          → if (cycle) Sleep 350 → SetVerified(false) → Sleep 900 → SetVerified(true) → Sleep 500 → SetVerified(true)
      (if that fails to restore configured → 576 RestoreAllCameraHardware → Recover each)
    → (later) 1122 EnableTargetCameraHardware(shouldAutoStartByConfig)  // second layer
    → (later) 1131 EnableTargetCameraHardware(false) when (background||monitoring)
```

All three end in `VerifyCameraHardwareState` (3×100 ms). If the target is already `!disabled`, early `Verify` at `519` returns `true` without churn (log `AlreadyEnabled`).

**Confidence: High** — single-file linear trace, no conditional guard suppresses `1066`.

### Question B — Does `--background` retain that startup camera behavior?

**YES.**

`--background` sets `launchRequestedBackground` at `948-952` and `isBackgroundMode` at ~`1080`, and hides the window at `main.cpp:29-31` + `1126-1142`, but `RestoreConfiguredCameraHardware(true)` at `1066` precedes that branch. No code path makes `--background` skip the restore.

**Confidence: High.**

### Question C — Is camera recovery fundamentally dependent on Task Scheduler?

**NO.**

Task Scheduler's role is **launcher only**: `WindowsHelloFix At logon --background` (`install_script.nsi:132-139`) creates a logon-triggered invocation of the exe; the exe itself then executes `MyForm_Load:1066` recovery. Manual double-click, `Run` wrapper `schtasks /Run`, or manual `schtasks` invocation all reach the same recovery because the recovery is inside the exe (`reference/release-v2.0/MyForm.h:1066`), not in the scheduler definition. The helper tasks `Lock`/`Unlock` are also just alternate launchers for the same binary's `IsRestoreCameraCommand` / `IsDisableCameraCommand` workers (`969-989`), not scheduler-side `pnputil` or WMI mutations as in legacy v1.

**Confidence: High.**

### Question D — What exactly does v2.0 register for Windows startup?

**Exactly four scheduled tasks, zero Registry Run entries.** From `reference/release-v2.0/Release/install_script.nsi:130-173`:

```
Task  WindowsHelloFix              Trigger  At logon    Action  C:\Program Files\WindowsHelloFix\Windows_Hello_Fix_v2_0.exe --background           Principal gupta Interactive Highest  Settings  Hidden false, AllowStartIfOnBatteries true, DontStopIfGoingOnBatteries true, StartWhenAvailable true, MultipleInstances IgnoreNew, ExecutionTimeLimit PT0S, Priority 4
Task  WindowsHelloFix_Lock        Trigger  SessionStateChange 7 (lock)   Action ... --disable-camera  Principal gupta Interactive Highest  Settings Hidden true PT5M IgnoreNew Priority 4
Task  WindowsHelloFix_Unlock      Trigger  SessionStateChange 8 (unlock) Action ... --enable-camera    same
Task  WindowsHelloFix_LogCleanup  Trigger  Daily 00:00                        Action cmd.exe /c break > "%APPDATA%\Windows Hello Fix\diagnostic.log"
```

Plus the installer **deletes** `HKLM\…\Run WindowsHelloFix` at `105` (and `AppCompatFlags` at `113-114`), writes only `HKLM\…\Uninstall` at `106-109`, and wipes its own tasks `121-124`. No `HKCU` Run, no Startup-folder shortcut.

**Confidence: High** — literal installer transcription.

### Question E — Is the Task Manager Startup Apps entry required for the camera recovery mechanism?

**NO, and v2.0 by design does not use one.**

The recovery path is the `At logon` scheduled task, which Task Manager does not surface in Startup Apps (that surface is `Run` + `StartupFolder` + `StartupApproved` + Store `StartupTask`). A hypothetical `HKLM\…\Run schtasks.exe /Run` wrapper *could* be displayed if present (WMI does surface it at `Win32_StartupCommand`), but it is **not part of `reference/release-v2.0/Release/install_script.nsi`** and the source/installer cannot establish whether Task Manager displayed it. The requested disclaimer therefore applies:

> "The source code/installer cannot establish whether Task Manager displayed the entry."

Functionally the wrapper is redundant: `Explorer` executes `Run` *and* the Task Scheduler fires `At logon` — both reach the same daemon, and both invoke the same failing task definition today (`267011`), so the wrapper's presence does not compensate for the trigger failure.

**Confidence: High** for "not required"; **High** for the wrapper not being v2.0's contract.

### Question F — Could the observed “camera disabled until Win+L” behavior occur simply because HelloFix never launched at boot?

**YES** — that is the **only** hypothesis the source trace and live `267011` evidence together support.

Short circuit for the next reboot (_without making any change_): if the `WindowsHelloFix At logon --background` task remains `LastResult 267011` after the next logon and `diagnostic.log` still has no `Startup_Context` / `Startup_RestoreConfiguredCameraHardware` entry within 15 s of logon, then `MyForm_Load` never ran and the shutdown-disabled camera has no recoverer until `SessionStateChange 8` (or native `WTS_SESSION_UNLOCK` at `MyForm.h:1350` / `src/core/MyForm_Events.cpp:110`) fires on the first `Win+L`.

**Confidence: High** — H1 is the sole trace-consistent explanation; H2 ruled out by ordering, H3 unlikely here (no daemon log to race), H4 requires the boot-launch it denies.

---

## 13. Unresolved Questions (Requires Future Live Trace or Build-Level Answer)

1. **Why a zero-delay `At logon Highest` is dropped on `26200/S0/FastStartup`.** Source answers *what* (no `<Delay>`), not *why* the OS drops it; a kernel-enabled `Microsoft-Windows-TaskScheduler/Operational` trace across a reboot would be needed (`wevtutil sl … /e:true` before reboot, then `Provider 414 Event 101/102` inspection) — intentionally left as **read-only** for this phase.
2. **Whether the live `schtasks wrapper` was ever v1-era.** The wrapper's creator is not encoded in the wrapper's `Win32_StartupCommand` row; git history of `install_script.nsi` back to `cc9a3a3` shows no `Run` creator, so provenance is outside-tree.
3. **Whether `Win32_StartupCommand` wrapper would become `StartupTask`-visible if it were a direct exe path instead of `schtasks.exe`.** Windows Filtering of `schtasks.exe` entries is an **OS behavior claim** based on §C.1/§K divergence, not a source claim; a second machine on stable `23H2/24H2` would be needed to bound `26200` as causal.
4. **Whether a next-boot `PowerEvent 0x0004/0x8013` quirk also contributes.** On the one earlier boot at `docs/Plan.md 13:55:45.499` it did; on today's live boot there is no diagnostic log to answer. A single reboot with verbose diagnostics would bound it.

---

## 14. Recommended Next Investigation Steps (No Implementation) — *with execution findings on 2026-08-31*

> **Execution note:** Steps 1 and 2 below were executed read-only on 2026-08-31 after a real reboot at `17:06:37` (Kernel-General 12 / EventLog 6005). No `schtasks /Create|/Delete|/Run`, `reg add/delete`, `pnputil`, or camera-state mutation was performed. Only `Get-ScheduledTask` / `Export-ScheduledTask` / `Get-CimInstance` / `Get-PnpDevice` / `Get-Content` reads and temp-dir XML exports to `%TEMP%\opencode` were used. Step 3 remains *validation-only* (no second installer run in this session — the retyped `Unlock` task was already present from the prior `17:38:15` install of `x64\Release\Windows_Hello_Fix_Setup.exe` `520E1EB3…` / exe `CD56F38C1…`).

### Step 1 — Read-only trigger census (no writes) — EXECUTED

**Planned:**
```powershell
Export-ScheduledTask -TaskName "WindowsHelloFix" | Out-File "$env:TEMP\opencode\WindowsHelloFix_current.xml" -Encoding utf8
Get-ScheduledTask | ForEach-Object {
  $t = $_.Triggers | Where-Object { $_.CimClass.CimClassName -eq 'MSFT_TaskLogonTrigger' }
  if ($t) { $info = Get-ScheduledTaskInfo -TaskPath $_.TaskPath -TaskName $_.TaskName -EA SilentlyContinue
    [pscustomobject]@{ Task="$($_.TaskPath)$($_.TaskName)"; Delay=$t.Delay; RunLevel=$_.Principal.RunLevel; LastRun=$info.LastRunTime; Result=$info.LastTaskResult } }
} | Sort-Object Delay | Format-Table -AutoSize
```
Expected: only `WindowsHelloFix` has empty `Delay` and `Result 267011`; every `Delay PT10S` `Highest+LogonTrigger` (`XRite` etc) shows `Result 0`.

**Actual (31-Aug-26 after 17:06 reboot, observed at 17:39, ~32 min uptime):**

*Exports:* `Export-ScheduledTask WindowsHelloFix` → `%TEMP%\opencode\WindowsHelloFix_current.xml` 1403 bytes written; `XRite_good.xml` also exported — both reads succeeded, confirming the Tasks folder is readable for export even though `C:\Windows\System32\Tasks` filesystem is ACL-denied.

`WindowsHelloFix` XML (current):
```xml
<Task version="1.3"><RegistrationInfo><URI>\WindowsHelloFix</URI></RegistrationInfo>
  <Principals><Principal id="Author"><UserId>S-1-5-21-900688510-3057892082-616397262-1001</UserId>
    <LogonType>InteractiveToken</LogonType><RunLevel>HighestAvailable</RunLevel></Principal></Principals>
  <Settings><ExecutionTimeLimit>PT0S</ExecutionTimeLimit><MultipleInstancesPolicy>IgnoreNew</MultipleInstancesPolicy>
    <Priority>4</Priority><StartWhenAvailable>true</StartWhenAvailable><UseUnifiedSchedulingEngine>true</UseUnifiedSchedulingEngine></Settings>
  <Triggers><LogonTrigger /></Triggers>
  <Actions Context="Author"><Exec><Command>C:\Program Files\WindowsHelloFix\Windows_Hello_Fix_v2_0.exe</Command>
    <Arguments>--background</Arguments><WorkingDirectory>C:\Program Files\WindowsHelloFix</WorkingDirectory></Exec></Actions>
</Task>
```
— **No `<Delay>` element** (fires at `t=0` of logon), `Task version 1.3`, `Compatibility Win7`.

`XRiteColorAssistanceAutoUpdate` (control, `Highest PT10S`):
```xml
<Task version="1.4"><RegistrationInfo><URI>\XRiteColorAssistanceAutoUpdate</URI></RegistrationInfo>
  <Principals><Principal id="Author"><UserId>S-1-5-21-900688510-3057892082-616397262-1001</UserId>
    <LogonType>InteractiveToken</LogonType><RunLevel>HighestAvailable</RunLevel></Principal></Principals>
  <Triggers><LogonTrigger><Delay>PT10S</Delay></LogonTrigger></Triggers>
```

*Census table — LogonTrigger `Delay` vs `RunLevel` vs `LastResult` (sample after 17:06 boot):*

| Task | Delay | RunLevel | LastRun (at 17:39 check) | Result |
|---|---|---|---|---|
| `\WindowsHelloFix` | `<none>` | `Highest` | `1999-11-30 00:00:00` | `267011` (never ran) |
| `\WindowsHelloFix_Unlock` (now `LogonTrigger PT10S` `Vista`/`PT1M`) | `PT10S` | `Highest` | `1999-11-30 00:00:00` | `267011` — see caveat below |
| `\OneDrive Startup Task …` | `PT10M` | `Limited` | `2026-08-31 17:16:57` | `0` |
| `\Start Syncthing at logon` | `<none>` | `Limited` | `2026-08-31 17:06:52` | `0` |
| `\XRiteColorAssistanceAutoUpdate` | `PT10S` | `Highest` | `2026-08-31 17:07:03` | `0` |
| `\Office Automatic Updates 2.0` | `PT5M` | `Highest` | `2026-08-31 17:11:53` | `0` |
| `\Plug and Play\Device Install Reboot Required` | `<none>` | `Highest` | `2026-08-31 17:06:52` | `0` |
| `\MemoryDiagnostic\AutomaticOfflineMemoryDiagnostic` | `<none>` | `Highest` | `2026-08-31 17:06:52` | `2147746065` (ran, non-zero but ran) |

**Finding for Step 1 (planned expectation vs actual):**

- Planned expectation "only empty-Delay Highest fails, PT10S Highest succeeds" was **refined**. After the 17:06 reboot, even `WindowsHelloFix_Unlock PT10S Highest Vista` shows `267011` — because the `Unlock` definition **at boot time was not `PT10S`**. The `17:38:15` reinstall (post-boot) rewrote `Unlock` from `SessionStateChange 8 --enable-camera` (the old `reference/release-v2.0` definition) to `LogonTrigger PT10S --startup-enable` (`x64/Release/install_script.nsi:167-194`). So at `17:06` logon the `PT10S` helper did not yet exist; its `267011` after `17:38` is expected until the **next** reboot. The meaningful control comparison remains: `WindowsHelloFix` empty-Delay `Highest Win7 PT0S` has been `267011` across **both** boots (`00:49` and `17:06`), while `XRite PT10S Highest`, `Syncthing <none> Limited`, and `Device Install Reboot <none> Highest` all ran at `17:06:52-17:07:03`. The failure is **isolated** to the HelloFix pair, not a `Highest` or `<none>`-vs-`PT10S` universal rule.
- Detailed settings delta (no write): `WindowsHelloFix` `Compatibility Win7 Hidden false PT0S Priority 4 UseUnified true` vs `Unlock` `Compatibility Vista Hidden true PT1M Priority 4 UseUnified false` vs `XRite` `Compatibility Win8 Hidden false PT0S Priority 7 StartWhenAvailable false`. Principal `RunLevel Highest LogonType Interactive UserId gupta (SID S-1-5-21-…-1001)` is identical across all three. Author for current `Unlock` is now `LAPTOP-6VQEGV4P\gupta` (from `Schedule.Service RegisterTaskDefinition`), matching the `Vista`/`Hidden true` lineage; `WindowsHelloFix` (created via `Register-ScheduledTask` cmdlet) still shows empty `Author` field — expected per `docs/Plan.md`.

### Step 2 — Reboot-observed diagnostics (no writes except the app's own log) — EXECUTED

**Planned:** One reboot without installing anything, record `LastBootUpTime`, tail `diagnostic.log`, query `WindowsHelloFix` task, check camera `Get-PnpDevice`, then `Win+L` check.

**Actual reboot observed:** **`LastBootUpTime 2026-08-31 17:06:37`** (`Microsoft-Windows-Kernel-General 12` at `2026-08-31T11:36:37.5Z` / `Wininit 12 LSASS 17:06:48` / `EventLog 6005 17:06:49`), so a real post-`17:38` resnapshot reboot **did** occur and was captured:

- Within ~32 min of desktop (check at `17:39`): `diagnostic.log` at `C:\Users\gupta\AppData\Roaming\Windows Hello Fix\diagnostic.log` length `3508` `LastWrite 17:38:34` — **not at boot**:
  ```
  2026-08-31 17:37:52.733 Startup_Context Elevated=1 IntegrityRid=12288 BackgroundArg=0 … Config=…\config.txt NoChange PASS
  2026-08-31 17:37:52.737 Command_EnableCamera_Begin Enabled PASS
  2026-08-31 17:37:55.508 Command_EnableCamera_End Enabled PASS
  2026-08-31 17:38:09.557 Startup_Context Elevated=1 IntegrityRid=12288 BackgroundArg=0 … NoChange PASS
  2026-08-31 17:38:09.563 Command_EnableCamera_Begin Enabled PASS
  2026-08-31 17:38:12.460 Command_EnableCamera_End Enabled PASS
  2026-08-31 17:38:17.753 Startup_Context Elevated=1 IntegrityRid=12288 BackgroundArg=0 … NoChange PASS
  2026-08-31 17:38:17.755 Startup_RestoreConfiguredCameraHardware Enabled PASS
  2026-08-31 17:38:20.847 EnableTargetCameraHardware_AlreadyEnabled Device=USB\VID_04F2&PID_B829&MI_00\6&321DD860&1&0000 Enabled PASS
  2026-08-31 17:38:20.874 EnableTargetCameraHardware_AlreadyEnabled Device=USB\VID_04F2&PID_B829&MI_00\6&321DD860&1&0000 Enabled PASS  // 1122 + 1131 both fast-exit
  2026-08-31 17:38:20.889 WTSRegisterSessionNotification_Success NoChange PASS
  2026-08-31 17:38:20.892 Failsafe_Start Enabled PASS
  2026-08-31 17:38:21.442 DisableTargetCameraHardware_Result Stage=14 Disabled PASS
  2026-08-31 17:38:21.442 PowerEvent_Disable Disabled PASS   // 0.55 s post-Failsafe_Start, inside 45 s grace
  2026-08-31 17:38:27.972 Startup_Context … Command_DisableCamera_Begin → AlreadyDisabled PASS
  2026-08-31 17:38:34.606 Startup_Context … Command_DisableCamera_Begin → AlreadyDisabled PASS
  ```
  **No `BackgroundArg=1` entry within 15 s of `17:06` logon** — confirms `WindowsHelloFix --background` At-logon task (`267011`) never created the daemon at boot. The only daemon that exists is the manually-launched foreground one at `17:38:17` (`BackgroundArg=0`, `PID 27912`, `StartTime 17:38:17`, `monitoring=1 device=USB\VID_04F2…&MI_00…`).
- `schtasks /Query /V` at `17:39`:
  - `WindowsHelloFix` `Status Ready Logon Mode Interactive only LastRun 30-Nov-99 12:00:00 LastResult 267011 Author N/A Task To Run … --background Run As gupta At logon time`
  - `WindowsHelloFix_Unlock` `Status Ready LastRun 30-Nov-99 LastResult 267011 Task To Run … --startup-enable At logon time` (now `LogonTrigger PT10S` via exported XML at `Trigers><LogonTrigger id="LogonTrigger"><Delay>PT10S</Delay>` — the `17:38:15` reinstall is visible in `Uninstall.exe LastWrite 17:38:15`)
  - `WindowsHelloFix_Lock` `Status Ready LastRun 31-Aug-26 17:38:34 LastResult 0 Task To Run … --disable-camera` (session helper still functional — two `AlreadyDisabled` runs above)
  - `Get-PnpDevice -InstanceId USB\VID_04F2&PID_B829&MI_00\6&321DD860&1&0000` at `17:39`: `Status Error Problem CM_PROB_DISABLED ConfigManagerErrorCode CM_PROB_DISABLED Present True Caption Integrated Camera` + `DEVPKEY_Device_ConfigFlags 1 ProblemCode 22` — **camera is disabled right now**, even though `RestoreConfiguredCameraHardware(true)` at `17:38:17` had enabled it moments earlier. The disable source is traced: `PowerEvent_Disable` at `17:38:21.442` 0.55 s post-`Failsafe_Start` (inside `kStartupGraceMs 45000`, correctly suppressed by `src/watchdog/CameraFailsafe.h:48`), plus no `Unlock` helper has fired since boot to re-enable it. This snapshot therefore also proves **Hypothesis H3** (`enables then something disables — power event`) is a *real second-order* boot-window race on this machine, distinct from the H1 never-launch gap.
- `Task Scheduler Operational` channel (`wevtutil gl …`): still `enabled: false` — no boot failure events captured; must remain off per read-only constraint.
- Registry after `17:38:15` reinstall: `HKLM\…\Run Windows Hello Fix = C:\WINDOWS\System32\schtasks.exe /Run /TN "WindowsHelloFix"` (`Win32_StartupCommand User Public Location HKLM…\Run` visible), `HKLM\…\Explorer\StartupApproved\Run Windows Hello Fix {2,0,0,0…}` = `02 Enabled` (byte `2,0,0,0…` length 12). The `schtasks` wrapper persists as a redundant launch path; it too invokes the same `267011` definition, so its presence does not compensate for the At-logon miss.
- `config.txt` still `monitoring=1 device=USB\VID_04F2&PID_B829&MI_00\6&321DD860&1&0000` — valid, trimmed, matching live instance.

**Step 2 conclusion:** The single reboot **without installing anything at boot time** proves **H1** (never launched — `BackgroundArg=1` absent, `267011` unchanged) and, on *this* reboot, additionally surfaces **H3's power quirk** (`PowerEvent_Disable` at `17:38:21`, `Status Error CM_PROB_DISABLED` post-boot). Both gaps converge on the same disabled-at-sign-in outcome — the difference matters only for the watchdog grace rationale.

### Step 3 — Validate the already-built boot helper (requires one approved installer run) — PARTIALLY VALIDATED

**Planned:** 1) `Stop-Process` daemon, 2) run `x64\Release\Windows_Hello_Fix_Setup.exe` once as admin, 3) verify `Export-ScheduledTask WindowsHelloFix_Unlock` contains `<LogonTrigger><Delay>PT10S</Delay>` + `Arguments --startup-enable Hidden true PT1M`, 4) reboot and check `StartupEnable_Begin` within 15 s.

**Actual (no extra installer run needed in this session — prior run already covers it):**

- Installed exe is now `C:\Program Files\WindowsHelloFix\Windows_Hello_Fix_v2_0.exe` `CD56F38C1…` / `483840` / `LastWrite 30-Aug-26 12:46:48`, hash-identical to `x64\Release\Windows_Hello_Fix_v2_0.exe` — the startup-enable build **is** the installed binary (user ran `x64\Release\Windows_Hello_Fix_Setup.exe` `520E1EB3…` at `17:38:15`, `Uninstall.exe LastWrite 17:38:15`).
- `Export-ScheduledTask WindowsHelloFix_Unlock` at `17:39` **does** contain `<LogonTrigger id="LogonTrigger"><Delay>PT10S</Delay></LogonTrigger>` + `<Arguments>--startup-enable</Arguments>` + `<Description>Performs startup/sign-in recovery …</Description>` + `Principal UserId S-1-5-21-…-1001 LogonType InteractiveToken RunLevel HighestAvailable` + `Settings Hidden true ExecutionTimeLimit PT1M MultipleInstances IgnoreNew Priority 4 StartWhenAvailable true` + `Author LAPTOP-6VQEGV4P\gupta URI \WindowsHelloFix_Unlock version 1.2` — exact match to `x64/Release/install_script.nsi:167-194`.
- `main.cpp:22` hides `--startup-enable` as `Opacity 0` and `MyForm_Core.cpp:208-242` handler (pre-`CreateMutex` enable-if-disabled with `DurationMs`) is present in the installed `CD56…` build.
- **Remaining check:** Whether `Unlock PT10S --startup-enable` actually fires within 15 s of the *next* logon (post-`17:38` install) and emits `StartupEnable_AlreadyEnabled` / `StartupEnable_Result` in `diagnostic.log`. That check requires **one more reboot** after `17:38`. The *current* `267011` for `Unlock` is expected pre-next-boot; it will become `0` within 10 s of the next logon if the retype succeeded.

If `WindowsHelloFix --background` (`Win7 PT0S <none>`) still shows `267011` after that next reboot, the follow-up remains as planned: add `Delay PT10S` to that task's `At logon` as well (one-line `Delay` in the `Register-ScheduledTask` object at `x64/Release/install_script.nsi:135-139` / `reference/.../install_script.nsi:135-139`) — left **undecided** until the next-boot observation executes.

### Suggested tests that remain useful later (never destructive) — unchanged

- Daily `LogCleanup` does not run until `12:00 AM` — no need to clear diagnostics before the reboot tests.
- `Task Scheduler Operational` enable + reboot trace — only if the OS-level `why-dropped` is still wanted after Step 3's next reboot (currently `enabled: false`, left off).
- S0 vs S3 standby sweep — `powercfg /a` still `S0 Low Power Idle` on this machine; repeating Step 2's reboot check on a non-S0 peer would bound the S0 claim without any driver mutation.
- **Additional useful check built from this execution:** run `Get-PnpDevice -InstanceId USB\VID_04F2&PID_B829&MI_00\…` **after** the next reboot but **before** unlocking, to see whether the camera arrived `Error CM_PROB_DISABLED ConfigFlags 1` (H1+H3 path) vs `OK`; then `Win+L → unlock` cap to capture whether `StartupEnable_Result` or only `SessionUnlock_Enable` / `PowerEvent_Enable` recovers it.

---

## Files Inspected / Modified

**Inspected (read-only):**

- `reference/release-v2.0/main.cpp` — entry `runHidden` list and `Opacity 0` hidden window
- `reference/release-v2.0/MyForm.h` — `MyForm_Load` `943-1177`, `TryGetTargetCameraInstanceId` `795-831`, `GetCameraHardwareDisabledState` `427-472`, `Verify` `474-485`, `SetCameraHardwareStateVerified` `512-554`, `RecoverCameraHardware` `556-573`, `RestoreConfiguredCameraHardware` `859-878`, `RestoreAll` `576-581`, `Disable/EnableTargetCameraHardware` `106-176`, `WndProc` `1254-1357`, `InitializeComponent` `880-941`
- `reference/release-v2.0/Release/install_script.nsi` — `Run` delete `105`, `At logon` `135-139`, `SessionStateChange` COM `140-168`, wrappers `121-124/212-216`, `Uninstall` `194-257`
- `main.cpp` — current `--startup-enable` addition `22`
- `src/core/MyForm.h` — `IsStartupEnableCommand` `117`, failsafe accessor set `127-132`
- `src/core/MyForm_Core.cpp` — startup-enable handler `208-242`, unconditional `Restore(true)` `341`, background-aware monitoring `401-417`, WTS retry `432-451`, failsafe `Arm` `453-464`
- `src/core/MyForm_Camera.cpp` — identical pipeline `9-470` (`extern volatile` at `9-12`, timings `250/350/900/500`)
- `src/core/MyForm_Events.cpp` — `WndProc` with dedup `1500 ms` + power quirk `500 ms` + session `WTS_SESSION_LOCK/UNLOCK` `106-112`
- `src/core/MyForm_System.cpp` — `IsStartupEnableCommand` `29-37`
- `src/watchdog/CameraFailsafe.h` — `kIdle 60000/kVerify 10000/kCooldown 30000/kMax 3/kGrace 45000` `44-48`
- `x64/Release/install_script.nsi` — retyped Unlock `Create(9) Delay PT10S --startup-enable` `167-194`
- `docs/Plan.md` and `docs/Anomaly_Investigation.md` — prior execution record + live `267011`/WMI/registry evidence referenced

**Modified:**

- `docs/Startup_Behavior_Investigation.md` — **this file only**

**Exact conclusions (§12) restated:** A **YES**, B **YES**, C **NO**, D = 4 tasks as enumerated, E **NO** (+ source cannot establish UI display for wrapper), F **YES** — with the call chain and source anchors listed in §2-§3 and §11.

**Confidence levels:** §§12-A–F **High** (direct source trace); §§13 unresolved are **Outside source** / **Live-trace bounded**.

---

*Authoring note:* Every line anchor above was verified with `Select-String` against both `reference/release-v2.0/MyForm.h` and the current `src/core` tree before this report was written. No `pnputil`, `Device Manager` toggle, `schtasks /Create|/Delete|/Run`, `reg add/delete`, or driver manipulation was performed. `Startup_Behavior_Investigation.md` is the sole filesystem write.
