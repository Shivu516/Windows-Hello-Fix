# Modular Extraction Order (Recommended Sequence)

The safest order for dismantling the v2.0 monolith into the `src/` modules
defined in `module-catalog.md` and `source-tree.md`. This document explains
**why** each step belongs where it does in the sequence. It is the reasoning
companion to `migration-phases.md`, which remains the operational plan
(phases, checkpoints, rollback).

## The Guiding Rules

The order is derived from three constraints of the actual code:

1. **Dependency order.** A module must exist before anything that depends on
   it can be rewired. `utilities/` has no dependencies and comes first;
   `application/` depends on everything and comes last.
2. **Risk order.** Pure, self-contained, native code with no managed
   dependencies and no behavioral coupling is safest to move first. Policy,
   lifecycle, and shutdown code is intertwined and moves last.
3. **Verifiability.** Each phase must end with the build green and the
   behavior oracle (`diagnostic.log` lines, timing) unchanged. Phases that
   are easy to verify (no behavior change by construction) precede phases
   whose verification is subtle.

## The Sequence

### Phase A — `utilities/` *(done — Phase 2 of the plan)*

**Scope:** `TrimTrailingChars` (verbatim move); `ProductionLogger` adoption.

**Why first:** zero dependencies, zero managed types, zero behavior coupling —
the moved logger has no live callers and the string helper's only caller
(`LoadConfigState`) keeps working through the include. Risk is none by
construction. It also establishes the `.h`/`.cpp` + namespace + logging
conventions every later module follows.

### Phase B — `system/PrivilegeInfo` *(plan Phase 3)*

**Scope:** `IsCurrentProcessElevatedNative`, `GetCurrentProcessIntegrityRid`.

**Why here:** the functions are dependency-free, self-contained token code.
They are *read* by the policy logging of `Disable/EnableTargetCameraHardware`
and by `Startup_Context`, but nothing depends on their location. Moving them
early removes the first OS-primitive calls from the form header with a
trivially verifiable checkpoint: the `Startup_Context` and `..._Result`
diagnostic lines must contain the same `Elevated=`/`IntegrityRid=` values.

### Phase C — `camera/` core *(plan Phase 4)*

**Scope:** `CameraDeviceInfo`, `ScanSystemCameras`, `ToggleCameraHardware`,
`LocateCameraDevInst`, `ToggleCameraHardwareCfgMgr`,
`GetCameraHardwareDisabledState`, `VerifyCameraHardwareState`,
`SetCameraHardwareStateVerified`, `RecoverCameraHardware`,
`RestoreAllCameraHardware`, cooldown helpers; `g_last*` globals → `DeviceError`.

**Why here:** this is the largest block of **pure native code with no managed
types** — the ideal early extraction. It is the biggest chunk of `MyForm.h`
by volume and removing it shrinks the header most. The risk (medium: raw PnP)
is mitigated by verbatim moves and the fact that the functions are already
self-contained at global scope. It must come **before** the controller
(Phase F) because the controller's policy methods call these functions, and
**before** `config/` only in the sense that it is independent — either could
be first after privileges. This phase also produces `DeviceError`, which
eliminates the shared `g_last*` mutable state the later policy extraction
otherwise depends on.

**Safety note:** verification, retry, and recovery (the hardware-state safety
layer, SF-5…SF-8, SF-13, SF-20) move *with* the hardware in this phase —
they are inseparable from the operations they guard.

### Phase D — `config/` *(plan Phase 5)*

**Scope:** `GetConfigFilePath`, `GetDiagnosticLogFilePath`,
`WriteDiagnosticLog`, `WriteDiagnosticLogWithDevice`, `SaveConfigState`,
`LoadConfigState`, `EnsureConfigFileExists`.

**Why here:** config is used by policy (Phase F), so it must exist first. It
depends only on `utilities/` (`TrimTrailingChars`). It is managed code
(StreamReader/Writer), so it stays managed initially — the native port is an
isolated, later phase with an encoding checkpoint. Moving it here removes
seven methods from the form and moves the `diagnosticLogSync` lock into the
module. Verifiable by byte-identical `config.txt`/`diagnostic.log` output.

### Phase E — `application/CommandLine` *(plan Phase 6)*

**Scope:** merge the three argument-parsing sites — `main.cpp:16–25`,
`MyForm_Load:939–945`, `IsRestoreCameraCommand`/`IsDisableCameraCommand`
(`MyForm.h:827–849`).

**Why here:** a small, low-risk consolidation that removes parsing from
`main.cpp` and the form. It is safe after `camera/` and `config/` because the
command modes (`/disable-camera`, `/restore-camera`) invoke those modules;
it must come before the controller (Phase F) so the controller receives
already-parsed intent. Verification is per-argument-form exit behavior
(preserving the documented `/enable-camera` inconsistency).

