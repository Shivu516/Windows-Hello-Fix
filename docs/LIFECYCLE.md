# Lifecycle: Startup & Shutdown

## Process startup

```
1. main.cpp
     [STAThread] main(array<String^>^ args)
       - EnableVisualStyles, SetCompatibleTextRenderingDefault(false)
       - MyForm form;
       - if any arg in {--background,/background,--disable-camera,/disable-camera,
                         --enable-camera,/enable-camera,/restore-camera,/repair-camera}:
           form.Opacity = 0; ShowInTaskbar=false; WindowState=Minimized
       - Application::Run(%form)
```

## WinForms initialization

```
2. MyForm() ctor                      [MyForm_Core.cpp]
     - init all members (false/NULL/new)
     - InitializeComponent()          [builds UI, wires events]
3. Application::Run dispatches messages; Load event fires -> MyForm_Load
```

## MyForm_Load — exact chronological sequence

```
4.  Read command line; detect /background -> launchRequestedBackground
5.  WriteDiagnosticLog("Startup_Context", NoChange, elevated)
6.  if IsRestoreCameraCommand(args):
        hide; RestoreConfiguredCameraHardware(true); log; Environment::Exit(0)
7.  if IsDisableCameraCommand(args):
        hide; DisableTargetCameraHardware(true); verify; log; Environment::Exit(0)
8.  hAppMutex = CreateMutex(Global\WindowsHelloFix_AppMutex)
      if ERROR_ALREADY_EXISTS:
        hOpen = OpenEvent(Global\WindowsHelloFix_WakeupEvent)
        if hOpen: SetEvent; Sleep(200); log; Exit(0)
        if launchRequestedBackground: log; Exit(0)
        else: "Already Running" dialog
              Yes -> recover saved/MI_00 device; SaveConfigState(true);
                     system("taskkill /F /IM ...exe /T"); Sleep(500);
                     Application::Restart();
              Exit(0)
9.  hWakeupEvent = CreateEvent(Global\WindowsHelloFix_WakeupEvent)
10. RestoreConfiguredCameraHardware(true)   // before dropdown build
11. RegisterPowerSettingNotification(lid + button) -> hLid/hButton
12. *cachedCameras = ScanSystemCameras(); LoadConfigState -> shouldAutoStartByConfig
13. Populate deviceDrop; choose savedIdx / autoIdx(MI_00) / 0
14. set *selectedInstanceId; EnsureConfigFileExists(...)
15. if selected: EnableTargetCameraHardware(shouldAutoStartByConfig)
16. if (background || shouldAutoStart) && selected:
        isMonitoring=true; EnableTargetCameraHardware(false);
        disable dropdown; "Stop Monitoring Service"; green;
        if background: hide/minimize
    else: isMonitoring=false; "Start Monitoring Service"; gray
17. backgroundWorker = Thread(ListenForWakeupSignal); IsBackground=true; Start()
18. for attempt 0..5:
        if WTSRegisterSessionNotification(handle, NOTIFY_FOR_THIS_SESSION): break
        Sleep(500)
    log success or last error
```

## Steady state

Window visible (or hidden if background), `isMonitoring` possibly true, wake listener running, power + WTS notifications registered. The app now reacts to lock/unlock/suspend/resume/shutdown via `WndProc`.

## Monitoring activation (user)

`btnToggle_Click` start path: set `selectedInstanceId`, `isMonitoring=true`, `SaveConfigState(true,id)`. (No immediate camera toggle — camera was already enabled at startup.)

## Monitoring deactivation (user)

`btnToggle_Click` stop path: `EnableTargetCameraHardware(false)`, `SaveConfigState(false,id)`, clear selection.

## Normal shutdown (user closes window)

`MyForm_FormClosing`: `CloseReason==UserClosing` → `e->Cancel=true`, `Hide()`, `ShowInTaskbar=false`, first time shows info box and sets `isBackgroundMode=true`. The process keeps running in the background; the window can be restored via the wake signal.

## System shutdown / logoff

`WndProc` receives `0x0011`/`0x0016` → `isSystemEnding=true`; if monitoring, `DisableTargetCameraHardware(true)`; `WTSUnRegisterSessionNotification`. Then the CLR tears down the form → destructor/finalizer runs.

## Destructor `~MyForm`

```
keepListening = false
if isSystemEnding:
    DisableTargetCameraHardware(true)
    if device selected: SaveConfigState(true, id)
else if device selected:
    EnableTargetCameraHardware(false)
    SaveConfigState(true, id)
else:
    RestoreConfiguredCameraHardware(false)
SetEvent + CloseHandle(hWakeupEvent)
UnregisterPowerSettingNotification(hLid/hButton)
delete cachedCameras; delete selectedInstanceId; delete components
CloseHandle(hAppMutex)
```

## Finalizer `!MyForm`

Same native/hardware cleanup minus the `SaveConfigState` calls; it is the GC safety net.

## Key timing summary

| Phase | Duration |
|---|---|
| WTS registration retries | up to 6 × 500 ms = 3 s |
| Startup camera restore (cycle) | ~1.75 s + verify |
| Suspend handler | disable + 500 ms |
| Resume handler | 1000 ms + enable |
| Shutdown handler | disable (verified) |

All timings are inherited unchanged from `release-v2.0/MyForm.h`.
