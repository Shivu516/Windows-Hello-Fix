# Data Flow

This document describes the important runtime flows of Windows Hello Fix. All
flows are derived from the v2.0 implementation; the target architecture preserves
them, with the same operations routed through the future modules shown in
parentheses.

## Flow Legend

The following pipelines are used by the flows below:

- **Decode** (`events/WinEventDecoder`): classify a Win32 `(msg, wParam, lParam)`
  into a semantic `SystemEvent`.
- **Debounce** (`events/EventCooldown`): suppress an identical event within
  1500 ms.
- **Policy** (`application/ApplicationController::HandleEvent`): gate on
  `isMonitoring`, apply the hardware-state lock, select the camera operation.
- **Hardware** (`camera/`): perform and verify the device operation.

## 1. Startup

```
main.cpp
  → parse args (application/CommandLine): background / command mode?
  → create MyForm (ui/)
  → Application::Run (message loop)

MyForm_Load (ui/, orchestrated by application/ApplicationController):
  → log startup context (elevation, integrity RID, exe, cwd, config path)
  → if restore command  (/restore-camera, /enable-camera, --enable-camera, /repair-camera)
        → restore configured camera (cycle) → Environment::Exit(0)
  → if disable command  (/disable-camera, --disable-camera)
        → disable + verify target → Environment::Exit(0)
  → acquire single-instance mutex (system/SingleInstance)
        → if already running:
            → signal wake event → Sleep(200) → exit (quiet)
            → else if background launch → exit (quiet)
            → else prompt force reset:
                 recover camera → taskkill old process → Application::Restart()
  → create wake event
  → startup recovery: RestoreConfiguredCameraHardware(cycle=true)   (camera/)
  → register power notifications (events/NotificationRegistrar):
        GUID_LIDSWITCH_STATE_CHANGE, GUID_POWER_BUTTON_TIMESTAMP
  → scan cameras (camera/CameraDevice::ScanSystemCameras) → populate dropdown
  → select target: saved config → MI_00 → first device
  → force-enable target camera
  → if background OR config monitoring=1:
        → start monitoring (isMonitoring=true), lock dropdown, update UI
        → hide window if background
  → start wake-up listener thread
  → register WTSRegisterSessionNotification (6 × 500 ms retry)
```

Behavioral notes preserved: startup recovery happens *before* dropdown building
so a disabled device is re-enumerated and appears; startup does not force a
disable→enable bounce unless config requests auto-start.

## 2. Session Lock

```
Windows sends WM_WTSSESSION_CHANGE, wParam = WTS_SESSION_LOCK
  → Decode  → SystemEvent::SessionLock
  → Debounce: if same event within 1500 ms → suppress (log SessionEvent_DedupIgnored)
  → Policy: if !isMonitoring → log MonitoringOff, no action
            else → DisableTargetCameraHardware(retry=true)   (application/ → camera/)
  → Hardware:
        resolve target (selection → config → MI_00 → first)
        if already disabled → log AlreadyDisabled, done
        SetCameraHardwareStateVerified(target, enable=false, reinit=true)
            → check-before-change (verify already-disabled?)
            → up to 3 attempts: SetupAPI toggle → verify (3 × 100 ms)
                                CFGMGR toggle → verify
                                on mismatch: reinit by opposite toggle + Sleep(250)
            → final retry + verify
  → Log: SessionLock_Disable | Verify=PASS/FAIL (diagnostic.log)
```

## 3. Session Unlock

```
Windows sends WM_WTSSESSION_CHANGE, wParam = WTS_SESSION_UNLOCK
  → Decode  → SystemEvent::SessionUnlock
  → Debounce (1500 ms)
  → Policy: if !isMonitoring → ignore
            else → EnableTargetCameraHardware(cycle=false)
  → Hardware:
        if already enabled → log AlreadyEnabled, done
        RecoverCameraHardware(target, cycleDevice=false)
            → SetCameraHardwareStateVerified(target, enable=true, reinit=false)
  → Log: SessionUnlock_Enable | Verify=...
```

## 4. Sleep (Suspend)

```
Windows sends WM_POWERBROADCAST, wParam = PBT_APMSUSPEND (0x0004)
  → Decode → SystemEvent::Suspend
  → Debounce (1500 ms per power event)
  → Policy: if isMonitoring AND !isAlreadyDisabled (hardware-state lock):
        → set isAlreadyDisabled = true
        → DisableTargetCameraHardware(retry=true)
        → Sleep(500)   (500 ms safety window — preserved behavior)
  → Log: PowerEvent_Disable
```

## 5. Lid Close / Power Button (PBT_POWERSETTINGCHANGE)

```
Windows sends WM_POWERBROADCAST, wParam = PBT_POWERSETTINGCHANGE (0x8013),
        lParam → POWERBROADCAST_SETTING
  → Decode:
        if PowerSetting GUID is neither GUID_LIDSWITCH_STATE_CHANGE
           nor GUID_POWER_BUTTON_TIMESTAMP → SystemEvent::PowerSettingOther
           → log PowerSetting_IrrelevantGuid, drop
        else → SystemEvent::PowerSettingLid / PowerSettingButton
  → Policy: same as Suspend (disable + isAlreadyDisabled lock + 500 ms)
  → Log: PowerEvent_Disable
```

## 6. Resume (Wake)

