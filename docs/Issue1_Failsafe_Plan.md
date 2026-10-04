# Issue #1 Plan — `CameraRecoveryFailsafe` (camera-stack health check + same-session device recovery)

> **Status: PROPOSED — awaiting approval to implement** (research in `docs/Issue1_Camera_Recovery_Research.md`)
> **Date:** 2026-10-03 · **Branch base:** `v2.2`
> **Architecture policy:** per `AGENTS.md` §8 this document is the pre-implementation plan for substantial architecture work. Nothing here is implemented yet.

---

## 1. Current watchdog architecture

Two enabled-state watchdogs observe "ExpectedEnabled vs observed PnP-Disabled" and recover through the single camera pipeline in `src/core/MyForm_Camera.cpp`:

- **`CameraFailsafe`** (owned by `MyForm`): 90 s poll → 10 s confirm → `RecoverCameraHardware(target, false)` + verify; 10/20/40 s backoff, 3 attempts, 30 s cooldown, 45 s startup grace.
- **`RecoveryLoopFailsafe`** (owned by `main.cpp`, attached to Load/FormClosing): 5 s startup check → 30 s poll → 5 s retry, 3 attempts, 30 s cooldown.

Both are managed `System::Windows::Forms::Timer` state machines on the UI thread; both guard on `IsMonitoringActive()`, `IsSystemEndingActive()`, `IsCameraExpectedEnabled()`; both log through `MyForm::LogFailsafe*` with `Op=` correlation. Neither performs device operations itself.

## 2. Existing relevant components

| Component | Role | Reuse for this mechanism |
|---|---|---|
| `src/core/MyForm_Camera.cpp` | single camera authority: `ToggleCameraHardware` (SetupAPI DICS_ENABLE/DISABLE), `ToggleCameraHardwareCfgMgr` (`CM_Enable/Disable_DevNode` + leaf `CM_Reenumerate_DevNode`), `GetCameraHardwareDisabledState`, `VerifyCameraHardwareState`, `SetCameraHardwareStateVerified`, `RecoverCameraHardware(cycle)` | reused as-is (rung 2); pattern template for the two new primitives |
| `GetCameraHardwareDisabledState` | PnP disabled check (`CONFIGFLAG_DISABLED` / `CM_PROB_DISABLED`) | pre-filter: only health-check when PnP-enabled |
| `MyForm` failsafe accessors (`IsMonitoringActive`, `IsSystemEndingActive`, `IsCameraExpectedEnabled`, `TryGetFailsafeTargetId`, `LogFailsafe*`, `NewOperationId`) | read-only state + logging bridge | reused verbatim |
| `main.cpp` watchdog ownership pattern (skip command workers, Load/FormClosing hooks) | lifecycle | same pattern for the new failsafe |
| `diagnostic.log` format (`[ts] [LEVEL] [CATEGORY] Event \| Op= \| Pid= \| …`) | telemetry | extended with `HRESULT=`/`CR=`/`Status=` fields; no new log framework |
| Installer `/restore-camera` warm-up calls | existing full-cycle recovery at install/uninstall | unchanged |

**Not duplicated:** no new SetupAPI/CfgMgr implementation in the watchdog (golden rule), no new mutex/event system, no new config parsing, no change to lock/unlock/power semantics.

## 3. Exact suspected failure state

```text
DEVINST(target): DN_STARTED, no DN_HAS_PROBLEM          ← what all current code checks
CONFIGFLAG_DISABLED: 0, problem code: none
Media Foundation machine state: MF_E_REBOOT_REQUIRED (0xC00D7167 — "We need to reboot the machine")
  → confirmed identity of the Issue #1 sub-code (local SDK Mferror.h, line 2668)
  → Windows Camera app: 0xA00F4241 <CameraSwitchFailed> (0xC00D7167)
  → existing watchdogs: silent ("AlreadyEnabled" everywhere)
```

The sub-code is a Media Foundation (`FACILITY_MEDIASERVER`) machine-level error — the failure is **above the driver**, which is why Device Manager stays green and why a reboot fixes it. The most probable carrier is the **Windows Camera Frame Server** service holding stale/wedged state after hundreds of PnP cycles (community reports tie this signature to the frame server and report its service restart as the working non-reboot fix — Likely). The exact carrier and raise-site are undocumented; the mechanism doubles as the instrumentation to settle this empirically (research doc §4 H0/H3, §9, §12).

## 4. Camera health-check strategy

**Minimum viable check (Level 2+3):** on a dedicated native thread, bounded ~10 s total (HLK budgets 9–10 s/op; MF has no internal timeouts):

