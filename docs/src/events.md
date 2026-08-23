# Module: `src/events/` — Windows event plumbing

Native C++ helpers converting raw window messages into typed events, deduping
them, and registering for WTS/power notifications.

---

## SystemEvent.h

| Field | Content |
|---|---|
| Purpose | Neutral enum consumed by the controller |
| Values | `None, SystemQueryEnd, SystemEnd, PowerSuspend, PowerSettingLid, PowerSettingButton, PowerSettingOther, PowerResumeSuspend, PowerResumeAutomatic, PowerOther, SessionLock, SessionUnlock, SessionOther` |
| Consumers | ApplicationController handlers; produced by WinEventDecoder |

## WinEventDecoder.h / WinEventDecoder.cpp

| Field | Content |
|---|---|
| Purpose | Raw message → `SystemEvent` translation |
| Functions | `Decode(msg, wparam, lparam)` (dispatcher); `DecodeSystemEvent` (`0x0011`→SystemQueryEnd, `0x0016`→SystemEnd); `DecodePowerEvent` (see table); `DecodeSessionEvent` (`WTS_SESSION_LOCK`→SessionLock, `WTS_SESSION_UNLOCK`→SessionUnlock, else SessionOther) |
| Power mapping | `0x0004`→PowerSuspend; `0x8013`→GUID compare against lid/button GUIDs (else PowerSettingOther; **null lParam treated as PowerSuspend**); `0x0007`→PowerResumeSuspend; `0x0012`→PowerResumeAutomatic; else PowerOther |
| APIs/constants | `WM_WTSSESSION_CHANGE`, `POWERBROADCAST_SETTING`, `IsEqualGUID`; local fallback defines for both power GUIDs |
| State/side effects | None — pure |
| Callers | MyForm::WndProc (power + session paths); generic `Decode` itself is currently uncalled (MyForm calls the two specific decoders) |

## EventCooldown.h / EventCooldown.cpp

| Field | Content |
|---|---|
| Purpose | 1500 ms per-channel duplicate suppression |
| State | statics: `s_lastPowerTick/Code`, `s_lastSessionTick/Code` (init −1 / 0) |
| Rule | same code within <1500 ms → suppress (true); otherwise record code+tick and pass |
| Inputs | Raw int codes (power wparam / session wparam) + `GetTickCount64()` value supplied by caller |
| Threading | Called from UI thread only; not synchronized if that ever changes |
| Callers | MyForm::WndProc |

## NotificationRegistrar.h / NotificationRegistrar.cpp

| Field | Content |
|---|---|
| Purpose | Register/unregister session and power-setting notifications |
| Functions | `RegisterPowerNotifications(hwnd, outLid, outButton)` → two `RegisterPowerSettingNotification(DEVICE_NOTIFY_WINDOW_HANDLE)` calls for `GUID_LIDSWITCH_STATE_CHANGE` and `GUID_POWER_BUTTON_TIMESTAMP`; always returns true. `UnregisterPowerNotifications` → guarded `UnregisterPowerSettingNotification`. `RegisterSessionNotificationWithRetry(hwnd)` → up to 6 × [`WTSRegisterSessionNotification(NOTIFY_FOR_THIS_SESSION)` + 500 ms sleep]. `UnregisterSessionNotification` |
| Libs | wtsapi32, user32 (#pragma comment) |
| Error handling | Session retry loop returns final bool; caller logs GetLastError. Power registration failures (null HPOWERNOTIFY) are not detected — return value is hardcoded true |
| Callers | ApplicationController::Initialize/RegisterNotifications, Shutdown/finalizer, HandleSystemEnd |
| Threading | Registration happens on UI thread during Load; unregistration on UI thread or teardown |
