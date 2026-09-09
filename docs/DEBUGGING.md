# Debugging Guide

All diagnostic output goes to:

```
%APPDATA%\Windows Hello Fix\diagnostic.log
```

and configuration to `%APPDATA%\Windows Hello Fix\config.txt`. Logs are appended, timestamped, and monitor-locked (see `MyForm_Config.cpp`).

Log line format (v2.1 professional format):
```
[yyyy-MM-dd HH:mm:ss.fff] [LEVEL] [CATEGORY] Event | Op=<id> | Pid=<pid> | <Key=Value ...> | Target=<state> | Verify=PASS|FAIL
```
- `LEVEL` is `DEBUG`, `INFO `, `WARN `, or `ERROR` (fixed width). `DEBUG` lines (dedup-ignored events, per-message receipts, startup heartbeat) are always recorded but only needed for deep timing forensics.
- `CATEGORY` is `STARTUP`, `COMMAND`, `LOCK`, `POWER`, `SESSION`, `CAMERA`, `FAILSAFE`, or `SYSTEM`.
- `Op=<PREFIX>-<nnnnnn>` correlates one operation end-to-end (e.g. `LOCK-000123` joins lock receipt → disable → verify). Prefixes: `STARTUP`, `CMD`, `LOCK`, `UNLOCK`, `POWER`, `SYSEND`, `FAILSAFE`, `RECLOOP`. Search the log for the `Op` value to follow a single transition.
- `Pid=` distinguishes the long-lived daemon from short-lived Task Scheduler workers (`--enable-camera` / `--disable-camera` / `/restore-camera`).
- Device-specific events carry `Device=<instanceId>`; camera `Result` lines also carry `DurationMs=`, `Attempt=`, `Path=` (`SetupAPI`/`CfgMgr`/`Final`/`Already`/`None`), `SetupErr=`/`CfgMgr=`/`Stage=`, and `ErrText=` (human-readable Win32 error text).
- Durations/latencies (`DurationMs`, `TriggerToStart`, `TriggerToComplete`, `DetectToRecoverMs`) are monotonic `GetTickCount64` deltas in milliseconds; timestamps are wall-clock for chronology.
- The nightly `WindowsHelloFix_LogCleanup` task truncates the log only when it exceeds 512 KB, so multi-day history is normally preserved.

## Symptom → where to look

| Symptom | Log events to look for | Source / function | Windows state to inspect |
|---|---|---|---|
| Camera not disabling on lock | `SessionLock_Disable` with `Verify=FAIL`; `DisableTargetCameraHardware_Result` with `SetupErr`/`CfgMgr`/`Stage` | `MyForm_Events.cpp` WndProc; `MyForm_Camera.cpp` `SetCameraHardwareStateVerified` | Device Manager → camera → enabled?; run as Administrator? |
| Camera not re-enabling on unlock | `SessionUnlock_Enable` `Verify=FAIL`; `EnableTargetCameraHardware_Result` | WndProc; `RecoverCameraHardware` | Same as above |
| Camera device unavailable / not found | `DisableTargetCameraHardware_NoTarget` / `EnableTargetCameraHardware_NoTarget` | `TryGetTargetCameraInstanceId` | Is the device present? `ScanSystemCameras` populates dropdown; check `MI_00` instance ID |
| Lock event not received | `SessionEvent_Received` (DEBUG) missing | WTS registration in `MyForm_Load` (`WTSRegisterSessionNotification_Success` with `Attempts=` vs `_Failed` with `LastError=`/`ErrText=`) | `qwinsta` / Session 0; WTS not available in some contexts |
| Unlock event not received | same as above | same | same |
| Power event not received | `PowerEvent_*` missing | `RegisterPowerSettingNotification` handles; `WndProc` 0x0218 | Power settings GUID registration; run interactively |
| App already running (window won't show) | `SingleInstance_WakeSignalSent` in *first* instance log | `MyForm_Load` mutex branch; `ListenForWakeupSignal` | Second launch should `SetEvent` the wake event; check it isn't blocked |
| GUI not appearing | `Startup_Context` present but no window | `main.cpp` (Opacity=0 if arg matched), `isBackgroundMode` | Was `/background` passed? Check taskbar hidden + minimized |
| Process stuck / high CPU | repeated `PowerEvent_DedupIgnored` / `SessionEvent_DedupIgnored` | WndProc debounce | Normal dedup; if truly stuck, inspect `isAlreadyDisabled` static |
| Camera left disabled after exit | `SystemEnd_Disable` then no enable in destructor | `~MyForm` / `!MyForm` with `isSystemEnding` | This is expected at system shutdown; for normal exit, `EnableTargetCameraHardware(false)` should run |

## Likely failure points (observed)

- **Not running elevated:** SetupAPI/CfgMgr disable/enable require administrator. `Startup_Context` logs `Elevated=0`. The `…_Result` lines embed `SetupErr`/`CfgMgr` codes plus `ErrText` — e.g. `SetupErr=5 | ErrText=Access is denied` means privilege failure.
- **Excel/Power Query analysis:** split lines on `|` (trim spaces), then split each fragment on the first `=`; the `[timestamp]`, `[LEVEL]`, `[CATEGORY]`, and leading event token stay human-readable while the `Key=Value` fields become columns. Filter by `Op=` to isolate one incident.
- **Device not present at startup:** `RestoreConfiguredCameraHardware` uses `DIGCF_PRESENT` discovery in `ScanSystemCameras`; a disabled device may not appear, but `RestoreConfiguredCameraHardware` first tries the saved instance ID directly via `RecoverCameraHardware`, so it usually still works.
- **Wrong target:** `MI_00` heuristic selects the RGB/IR sub-device. If the saved `device=` value is corrupted, `TrimTrailingChars` + the `MI_00` fallback mitigate it; otherwise the first camera is used.

## How to enable deeper inspection

- Inspect `g_last*` via the logged `Stage` values: `10–15` are SetupAPI path; `20–23` are CfgMgr path (see `MyForm_Camera.cpp`).
- The `diagnostic.log` is the only telemetry; there is no in-app UI for it. Open it with any text editor after reproducing the issue.

> This guide is documentation only. No code was changed to add instrumentation.
