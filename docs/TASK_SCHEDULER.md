# Task Scheduler Integration (as-built)

Baseline: branch `test`, commit `acc37d8`. Tasks are created by the NSIS
installer's generated PowerShell script (`INSTALLER.md §5`) and deleted by the
uninstaller. This document describes each task exactly as registered.

## 1. Task inventory

### 1.1 `WindowsHelloFix` — background daemon

Created with the `Register-ScheduledTask` cmdlet.

| Property | Value |
|---|---|
| Action | `Windows_Hello_Fix_v2_0.exe`, arguments **`--background`**, working directory `$INSTDIR` |
| Trigger | `New-ScheduledTaskTrigger -AtLogOn` (any logon of any user; no UserId bound to the trigger) |
| Principal | Installing user (`[WindowsIdentity]::GetCurrent().Name`), LogonType **Interactive**, RunLevel **Highest** |
| Settings | `-AllowStartIfOnBatteries -DontStopIfGoingOnBatteries -StartWhenAvailable -MultipleInstances IgnoreNew -ExecutionTimeLimit (New-TimeSpan -Seconds 0)` (= unlimited), Priority 4 |
| Interactive? | Runs in the interactive user session at logon → can create windows, but the app hides itself (`--background`) |
| Can start GUI? | Starts the process hidden; GUI only appears if another instance summons it via wake event |
| Can start daemon? | Yes — this is THE daemon task. Monitoring is forced ON when a device was previously selected |
| Duplicate instances? | Guarded twice: Task Scheduler `IgnoreNew` + app's `Global\WindowsHelloFix_AppMutex` (background second instances exit silently) |

### 1.2 `WindowsHelloFix_Lock` — lock failsafe

Created through `Schedule.Service` COM helper (`Register-WhfSessionTask`),
registered with `RegisterTaskDefinition(name, task, 6 /*create-or-update*/, …)`.

| Property | Value |
|---|---|
| Action | exe, arguments **`--disable-camera`**, working directory `$INSTDIR` |
| Trigger | Type **11** = session state change trigger; `StateChange = 7` (**session lock**); UserId = installing user; enabled |
| Principal | Same user; `LogonType = 3` (interactive token); `RunLevel = 1` (highest) |
| Settings | Hidden=true; DisallowStartIfOnBatteries=false; StopIfGoingOnBatteries=false; StartWhenAvailable=true; MultipleInstances=2 (ignore new); ExecutionTimeLimit=`PT5M`; Priority=4 |
| Interactive? | Interactive token, hidden task window |
| Can start GUI? | No lasting GUI: command mode exits before mutex handling; main.cpp hides the window anyway |
| Can start daemon? | No — exits after disabling+verifying (`Environment::Exit(0)` inside `Initialize`) |
| Duplicate risk | Runs **concurrently** with a healthy daemon by design (failsafe). Command check happens *before* mutex acquisition, so both processes can toggle hardware for the same lock event; per-process cooldowns cannot see each other |

### 1.3 `WindowsHelloFix_Unlock` — unlock failsafe

Identical to 1.2 except:

- Trigger `StateChange = 8` (**session unlock**).
- Arguments **`--enable-camera`** → runs `RestoreConfiguredCameraHardware(true)`
  (full recover cycle) against the configured device.

### 1.4 `WindowsHelloFix_LogCleanup`

Created with `Register-ScheduledTask` reusing the daemon task's
principal/settings objects.

| Property | Value |
|---|---|
| Action | `cmd.exe` with argument `/c break > "$APPDATA\Windows Hello Fix\diagnostic.log"` |
| Trigger | Daily at 00:00 |
| Principal / Settings | Same as 1.1 (Highest, interactive token, unlimited execution time, IgnoreNew) |
| Intent | Truncate the diagnostic log daily to cap growth |
| ⚠ Suspected defect | `cmd.exe` does not expand `$APPDATA` (PowerShell variable syntax). The redirect may create/overwrite a literal `$APPDATA\...` path under the task's default working directory instead of the real log. Documented as-is; requires runtime verification |

## 2. Cross-task behavior summary

```
User logs on ──► WindowsHelloFix (--background, unlimited runtime)
                     └─ hidden daemon; WTS/power notifications; monitoring ON

Lock ──► WindowsHelloFix_Lock  (--disable-camera, PT5M limit)
      └► daemon ALSO handles lock internally   ← double disable possible,
                                                  harmless but redundant
Unlock ► WindowsHelloFix_Unlock (--enable-camera, PT5M limit)
      └► daemon ALSO enables internally        ← double enable possible
Daily 00:00 ► WindowsHelloFix_LogCleanup      ← log truncation (see defect note)
```

- The failsafe tasks exist so that lock/unlock still works if the daemon died;
  when it lives, they duplicate its work.
- All tasks run elevated (RunLevel Highest) with the interactive-token logon
  type, so none trigger UAC prompts and all can perform device-node changes.
- Uninstaller deletes all four plus legacy name `WindowsHelloFix_Wake`
  (never created by the current installer).

## 3. What agents must know before touching anything here

1. Task names are load-bearing strings referenced in three places: installer
   creation block, uninstaller deletion list, and (indirectly) documentation.
   Renaming one side silently breaks cleanup or registration.
2. The COM-based registration uses raw numeric enums (trigger type 11,
   StateChange 7/8, LogonType 3, RunLevel 1, MultipleInstances 2,
   RegisterTaskDefinition type 6). Any change must preserve these exact
   semantics.
3. The daemon task intentionally has an unlimited execution-time limit while
   the failsafe tasks are capped at 5 minutes; do not "normalize" them without
   understanding that the daemon must run indefinitely.
4. Removing the failsafe tasks changes behavior (single-writer model), not just
   redundancy — treat as functional change requiring approval.
