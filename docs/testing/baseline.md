# Baseline — Known-Good v2.0 Behavior

This document defines the v2.0 known-good reference point. Every future
refactoring phase is compared against this baseline: **if the baseline behavior
no longer holds after a phase, that phase is not done.**

## Build Baseline

- **Configuration:** `Release | x64`
- **Toolchain:** Visual Studio Community 2022, MSVC v143, Windows SDK 10.0
- **Output:** `x64\Release\Windows_Hello_Fix_v2_0.exe`
- **Nature:** C++/CLI managed assembly targeting .NET Framework 4.7.2

## Runtime Conditions

- Runs as a normal elevated desktop application (`requireAdministrator`).
- Hardware operations (device disable/enable) require elevation; without it the
  operations fail gracefully and log an error.
- Writes configuration and diagnostics to
  `%APPDATA%\Windows Hello Fix\config.txt` and `diagnostic.log`.

## Known Behaviors (the compatibility contract)

### Startup

- Launching with no args shows the form with the camera dropdown populated.
- Launching with `/background` (or `--background`) hides the form and monitors if
  the config says `monitoring=1`.
- Launching with `/disable-camera` / `--disable-camera` disables the target and
  exits.
- Launching with `/restore-camera`, `/enable-camera`, `--enable-camera`, or
  `/repair-camera` restores the camera and exits.
- A second instance signals the first (wake event) and exits quietly; if the
  wake event is unavailable and the launch is background, it exits quietly; if
  interactive, it offers a force-reset path (recover camera → `taskkill` →
  restart).

### Monitoring (when active)

| Event | Action |
|---|---|
| Session lock | Disable target RGB camera (verified) |
| Session unlock | Enable target camera (verified) |
| Sleep / lid close / power button | Disable target camera (+500 ms safety window) |
| Resume | Enable target camera (after 1000 ms) |
| WM_ENDSESSION / WM_QUERYENDSESSION | Disable target camera; mark system-ending |

### Safety

- **Dedup:** identical session or power events within 1500 ms are suppressed.
- **Check-before-change:** no hardware command is issued if the device is
  already in the requested state.
- **Verification:** every state change is verified up to 3 times (100 ms apart);
  the retry loop is up to 3 attempts with SetupAPI → CFGMGR32 fallback and
  reinitialize-on-mismatch.
- **Wrong-device protection:** matching is by exact or case-insensitive instance
  ID; only `Camera`/`Image` class devices are scanned; auto-target prefers a
  device whose instance ID contains `MI_00`.
- **Single instance:** one process via `Global\WindowsHelloFix_AppMutex`.
- **Shutdown:** on normal exit the camera is re-enabled and config keeps
  `monitoring=1`; on system-ending the camera stays disabled and config keeps
  `monitoring=1`.

## Manual Verification Procedure (baseline)

1. Build `Release | x64`; confirm the exe is produced.
2. Launch the app. Confirm the dropdown lists the cameras and the status is
   "Service Stopped".
3. Select the RGB camera, click "Start Monitoring Service". Confirm status
   "Service Running" and the dropdown becomes disabled.
4. Press `Win+L`. Wait ~3 s. Confirm `diagnostic.log` contains
   `SessionLock_Disable | Verify=PASS` and Device Manager shows the RGB camera
   disabled.
5. Unlock. Confirm `SessionUnlock_Enable | Verify=PASS` and the camera is
   enabled again.
6. Click "Stop Monitoring Service". Confirm the camera is enabled, status is
   "Service Stopped", and `monitoring=0` is written to `config.txt`.
7. Launch with `/disable-camera` then `/restore-camera`; confirm both exit and
   the camera state matches each command.
8. Close via the X button; confirm the window hides and the process keeps
   running (taskbar shows it in the background).
9. Kill the process from Task Manager; confirm nothing is left disabled.

## What "Behavior Preserved" Means During Refactoring

- Same diagnostic log lines (`Event=...`, `Verify=PASS/FAIL`).
- Same timing behavior (1500 ms debounce, 100 ms verification, 250 ms retry,
  500/1000 ms event windows, 350/900/500 ms recovery cycle).
- Same device-selection order (selection → config → `MI_00` → first).
- Same single-instance and command-mode exit semantics.
- Same shutdown disable-vs-enable decision keyed on `isSystemEnding`.

## Related Documents

- `verification-matrix.md` — the full regression matrix.
- `regression-checklist.md` — the runnable per-phase checklist.
- `../architecture/data-flow.md` — flow-level behavior details.