1. `MFStartup(MF_VERSION)` on that thread.
2. `MFCreateAttributes` + `MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE = VIDCAP` — **no category attribute** → IR/sensor cameras (`KSCATEGORY_SENSOR_CAMERA`, used by Hello) are not even enumerated (Confirmed via Chromium factory behavior).
3. `MFEnumDeviceSources` → pick the activate whose `MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK` matches the configured target instance ID (case-insensitive, `\`↔`#` normalized). No match → `NotFound`.
4. `ActivateObject` → `MFCreateSourceReaderFromMediaSource` → one `ReadSample(first video stream)`.
5. Verdict: first `OnReadSample(S_OK)` non-null sample within budget → **Healthy**. `E_ACCESSDENIED` at activation → **PrivacyDenied** (never repair). Busy/locked errors (e.g. `0xC00D3704`, `MF_E_VIDEO_DEVICE_LOCKED`) → **Busy** (device responds; treat as healthy, never recover over another app's stream). Any other failure or budget exhaustion → **Wedged**/**Timeout** (repair candidates).
6. Cleanup every path: `Stop` → `IMFMediaSource::Shutdown` → release → `MFShutdown` (doc-mandated).
7. Stuck thread past a hard deadline: abandon (leak, never kill mid-call), count as Timeout.

Cost: ~1–3 s brief real capture (LED blip), shared access through the Frame Server (default since ~1703). Link libraries: `mfplat.lib`, `mfreadwrite.lib`, `mfuuid.lib`.

**When it runs:** (a) after each enable completes (unlock/resume/startup enable) — the exact moments the camera becomes expected-enabled; (b) low-frequency periodic backup (proposed: 10 min, configurable later); (c) never while expected-disabled (lock/suspend/shutdown guard short-circuits first).

## 5. Candidate recovery mechanisms

| Rung | Mechanism | Effect | Targets | Notes |
|---|---|---|---|---|
| 1 | **`DICS_PROPCHANGE` via `DIF_PROPERTYCHANGE`** (new) | stops and restarts the devnode's driver stack in place; enabled-state untouched; forces the Frame Server to drop/re-acquire the device | device-level wedge (H1); partially H3 | Microsoft's documented restart primitive (what devcon restart / pnputil /restart-device do); check `DI_NEEDRESTART/REBOOT` after |
| 2 | **Restart the `FrameServer` service** (Windows Camera Frame Server) via SCM (new) | resets the camera software stack shared by all apps; PnP untouched | **H3 — the confirmed `MF_E_REBOOT_REQUIRED` layer**; H0 | community-reported non-reboot fix for this exact signature (Likely); stack-wide momentary disruption → only after rung 1 fails; verify service exists (absent on very old builds) and log `SERVICE_` status codes |
| 3 | **Existing full cycle** `RecoverCameraHardware(target, true)` | query-remove + teardown + AddDevice + start | H1, H2 | already runs at sign-in; proven safe; insufficient alone per evidence |
| 4 | **`CM_Reenumerate_DevNode(composite parent, CM_REENUMERATE_NORMAL)`** (new) | bus re-scan; re-detects children | H2 | heaviest device rung: briefly disturbs sibling interfaces (IR MI_02, DFU MI_04); last resort, expected-enabled only, re-verify siblings after |
| — | `CM_Setup_DevNode(CM_SETUP_DEVNODE_READY)` | restart problem-state devnode | — | no-op here: device has no problem state; docs prefer DIF anyway |
| — | `CM_Query_And_Remove_SubTreeW` / eject / `DICS_STOP/START` | removal-level | — | docs direct apps to DIF; eject wrong for internal camera; rejected |
| — | `pnputil /restart-device` (external process) | same as rung 1 | — | no capability over in-process API; loses structured errors; rejected |
| — | `EnableFrameServerMode=0` registry bypass | removes frame-server sharing globally | — | needs a restart to take effect; global system-behavior change; rejected |
| — | Registry manipulation / state spoofing | — | — | rejected outright (research doc §7) |

## 6. Chosen recovery mechanism

**Escalation ladder of §5, rungs 1 → 2 → 3 → 4**, each followed by device re-location (`CM_Locate_DevNodeW` by instance ID) and a fresh health check. Stop at the first healthy verdict. After rung 4 also re-verify the IR sibling devnode state. Exhaustion → log `CAMREC RecoveryExhausted` **including the final health-check HRESULT** (if `MF_E_REBOOT_REQUIRED` persists, the log says so explicitly) + recommend reboot in the log; the failsafe parks until a state change (expected-disabled episode or a long cooldown ≥ 30 min). **Reboot is never automatic.**

Honesty constraint carried from the research: `MF_E_REBOOT_REQUIRED`'s literal semantics mean no non-reboot recovery is *guaranteed* — the ladder is the documented, least-invasive-first attempt sequence, and its instrumentation is designed to turn the first real-world incident into a definitive answer.

## 7. Why the alternatives were rejected

See research doc §7: Plan B (transition reduction) is complementary prevention, not a repair for the already-wedged state, and must not touch `src/core`; Plan C (enable-on-demand) has no reliable user-mode "camera in use" detector; Plan D (registry spoofing) creates the exact inconsistent-state hazard the issue warns about; the issue's periodic reset timer adds unverified toggles and cannot confirm success. Weaker checks (PnP-only, MF enumeration-only, activation-only) provably miss the failure class; DirectShow is legacy and doesn't exercise streaming.

## 8. Proposed failsafe name

**`CameraRecoveryFailsafe`** — files `src/watchdog/CameraRecoveryFailsafe.h` / `.cpp`; Op prefix `CAMREC`; log category `FAILSAFE` (existing).

Rejected: `HardwareHero` (breaks the `*Failsafe` naming convention; the mechanism restarts a Windows devnode, it does not repair "hardware"); `DeviceRecoveryFailsafe` / `HardwareRecoveryFailsafe` (broader than the camera-scoped reality); `CameraGuardian` (convention break).

## 9. Files that need modification

| File | Change | Why necessary |
|---|---|---|
| `src/watchdog/CameraRecoveryFailsafe.h` **NEW** | class declaration: health-check orchestration, incident state, ladder | the new mechanism (isolated in `src/watchdog` per task constraints) |
| `src/watchdog/CameraRecoveryFailsafe.cpp` **NEW** | managed orchestration (timers, guards, logging) + native MF health check on a native `std::thread` | same |
| `src/core/MyForm_Camera.cpp` **MINIMAL ADD** | `RestartCameraHardware(targetId)` (rung 1: `DICS_PROPCHANGE`), `RestartCameraFrameServerService()` (rung 2: SCM stop/start of the `FrameServer` service), `ReenumerateCameraParent(targetId)` (rung 4: `CM_Get_Parent` + `CM_Reenumerate_DevNode`) — following the existing `g_last*` attribution + logging conventions | preserves the single camera authority (documented golden rule: only `src/core` flips device/system state); ~90 lines total, additive only, no existing function touched |
| `src/core/MyForm.h` **MINIMAL ADD** | forward declarations for the three new native functions | required for cross-TU visibility (same pattern as existing declarations) |
| `main.cpp` **MINIMAL MODIFY** | instantiate `CameraRecoveryFailsafe` next to `RecoveryLoopFailsafe`, same command-worker skip + Load/FormClosing hooks | lifecycle wiring, existing pattern |
| `Windows_Hello_Fix_v2_2.vcxproj` + `.filters` **MODIFY** | register the two new files | build registration |
| `docs/CameraRecoveryFailsafe.md` **NEW** (at implementation time) | per-file documentation per `AGENTS.md` §12 + test results | documentation requirement |
| `docs/Issue1_Camera_Recovery_Research.md` **NEW** (done) | the investigation record | §13 of the task |

**Unchanged:** `CameraFailsafe.*`, `RecoveryLoopFailsafe.*`, `MyForm_Core/Events/System/UI/Config.cpp`, `MyForm_UI`, installer, `reference/`, `app.manifest`. `src/core` diff limited to the two additive functions + declarations — justified because device-state operations belong to the single camera authority; placing them in `src/watchdog` would create a second camera-control implementation, which the architecture rules forbid.

## 10. Why each modification is necessary (summary)

- The watchdog cannot *see* the failure without a camera-stack-level sensor (health check) — no existing component has one.
- The watchdog cannot *fix* the failure with existing primitives: enable is a no-op (already enabled), the cycle already runs at every sign-in without preventing the wedge, and re-enumeration is currently called at the wrong tree level. Rungs 1, 2 and 4 are new device/system operations and therefore belong in `src/core` (golden rule).
- Escalation must be bounded and observed from a component that owns no camera state — a third watchdog beside the existing two, reusing their guard/logging contract.

## 11. Validation gates (before merge)

1. `Release|x64` and `Release|Win32` rebuild: 0 errors, baseline C4793 warnings only.
2. `grep` audit: no `DICS_PROPCHANGE`/`CM_Reenumerate`/SCM/MF calls in `src/watchdog/CameraRecoveryFailsafe.*` beyond invocation of the three new core functions (no second authority); no disable-path in the new watchdog beyond rung 3's call into the existing cycle.
3. All new API failures logged with `HRESULT=`/`CR=`/`ErrText=`.
4. Static test-matrix walk of research doc §11; hardware-dependent items marked NOT TESTED, never claimed.
5. Regression: existing `Failsafe_*`/`RecoveryLoop_*` behavior unchanged; `src/core` diff shows only the two additive functions.

## 12. Risks

- **Rung 4 touches sibling interfaces** (IR) briefly — mitigated: last resort only, expected-enabled only, post-check of IR devnode state.
- **Rung 2 (Frame Server service restart) is stack-wide** — every camera client on the system momentarily loses its session, including apps streaming unrelated cameras — mitigated: only after rung 1 fails, expected-enabled only (session unlocked, no lock-screen Hello), check service existence first, log SCM status codes; explicitly researched at implementation time whether active-client detection is feasible (research doc §12.6).
- **No non-reboot recovery is guaranteed against `MF_E_REBOOT_REQUIRED`** — the error's literal semantics say only a reboot clears it. Mitigated by honesty: the ladder is bounded and every rung's outcome is logged with HRESULTs; exhaustion produces an explicit "reboot recommended" record instead of pretending success.
- **Health check opens the camera briefly** — mitigated: Frame Server shared access, ≤ ~10 s bound, runs when expected-enabled (not during lock-screen Hello), explicit Busy/Privacy classifications prevent acting over legitimate users.
- **Cannot reproduce the wedge on demand** — mitigated: the implementation doubles as the evidence-gathering instrumentation (research doc §9); recovery effectiveness is honestly labeled unverified until a real-world incident is observed and logged.
