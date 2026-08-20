# Contributing

This guide explains how to work with the Windows Hello Fix codebase safely —
for both human developers and AI coding agents.

## Where Things Belong

| Content | Location |
|---|---|
| Future modular source | `src/` (per module, see `../architecture/target-architecture.md`) |
| Current authoritative v2.0 source | repository root (`main.cpp`, `MyForm.h`, ...) — until its migration phase |
| Architecture / refactoring / build / testing docs | `docs/` |
| Generated build output | `x64/`, `x86/`, `ARM64/`, `Debug/`, `Release/` (never commit) |
| IDE state | `.vs/` (never commit) |

## The Current State (important)

The repository is between v2.0 and v2.1:

- The **root-level files are still authoritative** and the application is built
  entirely from `main.cpp` + `MyForm.h`.
- The `src/` tree is **architectural scaffolding only** — it contains no
  implementation yet. Do not treat it as live code.
- The `docs/` directory is the plan; `docs/refactoring/migration-phases.md`
  defines the approved order of extraction.

## How to Make a Change

### If the change is part of an approved migration phase

1. Read the phase in `docs/refactoring/migration-phases.md`.
2. Move code **verbatim** where the phase says verbatim.
3. Build before and after (`Release | x64`).
4. Run the full `docs/testing/regression-checklist.md`.
5. Commit one phase per commit.

### If the change is a bug fix or feature on v2.0

- The scope-restriction rules of the planning phase still apply: **do not touch
  files outside the current phase's scope**.
- If the fix requires touching `MyForm.h` before its migration phase, coordinate
  with the migration plan so the fix lands where the code will eventually live.

### General workflow

```
1. Identify the module that owns the responsibility (dependency-map.md).
2. Make the smallest change.
3. Build:  Release | x64  (and Debug | x64 when relevant).
4. Verify: regression-checklist.md.
5. Commit: concise message, one logical change.
```

## Dependency Rules (always)

- **Managed → native only.** Native modules (`camera/`, `system/`, `events/`,
  `utilities/`, `config/`) must never depend on managed types.
- Follow the allowed-dependency table in `docs/architecture/dependency-map.md`.
- Never create a dependency on `ui/` from another module.
- Prefer the public interface of a module; never reach into another module's
  internals.

## Build Before / After Changes

- Before starting: confirm the baseline builds (see `docs/testing/baseline.md`).
- After changing: rebuild and confirm no new warnings at /W3.
- If you touched resource files or the project file, confirm the resource
  compile succeeds (the icon asset must be present — see `docs/build/build-system.md`).

## Test Before / After Changes

- There is no automated test framework (by design). Verification is:
  1. `Release | x64` build.
  2. The `regression-checklist.md` run.
  3. `diagnostic.log` parity checks for the exercised paths.
- If your change alters logging or timing, document it in
  `docs/architecture/data-flow.md` (the timing table is the contract).

## Commit Strategy

- One logical change per commit.
- Reference the phase number when the change is part of a migration phase
  (e.g. `Phase 4: extract camera hardware ops into src/camera`).
- Do not mix unrelated fixes into a migration commit; unrelated issues go into
  the risk register or their own commit.
- Never commit generated artifacts, `x64/`, `.vs/`, or secrets.

## Avoiding Unrelated Modifications

- Scope check before editing: "is this file in the current phase's scope?"
- If you spot a problem that is out of scope (e.g. the resource header ID
  mismatch, the orphan `.rc`, the incomplete Win32 config), **document it** in
  `docs/refactoring/risk-register.md` rather than fixing it inline.
- Formatting/refactoring outside the phase scope is forbidden.

## AI Agents

- A request like "modify the session-lock event handling" should touch only
  `src/events/` and `src/application/` (once migrated).
- A request like "change camera device detection" should touch only `src/camera/`
  (and possibly `src/system/`).
- Start by reading `docs/architecture/target-architecture.md` and
  `docs/architecture/dependency-map.md` to find the owning module.
- Do not edit root-level v2.0 files unless the migration phase authorizes it.

## Related Documents

- `coding-guidelines.md` — naming, ownership, error handling conventions.
- `../architecture/architecture-contract.md` — binding rules.
- `../refactoring/migration-phases.md` — the approved sequence.