```
Windows sends WM_POWERBROADCAST, wParam = PBT_APMRESUMESUSPEND (0x0007)
        or PBT_APMRESUMEAUTOMATIC (0x0012)
  → Decode → SystemEvent::Resume
  → Policy: if isMonitoring:
        → Thread::Sleep(1000)   (allow device tree rebuild — preserved)
        → EnableTargetCameraHardware(cycle=false)
        → isAlreadyDisabled = false   (release lock)
  → Log: PowerEvent_Enable
```

## 7. Device Discovery

```
ScanSystemCameras (camera/CameraDevice):
  → SetupDiGetClassDevs(NULL, ..., DIGCF_ALLCLASSES | DIGCF_PRESENT)
  → for each device: SetupDiGetDeviceRegistryProperty(SPDRP_CLASS)
  → keep class == "Camera" or "Image"
  → collect { friendlyName (SPDRP_DEVICEDESC), instanceId (SetupDiGetDeviceInstanceId) }
  → SetupDiDestroyDeviceInfoList
```

Target identification order (`application/ApplicationController`):
current dropdown selection → saved config `device=` → first instance ID
containing `MI_00` → first scanned device. Instance-ID matching is exact or
case-insensitive.

## 8. Device State Change (Disable / Enable) with Verification

```
SetCameraHardwareStateVerified(target, enable, reinitOnMismatch)  (camera/CameraRecovery)
  → if target empty → fail
  → check-before-change: VerifyCameraHardwareState(target, shouldBeDisabled)
        → already in target state → success (no churn)
  → loop up to 3 attempts:
        → ToggleCameraHardware(target, enable)          (SetupAPI path)
        → VerifyCameraHardwareState (3 × 100 ms)        → success
        → ToggleCameraHardwareCfgMgr(target, enable)    (CFGMGR32 path)
        → VerifyCameraHardwareState                     → success
        → if reinitOnMismatch:
            toggle opposite state via both paths
            Sleep(250)
        → Sleep(250)
  → final ToggleCameraHardware + verify → success or fail
  → record cooldown timestamp on success
```

State query details (`camera/CameraHardware::GetCameraHardwareDisabledState`):
`CM_Get_DevNode_Status` problem code `CM_PROB_DISABLED` (22) **or**
`SPDRP_CONFIGFLAGS & CONFIGFLAG_DISABLED` (1).

## 9. Retry / Self-Healing / Recovery

```
RecoverCameraHardware(target, cycleDevice)  (camera/CameraRecovery)
  → SetCameraHardwareStateVerified(target, enable=true, reinit=false)
  → if cycleDevice:
        Sleep(350) → disable → Sleep(900) → enable → Sleep(500) → enable
        (power-cycle timing preserved exactly)
```

Used at: startup recovery (cycle=true), unlock (cycle=false), stop-button
(cycle=false), destructor normal exit (cycle=false), command restore
(cycle=true), force-reset path (cycle=true). `RestoreAllCameraHardware`
re-applies recovery to every scanned camera when no configured target exists.

## 10. Shutdown / Logoff

```
Windows sends WM_QUERYENDSESSION (0x0011) / WM_ENDSESSION (0x0016)
  → Decode → SystemEvent::SystemEnding
  → Policy: isSystemEnding = true
        → if isMonitoring: DisableTargetCameraHardware(retry=true)
        → WTSUnRegisterSessionNotification
  → Log: SystemEnd_Begin / SystemEnd_Disable

Destructor / finalizer (ui/MyForm, orchestrated by application/):
  → if isSystemEnding:
        → DisableTargetCameraHardware(true)
        → SaveConfigState(monitoring=true, device)     (camera stays disabled for next boot)
  → else if a device is selected:
        → EnableTargetCameraHardware(cycle=false)      (camera re-enabled on normal exit)
        → SaveConfigState(monitoring=true, device)
  → else:
        → RestoreConfiguredCameraHardware(cycle=false)
  → release wake event, power notifications, native buffers, mutex
```

## 11. Stop Monitoring (UI)

```
btnToggle_Click (ui/) → Stop:
  → isMonitoring = false
  → EnableTargetCameraHardware(cycle=false)
  → SaveConfigState(monitoring=false, device)
  → clear selectedInstanceId, re-enable dropdown, update status labels
```

## 12. Single-Instance / Wake-Up

```
Second instance starts:
  → CreateMutex(Global\WindowsHelloFix_AppMutex) → ERROR_ALREADY_EXISTS
  → OpenEvent(Global\WindowsHelloFix_WakeupEvent) → SetEvent → Sleep(200) → exit
  → (first instance's wake-up listener thread wakes → BringWindowToFrontDelegate)
First instance shutdown:
  → SetEvent(hWakeupEvent) → CloseHandle
```

## Timing Constants (Behavioral Contract)

| Constant | Value | Where used |
|---|---|---|
| Session/power event debounce | 1500 ms | `WndProc` static dedup |
| Hardware toggle cooldown | (dormant) | `TryEnterHardwareToggleCooldown` — never called |
| Verification attempts | 3 × 100 ms | `VerifyCameraHardwareState` |
| Toggle retry attempts | 3 (250 ms sleeps) | `SetCameraHardwareStateVerified` |
| Reinit sleep | 250 ms | mismatch reinitialization |
| Power-disable safety window | 500 ms | suspend/lid/button |
| Resume delay | 1000 ms | resume handling |
| Recovery cycle | 350 / 900 / 500 ms | `RecoverCameraHardware` cycle |
| Wake signal wait | 200 ms | second-instance wake path |
| WTS registration retry | 6 × 500 ms | startup registration |

## Related Documents

- `overview.md` — responsibility boundaries.
- `dependency-map.md` — module relationships.
- `target-architecture.md` — module contracts.