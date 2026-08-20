# Windows Hello Fix — Architecture Overview

## Project Purpose

Windows Hello Fix is a Windows desktop utility that forces Windows Hello facial
recognition to use the Infrared (IR) sensor instead of the slower, light-dependent
RGB color camera.

When the Windows session locks (or the machine sleeps, the lid closes, or the
power button is pressed), the application **disables the RGB camera** at the
hardware level. Deprived of its preferred sensor, Windows Hello authenticates
using the IR sensor. When the session unlocks (or the machine resumes), the
application **re-enables the RGB camera**.

The application runs as a passive, always-on background monitor. It reacts to
native Windows session and power notifications rather than polling.

## Current v2.0 Architecture

The v2.0 implementation is a **single-translation-unit C++/CLI WinForms
application**:

```
main.cpp  ──includes──►  MyForm.h  (1,362 lines: everything)
                            │
                            ├── managed ref class MyForm (the WinForms form)
                            ├── WndProc message handling (session/power/shutdown)
                            ├── native camera hardware functions (SetupAPI / CFGMGR32)
                            ├── configuration read/write (%APPDATA%)
                            ├── diagnostic logging
                            └── single-instance + inter-process wakeup logic
```

Key facts:

- `MyForm.h` is header-only: the entire application is compiled from one
  translation unit (`main.cpp`).
- The form mixes UI presentation, Win32 message handling, business policy,
  hardware control, configuration, and logging in one class.
- `ProductionUtilities.h` exists in the project but is **never included** by any
  source file. Its classes (`ProductionLogger`, `HardwareOperationQueue`,
  `SingleInstanceManager`, `ElevationChecker`, `EnhancedSetupAPI`,
  `ShutdownManager`) are currently dead code and represent a previously planned
  (unmerged) async refactor documented in `WndProc_Redesign.txt`.
- The project is a managed (.NET) assembly: `CLRSupport=true` (`/clr`),
  targeting .NET Framework 4.7.2, built with MSVC v143 / Visual Studio 2022,
  `Release | x64` producing `x64\Release\Windows_Hello_Fix_v2_0.exe`.

See `current-codebase.md` for the complete map of the v2.0 implementation.

## v2.1 Architectural Goals

v2.1 transforms the monolithic v2.0 source into a clean, modular, maintainable
structure:

- **Modules by responsibility**, not by file size.
- A **pure-native core** (camera, events decoding, system helpers, utilities,
  config) that has zero dependency on managed .NET types.
- A thin **C++/CLI shell** (UI form + application orchestration) that owns the
  managed-to-native boundary.
- A source tree where a developer or AI coding agent can modify one subsystem
  (e.g. "session-lock event handling" or "camera device detection") by
  inspecting only the relevant `src/` modules.
- **Behavior preservation**: the refactor must not change any v2.0 behavior
  unless a future v2.1 feature explicitly changes it.

The design deliberately follows the `src/` + `docs/` philosophy of the related
Primidian project, but is adapted for a native C++/Visual Studio application:
modular source files are compiled and linked by the normal C++ toolchain into a
single executable — not textually concatenated.

## Major Subsystems

| Subsystem | Directory (future) | Responsibility |
|---|---|---|
| Application orchestration | `src/application/` | Monitoring state machine, event policy, lifecycle, single-instance flow |
| Camera / device management | `src/camera/` | Device discovery, enable/disable, state verification, recovery/self-healing |
| Configuration | `src/config/` | `config.txt` + `diagnostic.log` storage and format |
| Event decoding | `src/events/` | Windows message decoding, debounce/dedup, notification registration |
| System helpers | `src/system/` | Privilege checks, single-instance primitives, process utilities |
| UI | `src/ui/` | WinForms form, presentation, WndProc dispatch |
| Utilities | `src/utilities/` | Logging, timing, string helpers |

## Responsibility Boundaries

- **UI** (presentation) must not implement hardware policy.
- **Camera** (hardware) must not touch managed types or UI.
- **Events** (decoding) must not perform hardware operations or make policy
  decisions; it only classifies messages.
- **Application** (orchestration) coordinates the subsystems; it does not
  implement their internals.
- **Config** owns storage format and paths only.
- **System** owns OS primitives (privileges, mutexes, processes).
- **Utilities** are dependency-free helpers.

## Dependency Direction

```
utilities  → (no dependencies)
config     → utilities
system     → utilities
events     → utilities
camera     → utilities, system
application→ camera, config, system, events, utilities
ui         → application, camera (scan only), events (decode only)
main.cpp   → ui, application
```

The rule that keeps the architecture safe:

> **Managed modules may call native modules. Native modules must never depend on
> managed types.**

## Relationship Between UI, Application Logic, Windows APIs, and Hardware

```
Windows OS message (lock/unlock/sleep/resume/lid/power/shutdown)
        │
        ▼
ui::MyForm::WndProc                (thin dispatcher; owns the HWND)
        │
        ▼
events::WinEventDecoder + EventCooldown   (pure native, no side effects)
        │
        ▼
application::ApplicationController        (policy: isMonitoring gate, lock, op selection)
        │
        ▼
camera::CameraHardware / CameraRecovery   (pure native)
        │
        ▼
SetupAPI / Configuration Manager          (PnP subsystem)
        │
        ▼
hardware state  →  verification  →  recovery/retry
```

Reverse dependencies flow back through the same modules: the controller reports
results to the UI through a narrow callback interface (`IUiSink`), so the
controller never manipulates `Form` controls directly.

## Source Modules vs. Generated Build Output

| Category | Location | In Git? |
|---|---|---|
| Editable source modules (future) | `src/` | Yes |
| Documentation | `docs/` | Yes |
| Current v2.0 source (pre-refactor) | repository root (`main.cpp`, `MyForm.h`, ...) | Yes |
| Generated build output | `x64/`, `x86/`, `ARM64/`, `Debug/`, `Release/` | No (gitignored) |
| IDE state | `.vs/` | No (gitignored) |

The source of truth will eventually be the modular `src/` tree. Until the
migration phases are complete, the root-level v2.0 files remain authoritative
and must not be edited except by the approved migration phases.

## Architectural Principles

1. **Preserve behavior.** Every extraction must keep v2.0 behavior intact unless
   a feature explicitly changes it.
2. **Small public interfaces.** Each module exposes a minimal API and keeps its
   implementation private.
3. **Obvious ownership.** OS handles are owned by RAII wrappers or created and
   destroyed within a single operation.
4. **Native core, managed shell.** Anything that can be native should be native;
   only the UI and orchestration that need .NET stay managed.
5. **No new monolithic files.** Do not recreate the `MyForm.h` problem elsewhere.
6. **Incremental migration.** The refactor is a sequence of small, verified,
   committed phases — never one giant rewrite.

## Further Reading

- `target-architecture.md` — the proposed `src/` tree and module contracts.
- `current-codebase.md` — where everything lives in v2.0 today.
- `dependency-map.md` — allowed vs. discouraged dependencies.
- `data-flow.md` — important runtime flows.
- `migration-map.md` — current code → future module mapping.
- `architecture-contract.md` — the rules future code must follow.
- `../refactoring/migration-phases.md` — the incremental extraction plan.
- `../build/build-system.md` — how the modular source will eventually build.