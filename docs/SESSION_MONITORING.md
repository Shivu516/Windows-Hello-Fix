# Session, Power and Lid Monitoring (as-built)

Baseline: branch `test`, commit `acc37d8`. Traces the exact native event path
from Windows to camera action. **No code was modified.**

## 1. Registration (who listens for what)

`ApplicationController::Initialize` → `NotificationRegistrar`:

- **Session events:** `RegisterSessionNotificationWithRetry(hwnd)` calls
  `WTSRegisterSessionNotification(hwnd, NOTIFY_FOR_THIS_SESSION)`, retrying up
  to 6 times with 500 ms sleeps (total ≤ ~3 s). Success/failure logged as
  `WTSRegisterSessionNotification_Success` /
  `WTSRegisterSessionNotification_Failed_LastError=N`.
  Unregistration: `WTSUnRegisterSessionNotification`.
- **Power settings:** `RegisterPowerSettingNotification(hwnd, &guid,
  DEVICE_NOTIFY_WINDOW_HANDLE)` twice — once for
  `GUID_LIDSWITCH_STATE_CHANGE` `{BA3E0F4D-B817-4094-A2D1-D56379E6A0F3}`
  and once for `GUID_POWER_BUTTON_TIMESTAMP`
  `{A70AFB22-3816-4584-9F24-810A4E2747FB}`. Handles stored as
  `m_hLidNotification` / `m_hButtonNotification`, unregistered via
  `UnregisterPowerSettingNotification`.
- Notifications are registered only after the startup camera restore and
  wake-listener start; events fired earlier are missed by design.

## 2. Message flow (lock/unlock)

```
Windows session change
    ↓ WM_WTSSESSION_CHANGE delivered to MyForm's HWND
MyForm::WndProc (src/ui/MyForm.cpp)
    ↓ logs "SessionEvent_Received_Code=<wparam>" for EVERY session message
    ↓ EventCooldown::ShouldSuppressSessionEvent(code, GetTickCount64())
    │     same code within 1500 ms → suppressed; logs "SessionEvent_DedupIgnored"
    ↓ WinEventDecoder::DecodeSessionEvent(wparam):
    │     WTS_SESSION_LOCK   (=7) → SystemEvent::SessionLock
    │     WTS_SESSION_UNLOCK (=8) → SystemEvent::SessionUnlock
    │     anything else            → SystemEvent::SessionOther
    ↓ ApplicationController::HandleSessionEvent(ev):
        !IsMonitoring        → log "SessionEvent_Ignored_MonitoringOff" (no action)
        SessionLock          → DisableTargetCameraHardware(true)
                               → log "SessionLock_Disable"
        SessionUnlock        → EnableTargetCameraHardware(false)
                               → log "SessionUnlock_Enable"
        SessionOther         → nothing
```

Raw codes seen in the log line: 7 = lock, 8 = unlock; WTS also delivers
console/remote connect-disconnect codes (1–4), which decode to
`SessionOther` and are ignored.

## 3. Message flow (power / lid / button)

```
WM_POWERBROADCAST (0x0218)
MyForm::WndProc
    ↓ EventCooldown::ShouldSuppressPowerEvent(powerEvent, tick)
    │     identical powerEvent code within 1500 ms → "PowerEvent_DedupIgnored"
    ↓ WinEventDecoder::DecodePowerEvent(wparam, lparam):
        PBT_APMSUSPEND           (0x0004) → PowerSuspend
        PBT_POWERSETTINGCHANGE   (0x8013) → inspect POWERBROADCAST_SETTING:
             GUID == lid    GUID → PowerSettingLid
             GUID == button GUID → PowerSettingButton
             any other GUID      → PowerSettingOther
             null lParam         → treated as PowerSuspend
        PBT_APMRESUMESUSPEND     (0x0007) → PowerResumeSuspend
        PBT_APMRESUMEAUTOMATIC   (0x0012) → PowerResumeAutomatic
        anything else                     → PowerOther
    ↓ PowerSettingOther → log "PowerSetting_IrrelevantGuid", no action
    ↓ ApplicationController::HandlePowerEvent(ev):
        PowerSuspend | PowerSettingLid | PowerSettingButton
            if IsMonitoring && !IsAlreadyDisabled:
                IsAlreadyDisabled = true
                DisableTargetCameraHardware(true)
                log "PowerEvent_Disable"
                Sleep(500)                       ← blocks UI thread
        PowerResumeSuspend | PowerResumeAutomatic
            if IsMonitoring:
                Thread::Sleep(1000)              ← blocks UI thread
                EnableTargetCameraHardware(false)
                log "PowerEvent_Enable"
                IsAlreadyDisabled = false
        others → nothing
```

Note: resume handling runs on either resume code; because Windows usually
sends `PBT_APMRESUMEAUTOMATIC` followed by `PBT_APMRESUMESUSPEND`, the 1500 ms
session/power dedup does NOT cover these (different codes), so the enable can
run more than once per wake — each pass is cheap due to the already-enabled
short-circuit.

## 4. Shutdown / logoff path

```
WM_QUERYENDSESSION (0x0011) or WM_ENDSESSION (0x0016)
MyForm::WndProc
    ↓ IsSystemEnding = true
    ↓ ApplicationController::HandleSystemEnd(hwnd):
        log "SystemEnd_Begin"
        if IsMonitoring: DisableTargetCameraHardware(true); log "SystemEnd_Disable"
        UnregisterSessionNotification(hwnd)
    ↓ base Form::WndProc(m)
later teardown (~MyForm / !MyForm → Shutdown(IsSystemEnding=true)):
    disable again (already-disabled short-circuit applies),
    save config with monitoring=1 and current device,
    signal+close wake event, unregister power notifications, release mutex.
```

## 5. Cooldown mechanics

`EventCooldown` keeps two static pairs (`lastCode`, `lastTick`) — one for
power, one for session. Suppression rule: *same raw code within 1500 ms of the
previous one* → drop. The first occurrence updates the tracker and passes.
The cooldown is per-process only; it cannot coordinate with concurrently
running scheduled-task instances (see `TASK_SCHEDULER.md`).

## 6. Monitoring gate semantics

- All lock/unlock reactions require `IsMonitoring == true` (set by
  `ToggleMonitoring`, or forced true at Load when launched with `--background`
  or when config says `monitoring=1`).
- Power/lid/button reactions additionally use the `IsAlreadyDisabled`
  latch so repeated suspend notifications don't re-disable.
- When monitoring is off the app still logs received session events
  (`MonitoringOff` target) — useful for diagnosing "nothing happens" reports.

## 7. Uncertainties (documented, not fixed)

- The lid/button GUID handlers treat *any* lid or button notification as a
  suspend-like trigger regardless of the new state value inside the
  `POWERBROADCAST_SETTING` payload; opening the lid fires the same disable as
  closing it.
- Session registration happens after several seconds of startup work; lock
  events during that window are lost silently.
