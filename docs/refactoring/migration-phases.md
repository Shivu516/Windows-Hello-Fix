# Migration Phases

The refactor is a sequence of small, verified, committed phases. **It is not one
giant rewrite.** Every phase ends with a build checkpoint, a behavior checkpoint,
and a commit.

## Strategy

```
Known-good v2.0
      ↓
Phase 0: Baseline snapshot
      ↓
Phase 1: Documentation + src/ scaffolding      (this phase)
      ↓
Phases 2–8: Extract low-risk subsystems one at a time
      ↓
Phase 9:  Extract the controller (highest risk)
      ↓
Phase 10: Reduce MyForm to a thin UI shell
      ↓
Phase 11: Cleanup (dead code, resources, project hygiene)
      ↓
Phase 12: v2.1 coordinated rename
```

## Phase Template

Every extraction phase uses this template:

- **Files involved** — which files change.
- **Functionality moved** — what code moves (verbatim where possible).
- **Dependencies** — what the moved code needs.
- **Risk** — Low / Medium / High, with the specific hazard.
- **Build checkpoint** — the command that must succeed.
- **Verification checkpoint** — the behavior checks that must pass (from
  `../testing/regression-checklist.md`).
- **Rollback** — how to undo (git revert of one commit).
- **Resulting architecture** — what the tree looks like after.

## Phase 0 — Baseline

- **Files involved:** none (read-only).
- **Functionality:** none.
- **Risk:** none.
- **Build checkpoint:** open `.sln`, build `Release | x64` successfully, confirm
  `x64\Release\Windows_Hello_Fix_v2_0.exe` exists.
- **Verification checkpoint:** capture a reference `%APPDATA%\Windows Hello
  Fix\diagnostic.log` after a lock/unlock cycle; archive the working `x64\Release`
  output.
- **Rollback:** n/a.
- **Resulting architecture:** unchanged v2.0.

## Phase 1 — Documentation + Scaffolding  *(current phase, completed)*

- **Files involved:** `docs/**`, `src/**` (new only).
- **Functionality:** none. Creates the architecture reference and the `src/`
  directory skeleton with module markers.
- **Risk:** none (additive; nothing compiled).
- **Build checkpoint:** v2.0 still builds (no project changes).
- **Verification checkpoint:** `git status` shows only new `docs/` and `src/`
  files.
- **Rollback:** `git clean -fd docs src` (before any commit) or revert the
  commit.
- **Resulting architecture:** documentation + empty module scaffolding.

## Phase 2 — `utilities/`

- **Files involved:** new `src/utilities/{Logging,Timing,}.h/.cpp`; `MyForm.h`
  gains `#include` only when a caller is introduced later (kept minimal).
- **Functionality moved:** `TrimTrailingChars`; adopt `ProductionLogger` as the
  unified logger (OutputDebugStringW + timestamp). Additive — no existing callers
  change yet.
- **Dependencies:** none.
- **Risk:** Low.
- **Build checkpoint:** `Release | x64` clean.
- **Verification checkpoint:** no behavior change; logger output identical format.
- **Rollback:** revert commit.
- **Resulting architecture:** `src/utilities/` populated.

## Phase 3 — `system/PrivilegeInfo`

- **Files involved:** new `src/system/PrivilegeInfo.h/.cpp`; `MyForm.h` includes
  it and drops the local definitions.
- **Functionality moved:** `IsCurrentProcessElevatedNative`,
  `GetCurrentProcessIntegrityRid` (verbatim); dead `ElevationChecker` removed.
- **Dependencies:** `advapi32` (`#pragma comment(lib)` stays with the module).
- **Risk:** Low.
- **Build checkpoint:** build passes; diagnostic log still reports the same
  `Elevated=` / `IntegrityRid=` values.
- **Verification checkpoint:** startup log line unchanged in content.
- **Rollback:** revert commit.
- **Resulting architecture:** `src/system/` started.

## Phase 4 — `camera/` core *(largest low-risk step)*

- **Files involved:** new `src/camera/{CameraDevice,CameraHardware,
  CameraRecovery}.h/.cpp`, `src/camera/DeviceError.h`; `MyForm.h` keeps
  forward-declarations and calls through the new headers.