### Phase F — `events/` decode + `events/` + `system/` registration & single-instance *(plan Phases 7–8)*

**Scope:** WndProc classification → `WinEventDecoder` + `EventCooldown`;
then power/WTS registration → `NotificationRegistrar`; mutex/wake event →
`system/SingleInstance`; `taskkill` → `system/ProcessUtils`.

**Why here (decode first, registration second):** the decode/cooldown
extraction is *read-only* — it classifies messages and suppresses duplicates
but performs no hardware action, so it can be verified purely by log lines
(`SessionEvent_DedupIgnored`, `PowerSetting_IrrelevantGuid`, etc.). The
registration extraction changes *handle ownership* (RAII), which is riskier
and therefore a separate step. Both must precede the controller (Phase G)
because the controller consumes decoded events and the startup sequence
needs the registrar.

### Phase G — `application/ApplicationController` *(plan Phase 9 — highest risk)*

**Scope:** `isMonitoring` gate, `isAlreadyDisabled` lock, event→operation
mapping, `DisableTargetCameraHardware`, `EnableTargetCameraHardware`,
`RestoreConfiguredCameraHardware`, `TryGetTargetCameraInstanceId` ordering,
startup orchestration, shutdown dual-path policy, ghost-recovery
orchestration, `IUiSink` interface.

**Why last (before the UI shell):** this is where all the behavioral state
lives. It is safe to extract now because every subsystem it calls
(`camera/`, `config/`, `system/`, `events/`, `utilities/`) already exists as
a module — the extraction is a re-wiring of calls, not a first move. Its
risk is high not because the code is complex to *move* but because startup
ordering, ghost recovery, and shutdown semantics are the behaviors users
depend on. It must come after F so `HandleEvent(SystemEvent)` receives
already-decoded events, and before H so the UI has a controller to delegate
to. One commit, full regression, line-for-line log parity.

### Phase H — `ui/MyForm` thin shell *(plan Phase 10)*

**Scope:** `InitializeComponent`, control handlers, form-hide behavior,
wake-listener + bring-to-front, dropdown population, dispatch-only WndProc,
`UiConstants`.

**Why last:** only after the controller exists can the form's handlers be
reduced to delegation without temporarily inventing policy elsewhere. This
phase deletes the root `MyForm.h` — the monolith is gone. Risk is medium and
verification is the full regression checklist, because the UI is the only
place where managed marshaling (`Invoke`, `marshal_as`) interacts with the
native modules.

### Phase I — Cleanup *(plan Phase 11)*

**Scope:** remove dead code (`ProductionUtilities.h` remnants,
`cameraStateInitialized`, `restartQueuedByMismatch`, `lastCameraToggleTick`,
`lastToggleTime`, `COOLDOWN_MILLISECONDS`, dormant
`TryEnterHardwareToggleCooldown` if not adopted), orphan `.rc` decision,
resource header consolidation, icon path fix, vcxproj hygiene.

**Why last:** dead-code removal is only safe once the live code has moved out
— anything referenced by a moved module would break the build if deleted
earlier. It is also the phase that makes a clean checkout build succeed.

### Phase J — v2.1 coordinated rename *(plan Phase 12)*

**Why last:** the rename (exe, project, mutex, wake event, taskkill target,
manifest identity, titles) must be atomic so the ghost-recovery path never
targets a stale name. Nothing architectural depends on it, so it is pure
finishing work.

## Why Not Another Order

- **UI first?** No — the form currently *contains* the policy and hardware
  it would need to delegate to; extracting UI first would leave policy
  homeless.
- **Controller before camera/config/events?** No — the controller would have
  to call functions still living in the form, recreating the circular
  dependency the architecture exists to break.
- **Registration before decode?** No — handle-lifetime changes (RAII) are
  harder to verify than pure classification; do the verifiable step first.
- **Cleanup early?** No — `ProductionUtilities.h` is only removable after its
  adopted parts are duplicated into live modules and its semantics are
  superseded by live module code.

## Phase Dependency Diagram

```
utilities ──► system/PrivilegeInfo ──► camera/ ──► config/ ──► CommandLine
                (B)                    (C)        (D)        (E)
                                                                │
events decode ◄────────────────────────────── events+system ◄──┘
(F, part 1)                                   (F, part 2)
                                                                │
ui shell ◄── ApplicationController ◄────────────────────────────┘
(H)          (G)
                                                                │
cleanup (I) ──► coordinated rename (J)
```

## Related Documents

- `migration-phases.md` — the operational plan (checkpoints, rollback).
- `code-classification.md` — exact symbols per phase.
- `module-catalog.md` — per-module extraction difficulty ratings.
- `risk-register.md` — the risks each phase mitigates.
- `regression-checklist.md` — the verification gate for every phase.