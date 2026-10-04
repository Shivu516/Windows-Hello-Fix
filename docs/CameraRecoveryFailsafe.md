# `CameraRecoveryFailsafe` — Camera-Stack Health Watchdog (Issue #1)

> **Status:** IMPLEMENTED — builds verified `Release|x64` and `Release|Win32`, 0 errors, 0 new warnings. **Not a guaranteed fix** — see §Limitations.
> **Date:** 2026-10-03 · **Files:** `src/watchdog/CameraRecoveryFailsafe.h` / `.cpp` (+ additive `src/core` primitives)
> **Investigation record:** `docs/Issue1_Camera_Recovery_Research.md` · **Plan:** `docs/Issue1_Failsafe_Plan.md`

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
                    detects / decides recovery
                                 │
                ┌────────────────┴────────────────┐
                │                                 │
          MF health check                   Recovery ladder
     (native thread, ~10 s budget)                 │
                │                    ┌────────────┼──────────────┐
        classify result             ↓            ↓              ↓
     Healthy/Busy/Privacy →    Rung 1        Rung 2         Rung 3
        stop, log         DICS_PROPCHANGE   FrameServer    existing full
                │           (src/core)      service restart   cycle (src/core)
        Wedged/Timeout →          │            (src/core)         │
        escalate                  └────────────┴──────────────────┘
                                                │
                                     Rung 4: composite-parent
                                       re-enumeration (src/core)
                                                │
                                       fresh health check
                                                │
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

One health-check failure opens an *incident* (correlated by `Op=CAMREC-…`). Rungs execute strictly in order, **stopping at the first healthy verdict**:

1. **Rung 1 — `DICS_PROPCHANGE` devnode restart** (device-scoped, least invasive; forces the Frame Server to drop/re-acquire the device).
2. **Rung 2 — Frame Server service restart** (stack-scoped; aimed at the confirmed `MF_E_REBOOT_REQUIRED` layer; only after rung 1 failed because it momentarily disrupts every camera client).
3. **Rung 3 — existing full cycle** `RecoverCameraHardware(target, true)` — the proven pipeline, invoked, never reimplemented.
4. **Rung 4 — composite-parent re-enumeration** (heaviest: re-detects sibling interfaces including the IR camera; therefore only while expected-enabled, with before/after sibling status logged).

After **every** rung: re-locate/resolv the target fresh (no DEVINST is ever cached — every core primitive re-locates by the stable device instance ID) and run a **fresh health check**. A rung whose worker exceeds its budget aborts the incident (`CameraRecovery_RungTimeout` → park) — a hung device/service operation means an unknown stack state, and stacking more operations onto it is unsafe.

Exhaustion logs `CameraRecovery_RecoveryExhausted` with the final health-check result and HRESULT — **if `MF_E_REBOOT_REQUIRED` persists, the log says so explicitly** (`MF_E_REBOOT_REQUIRED_persists | Recommendation=RebootWindows`) — then parks. Reboot is never automatic; it remains the user's final fallback.

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

- Bounded per incident: ≤ 4 recovery rungs + ≤ 5 health checks, then park.
- After exhaustion: parked 30 min. Parking is cleared early by the next expected-disabled episode (lock) — a fresh enable-episode gets a fresh allowance. No tight loops are possible: every path is either parked, cooled down, or guarded by the expected-state chain.
- Recovery-in-progress protection: single state machine (`Idle`/`Checking`/`Recovering`); all triggers coalesce.
- No automatic reboot, no second camera authority, no new mutex/event system, no installer changes.

## 7. Interaction with existing watchdogs