- **Functionality moved (verbatim, signatures unchanged):**
  `CameraDeviceInfo`, `ScanSystemCameras`, `ToggleCameraHardware`,
  `LocateCameraDevInst`, `ToggleCameraHardwareCfgMgr`,
  `GetCameraHardwareDisabledState`, `VerifyCameraHardwareState`,
  `SetCameraHardwareStateVerified`, `RecoverCameraHardware`,
  `RestoreAllCameraHardware`, `RecordHardwareToggleTime`; the `g_last*` globals
  move to `DeviceError`.
- **Dependencies:** `setupapi`, `cfgmgr32`, `devguid`.
- **Risk:** Medium (PnP ops), but bodies are self-contained native code — safe to
  move verbatim.
- **Build checkpoint:** `Release | x64` clean.
- **Verification checkpoint:** full regression checklist (device discovery,
  disable, enable, verification, retry, recovery, missing device).
- **Rollback:** revert commit.
- **Resulting architecture:** `src/camera/` populated; `MyForm.h` begins to shrink.

## Phase 5 — `config/`

- **Files involved:** new `src/config/{ConfigPaths,ConfigStore}.h/.cpp`;
  `MyForm.h` delegates.
- **Functionality moved:** `GetConfigFilePath`, `GetDiagnosticLogFilePath`,
  `WriteDiagnosticLog`, `WriteDiagnosticLogWithDevice`, `SaveConfigState`,
  `LoadConfigState`, `EnsureConfigFileExists`, `TrimTrailingChars` (or to
  utilities).
- **Dependencies:** `utilities/`.
- **Risk:** Medium. Start with the managed implementation for exact parity;
  native port is a separate isolated phase with an encoding checkpoint.
- **Build checkpoint:** build clean; `config.txt`/`diagnostic.log` byte-identical
  formats.
- **Verification checkpoint:** save/load round-trip; `\r\n` sanitization still
  works; log lines identical.
- **Rollback:** revert commit.
- **Resulting architecture:** `src/config/` populated.

## Phase 6 — `application/CommandLine`

- **Files involved:** new `src/application/CommandLine.h/.cpp`; `main.cpp` and
  `MyForm_Load` call it.
- **Functionality moved:** consolidate the three argument-scan sites
  (`main.cpp:16–25`, `MyForm_Load:944–953`, `IsRestoreCameraCommand`/
  `IsDisableCameraCommand`) into one parser.
- **Dependencies:** none.
- **Risk:** Low–Medium (command-mode exit paths). Behavior must be identical for
  every argument form. Note the existing `/enable-camera` inconsistency is
  preserved (documented, not fixed, unless separately approved).
- **Build checkpoint:** build clean; each command mode exits with the same
  diagnostic log lines.
- **Verification checkpoint:** all command modes (`/restore-camera`,
  `/enable-camera`, `--enable-camera`, `/repair-camera`, `/disable-camera`,
  `--disable-camera`, `/background`, `--background`).
- **Rollback:** revert commit.
- **Resulting architecture:** `src/application/` started.

## Phase 7 — `events/` decode + cooldown

- **Files involved:** new `src/events/{WinEventDecoder,EventCooldown}.h/.cpp`;
  `MyForm::WndProc` uses them.
- **Functionality moved:** the message-classification portion of `WndProc`
  (raw constants → `SystemEvent` enum, GUID filtering, 1500 ms dedup statics).
  Policy stays in `WndProc` for now (moves to the controller in Phase 9).
- **Dependencies:** `utilities/`.
- **Risk:** Medium — event misclassification is subtle. Keep the policy mapping
  table (`../architecture/data-flow.md`) open during the change.
- **Build checkpoint:** build clean.
- **Verification checkpoint:** lock/unlock/sleep/resume/lid/power-broadcast +
  duplicate-event suppression all log identically.
- **Rollback:** revert commit.
- **Resulting architecture:** `src/events/` started.

## Phase 8 — `events/NotificationRegistrar` + `system/SingleInstance`

- **Files involved:** new `src/events/NotificationRegistrar.h/.cpp`,
  `src/system/{SingleInstance,ProcessUtils}.h/.cpp`; `MyForm_Load` uses them.
- **Functionality moved:** power/WTS registration (+6×500 ms retry) into RAII;
  mutex/event handle creation + `taskkill` helper.
- **Dependencies:** `user32`, `wtsapi32`, `kernel32`.
- **Risk:** Medium — handle lifetime changes. Registration handles are released
  by the registrar instead of the form.
- **Build checkpoint:** build clean.
- **Verification checkpoint:** registration success log; second-instance wake
  path; no handle leaks across start/stop cycles.
