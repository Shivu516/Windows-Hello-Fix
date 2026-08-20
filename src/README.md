# Windows Hello Fix — `src/` Source Architecture

## What This Is

This directory is the **architectural scaffolding** for the Windows Hello Fix
v2.1 modularization. It defines *where code will live* after the refactor. It
does **not** yet contain the application implementation.

> ⚠️ **The new `src/` tree is architectural scaffolding until the migration
> phases are completed.**

## Why This Architecture Exists

The v2.0 application is a single monolithic header (`MyForm.h`, ~1,300 lines)
that mixes UI, Win32 message handling, business policy, hardware control,
configuration, and logging. v2.1 splits that monolith into modules by
**responsibility**, so that:

- a developer or AI agent can modify one subsystem without risking others;
- the native core has zero dependency on managed .NET types;
- the code becomes testable, maintainable, and portable.

## Relationship Between `src/` and the Current Root-Level Source

| Item | Status |
|---|---|
| Root-level `main.cpp`, `MyForm.h`, `ProductionUtilities.h` | **Authoritative today.** Untouched during this phase. |
| `src/` directories + markers | Scaffolding only. Not compiled. |
| Docs in `../docs/` | The approved plan (read before touching anything). |

The migration plan (`../docs/refactoring/migration-phases.md`) defines exactly
which root-level symbols move into which `src/` module, in which order, with a
build + behavior checkpoint after every phase.

## Module Layout

```
src/
├── main.cpp           (future — the v2.0 main.cpp migrates here; NOT created yet)
├── application/       C++/CLI — lifecycle, monitoring state machine, command line
├── camera/            native — device discovery + hardware control
├── config/            storage — config.txt + diagnostic.log
├── events/            native — message decode, dedup, notification registration
├── system/            native — privileges, single-instance, process utils
├── ui/                C++/CLI — WinForms presentation + WndProc dispatch
└── utilities/         native — logging, timing, string helpers
```

The module directories exist as empty scaffolding — they contain no per-directory
`README.md` markers and no placeholder `.cpp`/`.h` files. The intended content of
each module is defined in `../docs/architecture/source-tree.md` and
`../docs/architecture/module-catalog.md`.

## How Future Extraction Will Work

Per `../docs/refactoring/migration-phases.md`:

1. One subsystem is extracted at a time (safest first).
2. Code moves **verbatim** — behavior is preserved (the compatibility contract
   in `../docs/testing/baseline.md`).
3. Build + regression checklist gate every phase.
4. One commit per phase.

## How Modules Should Depend on One Another

The one binding rule:

> **Managed modules may call native modules. Native modules must never depend on
> managed types.**

Allowed dependency summary (full table in
`../docs/architecture/dependency-map.md`):

```
utilities  → (nothing)
config     → utilities
system     → utilities
events     → utilities
camera     → utilities, system
application→ camera, config, system, events, utilities
ui         → application, camera (scan), events (decode)
main.cpp   → ui, application
```

No module depends on `ui/`.

## What Developers Must NOT Do

- **Do not** treat `src/` as live code yet — it does not build.
- **Do not** copy `main.cpp`/`MyForm.h` contents into `src/` files (no
  duplication of application logic).
- **Do not** add the `src/` files to the Visual Studio project until its
  migration phase authorizes it.
- **Do not** edit root-level v2.0 files as part of scaffolding work.
- **Do not** create fake implementations or empty `.cpp`/`.h` stubs.
- **Do not** reintroduce the monolith in a new file
  (`../docs/architecture/architecture-contract.md`, Rule 6).

## Authority

- `../docs/architecture/target-architecture.md` — the module contracts.
- `../docs/architecture/migration-map.md` — current code → future module.
- `../docs/architecture/architecture-contract.md` — binding rules.
- `../docs/refactoring/migration-phases.md` — the extraction sequence.
