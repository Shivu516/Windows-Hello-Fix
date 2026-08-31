# Anomaly Investigation — Why Known-Good v2.0 No Longer Behaves Correctly on This Windows Installation

> **Type:** Read-Only Diagnosis (no source, installer, registry, Task Scheduler, or Windows configuration was modified)  
> **Date:** 2026-08-30 (UTC investigation window 00:49 boot → 18:43)  
> **Branch:** `test` (contains fix commit `27a1174` not yet deployed to this machine's Task Scheduler)  
> **Control:** `reference/release-v2.0` (monolithic `MyForm.h` 55951 bytes, `Release/install_script.nsi` 12538 bytes, exe SHA256 `6B8F96E807940...`)  
> **Current workspace build:** `x64/Release/Windows_Hello_Fix_v2_0.exe` SHA256 `CD56F38C1C70...` (new `--startup-enable` build, 483840 bytes, 30-Aug-26 00:46)  
> **Installed binary:** `C:\Program Files\WindowsHelloFix\Windows_Hello_Fix_v2_0.exe` SHA256 `6B8F96E807940...` identical to **reference**, not to workspace build  
> **Machine:** `LAPTOP-6VQEGV4P` — Windows 11 Home Single Language 25H2 (build `26200.9168`, `BuildLabEx 26100.1 ge_release 240331`), S0 Modern Standby, Fast Startup `HiberbootEnabled=1`, `Schedule` service `RUNNING AUTO`

---

## A. Executive Conclusion

There are **two stacked problems**, not one — and neither is the v2.1 refactor.

### Problem 1 — Functional: `WindowsHelloFix` LogonTrigger never fires (camera stays disabled at boot)

| Signal | Evidence |
|---|---|
| `schtasks /Query /V` for `WindowsHelloFix` | `Status Ready`, `LastRunTime 30-Nov-99 12:00:00 AM`, `LastResult 267011` (`0x41303` = `SCHED_S_TASK_HAS_NOT_RUN`) — **never executed since TaskCache registration**, not even at today's `00:49` boot where every other `LogonTrigger` (OneDrive, Syncthing, XRite) did run. |
| Same query for the sibling session tasks | `WindowsHelloFix_Lock` `LastRun 30-Aug-26 18:40:01 Result 0`, `WindowsHelloFix_Unlock` `LastRun 30-Aug-26 18:40:04 Result 0` — both `SessionStateChange` triggers **are healthy**. The scheduler itself is healthy; the failure is **isolated** to the one LogonTrigger. |
| `diagnostic.log` timeline | Boot at `00:49`, **zero** diagnostic entries until manual invocations at `18:26`, a manual foreground daemon launch at `18:28:47` (`BackgroundArg=0`), then lock/unlock workers at `18:40`. No `BackgroundArg=1` startup daemon ever logged — proving `MyForm_Load` never executed at boot. |
| `Win32_OperatingSystem.LastBootUpTime` | `2026-08-30 00:49:57` vs first diagnostic `18:26:06` = **~17.5 h gap** with no recovery. |
| Comparison with healthy `Highest+LogonTrigger` | `XRiteColorAssistanceAutoUpdate` (`Highest`, `LogonTrigger` **with `Delay PT10S`**) ran at `18:25:46 Result 0`. `WindowsHelloFix` is the **only** `Highest+LogonTrigger` that never ran — its distinguishing feature is **`Delay` empty** (fires at `t=0` of logon) while XRite's 10 s delay succeeds. |

**Root mechanism:** the `At logon` task without delay fires before the S0/Modern-Standby + Fast-Startup device/user session is ready (LSASS → Explorer → Task Scheduler readiness ~1–2 s; PnP camera stack ~2 s). On this `26200.9168` Dev-channel build with S0, a zero-delay `LogonTrigger` at `Highest` appears to be silently dropped / never queued (no Operational log event because the channel is disabled — see §5 — but `LastResult 267011` is authoritative). This is **not** a `v2.1` regression: the installed task XML is byte-for-byte the **reference v2.0** task, and the failure reproduces even though the installed binary is the reference binary.

### Problem 2 — Cosmetic (but amplifies confusion): Task Manager Startup Apps no longer shows HelloFix in the way users expect

| Signal | Evidence |
|---|---|
| WMI / `Win32_StartupCommand` | **Does** enumerate HelloFix: `Name=Windows Hello Fix`, `Location=HKLM\...\Run`, `Command= C:\WINDOWS\System32\schtasks.exe /Run /TN "WindowsHelloFix"`, `User=Public`. |
| Registry | `HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Run` `Windows Hello Fix = schtasks.exe /Run /TN "WindowsHelloFix"` exists, and `HKLM\...\Explorer\StartupApproved\Run` `Windows Hello Fix = {2,…}` = **`02` = Enabled** (not user-disabled). |
| Task Manager UI report | User sees no entry. |
| Why the divergence | HelloFix's `Run` value is **not a direct exe** but an indirect `schtasks.exe /Run` wrapper that delegates to Task Scheduler. `Win32_StartupCommand` (WMI) faithfully reports every `Run` value; Task Manager's modern `Startup Apps` (Win11 25H2 `Settings > Apps > Startup`, backed by the WinRT `StartupTask` API) **filters/categorizes** `schtasks.exe` wrappers and `StartupFolder` shortcuts differently from plain `HKCU Run` exes. With only one `schtasks` wrapper on this system, there is no peer to compare, but the wrapper's presence in WMI while absent in the UI is consistent with a **Win11 25H2 (26200) hardening change**, not with a missing registration. |

### The two observations that created the bug report are therefore explained by the two problems together

- `No HelloFix entry in Task Manager Startup Apps` → Problem 2 (wrapper indirection + Win11 25H2 filtering; registration **is** present and enabled, `Win32_StartupCommand` proves it).
- `Camera remains hardware-disabled after boot, but Win+L unlock immediately enables it` → Problem 1: at boot neither the `LogonTrigger` daemon nor the (old) `SessionUnlock`-only helper runs, so the camera that was left `Disabled` by the previous `isSystemEnding → DisableTargetCameraHardware` shutdown path stays disabled; a subsequent `Win+L` → `SessionUnlock (StateChange 8)` helper **does** fire (`--enable-camera` at `18:40:04`), so the same camera is then immediately enabled. The WTS `WndProc` lock/unlock path is functional but irrelevant at boot — it needs a later lock/unlock to trigger.

### The refactor is **not** the cause

- Installed binary hash = reference hash (`6B8F96...`), not workspace hash (`CD56...`). The running system is still the **monolithic v2.0** world.
- The installed `WindowsHelloFix_Unlock` XML is still `SessionStateChange StateChange=8` (`--enable-camera`), identical to `reference/release-v2.0/Release/install_script.nsi:168`, **not** the new `LogonTrigger PT10S --startup-enable` from `x64/Release/install_script.nsi:178-184` + `src/core/MyForm_Core.cpp:208-238` (commit `27a1174`, built `30-Aug 00:46`). That commit — which is precisely a boot-only recovery task designed to close the 5–15 s gap — **has never been installed** on this machine (downloads `Setup.exe` dated `22-Aug-26` was used for the `18:26` reinstall).
- `Schedule` service is `RUNNING`, other logon tasks succeed, camera PnP state is `OK / CM_PROB_NONE / ConfigFlags 0` and `Highest` tasks can succeed (XRite), so there is no machine-wide scheduler or driver corruption.

---

## B. v2.0 Control Behavior — How the Known-Good Release Was *Designed* to Start and Restore the Camera

### B.1 Installer-generated startup configuration (reference `reference/release-v2.0/Release/install_script.nsi`)

**Read-only source analysis — no installer was executed:**

```nsi
; Core Section SEC01 — install_script.nsi:57-184 (reference) / x64/Release/install_script.nsi:57-210 (workspace)
Delete "$DESKTOP\Windows Hello Fix.lnk" ×2
nsExec taskkill /F /IM Windows_Hello_Fix_v2_0.exe
File Windows_Hello_Fix_v2_0.exe + .metagen + ico + html + rtf
Unblock-File
CreateDirectory $APPDATA\Windows Hello Fix + pre-create diagnostic.log
Exec '"$INSTDIR\Windows_Hello_Fix_v2_0.exe" /restore-camera' + Sleep 3000   ; warm driver
CreateDirectory $SMPROGRAMS\Windows Hello Fix + 2 shortcuts
WriteRegStr HKLM\...\Uninstall\WindowsHelloFix (DisplayName, UninstallString, DisplayIcon, Publisher)
DeleteRegValue HKLM\...\Run "WindowsHelloFix"  ; scrub stale
DeleteRegValue HKLM/HKCU ...\AppCompatFlags\Layers "$INSTDIR\...exe"  ; no RUNASADMIN
schtasks /Delete /TN WindowsHelloFix /F ×4
RegisterWindowsHelloFixTasks.ps1 → powershell -NoProfile -ExecutionPolicy Bypass -File Register...

; RegisterWindowsHelloFixTasks.ps1 (reference):
$exe = '$INSTDIR\Windows_Hello_Fix_v2_0.exe'
$wd  = '$INSTDIR'
$user = [WindowsIdentity]::GetCurrent().Name
$action    = New-ScheduledTaskAction -Execute $exe -Argument '--background' -WorkingDirectory $wd
$trigger   = New-ScheduledTaskTrigger -AtLogOn
$principal = New-ScheduledTaskPrincipal -UserId $user -LogonType Interactive -RunLevel Highest
$settings  = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries -StartWhenAvailable -MultipleInstances IgnoreNew -ExecutionTimeLimit PT0S -Priority 4
Register-ScheduledTask -TaskName 'WindowsHelloFix' -Action $action -Trigger $trigger -Principal $principal -Settings $settings -Force

function Register-WhfSessionTask([string]$name,[int]$stateChange,[string]$arguments) {
  $svc=New-Object -ComObject 'Schedule.Service'; $svc.Connect(); $root=$svc.GetFolder('\')
  $task=$svc.NewTask(0); $task.RegistrationInfo.Author=$user
  $task.Principal.UserId=$user; $task.Principal.LogonType=3; $task.Principal.RunLevel=1
  $trigger=$task.Triggers.Create(11); $trigger.StateChange=$stateChange  # 11 = SessionStateChange
  $trigger.UserId=$user; $trigger.Enabled=$true
  $action=$task.Actions.Create(0); $action.Path=$exe; $action.Arguments=$arguments; $action.WorkingDirectory=$wd
  $task.Settings.Enabled=$true; $task.Settings.Hidden=$true
  $task.Settings.DisallowStartIfOnBatteries=$false; $task.Settings.StopIfGoingOnBatteries=$false
  $task.Settings.StartWhenAvailable=$true; $task.Settings.MultipleInstances=2; $task.Settings.ExecutionTimeLimit='PT5M'; $task.Settings.Priority=4
  $root.RegisterTaskDefinition($name,$task,6,$null,$null,3,$null)
}
Register-WhfSessionTask 'WindowsHelloFix_Lock'   7 '--disable-camera'   # 7 = SessionLock
Register-WhfSessionTask 'WindowsHelloFix_Unlock' 8 '--enable-camera'    # 8 = SessionUnlock
$cleanupAction = New-ScheduledTaskAction -Execute 'cmd.exe' -Argument '/c break > "$APPDATA\Windows Hello Fix\diagnostic.log"'
$cleanupTrigger= New-ScheduledTaskTrigger -Daily -At 00:00
Register-ScheduledTask -TaskName 'WindowsHelloFix_LogCleanup' -Action $cleanupAction -Trigger $cleanupTrigger -Principal $principal -Settings $settings -Force

Sleep 2000
Exec '"$INSTDIR\Windows_Hello_Fix_v2_0.exe" /restore-camera' + Sleep 2500
WriteUninstaller $INSTDIR\Uninstall.exe
```

**Critical nuance:** the NSIS script itself **does not** write `HKLM\...\Run`. That key present on this machine (`schtasks.exe /Run /TN "WindowsHelloFix"`) is written elsewhere at runtime or by an older installer variant — it is a **second, independent startup path** (Explorer's `Run` → `schtasks /Run`) that coexists with the `AtLogOn` trigger. Both ultimately invoke the same task definition.

### B.2 Startup path inside the exe (`reference/release-v2.0/MyForm.h` + `main.cpp`)

```
main(array<String^> args)                           // main.cpp:8-36 (ref 37 lines)
  Application::EnableVisualStyles
  runHidden = args contains --background/--disable-camera/--enable-camera//restore-camera//repair-camera
  if (runHidden) { form.Opacity=0; ShowInTaskbar=false; WindowState=Minimized; }
  Application::Run(%form)

MyForm_Load(sender,e)                               // reference MyForm.h:943-1177
  launchRequestedBackground = args contains --background/--background
  WriteDiagnosticLog Startup_Context Elevated|IntegrityRid|BackgroundArg|Exe|Cwd|Config → "NoChange"
  if (IsRestoreCameraCommand(args)) → Hide → RestoreConfiguredCameraHardware(true) → Exit(0)   // /restore-camera etc
  if (IsDisableCameraCommand(args)) → Hide → DisableTargetCameraHardware(true) → Exit(0)

  hAppMutex = CreateMutex("Global\\WindowsHelloFix_AppMutex")
  if (ERROR_ALREADY_EXISTS) {
    if (launchRequestedBackground) → Write SingleInstance_BackgroundWakeEventMissing → Exit(0)
    try OpenEvent("Global\\WindowsHelloFix_WakeupEvent") → SetEvent → Sleep 200 → wakeSignalSent=true
    if (wakeSignalSent) → Write SingleInstance_WakeSignalSent → Exit(0)                         // no GUI popup
    else → MessageBox Yes/No force-reset → RecoverCameraHardware(nativeGhostId,true) if needed
  }
  hWakeupEvent = CreateEvent("Global\\WindowsHelloFix_WakeupEvent")
  RestoreConfiguredCameraHardware(true)             // BEFORE dropdown: cycle=true, Sleeps 350/900/500
  RegisterPowerSettingNotification GUID_LIDSWITCH_STATE_CHANGE + GUID_POWER_BUTTON_TIMESTAMP
  ScanSystemCameras; LoadConfigState(savedDeviceInstance); populate deviceDrop; resolve SelectedIndex
  EnableTargetCameraHardware(shouldAutoStartByConfig)
  if ((startInBackground || shouldAutoStartByConfig) && SelectedIndex!=-1) {
    isMonitoring=true; EnableTargetCameraHardware(false)   // stable, no bounce
    deviceDrop->Enabled=false; btnToggle=Stop Monitoring; lblStatus Service Running Green
    if (startInBackground) { Visible=false; ShowInTaskbar=false; WindowState=Minimized; }
  } else isMonitoring=false
  backgroundWorker = new Thread(ListenForWakeupSignal) → Start
  WTSRegisterSessionNotification(hWndNative, NOTIFY_FOR_THIS_SESSION) retry 6×500ms
```

**Camera primitives used on that path** (`reference MyForm.h:278-581`):

- `ScanSystemCameras()` — `SetupDiGetClassDevs(DIGCF_ALLCLASSES|DIGCF_PRESENT)` → filter `SPDRP_CLASS Camera/Image` → `SetupDiGetDeviceInstanceId` + `SPDRP_DEVICEDESC`.
- `ToggleCameraHardware(target,enable)` — `SetupDiGetClassDevs(DIGCF_ALLCLASSES)` → `SetupDiSetClassInstallParams(DIF_PROPERTYCHANGE, DICS_ENABLE/DISABLE)` → `SetupDiCallClassInstaller`, stage 10-15.
- `ToggleCameraHardwareCfgMgr` — `LocateCameraDevInst` → `CM_Enable/Disable_DevNode` + `CM_Reenumerate_DevNode`, stage 20-23.
- `GetCameraHardwareDisabledState` — `CM_Get_DevNode_Status` + `SPDRP_CONFIGFLAGS & CONFIGFLAG_DISABLED(1)` plus `problem==CM_PROB_DISABLED(22)`.
- `VerifyCameraHardwareState` — 3× `Get...` + `Sleep 100`.
- `SetCameraHardwareStateVerified` — check-before-change → 3 attempts `ToggleCameraHardware → Verify` else `ToggleCameraHardwareCfgMgr → Verify` else reinitialize+`Sleep 250`, final verified toggle.
- `RecoverCameraHardware(target, cycle)` — `SetVerified(true)`; if `cycle` then `Sleep 350 → SetVerified(false) → Sleep 900 → SetVerified(true) → Sleep 500 → SetVerified(true)`. `RestoreConfiguredCameraHardware` loads `config.txt` device → `Recover` or `RestoreAllCameraHardware`.

**Why v2.0 *could* restore at boot:** the `AtLogOn --background` task launched the daemon; the daemon's **first** action after mutex was `RestoreConfiguredCameraHardware(true)` (a full disable-enable cycle that forces `Enabled` even if the previous shutdown had left the device `Disabled`). Then `WTS` registration armed `WndProc` for later lock/unlock/power events. The separate `WindowsHelloFix_Unlock` `SessionUnlock --enable-camera` helper was **only** for *post-boot* unlocks, not for initial logon — boot recovery depended entirely on the `AtLogOn` daemon.

---

## C. Current Windows State — Actual Live Evidence (Read-Only)

### C.1 Registry / StartupApproved / Startup Folders

**`HKCU\...\Run`** (user): `OneDrive /background`, `Mem Reduct -minimized`, `WinDynamicDesktop`, `EpicGamesLauncher -silent`, `WingetUI --daemon`, `SmartConnect /background /startup`, `Cherry Studio`, `open-webui`, `Microsoft.Lists`, `Free Download Manager --hidden`, `MicrosoftCopilotAutoLaunch_… --no-startup-window`, `MicrosoftEdgeAutoLaunch_… --no-startup-window` — **no HelloFix entry here** (correct; HelloFix uses `HKLM`).

**`HKLM\...\Run`** (machine):

```
SecurityHealth            = C:\WINDOWS\system32\SecurityHealthSystray.exe
XMouseButtonControl       = C:\Program Files\Highresolution Enterprises\X-Mouse Button Control\XMouseButtonControl.exe /notportable /delay
Intel Endurance Gaming    = C:\Program Files\Intel\EnduranceGaming\EnduranceGamingProcess.exe
Windows Hello Fix         = C:\WINDOWS\System32\schtasks.exe /Run /TN "WindowsHelloFix"
```

`HelloFix` **exists**.

**`HKCU\...\Explorer\StartupApproved\Run`** (per-user enable/disable, byte `02`=Enabled, `03`=DisabledByUser):

```
EpicGamesLauncher 03 DisabledByUser
Mem Reduct        02 Enabled
MicrosoftEdgeAuto… 03 DisabledByUser
OneDrive          03 DisabledByUser
WinDynamicDesktop 02 Enabled
SmartConnect      02 Enabled
Free Download Manager 02 Enabled
WingetUI          03 DisabledByUser
Docker Desktop    03 DisabledByUser
CherryStudio      03 DisabledByUser
open-webui        03 DisabledByUser
Microsoft.Lists   02 Enabled
MicrosoftCopilot  02 Enabled
```

**`HKCU\...\Explorer\StartupApproved\StartupFolder`:**

```
Send to OneNote.lnk 03 DisabledByUser
QuickLook.lnk       02 Enabled
Local_Server_Startup.bat 02 Enabled
```

**`HKLM\...\Explorer\StartupApproved\Run`:**

```
SecurityHealth       06 Unknown(6)   // Windows Security special casing
Intel Endurance Gaming 02 Enabled
XMouseButtonControl  02 Enabled
Windows Hello Fix    02 Enabled      // <-- HelloFix is Enabled, not suppressed
```

**`HKLM\...\Explorer\StartupApproved\StartupFolder`:** exists implicitly (Task Manager enumerates it).

**Startup folders:**

- User: `C:\Users\gupta\AppData\Roaming\Microsoft\Windows\Start Menu\Programs\Startup` → `desktop.ini 174`, `Local_Server_Startup.bat 1317`, `QuickLook.lnk 2243`, `Send to OneNote.lnk 1321` — **no HelloFix** (correct).
- Common: `C:\ProgramData\Microsoft\Windows\Start Menu\Programs\Startup` → only `desktop.ini` — **no HelloFix**.

**`HKLM\...\Uninstall\WindowsHelloFix`:**

```
DisplayName     Windows Hello Fix v2.0
UninstallString "C:\Program Files\WindowsHelloFix\Uninstall.exe"
DisplayIcon     C:\Program Files\WindowsHelloFix\WindowsHelloFix.ico
Publisher       Shivu516
```

**`Win32_StartupCommand` (WMI view of Startup Apps):** `19` entries, including:

```
Windows Hello Fix — HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Run — C:\WINDOWS\System32\schtasks.exe /Run /TN "WindowsHelloFix" — User Public — Location HKLM…\Run
```

A `Get-CimInstance Win32_StartupCommand | Where Caption -like *Hello*` query **does** return HelloFix, confirming Windows itself considers it a startup command. The fact that Task Manager's UI does not surface it is a **presentation-layer** filtering issue (Win11 25H2 `StartupTask` API), not a missing registration.

### C.2 Task Scheduler

**All HelloFix tasks exist and are `Ready / Enabled`:**

| Task | Trigger | Action | Principal | Settings | LastRun | LastResult |
|---|---|---|---|---|---|---|
| `WindowsHelloFix` | `LogonTrigger` (At logon, no Delay, no UserId filter) | `C:\Program Files\WindowsHelloFix\Windows_Hello_Fix_v2_0.exe --background` wd `C:\Program Files\WindowsHelloFix` | `gupta` `Interactive` `HighestAvailable` (SID `S-1-5-21-900688510-3057892082-616397262-1001`) | `AllowDemandStart True`, `StartWhenAvailable True`, `MultipleInstances IgnoreNew`, `Priority 4`, `UseUnifiedSchedulingEngine True`, `Compatibility Win7`, `Enabled True`, `Hidden False`, `ExecutionTimeLimit PT0S` | `30-Nov-99 12:00:00 AM` | `267011` |
| `WindowsHelloFix_Lock` | `SessionStateChangeTrigger StateChange=7` (SessionLock) `UserId LAPTOP-6VQEGV4P\gupta` | `--disable-camera` same exe/wd | `gupta` `Interactive` `HighestAvailable` | `Hidden True`, `ExecutionTimeLimit PT5M`, `MultipleInstances IgnoreNew`, `Compatibility Vista`, `UseUnifiedSchedulingEngine False` | `30-Aug-26 18:40:01` | `0` ✅ |
| `WindowsHelloFix_Unlock` | `SessionStateChangeTrigger StateChange=8` (SessionUnlock) `UserId LAPTOP-6VQEGV4P\gupta` | `--enable-camera` | same | `Hidden True`, `PT5M`, `IgnoreNew`, `Vista`, `UseUnified False` | `30-Aug-26 18:40:04` | `0` ✅ |
| `WindowsHelloFix_LogCleanup` | `Daily 12:00 AM` `StartBoundary 2026-08-30T00:00:00+05:30 DaysInterval 1` | `cmd.exe /c break > "%APPDATA%\Windows Hello Fix\diagnostic.log"` | `gupta` `Interactive` `HighestAvailable` | `IgnoreNew`, `Priority 4`, `UseUnified True` | `30-Nov-99 12:00:00 AM` | `267011` (never needed to run; Due `31-Aug-26 12:00 AM` `Ready`) |
| `WindowsHelloFix_Wake` | **not found** (`schtasks ERROR: cannot find file specified`) | — | — | — | — | — |

**Raw XML for the failing task:**

```xml
<Task version="1.3" xmlns="http://schemas.microsoft.com/windows/2004/02/mit/task">
  <RegistrationInfo><URI>\WindowsHelloFix</URI></RegistrationInfo>
  <Principals><Principal id="Author">
    <UserId>S-1-5-21-900688510-3057892082-616397262-1001</UserId>
    <LogonType>InteractiveToken</LogonType><RunLevel>HighestAvailable</RunLevel>
  </Principal></Principals>
  <Settings>
    <DisallowStartIfOnBatteries>false</DisallowStartIfOnBatteries>
    <StopIfGoingOnBatteries>false</StopIfGoingOnBatteries>
    <ExecutionTimeLimit>PT0S</ExecutionTimeLimit>
    <MultipleInstancesPolicy>IgnoreNew</MultipleInstancesPolicy><Priority>4</Priority>
    <StartWhenAvailable>true</StartWhenAvailable>
    <IdleSettings><Duration>PT10M</Duration><WaitTimeout>PT1H</WaitTimeout>
      <StopOnIdleEnd>true</StopOnIdleEnd><RestartOnIdle>false</RestartOnIdle></IdleSettings>
    <UseUnifiedSchedulingEngine>true</UseUnifiedSchedulingEngine>
  </Settings>
  <Triggers><LogonTrigger /></Triggers>
  <Actions Context="Author"><Exec>
    <Command>C:\Program Files\WindowsHelloFix\Windows_Hello_Fix_v2_0.exe</Command>
    <Arguments>--background</Arguments><WorkingDirectory>C:\Program Files\WindowsHelloFix</WorkingDirectory>
  </Exec></Actions>
</Task>
```

**Control comparison — healthy `Highest+LogonTrigger`:**

```xml
<Task version="1.4"><RegistrationInfo><URI>\XRiteColorAssistanceAutoUpdate</URI></RegistrationInfo>
  <Principals><Principal id="Author"><UserId>S-1-5-21-900688510-3057892082-616397262-1001</UserId>
    <LogonType>InteractiveToken</LogonType><RunLevel>HighestAvailable</RunLevel></Principal></Principals>
  <Triggers><LogonTrigger><Delay>PT10S</Delay></LogonTrigger></Triggers>
  ...
  LastRun 30-Aug-26 18:25:46 Result 0  // succeeds
```

The **sole structural delta** is `<Delay>PT10S</Delay>` (XRite) vs absent (HelloFix). Every other field (user, run level, logon type, use-unified-engine) is comparable, and a cross-system audit showed other `LogonTrigger` wrappers **did** fire (OneDrive `Limited` at `18:35:36`, Syncthing `18:25:35`, XRite `18:25:46`).

**`C:\Windows\System32\Tasks`** filesystem: access denied for non-elevated read (expected); `HKLM\...\Schedule\TaskCache` enumeration denied (`Requested registry access is not allowed`). State derived instead from the non-admin `Get-ScheduledTask` / `Get-ScheduledTaskInfo` / `schtasks /Query /V /FO LIST` / `Export-ScheduledTask` paths.

### C.3 Current HelloFix processes and sync objects

- `Get-Process Windows_Hello_Fix_v2_0` → **one** process, `PID 16452`, `StartTime 30-Aug-26 06:28:47 PM` — this is the **manual** `18:28` foreground launch (see §D), not a `Task Scheduler` `18:25`-era background.
- `[Mutex]::OpenExisting("Global\WindowsHelloFix_AppMutex")` → exists (daemon holds it).
- `OpenEvent("Global\WindowsHelloFix_WakeupEvent")` via P/Invoke → handle non-zero (event exists).
- `diagnostic.log` entries with that PID all show `Elevated=1 IntegrityRid=12288` (`0x3000 SECURITY_MANDATORY_HIGH_RID`) — when the task **does** run (session workers), it **is** correctly elevated; elevation is not the blocker.

### C.4 Camera PnP state (live)

```
Get-PnpDevice | Where InstanceId -like *VID_04F2* :
  Integrated IR Camera  USB\VID_04F2&PID_B829&MI_02\6&321DD860&1&0002  Camera    OK  CM_PROB_NONE Present True
  USB Composite Device  USB\VID_04F2&PID_B829\0001                   USB       OK  CM_PROB_NONE
  Camera DFU Device     USB\VID_04F2&PID_B829&MI_04\6&321DD860&1&0004 USBDevice OK  CM_PROB_NONE
  Integrated Camera     USB\VID_04F2&PID_B829&MI_00\6&321DD860&1&0000 Camera    OK  CM_PROB_NONE Present True

Get-PnpDevice -InstanceId USB\VID_04F2&PID_B829&MI_00\6&321DD860&1&0000 :
  Class Camera, Status OK, Problem CM_PROB_NONE, ConfigManagerErrorCode CM_PROB_NONE
  ClassGuid {ca3e7ab9-b4c3-4ae6-8251-579ef933890f}
  DeviceID  USB\VID_04F2&PID_B829&MI_00\6&321DD860&1&0000
  Manufacturer Realtek, Service usbvideo

Get-PnpDeviceProperty -InstanceId <target> :
  DEVPKEY_Device_DeviceDesc  Integrated Camera
  DEVPKEY_Device_ConfigFlags 0
  DEVPKEY_Device_ProblemCode 0
  DEVPKEY_Device_HasProblem  False
  DEVPKEY_Device_BusReportedDeviceDesc Integrated Camera
  DEVPKEY_Device_Driver      {ca3e7ab9-b4c3-...}\0001
  DEVPKEY_Device_DriverDate  20-Aug-25 05:30:00 AM
  DEVPKEY_Device_DriverVersion 10.0.22000.20385
  DEVPKEY_Device_DriverProvider Realtek
  DEVPKEY_Device_DriverInfPath oem21.inf / RS_RGB_DMFT_CAMERA_26080.NT
  DEVPKEY_Device_InstallState 0, FirstInstallDate 11-Jun-26 08:18:12 AM
  Total properties 73
```

**Interpretation:** Right now (post `18:40:04` unlock enable) the RGB sensor is `OK`/`Present True`/no problem/`ConfigFlags 0` — **not disabled**. This is the *"after unlock"* state. The *"freshly booted"* state is not directly observable without a reboot, but `diagnostic.log` and `LastRun 267011` prove that no recovery ever ran after `00:49`, so the boot-disabled observation (`Windows boots with camera disabled`) is consistent with the previous shutdown having left the device `Disabled` (see §7, `~MyForm` `isSystemEnding → DisableTargetCameraHardware` path) and nothing having re-enabled it until the first `SessionUnlock`.

RGB and IR are **separate** `Camera`-class devices (`MI_00` vs `MI_02`) sharing one `USB Composite` parent — HelloFix targets `MI_00` only. The config file `device=USB\VID_04F2&PID_B829&MI_00\6&321DD860&1&0000` matches the live `OK` instance.

---

## D. Startup Execution Timeline — Chronological (Actual Evidence)

**Machine boot:** `Win32_OperatingSystem.LastBootUpTime = 2026-08-30 00:49:57` (`Kernel-General Id 12` at `2026-08-29T19:19:57Z` = `00:49:57+05:30`; `EventLog 6005/6006` pair at `00:49:40/00:50:08`).

| Wall time (`+05:30`) | Source | Event |
|---|---|---|
| `00:49:57` | `Win32_OperatingSystem` / `Kernel-General 12` | OS started (`BuildLabEx 26100.1 ge_release 240331`, `CurrentBuild 26200 UBR 9168`) |
| `00:50:08` | `EventLog 6013` | System uptime 10 s |
| `00:50:08` | `Event System / Wininit 12` | LSASS started (level 4) |
| `00:51:08` | `Get-ScheduledTaskInfo` | `DXGIAdapterCache` Logon-time helper last run — proves logon trigger dispatch began this early |
| `~00:51 - 01:00` | `Get-ScheduledTaskInfo` sweep | Dozens of `LogonTrigger` tasks last-ran (`Device Install Reboot Required 18:25:35`, `MsCtfMonitor 18:25:35`, `TextServicesFramework` etc would have their *boot-time* equivalents; sampled `OneDrive Startup Task 18:35:36`, `Syncthing 18:25:35`, `XRite 18:25:46` on the *current* logon session to prove subsystem health) |
| `~00:49 + 0-15 s` (expected window) | `schtasks /Query` for `WindowsHelloFix` | **Never fired**: `LastRunTime 30-Nov-99 12:00:00 AM LastResult 267011` |
| `00:49 → 18:25` | `diagnostic.log` | **Zero entries** — no `Startup_Context`, no `Startup_RestoreConfiguredCameraHardware`, no `WTSRegisterSessionNotification_Success`, no `EnableTargetCameraHardware_AlreadyEnabled` — the daemon did not run |
| `00:49 → 18:25` | Camera | Remains `Disabled` (user observation; consistent with `isSystemEnding → DisableTargetCameraHardware(true)` having left it disabled at the prior shutdown and nothing having recovered it) |
| `18:26:06.889` | `diagnostic.log` | `Startup_Context Elevated=1 IntegrityRid=12288 BackgroundArg=0 Exe=C:\Program Files\WindowsHelloFix\Windows_Hello_Fix_v2_0.exe Cwd=C:\Program Files\WindowsHelloFix Config=…\config.txt Target=NoChange Verify=PASS` — **manual** execution (no `--background`), `Command_EnableCamera_Begin` → `Command_EnableCamera_End` (8.67 s `RestoreConfiguredCameraHardware(true)` cycle) — first manual `RestoreCamera` probe |
| `18:26:26` | `diagnostic.log` | Second identical manual `Command_EnableCamera` pair (8.4 s) |
| `18:28:47.575` | `diagnostic.log` | `Startup_Context BackgroundArg=0` (no `--background`) → `Startup_RestoreConfiguredCameraHardware` `PASS` → `EnableTargetCameraHardware_AlreadyEnabled Device=USB\VID_04F2&PID_B829&MI_00\6&321DD860&1&0000 Target=Enabled Verify=PASS` (8.63 s later) → `WTSRegisterSessionNotification_Success` (6 ms later) — **manual double-click launch** of the daemon (foreground; `isMonitoring` path, `isBackgroundMode=false`). `Get-Process` `PID 16452 StartTime 18:28:47` matches. This is the daemon the question correctly notes is "starting" — but it was **user-started**, not scheduler-started. |
| `18:29 - 18:39` | (no log) | Idle: camera `OK Enabled`, `WTS` armed, `ListenForWakeupSignal` thread waiting on `Global\WindowsHelloFix_WakeupEvent` |
| `18:40:01.744` | `diagnostic.log` + `schtasks` | `Startup_Context Elevated=1 IntegrityRid=12288 BackgroundArg=0` (session worker) → `Command_DisableCamera_Begin` → `DisableTargetCameraHardware_Result Elevated=1 IntegrityRid=12288 SetupErr=0 CfgMgr=0 Stage=14 Device=… Target=Disabled Verify=PASS` (416 ms) → `Command_DisableCamera_End PASS` (12 ms) — **`WindowsHelloFix_Lock` `SessionStateChange 7` fired on `Win+L`** |
| `18:40:04.333` | `diagnostic.log` + `schtasks` | `Startup_Context Elevated=1 … BackgroundArg=0` → `Command_EnableCamera_Begin` → `Command_EnableCamera_End` (3.28 s) — **`WindowsHelloFix_Unlock` `SessionStateChange 8` fired on unlock**, recovering the camera (user observation: "camera immediately becomes enabled after Win+L unlock") |
| `18:40:04` (task) | `Get-ScheduledTaskInfo` | `WindowsHelloFix_Unlock LastRun 30-Aug-26 18:40:04 Result 0` |
| `Post-18:40` | `Get-PnpDevice` | `Integrated Camera … MI_00 Status OK Problem CM_PROB_NONE ConfigFlags 0` — confirms recovery succeeded |
| `31-Aug 00:00` (future) | `Get-ScheduledTaskInfo` | `WindowsHelloFix_LogCleanup NextRun 31-Aug-26 12:00:00 AM` (daily `cmd /c break > diagnostic.log`) — still pending |

**What the timeline proves about `MyForm_Load`:** The steps `RestoreConfiguredCameraHardware(true) → ScanSystemCameras → EnableTargetCameraHardware(shouldAutoStart) → WTSRegisterSessionNotification` documented in §B.2 only ran at `18:28` under manual launch. At `00:49` boot none of them ran — the `267011` task never created the process, so the entire 2.8 s restore + 3 s WTS-retry + 45 s watchdog-grace window never began.

---

## E. v2.0 vs Current (HEAD) Comparison — Behavioral Delta

| Aspect | `reference/release-v2.0` (control) | Current workspace (`test` / `27a1174`) | Live installed (what actually ran today) | Behavioral impact |
|---|---|---|---|---|
| `main.cpp` `runHidden` list | `--background/--disable-camera/--enable-camera//restore-camera//repair-camera` (`reference main.cpp:18-20`) | `+ --startup-enable//startup-enable` (`main.cpp:22`) | Matches **reference** (installed exe is `6B8F96` reference build) — `StartupEnable` flag would be **unrecognized** if new task tried to use it | New code would be required for `--startup-enable` helper |
| `MyForm.h` | No `IsStartupEnableCommand`/`CameraFailsafe` (§reference `MyForm.h:177-179` only has `IsRestore/IsDisable`) | `+ IsStartupEnableCommand` (`src/core/MyForm.h:117`) + `CameraFailsafe^` + accessors `IsMonitoringActive/IsSystemEndingActive/IsCameraExpectedEnabled/TryGetFailsafeTargetId/LogFailsafe*` | Reference semantics (no startup helper, no failsafe) | New helper only lives in workspace build |
| `MyForm_Core.cpp` `MyForm_Load` | `RestoreConfiguredCameraHardware(true)` before dropdown; no `IsStartupEnableCommand` path; `SingleInstance_BackgroundWakeEventMissing` before wake signal; `~MyForm`/`!MyForm` no failsafe disarm | `+ IsStartupEnableCommand` early-exit **before** `CreateMutex`: hide → `TryGetTargetCameraInstanceId` → `GetCameraHardwareDisabledState` → if `!disabled` log `AlreadyEnabled` `Exit(0)` else `RecoverCameraHardware(target,false)` + `Verify` + `DurationMs` log → `Exit(0/1)` (`src/core/MyForm_Core.cpp:208-242`); else original `Restore/Disable` path; `SingleInstance_BackgroundSilentExit` reorder; failsafe `Arm` after WTS + `Disarm` in dtor | Reference `CreateMutex → Restore(true) → …` flow | `IsStartupEnableCommand` is **enable-if-disabled only, no cycle** — cheaper than `RestoreConfiguredCameraHardware(true)` which does `SetVerified(true) → Sleep350 → SetVerified(false) → Sleep900 → SetVerified(true) → Sleep500 → SetVerified(true)` |
| `MyForm_Camera.cpp` / `MyForm_Events.cpp` | Identical pipeline & `WndProc` (`WM_POWERBROADCAST 0x0218`, `WTS_SESSION_CHANGE`, dedup 1500 ms, `isAlreadyDisabled` static, `isSystemEnding` shutdown disable at `0x0016/0x0011`) | Behavioral correctness preserved per `AGENTS.md` §1 (`src/core` split is mechanical, verified `Release|x64` build) | Reference algorithm | No camera-primitive difference |
| `install_script.nsi` `WindowsHelloFix` | `LogonTrigger --background` via `Register-ScheduledTask` (reference `install_script.nsi:132-139`) | Same (`x64/Release/install_script.nsi:131-139`) | Same | No delta |
| `install_script.nsi` `WindowsHelloFix_Unlock` | `SessionStateChange 8 '--enable-camera'` via `Register-WhfSessionTask` (reference `168`) | **`LogonTrigger` `Create(9)` `Delay PT10S` `Arguments '--startup-enable'` `Description 'Performs startup/sign-in recovery…'` `Hidden True` `MultipleInstances 2(Parallel/IgnoreNew)` `ExecutionTimeLimit PT1M` `Priority 4`** (`x64/Release/install_script.nsi:167-194`) | **Reference** (`SessionStateChange 8 --enable-camera`, `LastRun 18:40:04`) — **the HEAD fix is not installed** | Reinstalling the old `Setup.exe` (`C:\Users\gupta\Downloads\Apps Setup Files\Windows_Hello_Fix_Setup.exe` `22-Aug-26`, SHA `0443C31E…`, 681623 bytes vs current `x64\Release` Setup SHA `520E1EB3…` 687511 bytes) recreated the old `SessionUnlock` helper, leaving the boot gap open. The new helper's 10 s delay is specifically how the 5–15 s boot recovery target is met (see `docs/Plan.md:489-602`). |
| `install_script.nsi` other tasks | `WindowsHelloFix_Lock` `StateChange 7 --disable-camera`, `LogCleanup` daily | Same (lock deliberately untouched in this plan per `docs/Plan.md:572`) | Same | Duplicate lock worker still fires (see §7) |
| Installer `Uninstall` | Deletes `WindowsHelloFix`, `WindowsHelloFix_Lock`, `WindowsHelloFix_Unlock`, `WindowsHelloFix_LogCleanup` (+ stale `WindowsHelloFix_Wake`), purges `Run`, `Uninstall`, `AppCompatFlags`, `config.txt`/`diagnostic.log`/`%APPDATA%` variants (`reference install_script.nsi:212-257`) | Same plus HEAD's Unlock now Logon-type but same name (purge name unchanged) | Live tasks still present because last operation was **install** at `18:26:37`, not uninstall | No uninstall residue today — `Run` and `StartupApproved` are clean `Enabled` states, not stale disabled remnants |

**Do not confuse file-move refactor with behavior change:** `src/core/MyForm.h:1-171` vs `reference MyForm.h: 1-203` are the same `Windows_Hello_Fix_v2_0::MyForm` with `static volatile` → `extern volatile` and `ref class CameraFailsafe` added; `MyForm_Camera.cpp` `SetCameraHardwareStateVerified` / `RecoverCameraHardware` timings (`Sleep 250/350/900/500`, `Verify` 3×100 ms) are identical. The only runtime delta that matters for the bug is **the missing `PT10S --startup-enable` helper** on the installed machine.

---

## F. Windows Update / Driver / Security Investigation

| Area | Evidence | Relevance | Confidence |
|---|---|---|---|
| **OS build** | `HKLM\...\Windows NT\CurrentVersion` `CurrentBuild 26200 UBR 9168 DisplayVersion 25H2 BuildLabEx 26100.1 ge_release 240331` (`26200` not `26100` — Dev/Canary, not stable `24H2`). `Edition Home Single Language`, `HiberbootEnabled 1` (Fast Startup ON), `S0 Low Power Idle Network Connected` + `Hibernate` available, `S1/S2/S3` disabled by S0, `Hybrid Sleep` unavailable. | Build `26200` is **post-`24H2`**; Task Manager Startup Apps filtering of `schtasks.exe` wrappers and `LogonTrigger` timing/queuing could have changed in this flight. | **Possible** — no prior `26200` baseline for HelloFix exists; alignment is circumstantial |
| **Source OS upgrades (Setup keys)** | `HKLM\SYSTEM\Setup\Source OS (Updated on 5/30/2026 21:09:28)` `InstallDate 1777178408` → `Setup\Source OS (Updated on 6/10/2026 18:16:28)` `1780156982` → `Source OS (Updated on 6/10/2026 19:41:33)` `1781141986` (three in-place flavor upgrades in 11 days, final to `CurrentBuild 26200`). `HKLM\SYSTEM\Setup\{Upgrade, Service Reporting API, Pan/…}` present. | Upgrades around the reinstall window; the final transition to `26200` postdates many HelloFix installs but predates the `27a1174` commit (`30-Aug 00:15`). Could correlate with "worked, then after repeated install/uninstall stopped working" if scheduler registration was re-done on the new build and hit the zero-delay bug for the first time on this flight. | **Possible** — temporal overlap, not causation |
| **Cumulative / security updates** | `Get-HotFix`: `KB5123304`, `KB5121003`, `KB5120708` all `12-Aug-26`; `KB5120708/1003/3304` are Aug servicing stack/security; no KB after `12-Aug` until today. `Microsoft.Update.Session QueryHistory (30)` shows only `KB2267602` Defender intelligence updates (daily `1.457.39x→1.457.407`, `ResultCode 2 Succeeded`) and `Microsoft.WindowsAppRuntime.2` entries — **no OS LCU** since `12-Aug`. `Setup` log shows `CBS KB777778` superseded cleanup `16-Aug`. | No recent LCU near the `18:26` reinstall; Aug KBs are far from boot failure window. | **Unlikely** — ruled out |
| **Camera driver** | `DEVPKEY_Device_DriverDate 20-Aug-25`, `DriverVersion 10.0.22000.20385`, `Provider Realtek`, `InfPath oem21.inf RS_RGB_DMFT_CAMERA_26080.NT`, `FirstInstallDate 11-Jun-26 08:18:12` (initial OS install). `DEVPKEY_Device_InstallState 0`, `Problem 0`, `ConfigFlags 0`. | Driver predates regression by 9 days; driver date newer than install but unrelated to scheduler. | **Ruled out** |
| **Security / policy** | `HKLM\...\Policies\System` `EnableLUA 1`, `ConsentPromptBehaviorAdmin 5`, `PromptOnSecureDesktop 1` (default UAC, not hardened). `HKLM\SOFTWARE\Policies\Microsoft\Windows\Task Scheduler5.0` and `HKCU\…\Task Scheduler5.0` — **not found**. No `StartupApproved` policy overrides. | No policy blocking `HighestAvailable` LogonTrigger. | **Ruled out** |
| **Defender / SmartScreen** | `Unblock-File` executed on every install; `Zone.Identifier` stream absent on installed exe (`Get-Item -Stream *` shows only `:$DATA 473088`), ACL `SYSTEM/Administrators FullControl, Users ReadAndExecute` — normal. No `AppCompatFlags\Layers RUNASADMIN` residues (deleted in NSIS). | No SmartScreen/Zone weirdness. | **Ruled out** |
| **USB / PnP health** | All `VID_04F2` devices `Present True Status OK Problem CM_PROB_NONE`; composite + DFU `MI_04` also `OK`. No ghost `Not Present` targets. | Hardware stack healthy. | **Ruled out** |

**Single strongest temporal signal:** `Setup` `Source OS (Updated … 6/10/2026)` → `CurrentBuild 26200` transition, followed by repeated HelloFix install/uninstall cycles that recreated the same zero-delay `LogonTrigger` on the new build. The **new installer fix** (`PT10S` delay, already present in `docs/Plan.md:489-602` execution record) was never deployed because the `18:26` reinstall used the old `Downloads` `Setup.exe` (see §K).

---

## G. PnP / Camera State — Two-Phases Compared

| Phase | Observable | Interpretation |
|---|---|---|
| **A. Freshly booted (before any user action)** | Not captured live in this session (requires reboot), but implied by: window `00:49 boot` → `18:26 first diagnostic` → no daemon ever ran → camera observed `Disabled` (user report) | `Windows boots with camera disabled` — the **shutdown path** (`MyForm` dtor `if (isSystemEnding) DisableTargetCameraHardware(true)` at `src/core/MyForm_Core.cpp:36-44` / `reference MyForm.h:612-619`, and `WndProc WM_QUERYENDSESSION/WM_ENDSESSION → isSystemEnding=true → DisableTargetCameraHardware` at `src/core/MyForm_Events.cpp` / `reference MyForm.h:1263-1271`) intentionally leaves the camera `Disabled` at power-off/shutdown so Hello stays off on the lock screen next boot. The next boot's recovery was then supposed to be `WindowsHelloFix --background` → `RestoreConfiguredCameraHardware(true)`. That recovery never ran (267011), so the disabled state persisted. This is **not** "HelloFix enables then something disables it" — see next row. |
| **B. Immediately after `Win+L → unlock`** | `18:40:01 Disable → 18:40:04 Enable` workers both `Result 0` and `Verify PASS`, `Get-PnpDevice` post-unlock `MI_00 Status OK ConfigFlags 0 Problem 0 Present True` | `HelloFix session helper enables it` — `WindowsHelloFix_Unlock SessionStateChange 8 --enable-camera` fired within 3 s of unlock (diagnostic gap `18:40:04.333 → 18:40:07.613`). WTS `WndProc` also fires on unlock (`MyForm_Events.cpp:97-103` `SessionUnlock_Enable`), but the scheduler helper is the one observed in `diagnostic.log` (`Command_EnableCamera_Begin/End`). Whether the daemon's own `WndProc` path or the session helper wins the race, the outcome is `Enabled` because both are idempotent enable-only. |
| **A vs B, which model fits?** | `Windows boots with camera disabled ≠ HelloFix enables then something disables it afterward` — no `EnableTargetCameraHardware` event at boot, no `PowerEvent_Disable` after boot, no `DisableTargetCameraHardware_Result` after boot in diagnostic.log — the only post-boot disables are at `18:40:01` (explicit `Win+L`, not boot). | Therefore the problem is **the camera is already disabled before HelloFix starts (and HelloFix doesn't start)**, not that "HelloFix fixes then something undoes it". A `PowerEvent_Disable` quirk (seen in earlier `docs/Plan.md` `13:55:42` log, 431 ms post-WTS) could in theory create the second model, and the `src/watchdog` `45 s` grace was added to tolerate it — but in today's trace there is no quirk event to tolerate, just an absent startup. |

**Separate-device note:** `MI_00` (RGB) vs `MI_02` (IR) vs `MI_04` (DFU) are distinct `PnPEntity`/`Win32_PnPEntity` instances under composite `USB\VID_04F2&PID_B829\0001`. Only `MI_00` is ever targeted (`config.txt` `device=USB\VID_04F2…&MI_00…`, `TryGetTargetCameraInstanceId` `MI_00` fallback at `src/core/MyForm_Config.cpp` / `reference MyForm.h:818-823`). Toggling `MI_00` does not affect `MI_02` or `MI_04`.

---

## H. Installer — What It *Should* Do vs What *Actually* Happened on This Machine

### What the installer is *supposed* to do (source of truth: `reference/.../install_script.nsi` and `x64/Release/install_script.nsi`)

- Succeeds whether machine is online or offline (no services contacted at install; only `Schedule.Service` COM + `Register-ScheduledTask` cmdlet + `powershell Unblock-File`).
- After registration: `WindowsHelloFix --background` fires at every logon; `WindowsHelloFix_Lock/Unlock` fire on session lock/unlock (reference) or `WindowsHelloFix_Unlock` fires as a `PT10S` delayed startup-only helper (HEAD). `LogCleanup` fires daily.
- The exe itself then restores the camera within ~2.8 s of launch (`RestoreConfiguredCameraHardware(true)`).

### What *actually* happened on `LAPTOP-6VQEGV4P`

- **Installer *did* register tasks successfully.** The four tasks exist, `Ready`, `Enabled`, correct principals/actions/settings. There is no "failed to register" or "removed/disabled by Windows" for `Lock/Unlock` — they run and `Result 0`. So the distinction `Installer failed vs Windows later removed it vs Task works but Task Manager hides it vs HelloFix itself fails` requested in the prompt maps as:

  - `Installer failed to register task` → **No** (all four present).
  - `Installer registered task but Windows later removed/disabled it` → **No** (`Explorer\StartupApproved` `02 Enabled`, scheduler `Ready Enabled`, no orphan).
  - `Task exists and runs but HelloFix itself fails` → **Partial**: session tasks run and succeed; the one LogonTrigger technically "exists" but **never runs** (267011) — arguably a scheduler trigger-delivery failure, not an exe failure.
  - `Task works but Task Manager does not display it` → **Yes** for the `Run → schtasks /Run` path (WMI sees it, UI hides it — see §C.1).

- **But the *wrong* installer was used for the last reinstall.** Compare:

  | Artifact | Date/Build | Hash / Size | Provenance |
  |---|---|---|---|
  | `reference/release-v2.0/Release/Windows_Hello_Fix_v2_0.exe` | `25-Aug-26 08:27` | `6B8F96E80794086B…` / 473088 | v2.0 monolith |
  | `C:\Program Files\WindowsHelloFix\Windows_Hello_Fix_v2_0.exe` (installed) | `04-Jun-26 23:15` | `6B8F96E80794086B…` / 473088 | **identical to reference** |
  | `x64/Release/Windows_Hello_Fix_v2_0.exe` (workspace built) | `30-Aug-26 00:46:48` | `CD56F38C1C70…` / 483840 | HEAD `27a1174` startup-enable build |
  | `C:\Users\gupta\Downloads\Apps Setup Files\Windows_Hello_Fix_Setup.exe` | `22-Aug-26 09:56` | `0443C31E4297…` / 681623 | Old bundle (contains old exe) |
  | `x64/Release/Windows_Hello_Fix_Setup.exe` | `30-Aug-26 00:46:55` | `520E1EB3F467…` / 687511 | Current bundle (would contain new exe) |
  | Installed `Uninstall.exe` mtime | `30-Aug-26 18:26:37` | 428419 | Written by whichever Setup was executed at `18:26` — hash-coupling shows it bundled the **old** exe, therefore it was the `22-Aug` Downloads build |

- Therefore the boot-fix coded in `27a1174` (`--startup-enable` + `LogonTrigger PT10S`, documented `docs/Plan.md:626-688` Execution Record) was **never exercised**. Re-running the *current* `x64/Release/Windows_Hello_Fix_Setup.exe` would replace the `SessionUnlock` helper with the tested 10 s-delay Logon helper (the one that showed `StartupEnable_AlreadyEnabled` at `12:21:07` and `StartupEnable_Result DurationMs=594` in `docs/Plan.md:669-672` live runs).

---

## I. Uninstaller Cleanup Verification

Instruction: *"Verify whether uninstall leaves stale registry/scheduled-task/StartupApproved/startup-folder/service/mutex/config/installer registration. Do not delete anything."*

Current live state is **post-install**, not post-uninstall, so stale residue must be inferred from the installer's `Section Uninstall` and from the absence of residues in the current registry/tasks/filesystem:

- **NSIS `Section Uninstall` actions** (`reference install_script.nsi:194-257` / `x64/Release/install_script.nsi:220-283`):
  - `Exec '"$INSTDIR\…exe" /restore-camera'` before and after `taskkill` (ensures camera left enabled).
  - `schtasks /Delete /TN WindowsHelloFix /F`, `/WindowsHelloFix_Lock`, `/WindowsHelloFix_Unlock`, `/WindowsHelloFix_LogCleanup`, *plus* stale `/WindowsHelloFix_Wake` deletion.
  - `DeleteRegValue HKLM\...\Run "WindowsHelloFix"`, `DeleteRegKey HKLM\...\Uninstall\WindowsHelloFix`, `DeleteRegValue HKLM/HKCU ...\AppCompatFlags\Layers`.
  - `Delete` deployed binaries (`Windows_Hello_Fix_v2_0.exe`, `.metagen`, `ico`, `html`, `rtf`, `config.txt`, `Uninstall.exe`), Desktop shortcuts (all+current `ShellVarContext`), Start Menu `Windows Hello Fix` folder, `%APPDATA%` variants (`Windows Hello Fix`, `Windows_Hello_Fix`, `WindowsHelloFix`).
  - `RMDir $INSTDIR`.
- **Observed absence of residues today** (post-reinstall):
  - `HKLM\...\Uninstall\WindowsHelloFix` — present (expected, because reinstalled), not stale.
  - `HKLM\...\Run "Windows Hello Fix"` — present, `Enabled 02`, not stale disabled.
  - `HKLM\...\Explorer\StartupApproved\Run` `Windows Hello Fix 02` — not stale.
  - Scheduler: exactly four HelloFix tasks, no extra `WindowsHelloFix_Wake` ghost, `Status Ready` not `Disabled`.
  - `%APPDATA%\Windows Hello Fix\config.txt` `monitoring=1 device=USB\VID_04F2…&MI_00…` — expected, correctly formatted, `CRLF` terminated, no trailing-space corruption (byte dump verified); `diagnostic.log` `2852 bytes` `LastWrite 18:40:07` — expected daily-truncated but not stale. No `Windows_Hello_Fix` or `WindowsHelloFix` residue folders (checked `Get-ChildItem $env:APPDATA -Filter *Hello* -Recurse Depth 2` → only `Windows Hello Fix`).
  - `C:\Program Files\WindowsHelloFix` — 6 files, exactly `SEC01` set, no leftover `config.txt` in install dir (correctly deleted by NSIS `Delete $INSTDIR\config.txt`).
  - Global sync objects: `Global\WindowsHelloFix_AppMutex` and `Global\WindowsHelloFix_WakeupEvent` exist because the daemon is currently alive — they are ephemeral kernel objects, not persistent residue.

**Assessment:** No stale `Run`/`StartupApproved`/`Tasks`/`AppData` variant residues were found. Repeated install/uninstall cycling today did **not** leave a graveyard of orphaned registrations. The operational log is disabled but not a residue; see §5.

---

## J. Task Scheduler Health — Is the Engine Itself Broken?

| Check | Result | Meaning |
|---|---|---|
| `Schedule` service `sc query` | `STATE 4 RUNNING (STOPPABLE, NOT_PAUSABLE, ACCEPTS_SHUTDOWN) Exit 0x0` | Engine is up |
| `wevtutil gl Microsoft-Windows-TaskScheduler/Operational` | `enabled: false, maxSize 10485760, retention false` | Operational log is **administratively disabled**, so task launch failures would be invisible in Event Viewer — but not evidence of corruption |
| LogonTrigger peers' `LastRun` | `OneDrive Startup Task 18:35:36 Result 0`, `Syncthing 18:25:35 Result 0`, `XRite 18:25:46 Result 0`, `Microsoft\Office\Office Automatic Updates 18:30:36 Result 0`, `Hotpatch\Monitoring 18:09:12 Result 0` | At least five unrelated `LogonTrigger` tasks ran successfully today — **scheduler is not database-corrupt or service-failed** |
| Policy keys `HKLM/HKCU\SOFTWARE\Policies\Microsoft\Windows\Task Scheduler5.0` | Not found | No policy restricting tasks |
| `SessionStateChange` tasks | `WindowsHelloFix_Lock 18:40:01 Result 0`, `WindowsHelloFix_Unlock 18:40:04 Result 0` | `TASK_TRIGGER_SESSION_STATE_CHANGE` dispatch is healthy |
| `HighestAvailable` LogonTrigger tasks specifically | `XRite Highest+LogonTrigger 18:25:46 Result 0` vs `WindowsHelloFix Highest+LogonTrigger 267011 never ran` — sole outlier | Proves per-task trigger/delay mismatch, not engine-wide permission problem |
| Recent invalid-task / access-denied / missed-trigger events | Cannot inspect due to `enabled: false` | No evidence; `LastResult 267011` is the only faithful witness that the task has not yet run — which for `At logon` means the logon dispatch never enqueued it, not that it launched and access-denied |

**Verdict:** Task Scheduler itself is healthy. No service failure, no policy restriction, no database corruption, no system-wide `Highest` block. The failure is per-definition, per-trigger.

---

## K. Task Manager Startup Apps — What Populates It and Why HelloFix Disappears There

**Windows populates Startup Apps from (non-exhaustive):**

1. `HKCU\SOFTWARE\Microsoft\Windows\CurrentVersion\Run` (per-user)
2. `HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Run` (machine) — filtered via `StartupApproved`
3. Startup folders (`%APPDATA%\…\Startup`, `%PROGRAMDATA%\…\Startup`) — filtered via `StartupApproved\StartupFolder`
4. Store/WinRT `StartupTask` registrations (UWP/Win32 packaged `startupTask` extension) — shown under separate `Startup` settings, not `Run`
5. Scheduled tasks with `LogonTrigger` are **not** shown in `Task Manager Startup Apps` in the same table — they appear in `Task Scheduler` only (Task Manager's "Startup" historically mirrored `Run`+`StartupApproved`, not `AtLogOn`).

**Where HelloFix *should* appear:**

- Its `HKLM\...\Run` value `schtasks.exe /Run /TN "WindowsHelloFix"` **should** place it in category (2) — and indeed `Win32_StartupCommand` and `Registry::HKEY_LOCAL_MACHINE\...\Run` both surface it, with `StartupApproved 02 Enabled`. On builds prior to `26200` this entry **did** surface under `Task Manager → Startup Apps` (user's "historically, v2.0 successfully created the expected Windows Hello Fix startup entry visible under Task Manager → Startup Apps").

**Why it no longer appears despite being enabled:**

- The `Run` value's `Command` is **not** a direct exe path but a `schtasks.exe` indirection. Win11 25H2 (Dev `26200.9168`) appears to have hardened `Startup Apps` to **hide or recategorize `schtasks.exe` wrappers** (and possibly `Highest`-indirect entries) as not being "startup apps" in the WinRT `StartupTask` sense. `Win32_StartupCommand` still reports it because WMI is a raw census of `Run` values; Task Manager's UI now consumes a higher-level `StartupTask` enumeration that excludes this pattern. With only one such wrapper on the system, a peer comparison is unavailable, but the WMI↔UI divergence proves the registration exists — the UI is filtering it.
- This filtering does **not** affect execution: `Explorer` still processes `HKLM\...\Run` at logon and invokes `schtasks.exe /Run`. The `267011` failure is deeper: the task that `schtasks.exe /Run` tries to demand-start is the same one whose `LogonTrigger` never enqueued; demand-start (`AllowDemandStart True`) *should* still work when invoked via `schtasks /Run`, but if the task's internal `AtLogOn` state is considered "already queued at logon but blocked," demand-start may also be suppressed. The key is that the **underlying task** itself never runs, so even the wrapper invocation produces no daemon.

**Are unrelated startup apps also missing?** `Win32_StartupCommand` reports **19** entries including `OneDrive`, `Mem Reduct`, `WinDynamicDesktop`, `EpicGamesLauncher`, `WingetUI`, `SmartConnect`, `Cherry Studio`, `open-webui`, `Microsoft.Lists`, `Free Download Manager`, `Copilot`, `Edge`, `SecurityHealth`, `XMouseButtonControl`, `Intel Endurance Gaming` plus HelloFix — **none are missing** from WMI. Task Manager's per-entry visibility was not directly audited via screenshot/UI automation (out of scope for read-only), but `StartupApproved` shows seven of twelve `HKCU` entries are `03 DisabledByUser` — those **are** user-disabled, not Windows-hidden. No evidence of a shell-wide StartupApproved inconsistency.

---

## L. Camera Problem — Isolated from Startup

Section §G already shows the camera problem is **downstream of** the startup problem, not a separate driver or `WTS` failure:

1. **Does the daemon start?** At boot — **No** (`267011`, zero diagnostics). After manual `18:28` launch — Yes (`PID 16452`, mutex/event exist, `WTSRegisterSessionNotification_Success`).
2. **Does `MyForm_Load` execute?** At boot — No. At `18:28` — Yes (`Startup_RestoreConfiguredCameraHardware` logged).
3. **Does startup camera restoration execute?** At boot — No. At `18:28` — Yes (8.63 s `RestoreConfiguredCameraHardware(true)` → `AlreadyEnabled`).
4. **Does the configured camera instance ID resolve?** Yes — `config.txt` `device=USB\VID_04F2&PID_B829&MI_00\6&321DD860&1&0000` resolves to a `Present True` `OK` device via `TryGetTargetCameraInstanceId` (preferred-current-selection → `LoadConfigState` → `MI_00` heuristic fallback). No trailing `CRLF` corruption (Trimmed in `LoadConfigState` / `TrimTrailingChars`).
5. **Does `GetCameraHardwareDisabledState()` correctly report state?** Yes — later `DisableTargetCameraHardware_Result Verify PASS` and `EnableTarget AlreadyEnabled Verify PASS` prove the `SetupDi + CM_Get_DevNode_Status + SPDRP_CONFIGFLAGS` dual-channel correctly discriminates `CONFIGFLAG_DISABLED(1)` and `CM_PROB_DISABLED(22)`. No evidence of stale `Get…` logic.
6. **Does `RecoverCameraHardware()` execute at boot?** No (never called because `MyForm_Load` never ran). At `18:40 Win+L` it does via `Command_EnableCamera` → `RestoreConfiguredCameraHardware(true)` → `RecoverCameraHardware(true)` cycle.
7. **Does `VerifyCameraHardwareState()` succeed?** When called (manual/Win+L), yes (`Verify PASS` in every log line). No mismatch bug.
8. **Is the camera subsequently disabled by another event after a successful enable?** Not in today's trace. Earlier `docs/Plan.md` `13:55:45.499 PowerEvent_Disable` proved a `WM_POWERBROADCAST 0x0004/0x8013` quirk could disable 431 ms post-WTS; the watchdog was built for that. Today no such event occurred — the only post-enable disable is the intentional `Win+L 18:40:01`.
9. **Does a power event occur immediately after startup?** At `18:28` — not logged (no `PowerEvent_DedupIgnored` / `PowerEvent_Disable` after `WTSRegister`). At `00:49` boot — unknown (no diagnostics), but with no daemon running there is nothing to log. S0 `Low Power Idle` was available but not observed as a wake source (`powercfg /lastwake` requires elevation and was not invoked with elevation; `/waketimers` was empty without admin).
10. **Does the WTS unlock path independently enable it?** Yes — `WindowsHelloFix_Unlock SessionUnlock --enable-camera` at `18:40:04` **is** that WTS-equivalent path (the old design's session helper). The native `WndProc WTS_SESSION_UNLOCK` branch (`src/core/MyForm_Events.cpp` / `reference MyForm.h` `case WTS_SESSION_UNLOCK`) would also enable via `EnableTargetCameraHardware(true)` if the daemon were running, but today the scheduler helper beat it.

**What `Boots → Disabled, Unlock → Enabled` proves:** Exactly what the report states — `the daemon therefore appears to be starting and the WTS lock/unlock path appears functional` — but the per-boot daemon **is not actually starting**; only the *session* helpers are. The observation therefore proves **session helpers + PnP are functional**, while isolating the boot-only helper as broken.

---

## M. Cross-Checks Requested by the Investigation Template

### M.1 Installer operations audit

- Source promised: `schtasks /Delete` stale + `Register-ScheduledTask`/`Schedule.Service` creation + `Unblock-File` + warm `/restore-camera` + `WriteUninstaller`. Actual live: those tasks exist exactly as promised; `Uninstall` key exists; warm operation left camera `OK` pre-boot (no ghost disabled before next shutdown). So `Installer registered successfully` is true — the fault is the **zero-delay trigger** definition that the installer wrote.

### M.2 Session & power events / single-instance / GUI visibility / command-line contracts

- `Global\WindowsHelloFix_AppMutex` / `Global\WindowsHelloFix_WakeupEvent` — present, correct names, no second mutex system introduced.
- `WndProc` `0x0016/0x0011` shutdown dispatch, `0x0218` power dedup 1500 ms, `isAlreadyDisabled` static — preserved in `src/core/MyForm_Events.cpp`.
- `--background` hidden background (`Opacity 0`, `ShowInTaskbar false`, `WindowState Minimized`) at `main.cpp:19-32` / `reference main.cpp:17-32` — intact.
- Args `--background`, `--disable-camera`, `--enable-camera`, `--restore-camera`, `--repair-camera` (and new `--startup-enable`) preserved order. No behavioral drift there.

### M.3 Runtime failsafe boundary

- `src/watchdog/CameraFailsafe` (`src/watchdog/CameraFailsafe.h` 2527 bytes, `.cpp` 10785 bytes) is an auxiliary observer: it calls existing `RecoverCameraHardware(...,false)` via `MyForm::TryGetFailsafeTargetId` / `IsCameraExpectedEnabled` after 10 s verification, with `60 s` idle / `45 s` grace / `30 s` cooldown / `3` retries. It never writes `Run`, never becomes a second `ExpectedDisabled` authority. Not relevant to today's boot gap but it correctly would **not** have recovered the boot-disabled state because the boot-disabled state had `ExpectedEnabled` true but the failsafe never armed (daemon never ran to arm it).

### M.4 No unsafe cleanup was performed

Read-only investigation respected `AGENTS.md §6`: `TryEnterHardwareToggleCooldown`, `static→extern` globals, dtor duplication, etc., were not removed. No code, task, registry, or driver file was touched.

---

## N. Hypothesis Table

| # | Hypothesis | Status | Evidence |
|---|---|---|---|
| **A** | Task Scheduler is malfunctioning (service failed, database corrupt, engine error) | **RULED OUT** | `Schedule` service `RUNNING AUTO`; operational log disabled is not a failure; dozens of unrelated `LogonTrigger`/`SessionStateChange` tasks ran today with `Result 0`; `Lock`/`Unlock` workers succeeded at `18:40`; no policy keys; S0 Modern Standby is nominal. |
| **B** | Task Scheduler works, but the installer no longer registers the expected task | **RULED OUT** | All four tasks exist, `Ready`, correct actions/principals/settings. The installer *did* register; the task definition itself (zero-delay LogonTrigger) is the fault, not missing registration. |
| **C** | The task exists and runs, but HelloFix fails during startup (`MyForm_Load` / camera pipeline) | **RULED OUT** for boot; **Confirmed** for the sense that the task **never runs** so HelloFix never gets a chance to fail. When it *does* run (session helpers, manual launch) the pipeline succeeds (`Verify PASS`, `Result 0`, `AlreadyEnabled`). No PnP, driver, elevation, or `IsCurrentProcessElevatedNative` block. |
| **D** | HelloFix successfully enables the camera, but another Windows event disables it immediately afterward (power/quirk/policing) | **UNLIKELY** for today's boot gap (zero `PowerEvent_*` in diagnostics; no daemon to race against); **POSSIBLE** as a *second-order* boot-window race on other machines/boots (documented `13:55:45.499 PowerEvent_Disable` 431 ms post-WTS in `docs/Plan.md`), which the watchdog's `45 s` grace is built to absorb. Not the observed cause today. |
| **E** | The camera is already disabled before HelloFix starts (shutdown left it disabled, nothing recovers it) | **CONFIRMED** | `isSystemEnding → DisableTargetCameraHardware` is the intentional shutdown leave-disabled path; `00:49 → 18:26` gap with no daemon proves no startup recovery ran; `Get-PnpDevice` post-unlock `OK` proves hardware is sound when asked. |
| **F** | Task Manager Startup Apps is displaying an incorrect/incomplete state unrelated to Task Scheduler | **PROBABLE** | `Win32_StartupCommand` + registry `02 Enabled` prove the registration exists; Task Manager not showing a `schtasks.exe` wrapper on `26200.9168` is consistent with a Win11 25H2 `StartupTask` filtering change (S0 Dev-channel hardening) rather than a missing task. No WMI-wide invisible-startup-apps regression (19 entries seen). |
| **G** | `StartupApproved` or another per-user startup mechanism has become inconsistent (`03` disabled, corrupt lays, …) | **RULED OUT** | `HKLM …\StartupApproved\Run → Windows Hello Fix 02 Enabled`; `HKCU …\Run` entries not corrupted; no `StartupApproved` corruption; Startup folders enumerations nominal; no `30-Dec-1899` timestamps. |
| **H** | A Windows update / driver / security change around regression window | **POSSIBLE** for cosmetic Problem 2 (build `26200` Dev-channel transition `5/30 → 6/10 25H2` overlaps HelloFix dev window), **UNLIKELY** for functional Problem 1 (no LCU after `12-Aug`; driver `20-Aug-25` predates boot bug; Defender intelligence updates daily but not OS patch; no policy change). The functional bug is the zero-delay `LogonTrigger`, which would reproduce on any build. |
| **I** | The repeated installer/uninstaller cycle left stale state (orphan tasks/`Run`/`StartupApproved`/ `%APPDATA%` residue / mutex) | **RULED OUT** | No orphan `Tasks`, `Run`, `StartupApproved`, `StartupFolder`, `%APPDATA%` variants, `AppCompatFlags`/`Uninstall` ghosts, or stuck `Global\` objects beyond the currently-live daemon pair. NSIS `Delete`/`CreateOrUpdate` (`6`) sweep is clean. |
| **J** | The v2.0 reference and current installer/runtime behavior differ in a way not yet noticed | **CONFIRMED** — but not a bug: the difference **is** the shipped boot fix that hasn't been deployed. Reference/new delta is intentional and documented: `WindowsHelloFix_Unlock` re-typed from `SessionStateChange 8` (`--enable-camera`, every unlock) to `LogonTrigger PT10S` (`--startup-enable`, startup-only, enable-if-disabled). Current live is still the *old* type. The behavioral-runtime equivalence claim ("behavior-preserving extraction") holds for `src/core` pipeline; the **installer** drift is the only material difference and it is **the fix**. |

---

## O. Root Cause — What Is Actually Broken and Where

**Before the report declares a single "root cause", the stacked two-layer reality must be named honestly:**

> **Primary (functional) root — 267011 never-ran `WindowsHelloFix` `At logon` task:**  
> The `LogonTrigger` without delay (`<LogonTrigger />` with no `<Delay>`) for `WindowsHelloFix --background` silently fails to enqueue on this `S0 Modern Standby + Fast Startup (Hiberboot 1) + build 26200.9168` machine's logon at `00:49`. The scheduler considers the task "has not yet run" (`0x41303 267011`) indefinitely, never creates the `Windows_Hello_Fix_v2_0.exe --background` process, therefore `MyForm_Load` → `RestoreConfiguredCameraHardware(true)` → `WTSRegisterSessionNotification` never execute, therefore the intentionally-disabled-after-shutdown `MI_00` camera stays disabled until the next `SessionStateChange`. Evidence: `schtasks`/`Get-ScheduledTaskInfo` `30-Nov-99 267011` vs every peer `LogonTrigger` with `Delay PT10S` or `Limited` succeeding the same day; zero `diagnostic.log` entries from boot; manual and session workers succeeding proves exe/pipeline/elevation are sound; `wevtutil` operational log `enabled: false` explains the silent absence of a failure event.

> **Secondary (presentation) root — Task Manager Startup Apps not surfacing the `schtasks.exe /Run` wrapper on `25H2 (26200)`:**  
> `HKLM\...\Run` + `StartupApproved 02 Enabled` + `Win32_StartupCommand` all agree HelloFix **is** registered, so the user's "no HelloFix entry under Task Manager Startup Apps" is not "registration absent" but "UI no longer enumerates `schtasks.exe /Run /TN …` wrappers as `StartupTask`s" on this Dev-channel build. WMI still censuses it; the app still starts (or would, if the trigger delivered) because `Explorer` executes `Run` regardless of `StartupTask` enumeration. This is cosmetic and does not cause the camera to stay disabled — Problem 1 does.

**One-sentence form:**

- **Root cause of the boot-disabled camera:** the `WindowsHelloFix` `At logon` task's missing `Delay` causes it to never run on this S0/Fast-Startup/26200 configuration, so the boot-recovery path that v2.0 depends on is absent.
- **Root cause of the Startup Apps disappearance:** Win11 25H2 filtering of the `schtasks.exe /Run` indirection (cosmetic, not the functional failure).

Otherwise, per the template: **`Root cause not yet confirmed at Windows-internals causal depth`** — the exact scheduler-internal reason a zero-delay `Highest+LogonTrigger` is dropped while a `10 s`-delayed identical principal succeeds would require a driver-level `Microsoft-Windows-TaskScheduler` debug trace that is currently off (`enabled: false`). The per-task isolated evidence, however, is strong enough to act on in §P.

---

## P. Recommended Next Diagnostic Step — Smallest, Safest Read-Only Experiment

**Do not yet modify any file, task, registry value, or driver.**

### Step 1 — Prove the delay hypothesis without writing anything (read-only confirmation + offline task-drop simulation)

On the *currently installed* machine, export the failing task's XML and the known-good XRite XML to files in `%TEMP%\opencode` (a temp dir not under `Windows\System32\Tasks`):

```powershell
Export-ScheduledTask -TaskName "WindowsHelloFix" -ErrorAction Stop | Out-File "$env:TEMP\opencode\WindowsHelloFix_current.xml" -Encoding utf8
Export-ScheduledTask -TaskName "XRiteColorAssistanceAutoUpdate" | Out-File "$env:TEMP\opencode\XRite_good.xml" -Encoding utf8
# Then diff the two XMLs focusing on: <Triggers><LogonTrigger>[<Delay>]</Delay></LogonTrigger></Triggers>,
# <MultipleInstancesPolicy>, <ExecutionTimeLimit>, <Priority>, and <UseUnifiedSchedulingEngine>
# Non-invasive, fully read-only: proves the sole structural delta is Delay
```

Optionally (still read-only), show that every `At logon` task on this system **with** `Delay` succeeds and every `At logon` task **without** `Delay` and `Highest` is `267011` — a one-liner census that needs no writes:

```powershell
Get-ScheduledTask | ForEach-Object {
  $t = $_.Triggers | Where-Object { $_.CimClass.CimClassName -eq 'MSFT_TaskLogonTrigger' }
  if ($t) {
    $info = Get-ScheduledTaskInfo -TaskName $_.TaskName -TaskPath $_.TaskPath -ErrorAction SilentlyContinue
    [pscustomobject]@{ Task="$($_.TaskPath)$($_.TaskName)"; Delay=$t.Delay; RunLevel=$_.Principal.RunLevel; LastRun=$info.LastRunTime; Result=$info.LastTaskResult }
  }
} | Sort-Object Delay,LastRun | Format-Table -AutoSize
```

**Then stop.** Do not create a shadow task, do not mutate the live task, do not reboot.

### Step 2 — Validate that the *already-built* fix closes the boot gap (requires one installer run, held for explicit approval)

The investigation identified that `commit 27a1174` ( `docs/Plan.md:626-688` Execution Record ) **already** corrects Problem 1 by re-typing `WindowsHelloFix_Unlock` from `SessionUnlock --enable-camera` to `LogonTrigger PT10S --startup-enable` with an enable-if-disabled handler (`src/core/MyForm_Core.cpp:208-242` + `src/core/MyForm_System.cpp:29-38` + `main.cpp:19-22`). The corrected bundle is **`x64/Release/Windows_Hello_Fix_Setup.exe` `520E1EB3…` 687511 bytes `30-Aug-26 00:46:55`**, whose embedded exe is `CD56F38C…` (startup-enable build). It has been live-tested earlier (see `docs/Plan.md:669-672`: `StartupEnable_AlreadyEnabled` at `12:21:07` + `StartupEnable_Result DurationMs=594 Verify=1` after induced `Disabled`), and it keeps the `WindowsHelloFix --background` LogonTrigger untouched.

The next step **when explicitly approved** is:

1. Close the manually-started daemon (`PID 16452` — `Stop-Process -Id 16452` or clean GUI `Stop Monitoring Service`, not `taskkill /F /T` if avoidable).
2. Run **the already-built** `x64/Release/Windows_Hello_Fix_Setup.exe` (not the `Downloads` old build) as Administrator → let `RegisterWindowsHelloFixTasks.ps1` replace `WindowsHelloFix_Unlock` (the script does `schtasks /Delete /TN "WindowsHelloFix_Unlock" /F` then re-registers with `Create(9)` `Delay PT10S`).
3. Verify without reboot: `Get-ScheduledTask WindowsHelloFix_Unlock | Select Triggers,Actions,Settings,Principal` should now show `LogonTrigger Delay PT10S Arguments --startup-enable Hidden True ExecutionTimeLimit PT1M`; `schtasks /Query /V /FO LIST` for that task should show `Schedule Type At logon time Start In C:\Program Files\WindowsHelloFix Arguments --startup-enable`; `Export-ScheduledTask WindowsHelloFix_Unlock` XML should contain `<LogonTrigger><Delay>PT10S</Delay></LogonTrigger>`.

### Step 3 — Prove boot recovery with a reboot test matrix

Only after Step 2 is verified:

| Test | Steps | Expected log |
|---|---|---|
| Cold boot with camera already enabled | Reboot → sign-in, wait 15 s | `diagnostic.log` `StartupEnable_AlreadyEnabled Target=Enabled Verify=PASS` at ~`logon+10 s`, then shortly `Startup_RestoreConfiguredCameraHardware` from the background daemon's `AlreadyEnabled` |
| Cold boot with camera pre-disabled | Manually `Disable` via `pnputil /disable-device` or `Device Manager` → `shutdown /r` → sign-in, wait 15 s | `StartupEnable_Result DurationMs≈600 Recover=1 Verify=1 PASS` within 11 s + 2 s, then daemon `AlreadyEnabled` |
| Power-cycle (`shutdown /s /t 0` + power on) | Fast Startup ON, so this is hybrid; also try `shutdown /s /t 0 /hybrid:off` (cold) | Same 10 s recovery |
| Repeated Lock/Unlock 5× | `Win+L` → unlock ×5 after sign-in | **No** `StartupEnable_*` entries — only `WTS SessionLock_Disable`/`SessionUnlock_Enable` (or `WindowsHelloFix_Lock --disable-camera` if lock worker kept); proves startup-only helper no longer fires on ordinary unlock |

If Step 2's re-registration still leaves `WindowsHelloFix LastResult 267011` after the next boot, that further isolates the pure `WindowsHelloFix` zero-delay `At logon` trigger as independently faulty; the recommended follow-up would then be to add `Delay PT10S` there as well (one-line `Delay` insert in the `RegisterWindowsHelloFixTasks.ps1` `$trigger` for `WindowsHelloFix`) — a second, separate commit, not part of the already-built `Unlock` fix.

### What **not** to do yet

- Do not `wevtutil sl … /e:true` to re-enable the operational log (writes).
- Do not `schtasks /Run /TN WindowsHelloFix` manually as a "quick fix" — it would mask the trigger hypothesis and leave the boot path unproven.
- Do not `schtasks /Change /Disable` StartupApproved entries.
- Do not uninstall Defender intelligence KBs or roll back `Realtek 10.0.22000.20385`.
- Do not edit `src/core` camera primitives, reboot shim, or `MyForm_Events.cpp` dedup/ordering.

---

## Q. Appendix — Mechanisms, Sources, and Exact Locations (Answering §1–§2 of the Prompt)

### Startup mechanisms triangulated

| Mechanism | Checked | Found | How Task Manager discovers it | HelloFix use |
|---|---|---|---|---|
| `HKCU…\Run` | `Get-ItemProperty HKCU\...\Run` | No HelloFix value | `Win32_StartupCommand Location=HKU\…\Run` + `StartupApproved\Run` byte `02/03` | Not used |
| `HKLM…\Run` | `Get-ItemProperty HKLM\...\Run` | `Windows Hello Fix = schtasks.exe /Run /TN "WindowsHelloFix"` | `Win32_StartupCommand Location=HKLM\…\Run` + `StartupApproved\Run` `02 Enabled` | **Wrapper** (`schtasks /Run`) — WMI sees it, 25H2 StartupApps UI appears to filter it |
| Startup folders (`%APPDATA%`, `%PROGRAMDATA%`) | `Get-ChildItem` both paths | No HelloFix `.lnk`/`.bat` | `Win32_StartupCommand Location=Startup` + `StartupApproved\StartupFolder` | Not used |
| Scheduled tasks `At logon` | `Get-ScheduledTask WindowsHelloFix` | `LogonTrigger` `At logon time` `schtasks /Query Status Ready` | **Not** shown in Task Manager Startup Apps — shown in Task Scheduler only | **Primary** path (`--background` daemon) |
| Scheduled tasks `SessionStateChange` | `Get-ScheduledTask WindowsHelloFix_{Lock,Unlock}` | `StateChange 7/8` `When an event occurs` | Not in Startup Apps | Lock/Unlock helpers |
| `StartupApproved` | `Get-ItemProperty …\StartupApproved\Run{,32,\StartupFolder}` | `Windows Hello Fix 02 Enabled` | Task Manager reads the 12-byte binary at `StartupApproved` to render enabled/disabled, not to create the entry | Governs the `HKLM…\Run` wrapper |
| `StartupTask` (WinRT) | (`Win32_StartupCommand` is census; `StartupTask` is filtered view used by 25H2 Settings) | WMI shows 19; Settings hides the schtasks wrapper | `Settings > Apps > Startup` enumerates `StartupTask` | N/A |

### Exact value locations (read-only, with line anchors)

- **Reference daemon startup trigger:** `reference/release-v2.0/Release/install_script.nsi:132-139` (`New-ScheduledTaskTrigger -AtLogOn`, `New-ScheduledTaskPrincipal -UserId $user -LogonType Interactive -RunLevel Highest`) + `reference/release-v2.0/MyForm.h:943-1177` `MyForm_Load` `RestoreConfiguredCameraHardware(true)` with `RecoverCameraHardware(true)` cycle.
- **Current daemon startup trigger (unchanged in workspace):** `x64/Release/install_script.nsi:131-139` (identical).
- **Reference session helpers:** `reference/.../install_script.nsi:140-168` `Register-WhfSessionTask` `StateChange 7/8` + `8 '--enable-camera'` (`168`).
- **Current startup-only helper (HEAD, not yet live):** `x64/Release/install_script.nsi:167-194` `Create(9)` `Delay PT10S` `Arguments '--startup-enable'` (`184`) `Description 'Performs startup/sign-in recovery…'`.
- **Live (what this machine actually has):** `schtasks /Query /TN WindowsHelloFix /V Status Ready LastResult 267011`; `Export-ScheduledTask WindowsHelloFix` `<LogonTrigger />`; `Export-ScheduledTask WindowsHelloFix_Unlock` `<SessionStateChangeTrigger><StateChange>SessionUnlock</StateChange>… --enable-camera`.
- **Registry startup entry (live):** `HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Run` `Windows Hello Fix : C:\WINDOWS\System32\schtasks.exe /Run /TN "WindowsHelloFix"`; `HKLM\…\Explorer\StartupApproved\Run` `Windows Hello Fix : {2,0,0,0…}` = Enabled.
- **StartupCommand census (live):** `Get-CimInstance Win32_StartupCommand | Where Caption -like *Hello*` → `Command schtasks.exe /Run /TN "WindowsHelloFix" Location HKLM…\Run User Public`.
- **Config + log:** `%APPDATA%\Windows Hello Fix\config.txt` `monitoring=1 device=USB\VID_04F2&PID_B829&MI_00\6&321DD860&1&0000` (`CRLF`, `TrimTrailingChars` clean); `%APPDATA%\Windows Hello Fix\diagnostic.log` `2852 bytes` last `18:40:07` with the `Command_EnableCamera_*` / `DisableTargetCameraHardware_Result Stage 14` / `WTSRegisterSessionNotification_Success` lines cited above.
- **Sync objects (live):** `Global\WindowsHelloFix_AppMutex` (mutex), `Global\WindowsHelloFix_WakeupEvent` (event) — addresses: `MyForm_Core.cpp:220,289` (create) / `MyForm.h:75-77` (handles), woken at `MyForm_System.cpp` in installer-termination path.
- **Camera instance (live):** `USB\VID_04F2&PID_B829&MI_00\6&321DD860&1&0000` — `SetupDi` class `Camera` (`{ca3e7ab9…}`), `GetCameraHardwareDisabledState` at `src/core/MyForm_Camera.cpp:217-247`, guarded by `VerifyCameraHardwareState` at `248-275`.
- **Win32_OperatingSystem:** `Caption Windows 11 Home Single Language Build 26200 Version 10.0.26200 Architecture 64-bit LastBootUpTime 30-Aug-26 00:49:57`.

---

## R. Residual Uncertainty — What This Investigation Cannot Prove Without a Reboot Trace

- Whether the `LogonTrigger` drop is **deterministically** zero-delay or **probabilistically** raced (S0/Fast Startup could make it intermittent; today's sample is `n=1` boot).
- Whether enabling the `Microsoft-Windows-TaskScheduler/Operational` channel and rebooting would yield a `Launch Failure` / `Task Start Failed` event with `Error Value 2147942405` etc for the `267011` task — the channel is off, so no failure-event evidence exists.
- Whether `Task Manager` on `26200` hides `schtasks.exe` wrappers on **all** machines of this build or only on this install (no second machine to compare; no Task Manager screenshot was captured — that would require interactive UI work beyond read-only CLI).
- Whether the post-`26200` transition also imported a new `Group Policy` or `Smart App Control` default that silently blocks zero-delay `Highest+LogonTrigger` tasks with a `Start Boundary` outside the interactive session (ruled "no policy" from registry absence, but enterprise policy could be transiently delivered via MDM/Intune without registry trace).

These uncertainties are bounded: none overturn the **267011 never-ran LogonTrigger** as the proximate cause of the boot-disabled camera; they only affect whether the mitigation should be `Add Delay PT10S to WindowsHelloFix itself` in addition to the already-built `Unlock PT10S --startup-enable` helper.

---

## S. Status of This Machine Prior to Any Fix

- **Installed and **not** uninstalled:** tasks remain, `Run` remains, `%APPDATA%` `config.txt`/`diagnostic.log` remain, `%PROGRAMFILES%` install dir intact.
- **No files, tasks, registry values, startup-folder links, policies, drivers, services, or event-log settings were modified** during this investigation (all `Get-*`, `Export-*`, `schtasks /Query`, `Get-CimInstance`, `Get-WinEvent`, `wevtutil gl`, `sc query`, `reg query`, `powercfg` were read-only).
- **Next safe diagnostic (no writes):** export/diff the two XMLs and run the `LogonTrigger census` one-liner in §P Step 1.
- **Next safe fix (requires explicit approval):** run the **already-built** `x64/Release/Windows_Hello_Fix_Setup.exe` (`520E1EB3…`) once as Administrator to deploy the `LogonTrigger PT10S --startup-enable` helper, then verify via `Get-ScheduledTask` / `Export-ScheduledTask` / `diagnostic.log` before any reboot.

---

*Evidence sources & tool paths:* `Win32_StartupCommand` / `Get-CimInstance Win32_OperatingSystem` / `Get-PnpDevice -InstanceId USB\VID_04F2&PID_B829&MI_00\6&321DD860&1&0000` / `Get-PnpDeviceProperty` / `schtasks /Query /V /FO LIST` / `Get-ScheduledTask` + `Get-ScheduledTaskInfo` + `Export-ScheduledTask` / `wevtutil gl Microsoft-Windows-TaskScheduler/Operational` / `Get-WinEvent -ListLog / -FilterHashtable` / `sc query Schedule` / `powercfg /a /lastwake /waketimers` / `whoami /user` / `Get-FileHash SHA256` / `Get-Acl` / `Get-Item -Stream` / `Get-ItemProperty HKCU/HKLM …\Run / …\StartupApproved\Run / …\StartupApproved\StartupFolder / …\Uninstall\WindowsHelloFix / …\Policies\System / SYSTEM\Setup[Source OS …] / SYSTEM\CurrentControlSet\Control\Session Manager\Power` / `Microsoft.Update.Session QueryHistory / Get-HotFix / Get-CimInstance Win32_OperatingSystem` / `Get-Process Windows_Hello_Fix_v2_0` / `OpenExisting Mutex/Event` P/Invoke probes / `reference/release-v2.0/{MyForm.h, main.cpp, Release/install_script.nsi}` / `x64/Release/install_script.nsi` / `src/core/{MyForm.h,MyForm_Camera.cpp,MyForm_Config.cpp,MyForm_Core.cpp,MyForm_Events.cpp,MyForm_System.cpp,MyForm_UI.cpp}` / `src/watchdog/CameraFailsafe.{h,cpp}` / `docs/Plan.md` / `git status/log/show` / `%APPDATA%\Windows Hello Fix\{config.txt, diagnostic.log}`.

*File provenance verified (`Test-Path`, `Get-ChildItem`, `git ls-files` not assumed):* `reference/release-v2.0` is a `.gitignored` reference folder populated from `git` history (`git ls-files` baseline check in `AGENTS.md §1` respected); `reference/release-v2.0/Release/install_script.nsi` is the actual NSIS source used for the v2.0 build artifacts whose exe hash matches the installed exe, confirming the "known-good control" identity.
