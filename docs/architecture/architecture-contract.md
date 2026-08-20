# Architecture Contract (v2.1)

These rules are binding for all future code written or moved during the v2.1
refactor. They exist to prevent the return of the monolithic `MyForm.h` problem
and to keep the codebase safe for human and AI-agent editing.

## Rule 1 — UI must not implement hardware policy

UI modules (`src/ui/`) may display state, accept user intent, and decode
messages. They must never decide which hardware operation to run, when, or under
which state gates.

- Allowed: call `ScanSystemCameras()` to populate a dropdown.
- Forbidden: `SetCameraHardwareStateVerified` / `RecoverCameraHardware` calls or
  `isMonitoring` policy logic inside `ui/`.

## Rule 2 — Hardware operations live behind module interfaces

All SetupAPI / Configuration Manager / device-registry interaction lives in
`src/camera/` and is reachable only through its public functions.

- `HDEVINFO`, `SP_DEVINFO_DATA`, `DEVINST`, and `CONFIGRET` never leak above
  `camera/`.
- `camera/` never leaks managed types.

## Rule 3 — Application orchestration coordinates, it does not implement internals

`src/application/` decides *what happens* (policy, ordering, state gates). It
delegates *how* to the subsystems:

- `application/ → camera/` for device operations.
- `application/ → config/` for storage.
- `application/ → system/` for OS primitives.
- `application/ → events/` for decode and registration.

## Rule 4 — Small public interfaces

Every module exposes a minimal API surface. Everything that can be private or
`file-static` must be. A module's header should be readable in under a minute;
its internals stay in the `.cpp`.

## Rule 5 — OS-specific functionality is isolated

- `system/` owns tokens, mutexes, events, processes.
- `camera/` owns PnP device APIs.
- `events/` owns message/GUID interpretation.
- `ui/` owns the `HWND`.

No other module may call these APIs directly.

## Rule 6 — Never recreate the monolithic `MyForm.h` problem

- No file should combine presentation + message handling + business policy +
  hardware + config + logging.
- A single header must not grow unboundedly; when a module's responsibilities
  split, split the module.
- Header-only implementation is discouraged for non-trivial classes; use
  `.h`/`.cpp` pairs.

## Rule 7 — Every extraction preserves v2.0 behavior

Behavior is the compatibility contract (see `../testing/baseline.md`). A
migration step may only *move* code, not *change* it. Any behavioral change must
be an explicit, separately-approved feature.

## Rule 8 — Dependency direction is one-way

| From | To |
|---|---|
| managed | native |
| `ui/` | `application/`, `camera/` (scan), `events/` (decode) |
| `application/` | `camera/`, `config/`, `system/`, `events/`, `utilities/` |
| native modules | `utilities/` (and `system/` for `camera/`, optionally) |

Native modules must never depend on managed types. No module may depend on
`ui/`.

## Rule 9 — Ownership is explicit; handles use RAII

Every Windows handle has a single owner and a deterministic release path:

- `system/SingleInstance` — mutex and wake event.
- `events/NotificationRegistrar` — `HPOWERNOTIFY` and WTS registration.
- `camera/` — `HDEVINFO` created and destroyed inside each call; never stored.
- No `new`/`delete` of native objects across the managed boundary; use owner
  types and marshal only at the boundary.

## Rule 10 — Error handling is consistent

- Hardware operations return `bool` plus an optional `DeviceError{ setupErr,
  configRet, stage }` — never mutate module-global error state.
- Errors are logged through the unified logger (`utilities/Logging`).
- Recoverable vs. fatal is decided in `application/`, not in `camera/`.
- Exceptions do not cross the native/managed boundary.

## Rule 11 — No new technology without justification

The project remains: **C++ + Win32 + C++/CLI + the existing Visual
Studio/MSBuild infrastructure**. No new package managers, frameworks, CMake,
scripting languages, or test frameworks unless a future analysis demonstrates a
concrete benefit.

## Rule 12 — One source of truth per concern

- One command-line parser (`application/CommandLine`).
- One config store (`config/ConfigStore`).
- One logger (`utilities/Logging`).
- One decode layer (`events/WinEventDecoder`).
- One policy/state machine (`application/ApplicationController`).

Duplicate implementations discovered in v2.0 (elevation, toggle fallback, arg
parsing, shutdown cleanup) must not be re-introduced.

## Rule 13 — Resource and build hygiene

- Resources are source-controlled assets referenced by relative paths.
- The `src/` tree and `docs/` are source-controlled.
- Generated build output never enters Git.

## Related Documents

- `overview.md` — principles.
- `target-architecture.md` — module contracts.
- `dependency-map.md` — allowed dependencies.
- `migration-map.md` — where v2.0 code goes.
- `../refactoring/migration-phases.md` — how to migrate safely.