- **Rollback:** revert commit.
- **Resulting architecture:** `src/events/` and `src/system/` complete.

## Phase 9 — `application/ApplicationController` *(highest risk)*

- **Files involved:** new `src/application/ApplicationController.h/.cpp` (with an
  `IUiSink` callback interface); `MyForm.h` delegates.
- **Functionality moved:** the state machine and policy — `isMonitoring`,
  `isAlreadyDisabled` lock, event→operation mapping, `DisableTargetCameraHardware`,
  `EnableTargetCameraHardware`, `RestoreConfiguredCameraHardware`,
  `TryGetTargetCameraInstanceId` ordering, the single-instance flow, ghost
  recovery, startup orchestration, shutdown policy (disable-vs-enable).
- **Dependencies:** all modules.
- **Risk:** High — startup ordering, ghost recovery, destructor semantics, and
  the event-policy mapping are intertwined. Do this only after Phases 2–8.
- **Build checkpoint:** build clean.
- **Verification checkpoint:** full regression checklist, especially lifecycle
  and shutdown; diagnostic log line-for-line parity on lock/unlock/sleep/shutdown.
- **Rollback:** revert commit (largest single revert; keep Phase 9 as one commit).
- **Resulting architecture:** `src/application/` complete; `MyForm.h` now contains
  mostly presentation.

## Phase 10 — `ui/MyForm` thin shell

- **Files involved:** new `src/ui/{MyForm.h,MyForm.cpp,UiConstants.h}`;
  root `MyForm.h` content migrates here.
- **Functionality moved:** presentation — `InitializeComponent`, control handlers,
  `WndProc` dispatch-only, `FormClosing` hide behavior, wake-listener +
  bring-to-front.
- **Dependencies:** `application/`, `camera/` (scan), `events/` (decode).
- **Risk:** Medium — this is the phase where the 1,300-line header finally
  disappears.
- **Build checkpoint:** build clean.
- **Verification checkpoint:** full regression checklist.
- **Rollback:** revert commit.
- **Resulting architecture:** the monolith is gone; root `MyForm.h` removed or
  reduced to a thin forwarding header, then deleted.

## Phase 11 — Cleanup

- **Files involved:** root-level dead code and project hygiene.
- **Functionality:** remove dead code (`ProductionUtilities.h` superseded pieces,
  unused members `cameraStateInitialized`, `restartQueuedByMismatch`,
  `lastCameraToggleTick`, `lastToggleTime`, `COOLDOWN_MILLISECONDS`, dormant
  `TryEnterHardwareToggleCooldown`), resolve the orphan `.rc` decision,
  consolidate `resource.h`/`resource1.h`, fix the icon reference (relative path
  + committed asset), pin `WindowsTargetPlatformVersion`, repair Win32 configs,
  drop stale vcxproj doc items.
- **Risk:** Low–Medium.
- **Build checkpoint:** clean build; **clean-checkout build** must now succeed.
- **Verification checkpoint:** fresh clone → open → build `Release | x64` →
  exe runs.
- **Rollback:** revert commit.
- **Resulting architecture:** clean, reproducible, source-controlled build.

## Phase 12 — v2.1 Coordinated Rename

- **Files involved:** all names in one commit: project/exe name, `.sln`,
  `.vcxproj`, `.vcxproj.filters`, `.rc`, `resource.h`, `app.manifest` identity,
  mutex name, wake-event name, `taskkill` target, form title, README, version
  strings.
- **Functionality:** branding only. The rename must be atomic so the ghost-recovery
  path never references a stale exe name.
- **Risk:** Medium (renaming is the classic "miss one reference" bug).
- **Build checkpoint:** clean build producing `Windows_Hello_Fix_v2_1.exe`.
- **Verification checkpoint:** full regression checklist + single-instance +
  force-reset paths with the new name.
- **Rollback:** revert commit.
- **Resulting architecture:** the v2.1 modular baseline.

## Future (not part of this refactor)

- Async hardware queue (from `WndProc_Redesign.txt`) as an explicit feature.
- Self-test CLI mode.
- ARM64 build (blocked by C++/CLI UI — see `../build/architecture-support.md`).

## Related Documents

- `../architecture/migration-map.md` — granular code→module table.
- `../architecture/architecture-contract.md` — rules each phase must respect.
- `../testing/regression-checklist.md` — the per-phase verification checklist.
- `../refactoring/risk-register.md` — the risks this sequence mitigates.