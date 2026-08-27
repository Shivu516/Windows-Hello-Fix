# `src/core/MyForm_Events.cpp` — `WndProc` Event Dispatch

**Path:** `src/core/MyForm_Events.cpp`
**Lines:** 110
**Included by:** built directly; `#include "MyForm.h"`

## Purpose

Contains the overridden `MyForm::WndProc`, the single Windows message dispatcher for the form. All session-lock, power, and shutdown behavior is centralized here. It is the **only** place that reacts to `WM_WTSSESSION_CHANGE`, `WM_POWERBROADCAST`, and the shutdown/logoff messages.

## Static deduplication state (6–10)

Declared `static` inside `WndProc` (function-local, persists across calls):
- `isAlreadyDisabled` — hardware-state lock so suspend does not disable repeatedly.
- `lastSessionEventTick` / `lastSessionEventCode` — session-event debounce.
- `lastPowerEventTick` / `lastPowerEventCode` — power-event debounce.
- `nowTick = GetTickCount64()` — captured once per message.

## Message handling

### 1. Shutdown / Logoff (14–22)
`if (m.Msg == 0x0016 /*WM_ENDSESSION*/ || m.Msg == 0x0011 /*WM_QUERYENDSESSION*/)`:
- `isSystemEnding = true`.
- Log `SystemEnd_Begin`.
- If `isMonitoring`: `DisableTargetCameraHardware(true)`; log `SystemEnd_Disable`.
- `WTSUnRegisterSessionNotification(this->Handle)`.

### 2. Power Broadcast (25–74) — `m.Msg == 0x0218` (`WM_POWERBROADCAST`)
- `powerEvent = m.WParam`.
- **Debounce:** if same `powerEvent` within **1500 ms**, log `PowerEvent_DedupIgnored`, call `Form::WndProc(m)`, return.
- Update `lastPowerEventCode`/`lastPowerEventTick`.

**Sleep / low-power intercept** (`powerEvent == 0x0004` `PBT_APMSUSPEND` or `0x8013` `PBT_POWERSETTINGCHANGE`):
- Only if `isMonitoring && !isAlreadyDisabled`.
- If `0x8013`: cast `LParam` to `POWERBROADCAST_SETTING*`; if the GUID is **not** lid-switch or power-button, log `PowerSetting_IrrelevantGuid`, call base `WndProc`, return (ignore unrelated setting changes).
- Set `isAlreadyDisabled = true` (structural lock).
- `DisableTargetCameraHardware(true)`; log `PowerEvent_Disable`.
- `Sleep(500)` — "critical time window bypass" safety delay.

**Resume** (`powerEvent == 0x0007` `PBT_APMRESUMESUSPEND` or `0x0012` `PBT_APMRESUMEAUTOMATIC`):
- If `isMonitoring`: `System::Threading::Thread::Sleep(1000)` (let device tree rebuild), then `EnableTargetCameraHardware(false)`; log `PowerEvent_Enable`; reset `isAlreadyDisabled = false`.

### 3. Session Lock / Unlock (77–105) — `m.Msg == WM_WTSSESSION_CHANGE`
- `sessionEvent = m.WParam`; log `SessionEvent_Received_Code`.
- **Debounce:** same `sessionEvent` within **1500 ms** → `SessionEvent_DedupIgnored`, base `WndProc`, return.
- Update debounce state.
- `!isMonitoring` → log `SessionEvent_Ignored_MonitoringOff`.
- `WTS_SESSION_LOCK` → `DisableTargetCameraHardware(true)`; log `SessionLock_Disable`.
- `WTS_SESSION_UNLOCK` → `EnableTargetCameraHardware(false)`; log `SessionUnlock_Enable`.

### Final (107)
Always calls `Form::WndProc(m)` (base handling) unless an early return was taken.

## Event-flow table

| Windows Event | Message / code | Current behavior | Camera effect | Log |
|---|---|---|---|---|
| Shutdown / Logoff | `0x0011` / `0x0016` | `isSystemEnding=true`; if monitoring → disable | Disabled (if monitoring) | `SystemEnd_Begin`, `SystemEnd_Disable` |
| Suspend / power-setting (lid/button) | `0x0218` / `0x0004` or `0x8013` | Debounce 1500 ms; lock `isAlreadyDisabled`; disable; `Sleep(500)` | Disabled | `PowerEvent_Disable` (or `PowerSetting_IrrelevantGuid`) |
| Resume | `0x0218` / `0x0007` or `0x0012` | `Thread::Sleep(1000)`; enable; release lock | Enabled | `PowerEvent_Enable` |
| Session lock | `WM_WTSSESSION_CHANGE` / `WTS_SESSION_LOCK` | Debounce 1500 ms; disable | Disabled | `SessionLock_Disable` |
| Session unlock | `WM_WTSSESSION_CHANGE` / `WTS_SESSION_UNLOCK` | Debounce 1500 ms; enable | Enabled | `SessionUnlock_Enable` |

## Dependencies
- **Calls:** `DisableTargetCameraHardware`/`EnableTargetCameraHardware` (Camera), `WriteDiagnosticLog` (Config), `IsEqualGUID`, `WTSUnRegisterSessionNotification`.
- **Called by:** the Windows message pump (WinForms `Form` base routes messages to the overridden `WndProc`).

## Threading context
Runs on the **UI thread** (message pump). `Sleep(500)`/`Thread::Sleep(1000)` block the UI thread briefly by design (preserves original timing).

## State modified
- `isSystemEnding` (shutdown path).
- `isAlreadyDisabled` (static, persists across messages).
- `cameraExpectedDisabled` (via the camera members).
- Camera device hardware state.
