# Risk Register

Risks identified during the v2.0 analysis and the v2.1 migration planning.
Severity and likelihood are scored for the migration context.

## Register

| # | Risk | Cause | Severity | Likelihood | Mitigation | Verification |
|---|---|---|---|---|---|---|
| 1 | **Clean-checkout build fails** | The compiled `.rc` references `x64\Release\WindowsHelloFix.ico` (gitignored output); the orphan `.rc` references absolute machine paths | High | Certain today | Commit the icon under `resources/`; use relative RC paths; single active `.rc` (Phase 11) | Fresh clone → build → exe runs |
| 2 | **Disabling the wrong camera** | Instance-ID matching change, `MI_00` heuristic regression, or case-insensitive fallback removed | High | Low–Med | Keep exact + case-insensitive matching verbatim; keep `MI_00` heuristic; never match on friendly name | Device state test on a known machine |
| 3 | **Breaking event debouncing** | Changing the 1500 ms dedup statics or moving them into a shared object incorrectly | Medium | Low–Med | Move dedup state into `EventCooldown` with identical semantics (same code + same tick) | Duplicate-event test; log shows `DedupIgnored` |
| 4 | **Breaking retry/self-healing** | `SetCameraHardwareStateVerified`/`RecoverCameraHardware` timing or attempt-count drift during extraction | High | Low–Med | Move bodies verbatim (Phases 4, 9); centralize all timing constants; document them | Verification/retry/recovery matrix |
| 5 | **Introducing race conditions** | Moving hardware calls off the UI thread (async queue) without full analysis | High | Low (only if adopted) | Async queue is a *future feature*, not part of this refactor; v2.0 blocking behavior preserved | n/a until adopted |
| 6 | **Resource lifetime / handle leaks** | `hAppMutex`, `hWakeupEvent`, `hLidNotification`, `hButtonNotification` created in `MyForm_Load` and released in destructor + finalizer; wake thread waits on a handle the destructor closes | Medium | Med | RAII owners: `system/SingleInstance`, `events/NotificationRegistrar` (Phase 8) | Handle-count over start/stop cycles; clean exit |
| 7 | **UI/business-logic coupling returning** | The controller starts manipulating `Form` controls, or UI embeds policy | Medium | Med | `IUiSink` callback interface; Rule 1 and Rule 3 of the architecture contract | Code review; grep for control access outside `ui/` |
| 8 | **Build configuration regression** | Adding `src/` items to the vcxproj, per-file `CLRSupport` toggling, or lib placement | Medium | Med | Only native modules get `<CLRSupport>false</CLRSupport>`; keep `#pragma comment(lib)` in owning modules; add files one phase at a time | Build after every phase |
| 9 | **Architecture-specific compilation issues** | x86 configs currently incomplete (no libs/subsystem listed); future ARM64 blocked by C++/CLI | Medium | Med (x86), High (ARM64 if required) | Repair x86 configs (Phase 11); keep native core pure so an ARM64 path stays possible; document the C++/CLI ARM64 constraint | x86 build checkpoint; ARM64 feasibility check |
| 10 | **Accidental behavior change during extraction** | Copying code changes semantics (e.g. an extra `Sleep`, reordered check, changed `!` vs `==`) | High | Med | Verbatim moves; diff the moved function against the original; Phase 7/9 policy tables | Line-for-line log parity; regression checklist |
| 11 | **Shutdown semantics broken** | Destructor/finalizer disable-vs-enable path (`isSystemEnding`) altered | High | Med | Preserve both paths exactly; Phase 9 keeps the dual-path policy; Phase 12 rename touches `taskkill`/exe names atomically | Lifecycle + shutdown tests |
| 12 | **Single-instance behavior broken** | Mutex/event names drift during the v2.1 rename, or `Local\` dead-code manager adopted accidentally | Medium | Low–Med | Live code uses `Global\WindowsHelloFix_AppMutex` — that is the contract; rename is one atomic commit | Two-instance test; wake-path test |
| 13 | **Ghost-process recovery broken** | `taskkill /IM Windows_Hello_Fix_v2_0.exe` targets a stale name after rename | Medium | Med | Phase 12 renames exe name + taskkill target + README in one commit | Force-reset test |
| 14 | **Lost state verification** | Removing `cameraExpectedDisabled`/check-before-change logic | Medium | Low–Med | Keep `SetCameraHardwareStateVerified` check-before-change verbatim | Already-disabled / already-enabled tests |
| 15 | **Config encoding corruption** | Porting `config/` to native with the wrong encoding assumption | Medium | Med | Keep managed impl for parity first; native port isolated with encoding checkpoint (ASCII content today) | Save/load round-trip test |
| 16 | **Icon/resource identity mismatch** | `resource.h` (`IDI_ICON1`=102) vs `resource1.h` (`IDI_ICON1`=114); form icon load silently fails | Low | Certain today | Consolidate resource headers; fix the form icon load (Phase 11) | Form icon visible |
| 17 | **Header bloat returning** | A new module recreates the god-object (e.g. `ApplicationController` grows unbounded) | Medium | Med | Rule 6 (no monolith) + Rule 4 (small public interfaces); code review | Module header size check |
| 18 | **Orphaned code after extraction** | Old functions left in root files after migration; duplicate implementations | Low | Med | Grep for no-orphan references after each move; remove dead code (Phase 11) | `grep` for each moved symbol |

## Risk Summary by Phase

| Phase | Primary risks |
|---|---|
| 0 | none |
| 1 (docs/scaffolding) | none (additive) |
| 2 (utilities) | none |
| 3 (privilege) | 10 |
| 4 (camera) | 2, 4, 10 |
| 5 (config) | 15 |
| 6 (command line) | 10 |
| 7 (events decode) | 3, 10 |
| 8 (registrar/single-instance) | 6, 12 |
| 9 (controller) | 4, 10, 11, 13 |
| 10 (ui shell) | 7, 10 |
| 11 (cleanup) | 1, 8, 16, 18 |
| 12 (rename) | 12, 13 |

## Top Mitigations (do these every phase)

1. **Verbatim moves:** diff moved functions against the originals before commit.
2. **Log parity:** compare `diagnostic.log` output before/after each phase.
3. **One phase = one commit:** makes rollback trivial.
4. **Full regression checklist** (`../testing/regression-checklist.md`) after
   every phase, not just "it builds".
5. **No fixes of unrelated issues:** document, don't fix, out-of-scope problems
   (they get their own phase).

## Related Documents

- `../architecture/current-codebase.md` — the code the risks refer to.
- `../architecture/migration-map.md` — what moves where.
- `../refactoring/migration-phases.md` — the sequence and checkpoints.