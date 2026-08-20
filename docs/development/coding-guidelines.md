# Coding Guidelines (v2.1)

These conventions apply to **future v2.1 code** written or moved during the
refactor. Existing v2.0 code is intentionally not reformatted in this phase.

## File Naming

- Module directories: lowercase singular — `application/`, `camera/`, `config/`,
  `events/`, `system/`, `ui/`, `utilities/`.
- Source files: PascalCase matching the primary type — `CameraHardware.cpp`,
  `ApplicationController.h`.
- Use `.h`/`.cpp` pairs for non-trivial classes. Header-only implementation is
  discouraged.

## Types

- Classes/structs: PascalCase (`CameraDeviceInfo`, `DeviceError`,
  `ApplicationController`).
- Enums: scoped `enum class` with PascalCase members
  (`SystemEvent::SessionLock`).
- Namespaces: `WindowsHelloFix::Camera`, `WindowsHelloFix::Events`, etc. Native
  free functions from v2.0 keep their existing names (`ScanSystemCameras`,
  `ToggleCameraHardware`) for continuity.

## Functions

- Public functions: PascalCase (`ScanSystemCameras`, `RecoverCameraHardware`).
- Private/internal functions and members: `m_` prefix for native classes
  (`m_deviceList`); static/private helpers that stay in the `.cpp` may use
  `file`-scoped names or `Detail::` namespaces.
- Prefer small functions with one responsibility (per
  `../architecture/architecture-contract.md`, Rule 4).

## Constants

- Macros: `UPPER_SNAKE` only where Win32 requires them
  (`CONFIGFLAG_DISABLED`). Prefer `constexpr`.
- Named constants: `k`-prefixed or PascalCase (`kCooldownMs`).
- **Timing constants are the behavioral contract** — centralize them in
  `utilities/Timing.h` and keep the values from `docs/architecture/data-flow.md`
  (1500 ms debounce, 100/250/500/1000 ms, 350/900/500 ms recovery).

## Windows Handles and Ownership

- Windows handles: `h`-prefix (`hDevInfo`, `hWakeupEvent`).
- Every handle has one owner and a deterministic release path (RAII):
  - `system/SingleInstance` → mutex, wake event.
  - `events/NotificationRegistrar` → `HPOWERNOTIFY`, WTS registration.
  - `camera/` → `HDEVINFO` created and destroyed inside each call; never stored.
- Never `new`/`delete` native objects across the managed boundary; marshal only
  at the boundary.

## Header / Source Separation and Include Discipline

- A module's public header exposes only its public interface
  (small API surface per Rule 4).
- Include only what you use; prefer the module's own header first, then standard
  headers, then Windows headers, then project headers.
- Use `#pragma once`.
- Do not pull Windows headers into headers that don't need them; keep Win32
  includes in the `.cpp` where possible.

## Error Handling

- Hardware operations return `bool` plus an optional result/detail object
  (`DeviceError{ setupErr, configRet, stage }`). Do not mutate module-global
  error state.
- Recoverable vs. fatal decisions live in `application/`, not in `camera/`.
- Exceptions do not cross the native/managed boundary; the managed layer keeps
  the existing `catch (...) { log; }` pattern.
- Failures are always logged through the unified logger; silent failures are the
  exception, not the rule.

## Logging

- Use the unified logger (`utilities/Logging`, based on the v2.0
  `ProductionLogger` design: `OutputDebugStringW` with timestamped, structured
  lines).
- Preserve the `diagnostic.log` format and event names from v2.0
  (`Event=...`, `Target=...`, `Verify=PASS|FAIL`) — they are the regression
  oracle (see `docs/testing/baseline.md`).

## Dependency Boundaries

- Managed → native only; native modules never depend on managed types.
- No module depends on `ui/`.
- Module dependency table: see `docs/architecture/dependency-map.md`.
- Do not reach into another module's internals; use its public interface.

## What NOT to Do

- Do not recreate the `MyForm.h` monolith (Rule 6) — no file mixing UI +
  message handling + policy + hardware + config + logging.
- Do not add new third-party libraries, frameworks, package managers, CMake, or
  test frameworks without a documented, approved justification.
- Do not duplicate logic that already has a single owner (command-line parsing,
  config, logging, decode, policy).
- Do not "fix" out-of-scope issues inline; add them to the risk register.

## Related Documents

- `contributing.md` — workflow.
- `../architecture/architecture-contract.md` — the binding rules.
- `../architecture/target-architecture.md` — module contracts.
- `../architecture/data-flow.md` — timing constants (behavioral contract).