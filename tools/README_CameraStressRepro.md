# Camera Stress Repro — Issue #1 (`0xA00F4241` / `0xC00D7167`) test harness

Tools for deliberately reproducing the Issue #1 camera wedge and then validating
whether `CameraRecoveryFailsafe` can recover the camera in the same session.

| File | Purpose |
|---|---|
| `CameraStressRepro.ps1` | The harness (stress modes + read-only probe) |
| `CameraStressRepro.bat` | Double-click launcher (passes arguments through) |

## What this can and cannot do — read first

The failure signature is `0xC00D7167 = MF_E_REBOOT_REQUIRED` ("We need to reboot the
machine") — a Media Foundation machine-level state whose raise-site is undocumented.
**No tool can set that flag directly.** What this harness does instead is compress the
*mechanism* that is suspected of producing it — hundreds of camera device-identity
changes while the Windows Camera Frame Server holds live client sessions — from
days of normal HelloFix use down to minutes, by doing what HelloFix never does:
**disabling the camera device while a live capture stream is attached** (the classic
frame-server wedge pattern), then re-enabling and re-probing, over and over.

Honest expectation: this may reproduce the wedge in minutes, may take hours, or may
not reproduce it at all on a given build/driver. Every attempt is logged either way,
and a non-reproduction is itself useful evidence.

## Requirements

- Windows PowerShell **5.1** (`powershell.exe` — the batch launcher uses it; `pwsh` 7 does not project WinRT).
- Windows 10 2004+ for `pnputil /disable-device` (your build 26200 is fine).
- Administrator only for the toggle modes (`IdleToggleChurn`, `MidStreamToggle`) — the script self-elevates via UAC on first run. `ProbeOnly` and `ActivationChurn` are read-only and run without elevation.
- Close the Camera app / other camera users for cleanest results (frame-server sharing makes coexistence possible, but cleanliness helps reproduction).

## Modes

| Mode | What it does | Danger level |
|---|---|---|
| `ProbeOnly` | One read-only frame grab through MediaCapture → Frame Server. Reports the raw HRESULT. | none |
| `ActivationChurn` | Open/close a live frame reader in a loop (no device toggles). Stresses Frame Server client-session lifecycle. | low |
| `IdleToggleChurn` | Disable → enable cycles with no stream attached, plus an activation-churn burst each cycle (accelerated HelloFix). | medium |
| `MidStreamToggle` | **The aggressive one:** holds a LIVE stream, disables the device mid-stream (Frame Server client attached), re-enables, probes. Repeat. | high |

The probe classification mirrors `CameraRecoveryFailsafe` exactly:
`Healthy` / `RebootRequired` (0xC00D7167 — the Issue #1 signature) / `AccessDenied`
(privacy — not a wedge) / `InUse` (busy = responsive) / `Timeout` (started, no frames —
wedge suspect) / `Failed`. Each result also records the PnP status, so the log shows
the Issue #1 signature (**PnP OK + camera stack broken**) the moment it appears.

## Usage

```bat
:: 1. Baseline / "is it wedged right now?" (no admin, no device changes)
tools\CameraStressRepro.bat -Mode ProbeOnly

:: 2. The aggressive reproduction attempt (self-elevates; runs 60 mid-stream cycles)
tools\CameraStressRepro.bat -Mode MidStreamToggle -UntilFailure

:: 3. Gentler accelerated toggling (mimics HelloFix's own behavior, faster)
tools\CameraStressRepro.bat -Mode IdleToggleChurn -Iterations 200

:: 4. Use HelloFix's own toggle path instead of pnputil
tools\CameraStressRepro.bat -Mode IdleToggleChurn -ToggleTool HelloFix

:: 5. Escalate scope: toggle the composite parent (briefly re-detects the IR sibling too)
tools\CameraStressRepro.bat -Mode MidStreamToggle -ToggleParent -UntilFailure
```

The target camera is auto-detected from `%APPDATA%\Windows Hello Fix\config.txt`
(`device=`), falling back to the `MI_00` RGB heuristic — the same rule HelloFix uses.
Override with `-InstanceId "<USB\VID_...&MI_00\...>"`. The IR/Hello camera is never
selected unless you explicitly pass its instance id or use `-ToggleParent`.

A timestamped log (`CameraStressRepro_*.log` next to the script) records every toggle,
probe, HRESULT and PnP state — attach it to the issue together with
`%APPDATA%\Windows Hello Fix\diagnostic.log`.

## The recovery validation procedure (the actual point)

1. **Prepare:** install/run the v2.2 build so the daemon is running with monitoring
   ON (`config.txt` → `monitoring=1`). Confirm a healthy baseline:
   `tools\CameraStressRepro.bat -Mode ProbeOnly` → `Healthy`.
2. **Reproduce:** run `-Mode MidStreamToggle -UntilFailure` (or overnight). Stop when
   you see the red **WEDGE SIGNATURE OBSERVED** banner, or verify in the Camera app:
   `0xA00F4241 (0xC00D7167)` while Device Manager still shows the camera OK.
3. **Recover — no reboot:** with the daemon running, trigger a recovery pass:
   - lock the session (`Win+L`) then unlock → the enable-transition health check fires, or
   - simply wait ≤ 10 minutes for the backup poll.
4. **Watch:** `%APPDATA%\Windows Hello Fix\diagnostic.log` — filter `CameraRecovery_`:
   `HealthCheck → RungStart (Rung=1..4) → RungResult (Stage=/SetupErr=/CfgMgr=/ServiceState=) →
   Recovered | RungTimeout | RecoveryExhausted (…MF_E_REBOOT_REQUIRED_persists…)`.
5. **Verify:** re-run `-Mode ProbeOnly` and open the Camera app. Healthy again?
   Note **which rung** recovered it (the `RungResult` lines tell you).
6. **Report:** stress log + `diagnostic.log` + which rung (1: `DICS_PROPCHANGE`,
   2: Frame Server restart, 3: full cycle, 4: parent re-enumeration) — or that none
   worked and reboot was required. Either outcome answers the open research question
   in `docs/Issue1_Camera_Recovery_Research.md` §12.

## Notes & safety

- Toggling the RGB camera is exactly what HelloFix does on every lock/unlock; this
  tool just does it much more often, and (in `MidStreamToggle`) while a client is
  streaming. If a genuine wedge appears, a reboot may be needed — that is the point.
- `-ToggleParent` briefly re-detects the IR camera too (same caveat as failsafe rung 4);
  use it only while unlocked.
- `AccessDenied` results mean the Windows privacy toggle is off — flip it back on in
  Settings → Privacy → Camera instead of testing.
- The mid-stream mode keeps the Frame Server client session attached across the
  device disable/enable — that stale-session-across-identity-change is precisely the
  suspected ghost-reference mechanism from the research.
