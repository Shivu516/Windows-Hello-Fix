# `CameraRecoveryFailsafe` — Camera-Stack Health Watchdog (Issue #1)

> **Status:** IMPLEMENTED incl. Level 5 deep device-instance removal — builds verified `Release|x64` and `Release|Win32`, 0 errors, 0 new warnings. **Not a guaranteed fix** — see §Limitations.
> **Dates:** 2026-10-03 (initial ladder) · 2026-10-04 (ladder ran against a real wedge — rungs 1–4 exhausted; Level 5A/5B deep removal added per `docs/Issue1_Deep_Device_Recovery_Research.md`)
> **Files:** `src/watchdog/CameraRecoveryFailsafe.h` / `.cpp` (+ additive `src/core` primitives)
> **Investigation record:** `docs/Issue1_Camera_Recovery_Research.md` · **Deep-recovery research:** `docs/Issue1_Deep_Device_Recovery_Research.md` · **Plan:** `docs/Issue1_Failsafe_Plan.md`

---

## 1. Purpose

Implements same-session recovery for the Issue #1 failure state:

```text
PnP/device state = Enabled          ← everything the existing watchdogs check
        BUT
Windows camera stack = unusable     ← Camera app: 0xA00F4241 (0xC00D7167 = MF_E_REBOOT_REQUIRED)
```

The sub-code `0xC00D7167` was confirmed against the Windows SDK (`Mferror.h`) to be **`MF_E_REBOOT_REQUIRED`** ("We need to reboot the machine") — a Media Foundation machine-level error above the driver, which is why Device Manager stays green, why a reboot fixes it, and why the PnP-only watchdogs (`CameraFailsafe`, `RecoveryLoopFailsafe`) are silent on it.

This watchdog adds the missing sensor — a real camera-stack health check — and a bounded escalation ladder of documented device/stack recovery mechanisms. **It is an attempted same-session recovery, not a replacement for reboot.** It never reboots the machine.

## 2. Architecture

```text
                    ┌─────────────────────────┐
                    │ CameraRecoveryFailsafe  │   src/watchdog (owned by main.cpp,
                    │      src/watchdog       │   attached to Load/FormClosing)
                    └────────────┬────────────┘
                                 │
              Level 0: MF health check (repeated after EVERY level)
                                 │
              Healthy/Busy/Privacy → STOP (log, reset)
                                 │
              Wedged/Timeout/NotFound → escalate:
                                 │
   Level 1  DICS_PROPCHANGE restart            (src/core)
   Level 2  FrameServer service restart        (src/core)
   Level 3  existing full cycle                (src/core)
   Level 4  composite-parent re-enumeration    (src/core)
   Level 5A DIF_REMOVE MI_00 → wait-gone → parent re-enum → wait-return   (src/core)
   Level 5B composite-parent removal → hub re-enum (whole camera unit)    (src/core)
                                 │
              each level: fresh health check → STOP on success
                                 │
              5B failed → RecoveryExhausted
              + MF_E_REBOOT_REQUIRED_persists → recommend reboot (NEVER automatic)

                              src/core remains the single authority
                                 for actual device/system changes
```

Division of responsibility (per `AGENTS.md` and the watchdog golden rule):

- **`src/watchdog`** owns detection (health check), classification, orchestration/escalation, retry/cooldown state, timers, and logging. It contains **no** SetupAPI/CfgMgr/SCM implementation (verified by grep audit) and never disables the camera on its own authority.
- **`src/core`** owns every state-changing operation. Additive primitives only — the pre-existing camera functions are untouched (verified: `git diff -- src/core/` is insertions-only):