- `CameraFailsafe` (90 s poll) and `RecoveryLoopFailsafe` (5 s/30 s) are **unchanged** and keep owning the "unexpectedly PnP-disabled" contract. This watchdog owns a different sensor (camera-stack health) and different rungs (restart/re-enumerate/service).
- A PnP-disabled camera is skipped here (Debug log `CameraRecovery_SkippedPnpDisabled`) — no double-acting.
- Lifecycle: instantiated in `main.cpp` beside `RecoveryLoopFailsafe` (command workers skipped), armed on `Load`, disarmed on real `FormClosing`. Unlike `RecoveryLoopFailsafe` (see `docs/KNOWN_ISSUES.md` #7), `UserClosing` — the hide-to-background cancel — does **not** disarm it, so it keeps watching while the daemon is hidden.
- Logging uses the existing framework (`MyForm::LogFailsafeExWithDevice`, category `FAILSAFE`, `Op=CAMREC-…`): `CameraRecovery_Start / HealthCheck / HealthCheckResult / RungStart / RungResult / Recovered / RungTimeout / Aborted / RecoveryExhausted / NotFoundPersistent / SkippedPnpDisabled / SkippedExpectedDisabled`. Rung results carry `Stage=`, `SetupErr=`, `CfgMgr=`, `PropFlags=0x…`, `ServiceState=`, `ServiceErr=` plus rung-specific detail (rung 4: parent ID + sibling before/after status).

## 8. Windows APIs used

- **Media Foundation** (health check, native thread): `MFStartup`/`MFShutdown`, `MFCreateAttributes`, `MFEnumDeviceSources`, `IMFActivate::ActivateObject`, `IMFActivate::GetAllocatedString`, `MFCreateSourceReaderFromMediaSource`, `IMFSourceReader::ReadSample`, `IMFMediaSource::Shutdown`. Libs: `mfplat.lib`, `mfcore.lib` (exports `MFEnumDeviceSources`), `mfreadwrite.lib`, `mfuuid.lib`, `ole32.lib` (`CoTaskMemFree`).
- **SetupAPI** (rung 1): `SetupDiGetClassDevs`/`SetupDiEnumDeviceInfo`/`SetupDiGetDeviceInstanceId`, `SetupDiSetClassInstallParams(DIF_PROPERTYCHANGE, DICS_PROPCHANGE)`, `SetupDiCallClassInstaller`, `SetupDiGetDeviceInstallParams` (`DI_NEEDRESTART`/`DI_NEEDREBOOT`).
- **Service Control Manager** (rung 2): `OpenSCManagerW`, `OpenServiceW`, `QueryServiceStatus`, `ControlService(SERVICE_CONTROL_STOP)`, `StartServiceW` — bounded state-transition waits.
- **Configuration Manager** (rung 4 + status checks): `CM_Get_Parent`, `CM_Reenumerate_DevNode(CM_REENUMERATE_NORMAL)`, `CM_Get_Child`/`CM_Get_Sibling`/`CM_Get_Device_IDW`/`CM_Get_DevNode_Status` (sibling evidence), plus the existing `LocateCameraDevInst`/`GetCameraHardwareDisabledState`.
- Threading: `CreateThread` + manual-reset events; zero-timeout `WaitForSingleObject` polling from a UI-thread timer (never blocking).

## 9. Risks & mitigations

| Risk | Mitigation |
|---|---|
| Rung 4 re-detects sibling interfaces (IR camera) | Last rung only; expected-enabled only (never during lock-screen Hello); before/after sibling status logged |
| Rung 2 is stack-wide (all camera clients lose their session briefly) | Only after rung 1 fails; bounded waits; single service; logged with SCM state codes |
| No non-reboot recovery is guaranteed against `MF_E_REBOOT_REQUIRED` | Honest logging: exhaustion records the persisting HRESULT and recommends reboot; the ladder is instrumented so the first real-world incident produces definitive evidence |
| Hung MF/SCM/device call | Worker abandoned past budget (leak-by-design, never killed mid-call); rung timeout aborts the incident instead of escalating |
| Stale device identity after restart/re-enumeration | No DEVINST is ever cached; every core primitive re-locates by instance ID; health check re-matches the symbolic link |
| Orphaned health worker holds a shared frame-server client | Frame Server sharing means later checks still work; a rung-1 device restart forcibly releases it; exclusive-access machines (Frame Server disabled by registry) are documented as a limitation |

## 10. Test results

| Check | Result |
|---|---|
| Build `Release|x64` | **PASS** — 0 errors; `x64\Release\Windows_Hello_Fix_v2_2_x64.exe` (527,872 bytes) |
| Build `Release|Win32` | **PASS** — 0 errors; `Release\Windows_Hello_Fix_v2_2_x86.exe` (522,752 bytes) |
| Warnings | **No new warnings** — 5× C4793 on both platforms, byte-identical to the pre-change baseline (verified by building pristine `HEAD` in a temporary worktree and diffing warning sets) |
| Static audit — no device manipulation in watchdog | **PASS** — no `DICS_*`/`CM_Reenumerate`/`CM_Enable/Disable_DevNode`/`SetupDiCallClassInstaller`/SCM calls in `src/watchdog/CameraRecoveryFailsafe.cpp`; only invocations of the four core primitives + read-only `GetCameraHardwareDisabledState` |
| Static audit — core diff additive | **PASS** — `git diff -- src/core/` is insertions-only (+24 declarations in `MyForm.h`, +288 lines in `MyForm_Camera.cpp`: 3 functions + 1 static helper + 3 globals); all listed existing camera functions untouched |
| Static audit — MF call confinement | **PASS** — MF/COM calls exist only inside the native health worker |
| Static audit — registration | **PASS** — `.vcxproj` (+2) and `.filters` (+6) entries match the existing watchdog pattern |
| Runtime: healthy camera health check / target matching / classification / transition triggering | **NOT RUN on this session's machine — requires the installed daemon environment and camera hardware; no runtime camera operations were performed during this implementation session** |
| Runtime: recovery ladder against a real Issue #1 wedge | **NOT TESTED — hardware failure state not reproducible on demand** |
| Runtime: lock/unlock/suspend regression | **NOT TESTED** (static review only: no existing event/WndProc/core behavior was modified) |

Honest statement: **compilation, linkage, and the static architecture contract are verified; runtime behavior is not.** The first real-world Issue #1 occurrence should now produce enough telemetry (health-check HRESULTs before/after every rung, SCM states, PnP stage codes, sibling status) to determine which rung — if any — clears the wedge, and to confirm or refute the Frame Server hypothesis.

## 11. Limitations

1. **Not a guaranteed fix.** `MF_E_REBOOT_REQUIRED` literally means Windows believes a reboot is required. The ladder is the documented, least-invasive attempt sequence; a reboot may still be the only cure. The mechanism is designed to say so honestly instead of pretending success.
2. `NotFound` is deliberately never recovered (per the approved classification policy) — a vanished device is left to the routine triggers and to the existing watchdogs' domain.
3. Cameras that never appear in default MF VIDCAP enumeration (exotic/virtual devices) will classify `NotFound` — the health check does not apply to them.
4. If the Frame Server sharing mode is disabled by registry (`EnableFrameServerMode=0`), the brief health check takes the camera exclusively and an orphaned timeout worker would hold it — such setups are outside the supported envelope.
5. Effectiveness of rungs 1/2/4 against the actual wedge is **unverified** until a real incident is logged (by design — see §10).
