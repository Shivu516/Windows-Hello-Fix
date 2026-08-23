# Logging System (as-built)

Baseline: branch `test`, commit `acc37d8`. Two independent logging mechanisms
exist; only one is actually used by the running application.

## 1. Primary: diagnostic log file (`ConfigStore`)

| Aspect | Value |
|---|---|
| Location | `%APPDATA%\Windows Hello Fix\diagnostic.log` (resolved by `ConfigPaths::GetDiagnosticLogFilePath`; directory auto-created on every call) |
| Mode | Append, opened/closed per write |
| Format | `yyyy-MM-dd HH:mm:ss.fff | Event=<eventName> | Target=<state> | Verify=PASS\|FAIL` |
| Device variant | `WriteDiagnosticLogWithDevice` appends `| Device=<instanceId>` **to the event-name field** before formatting |
| Thread safety | All writes serialized through a static `Monitor` lock object |
| Error handling | Any exception (I/O, marshal) silently swallowed — logging never crashes the app but can silently stop |

`Target=` values observed in code: `Disabled`, `Enabled`, `NoChange`,
`ActiveMonitoring`, `MonitoringOff`. `Verify=PASS/FAIL` reflects the caller's
success boolean (usually "operation verified").

## 2. Event catalog

### Startup / lifecycle
| Event | Meaning |
|---|---|
| `Startup_Context \| Elevated=E \| IntegrityRid=R \| BackgroundArg=B \| Exe=… \| Cwd=… \| Config=…` | First line of every launch; process elevation/integrity + args + paths |
| `Startup_RestoreConfiguredCameraHardware` | Startup camera-restore pass begins |
| `WTSRegisterSessionNotification_Success` / `_Failed_LastError=N` | Session-notification registration result (6×500 ms retries) |
| `SingleInstance_BackgroundSilentExit` | Second instance with `--background` exited silently |
| `SingleInstance_WakeSignalSent` | Second instance signaled the daemon's wake event and exits |
| `SingleInstance_ForceResetRequested` | User accepted ghost-reset prompt |

### Command modes
| Event | Meaning |
|---|---|
| `Command_EnableCamera_Begin` / `_End` | `/restore-camera` `/enable-camera` `/repair-camera` execution window |
| `Command_DisableCamera_Begin` / `_End` | `/disable-camera` execution window |

### Camera operations
| Event | Meaning |
|---|---|
| `DisableTargetCameraHardware_NoTarget` | No target device resolvable; disable failed |
| `DisableTargetCameraHardware_AlreadyDisabled \| Device=<id>` | Check-before-change short-circuit (counts as PASS) |
| `DisableTargetCameraHardware_Result \| Elevated=E \| IntegrityRid=R \| SetupErr=W32 \| CfgMgr=CR \| Stage=S \| Device=<id>` | Disable attempt outcome incl. raw error slots & stage marker (see CAMERA_HARDWARE.md) |
| `EnableTargetCameraHardware_NoTarget` / `_AlreadyEnabled` / `_Result` | Enable-path equivalents (Stage 10–23 semantics) |

### Session events
| Event | Meaning |
|---|---|
| `SessionEvent_Received_Code=N` | Raw WTS code arrived (7=lock, 8=unlock, others pass through); Target shows monitoring state |
| `SessionEvent_DedupIgnored` | Same code within 1500 ms cooldown dropped |
| `SessionEvent_Ignored_MonitoringOff` | Lock/unlock received while monitoring off |
| `SessionLock_Disable` | Lock → disable outcome |
| `SessionUnlock_Enable` | Unlock → enable outcome |

### Power events
| Event | Meaning |
|---|---|
| `PowerEvent_DedupIgnored` | Duplicate power code suppressed |
| `PowerSetting_IrrelevantGuid` | PBT_POWERSETTINGCHANGE for an unregistered GUID |
| `PowerEvent_Disable` | Suspend/lid/button → disable outcome |
| `PowerEvent_Enable` | Resume → enable outcome |

### System end
| Event | Meaning |
|---|---|
| `SystemEnd_Begin` | WM_QUERYENDSESSION/WM_ENDSESSION entered |
| `SystemEnd_Disable` | Final shutdown-time disable outcome |

## 3. Secondary: `ProductionLogger` (src/utilities)

A native structured logger writing to **`OutputDebugStringW`** only:

```
[yyyy-MM-dd HH:mm:ss] OP=<op> | DEVICE=<id> | RESULT=SUCCESS|FAILED
    [| WIN_ERROR=0x…] [| CONFIGRET=n] [| DURATION=nms] | PID=<pid> | TID=<tid>
[timestamp] ERROR | <context> | <message>
[timestamp] INFO  | <context> | <message>
```

As of this baseline **no production code calls it** — it is compiled into the
binary (vcxproj includes Logging.cpp) but inert unless attached to a debugger
that captures debug output. Do not confuse its output with `diagnostic.log`.

## 4. Practical usage for debugging

1. Reproduce the issue.
2. Read `%APPDATA%\Windows Hello Fix\diagnostic.log` bottom-up.
3. Correlate pairs: every action has Begin/end or Result events; `Verify=FAIL`
   lines are the failure signature; `SetupErr/CfgMgr/Stage` fields identify the
   exact hardware-API failure point (stage table in CAMERA_HARDWARE.md §8).
4. Remember the daily `WindowsHelloFix_LogCleanup` task intends to truncate
   this file at midnight (with a suspected path defect — TASK_SCHEDULER.md),
   so old history may not exist.

## 5. Known gaps

- No rotation beyond the daily cleanup task; no size cap in-app.
- Exceptions during log write are swallowed without any fallback channel.
- Native camera layer itself logs nothing directly; all file logging happens
  via managed callers.