| Primitive (new, `src/core/MyForm_Camera.cpp`) | Mechanism | Stage codes |
|---|---|---|
| `RestartCameraHardware(targetId)` | `DIF_PROPERTYCHANGE` + `DICS_PROPCHANGE` — the documented "stops and restarts the device" (what `devcon restart` / `pnputil /restart-device` do). Does **not** change the enabled state. Captures `DI_NEEDRESTART`/`DI_NEEDREBOOT` deferral into `g_lastPropChangeFlags`. | 30–36 |
| `RestartCameraFrameServerService()` | SCM restart of the `FrameServer` service (Windows Camera Frame Server). Absence (`ERROR_SERVICE_DOES_NOT_EXIST`) is reported, not failed hard. Bounded 10 s waits per transition — never hangs. Only this one service. | 40–50 |
| `ReenumerateCameraParent(targetId, &parentId, &siblingStatus)` | `CM_Get_Parent` + `CM_Reenumerate_DevNode(CM_REENUMERATE_NORMAL)` on the **composite parent** — the correct tree level for re-detection (the existing CfgMgr toggle re-enumerates the `MI_00` leaf, which has no children, i.e. a no-op). **Refuses non-composite targets** (`&MI_` guard) so it can never re-enumerate a whole hub. Returns before/after sibling devnode status (IR-safety evidence). | 60–65 |
| `RemoveAndReenumerateCameraHardware(targetId, deepScope, &report)` | **Level 5 (deep removal — the "software unplug/replug")**: `DIF_REMOVE` + `SP_REMOVEDEVICE_PARAMS`/`DI_REMOVEDEVICE_GLOBAL` deletes the device instance (devnode + registry keys) **without touching the driver package** and — unlike propchange/disable/query-remove — is **not vetoed by open holders**. Bounded wait-gone (≤10 s, `CM_Locate_DevNodeW` → `CR_NO_SUCH_DEVNODE`), `CM_Reenumerate_DevNode(SYNCHRONOUS)` on the smallest appropriate ancestor (5A: composite parent; 5B: its hub), bounded wait-return+started for **every expected sibling interface** (same instance IDs). Scope 5B is refused for non-`&MI_` targets so a hub can never be removed. | 70–80 |

New attribution globals (same Interlocked pattern): `g_lastPropChangeFlags`, `g_lastServiceState`, `g_lastServiceError`.

## 3. Health check (the new sensor)

Native worker on a dedicated thread (the UI/message pump never blocks — `WndProc` must stay responsive for session/power events). Sequence:

```text
MFStartup(MF_VERSION, MFSTARTUP_NOSOCKET)          (per-thread; paired with MFShutdown)
  → MFCreateAttributes + MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP   (NO category attribute)
  → MFEnumDeviceSources
  → match MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK against the configured
    target instance ID (case-insensitive, '\'→'#' normalization, link-contains-target)
  → ActivateObject(IID_IMFMediaSource)
  → MFCreateSourceReaderFromMediaSource
  → ReadSample(FIRST_VIDEO_STREAM)   (synchronous, blocks until first frame or error)
  → classify → cleanup (reader release → IMFMediaSource::Shutdown → release → MFShutdown)
```

Design points from the research:

- **No category filtering is set.** Default VIDCAP enumeration lists normal video cameras (including the configured RGB target) and does not list `KSCATEGORY_SENSOR_CAMERA` devices — the IR/Windows Hello camera is not even enumerable by this check. The symbolic-link match additionally guarantees **no camera other than the configured target** can ever be activated.
- `ReadSample` is the proof: activation alone misses run-state failures (Microsoft's own HLK camera tests validate "the device is producing valid samples", budgeting 9–10 s per operation).
- MF has no internal timeouts (`GetEvent`/`ReadSample` can block indefinitely on a wedged camera). The orchestrator owns the deadline (~10.5 s, HLK-aligned): a `WinForms::Timer` polls the worker's done-event every 500 ms; past the budget it marks the context **orphaned and abandons it** — never killed mid-call. An abandoned worker self-cleans (including `MFShutdown`) if it ever completes; otherwise it leaks by design (bounded: one per timeout; documented and accepted — the alternative, killing a thread holding device handles, is worse).
- Cleanup runs on every exit path (`Stop`-less path: reader release → `IMFMediaSource::Shutdown` → release all → `MFShutdown`), per the Media Foundation doc-mandated shutdown requirement.

### Result classification

| Verdict | Meaning | Action |
|---|---|---|
| `Healthy` | First video frame obtained | none — log and continue |
| `Busy` | `MF_E_VIDEO_DEVICE_LOCKED` / `MF_E_HW_MFT_FAILED_START_STREAMING` — another client holds the camera | none — the device **responds**; never recovered over |
| `PrivacyDenied` | `E_ACCESSDENIED` (Settings → Privacy → Camera) | none — **never** a recovery candidate |
| `NotFound` | Target not enumerable (or device-invalidated mid-check) | no hardware recovery: one delayed re-check (15 s), then `CameraRecovery_NotFoundPersistent`; routine triggers revisit later |
| `Wedged` | Init/stream failure consistent with Issue #1 | recovery candidate → ladder |
| `Timeout` | Budget exceeded, worker abandoned | recovery candidate → ladder |
| `Error` | Camera-stack infrastructure failure (`MFStartup`/enumeration) | recovery candidate → ladder (a broken MF infra can be what rung 2 fixes) |

## 4. Recovery ladder

One health-check failure opens an *incident* (correlated by `Op=CAMREC-…`). Levels execute strictly in order — **after every level a fresh Media Foundation health check runs, and the ladder stops at the first healthy verdict**. An API returning success is never treated as recovery; only the health check decides:

1. **Level 1 — `DICS_PROPCHANGE` devnode restart** (device-scoped, least invasive; forces the Frame Server to drop/re-acquire the device). Known limit (measured live 2026-10-04): deferred with `DI_NEEDREBOOT` when handles are open — a silent no-op in exactly the Issue #1 state.
2. **Level 2 — Frame Server service restart** (stack-scoped; momentarily disrupts every camera client). Measured live: succeeds but does not clear the wedge — the state is not resident in the service.
3. **Level 3 — existing full cycle** `RecoverCameraHardware(target, true)` — the proven pipeline, invoked, never reimplemented. Note: its disable step can itself be handle-vetoed; PnP-level verification cannot distinguish that, which is why the health check is the only success criterion.
4. **Level 4 — composite-parent re-enumeration** (re-detects sibling interfaces including IR; before/after sibling status logged). Known limit: a no-op for already-started children.
5. **Level 5A — deep device-instance removal, RGB scope:** `DIF_REMOVE` (`DI_REMOVEDEVICE_GLOBAL`) on the configured RGB interface devnode only → bounded wait-gone (≤10 s) → `CM_Reenumerate_DevNode` on the **composite parent** (smallest ancestor that re-creates it) → bounded wait-return+started (≤25 s) for **every expected sibling interface, same instance IDs**. This is the documented software equivalent of Uninstall device → Scan for hardware changes; it is **not vetoed by open handles** and necessarily destroys the `DN_NEED_RESTART` state, because it destroys the devnode carrying it.
6. **Level 5B — deep removal, whole-unit scope** (only if 5A failed): the same lifecycle against the **composite parent itself** (refused for non-`&MI_` targets, so a hub can never be removed), re-enumerating its hub ancestor; the IR/DFU siblings are recreated with the camera unit and every one of them must return — a partial recovery counts as failure.

After **every** level the target is re-located fresh (no DEVINST is ever cached — every core primitive re-locates by the stable device instance ID) and a **fresh health check** runs (5 s settle via the gap timer first; the driver-store reinstall after a removal takes seconds). A level whose worker exceeds its budget aborts the incident (`CameraRecovery_RungTimeout` → park) — a hung device/service operation means an unknown stack state, and stacking more operations onto it is unsafe.

Exhaustion (after 5B) logs `CameraRecovery_RecoveryExhausted` with the final health-check result and HRESULT — **if `MF_E_REBOOT_REQUIRED` persists, the log says so explicitly** (`MF_E_REBOOT_REQUIRED_persists | Recommendation=RebootWindows`) — then parks. Reboot is never automatic; it remains the user's final fallback (Level 6).

## 5. When the health check runs

| Trigger | Mechanism | Notes |
|---|---|---|
| `EnableTransition` | 10 s transition poll observes expected-enabled + PnP-enabled right after an expected-disabled episode | primary trigger — covers unlock, resume, startup enable |
| `Startup` | one-shot 15 s after `Arm()` | covers the startup-enable path even without a transition |
| `Backup` | every 10 min | low-frequency drift detection |
| `PostRungN` | 5 s settle after a successful rung | incident-internal, bypasses cooldown |
| `NotFoundRecheck` | 15 s after a `NotFound` | exactly one, never more |

Routine checks are cooled down (≥ 60 s apart). **Every trigger and every result passes the full guard chain — the watchdog never health-checks or recovers while the camera is expected-disabled** (lock/suspend/shutdown/monitoring-off): `IsMonitoringActive()`, `IsSystemEndingActive()`, `IsCameraExpectedEnabled()`, and a PnP pre-check (a PnP-disabled camera belongs to the existing watchdogs). Guards are re-checked after every worker returns, so a lock mid-incident aborts it immediately.

## 6. Cooldown / loop protection

- Bounded per incident: ≤ 6 recovery levels (1, 2, 3, 4, 5A, 5B) + ≤ 7 health checks, then park.
- After exhaustion: parked 30 min. Parking is cleared early by the next expected-disabled episode (lock) — a fresh enable-episode gets a fresh allowance. No tight loops are possible: every path is either parked, cooled down, or guarded by the expected-state chain.
- Recovery-in-progress protection: single state machine (`Idle`/`Checking`/`Recovering`); all triggers coalesce.
- No automatic reboot, no second camera authority, no new mutex/event system, no installer changes.

## 7. Interaction with existing watchdogs

- `CameraFailsafe` (90 s poll) and `RecoveryLoopFailsafe` (5 s/30 s) are **unchanged** and keep owning the "unexpectedly PnP-disabled" contract. This watchdog owns a different sensor (camera-stack health) and different rungs (restart/re-enumerate/service).
- A PnP-disabled camera is skipped here (Debug log `CameraRecovery_SkippedPnpDisabled`) — no double-acting.
- Lifecycle: instantiated in `main.cpp` beside `RecoveryLoopFailsafe` (command workers skipped), armed on `Load`, disarmed on real `FormClosing`. Unlike `RecoveryLoopFailsafe` (see `docs/KNOWN_ISSUES.md` #7), `UserClosing` — the hide-to-background cancel — does **not** disarm it, so it keeps watching while the daemon is hidden.
- Logging uses the existing framework (`MyForm::LogFailsafeExWithDevice`, category `FAILSAFE`, `Op=CAMREC-…`): `CameraRecovery_Start / HealthCheck / HealthCheckResult / RungStart / RungResult / Recovered / RungTimeout / Aborted / RecoveryExhausted / NotFoundPersistent / SkippedPnpDisabled / SkippedExpectedDisabled`. Levels are logged as `Rung=1…4, 5A, 5B`. Results carry `Stage=`, `SetupErr=`, `CfgMgr=`, `PropFlags=0x…`, `ServiceState=`, `ServiceErr=` plus level-specific detail — Level 5 logs the full removal lifecycle in one correlated line: `Scope=5A|5B | Removed=<id> | Ancestor=<hub/parent id> | GoneMs= | ReturnMs= | Before{siblings} After{siblings}`.

## 8. Windows APIs used

- **Media Foundation** (health check, native thread): `MFStartup`/`MFShutdown`, `MFCreateAttributes`, `MFEnumDeviceSources`, `IMFActivate::ActivateObject`, `IMFActivate::GetAllocatedString`, `MFCreateSourceReaderFromMediaSource`, `IMFSourceReader::ReadSample`, `IMFMediaSource::Shutdown`. Libs: `mfplat.lib`, `mfcore.lib` (exports `MFEnumDeviceSources`), `mfreadwrite.lib`, `mfuuid.lib`, `ole32.lib` (`CoTaskMemFree`).
- **SetupAPI** (Levels 1 and 5): `SetupDiGetClassDevs`/`SetupDiEnumDeviceInfo`/`SetupDiGetDeviceInstanceId`, `SetupDiSetClassInstallParams` — Level 1: `(DIF_PROPERTYCHANGE, DICS_PROPCHANGE)`; Level 5: `(DIF_REMOVE, SP_REMOVEDEVICE_PARAMS/DI_REMOVEDEVICE_GLOBAL)` — `SetupDiCallClassInstaller`, `SetupDiGetDeviceInstallParams` (`DI_NEEDRESTART`/`DI_NEEDREBOOT`).
- **Service Control Manager** (Level 2): `OpenSCManagerW`, `OpenServiceW`, `QueryServiceStatus`, `ControlService(SERVICE_CONTROL_STOP)`, `StartServiceW` — bounded state-transition waits.
- **Configuration Manager** (Levels 4/5 + status checks): `CM_Get_Parent`, `CM_Reenumerate_DevNode` (`CM_REENUMERATE_NORMAL` L4 / `CM_REENUMERATE_SYNCHRONOUS` L5), `CM_Locate_DevNodeW` (Level 5 wait-gone/wait-return polling), `CM_Get_Child`/`CM_Get_Sibling`/`CM_Get_Device_IDW`/`CM_Get_DevNode_Status` (sibling discovery + started-state verification), plus the existing `LocateCameraDevInst`/`GetCameraHardwareDisabledState`.
- Threading: `CreateThread` + manual-reset events; zero-timeout `WaitForSingleObject` polling from a UI-thread timer (never blocking).

## 9. Risks & mitigations

| Risk | Mitigation |
|---|---|
| Level 5A/5B removes a device | Scope discipline: 5A touches only the configured RGB interface devnode; 5B requires an `&MI_` composite target so the removed node is provably the camera unit, never a hub. Driver package is never deleted (`DI_REMOVEDEVICE_GLOBAL` removes the instance↔driver association only). |
| Device absent if re-enumeration fails after Level 5 | Bounded wait-gone (10 s) *before* re-enumeration; re-enumeration is synchronous on the smallest appropriate ancestor; wait-return polls every expected sibling by instance ID (25 s). Failure paths log `Phase=… Failed` and the incident aborts — a further manual "scan for hardware changes" or reboot always re-detects the device. |
| Level 5B re-detects the IR/DFU siblings | 5B runs only after 5A failed; every expected sibling must return **started** or the level is a failure (partial recovery is never success); before/after topology logged. Expected siblings are discovered dynamically (children of the composite), not hardcoded. |
| Levels 2/4/5 are stack-wide or re-detect siblings | Ordered escalation: each level only runs after every less-invasive level demonstrably failed the fresh health check; expected-enabled only (never during lock-screen Hello) |
| No non-reboot recovery is guaranteed against `MF_E_REBOOT_REQUIRED` (owner is most probably the per-boot PEAuth trust session — see `docs/Issue1_Deep_Device_Recovery_Research.md` §E) | Honest logging: exhaustion records the persisting HRESULT and recommends reboot; Level 5's outcome (logged with the fresh devnode's status) is the designed experiment that answers the open question |
| Hung MF/SCM/device call | Worker abandoned past budget (leak-by-design, never killed mid-call); rung timeout aborts the incident instead of escalating; if a health worker was orphaned, Level 5 logs `Note=OrphanedHealthWorkerMayHoldHandles` before proceeding (removal is not handle-vetoed, so the stuck worker's I/O fails and it self-cleans) |
| Stale device identity after restart/re-enumeration/removal | No DEVINST is ever cached; every core primitive re-locates by instance ID; Level 5 requires the *same* instance IDs back; health check re-matches the symbolic link |
| Orphaned health worker holds a shared frame-server client | Frame Server sharing means later checks still work; device-level rungs forcibly release it; exclusive-access machines (Frame Server disabled by registry) are documented as a limitation |

## 10. Test results

| Check | Result |
|---|---|
| Build `Release|x64` | **PASS** — 0 errors; `x64\Release\Windows_Hello_Fix_v2_2_x64.exe` (546,304 bytes) |
| Build `Release|Win32` | **PASS** — 0 errors; `Release\Windows_Hello_Fix_v2_2_x86.exe` (540,672 bytes) |
| Warnings | **No new warnings** — 5× C4793 on both platforms, byte-identical to the pre-change baseline (verified by building pristine `HEAD` in a temporary worktree and diffing warning sets) |
| Static audit — no device manipulation in watchdog | **PASS** — no `DICS_*`/`DIF_REMOVE`/`CM_Reenumerate`/`CM_Enable/Disable_DevNode`/`SetupDiCallClassInstaller`/SCM calls in `src/watchdog/CameraRecoveryFailsafe.cpp`; only invocations of the five core primitives + read-only `GetCameraHardwareDisabledState` |
| Static audit — core diff additive | **PASS** — `git diff -- src/core/` is insertions-only; all pre-existing camera functions untouched |
| Static audit — MF call confinement | **PASS** — MF/COM calls exist only inside the native health worker |
| Runtime: health check against a **real Issue #1 wedge** | **RUNTIME TESTED (2026-10-04 19:18)** — detected `Result=Wedged HRESULT=0xC00D7167` on the live wedge (see `diagnostic.log`, `CAMREC-000005`) |
| Runtime: Levels 1–4 against the real wedge | **RUNTIME TESTED — INSUFFICIENT** — all four executed successfully at API level (rung 1 deferred with `DI_NEEDREBOOT=0x100`; rung 2 restarted FrameServer, State=4; rung 3 cycled; rung 4 re-enumerated the parent) and the wedge persisted → `RecoveryExhausted … MF_E_REBOOT_REQUIRED_persists`. This measured result is what motivated Levels 5A/5B (`docs/Issue1_Deep_Device_Recovery_Research.md`) |
| Runtime: **Level 5A/5B against the real wedge** | **NOT YET TESTED** — the deep-removal build postdates the captured wedge. Reproduce with `tools\CameraStressRepro.ps1 -Mode MidStreamToggle -UntilFailure`, then let the daemon run the ladder; the `Rung=5A/5B` result lines (`GoneMs/ReturnMs/Before/After`) will answer whether device-instance recreation clears `MF_E_REBOOT_REQUIRED` |
| Runtime: healthy-camera checks, lock/unlock regression | Health check verified healthy on the live camera (probe + daemon); lock/unlock/suspend regression **NOT TESTED** (static review only: no existing event/WndProc/core behavior was modified) |

Honest statement: **compilation, linkage, and the static architecture contract are verified. The health check and Levels 1–4 have run against a real Issue #1 wedge (detected it, could not clear it). Levels 5A/5B are compiled and architecturally audited but their recovery efficacy is unverified** — they exist precisely to run the discriminating experiment from `docs/Issue1_Deep_Device_Recovery_Research.md` §E.5.

## 11. Limitations

1. **Not a guaranteed fix.** The deep-recovery research (`docs/Issue1_Deep_Device_Recovery_Research.md` §E) found the wedge's owner is most probably the **per-boot PEAuth trust session** (kernel Protected Media Path state reached because the camera DMFTs are Windows Hello–signed trusted components). If that is confirmed, **no device-level operation — including Level 5 — clears the wedge and a reboot is genuinely required**; Level 5 exists to run that experiment in-session and the failsafe says so honestly when it fails.
2. **Level 5 makes the camera temporarily disappear.** Between `DIF_REMOVE` and successful re-enumeration the device is gone from PnP (bounded: ≤10 s wait-gone + ≤25 s wait-return). If re-enumeration failed, a manual "scan for hardware changes" or reboot always re-detects it; the driver package is never deleted.
3. **Level 5B recreates the IR camera too.** It runs only after 5A failed, while expected-enabled (never during lock-screen Hello), and requires every sibling to return started — otherwise it is logged as a failure and the incident exhausts.
4. `NotFound` is deliberately never recovered (per the approved classification policy) — a vanished device is left to the routine triggers and to the existing watchdogs' domain.
5. Cameras that never appear in default MF VIDCAP enumeration (exotic/virtual devices) will classify `NotFound` — the health check does not apply to them.
6. If the Frame Server sharing mode is disabled by registry (`EnableFrameServerMode=0`), the brief health check takes the camera exclusively and an orphaned timeout worker would hold it — such setups are outside the supported envelope.
7. Levels 1–4 are **measured insufficient** against the reproduced Issue #1 wedge; Level 5's efficacy is the open experiment (§10).
