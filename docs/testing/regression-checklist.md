# Regression Checklist

Run this checklist after **every** migration phase. It is the practical,
repeatable gate for "behavior preserved."

Expected results reference the `diagnostic.log` lines that the application
writes to `%APPDATA%\Windows Hello Fix\diagnostic.log`.

## 0. Prepare

- [ ] Baseline committed; `git status` clean before starting the phase.
- [ ] Note the current `diagnostic.log` size/state (append only).

## 1. Build

- [ ] Build `Release | x64` succeeds with no new warnings.
- [ ] `x64\Release\Windows_Hello_Fix_v2_0.exe` is produced.
- [ ] (After phase 11) Clean checkout → build succeeds.

## 2. Launch / Startup

- [ ] Launch normally: form appears, dropdown lists cameras, status
      "Service Stopped".
- [ ] Select target; click "Start Monitoring Service"; status
      "Service Running"; dropdown disabled.
- [ ] `diagnostic.log` contains the `Startup_Context` line with expected
      `Elevated` / `IntegrityRid` values.

## 3. Command Modes

- [ ] `/restore-camera` exits after restoring; camera enabled.
- [ ] `/disable-camera` exits after disabling; camera disabled.
- [ ] `/background` launch hides the form (and monitors if config says so).

## 4. Event Handling

- [ ] **Lock** (Win+L): `SessionLock_Disable` + `Verify=PASS`; RGB camera
      disabled in Device Manager.
- [ ] **Unlock**: `SessionUnlock_Enable` + `Verify=PASS`; camera enabled.
- [ ] **Duplicate lock within 1.5 s**: `SessionEvent_DedupIgnored`; no extra
      toggle.
- [ ] **Sleep/resume**: `PowerEvent_Disable` on suspend, `PowerEvent_Enable` on
      resume.
- [ ] **Monitoring off**: session event logged `MonitoringOff`; no hardware
      change.

## 5. Target Device

- [ ] Dropdown auto-selects saved device (else `MI_00`, else first).
- [ ] Saved config round-trips: stop → relaunch → same device selected.

## 6. Device State Transition

- [ ] Disable when already disabled: `..._AlreadyDisabled`; no churn.
- [ ] Enable when already enabled: `..._AlreadyEnabled`; no churn.
- [ ] Manual disable via Device Manager, then lock: app recovers state.

## 7. Recovery / Verification

- [ ] Force-verify: after a state change, log shows `Verify=PASS`.
- [ ] Missing/invalid target: `..._NoTarget` logged, no crash.
- [ ] (Optional) Simulate SetupAPI failure path → CFGMGR32 fallback exercised.

## 8. Single Instance

- [ ] Start second instance: first comes to front; second exits quietly
      (`SingleInstance_WakeSignalSent`).
- [ ] (Interactive duplicate) Force-reset path recovers camera and restarts.

## 9. Shutdown

- [ ] Stop button: camera enabled; `monitoring=0` in `config.txt`.
- [ ] X button: window hides; process alive; notice shown once.
- [ ] Kill process: nothing left disabled.
- [ ] Logoff (or force `WM_ENDSESSION`): camera disabled; `monitoring=1`.

## 10. Hygiene

- [ ] `git status` shows only the intended phase files.
- [ ] No generated artifacts added.
- [ ] No unrelated files modified.
- [ ] Diagnostic log lines identical in format/content to the pre-phase run
      (for the exercised paths).
- [ ] Commit the phase with a clear message referencing the phase number.

## Legend

- ✔ = passes
- ✘ = fails — phase is not complete; investigate and fix or revert
- N/A = not applicable to this phase (state why)

## Related Documents

- `baseline.md` — reference behavior definition.
- `verification-matrix.md` — full matrix this checklist condenses.
- `../refactoring/migration-phases.md` — when to run this.