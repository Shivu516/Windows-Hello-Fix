# Target Architecture (v2.1) — Architectural Contract

This document is the architectural contract for the future refactor. It defines
the intended `src/` tree, the responsibility of every module, and the rules for
what belongs (and must not belong) in each module.

## The Core Design Decision: Native Core, Managed Shell

The application is a C++/CLI (managed .NET) assembly. The v2.1 architecture is
therefore **two-tier**:

- **Tier 1 — pure native modules** (`camera/`, `system/`, `events/`,
  `utilities/`, and `config/` if ported). Plain C++, `std::wstring`, Win32 API
  only. Compilable with `/clr` disabled per-file
  (`<CLRSupport>false</CLRSupport>`). Zero managed dependency.
- **Tier 2 — C++/CLI modules** (`ui/`, `application/`). These own the
  managed↔native boundary because they must touch .NET (WinForms, `Environment`,
  `MessageBox`, `Application::Restart`, interop marshaling).

### The one rule that keeps this safe

> **Managed modules may call native modules. Native modules must never depend on
> managed types.**

This keeps every non-UI subsystem independently editable and testable, and it
keeps the door open for future ARM64 support (the C++/CLI UI layer is the only
part that cannot reliably target ARM64).

## Proposed Source Tree

```
src/
├── main.cpp                      C++/CLI bootstrap
├── application/                  C++/CLI — orchestration + command-line
├── camera/                       native — device discovery + hardware control
├── config/                       storage — config.txt + diagnostic.log
├── events/                       native — message decode, dedup, registration
├── system/                       native — privileges, single-instance, process utils
├── ui/                           C++/CLI — WinForms presentation + WndProc dispatch
└── utilities/                    native — logging, timing, string helpers
```

## Module Contracts

### `src/main.cpp`

- **Responsibility:** process bootstrap. Parse command line via
  `application/CommandLine`, create the form, run the message loop.
- **Belongs:** argument classification for hidden/background launch.
- **Does NOT belong:** business logic, hardware calls, config.
- **Dependencies:** `ui/`, `application/CommandLine`.
- **Future files:** `main.cpp`.
- **From v2.0:** the current `main.cpp`, minus its inline arg-scan loop.

### `src/application/`

- **Responsibility:** application lifecycle and the monitoring state machine.
  Owns the event→action policy (which `SystemEvent` triggers which camera
  operation, gated by `isMonitoring` and the hardware-state lock). Owns the
  single-instance flow (mutex, wake event, ghost reset) and shutdown behavior.
- **Belongs:** `ApplicationController` (state machine, startup/shutdown
  orchestration), `CommandLine` (single arg-parsing source of truth).
- **Does NOT belong:** raw `SP_DEVINFO_DATA`, Win32 decoding, control
  manipulation, hardware implementation.
- **Public interface (intended):**
  - `CommandLine`: `ParseArgs(...) → { background, restoreCamera, disableCamera }`.
  - `ApplicationController`: `Start(...)`, `Stop()`, `HandleEvent(SystemEvent)`,
    `HandleSystemEnd()`, `Shutdown(...)`; a narrow `IUiSink` callback interface
    for UI updates.
- **Dependencies:** `camera/`, `config/`, `system/`, `events/`, `utilities/`.
- **From v2.0:** `DisableTargetCameraHardware`, `EnableTargetCameraHardware`,
  `RestoreConfiguredCameraHardware`, `TryGetTargetCameraInstanceId` (ordering),
  `IsRestoreCameraCommand`, `IsDisableCameraCommand`, the single-instance block in
  `MyForm_Load`, ghost-recovery orchestration, and the destructor's
  disable-vs-enable policy.
- **Risk:** highest-risk extraction (startup ordering, ghost recovery, shutdown
  semantics). Migrate last, after `camera/`, `config/`, `events/`, `system/`.

### `src/camera/`

- **Responsibility:** everything about camera/device hardware.
- **Belongs:** `CameraDeviceInfo`, `ScanSystemCameras`, the `MI_00` target
  heuristic, SetupAPI toggle, CFGMGR32 fallback, disabled-state query,
  verification loop, retry/self-healing, recovery cycle, cooldown timing, and the
  `DeviceError` result struct (replacing the `g_last*` globals).
- **Does NOT belong:** managed types, `String^`, `Invoke`, UI strings, config
  paths, `MessageBox`.
- **Public interface (intended):**
  - `CameraDevice`: `CameraDeviceInfo`, `ScanSystemCameras()`, `PickInfraredConflict(...)`.
  - `CameraHardware`: `ToggleCameraHardware`, `LocateCameraDevInst`,
    `ToggleCameraHardwareCfgMgr`, `GetCameraHardwareDisabledState`,
    `VerifyCameraHardwareState`.
  - `CameraRecovery`: `SetCameraHardwareStateVerified`, `RecoverCameraHardware`,
    `RestoreAllCameraHardware`.
  - `DeviceError`: `{ setupErr, configRet, stage }`.
- **Dependencies:** `utilities/`, optionally `system/PrivilegeInfo` for
  elevation-aware logging.
- **From v2.0:** the native functions at `MyForm.h:205–581`.
- **Risk:** medium (raw PnP operations), but safe to move early because the
  function bodies are self-contained native code.

### `src/config/`

- **Responsibility:** `%APPDATA%\Windows Hello Fix\config.txt` and
  `diagnostic.log` — paths, format, read/write, string sanitization.
- **Belongs:** `ConfigPaths`, `ConfigStore` (Load/Save/Ensure/WriteDiagnostic),
  `TrimTrailingChars` (or `utilities/`).
- **Does NOT belong:** hardware control, policy, logging-format policy beyond its
  own files.
- **Dependencies:** `utilities/`.
- **From v2.0:** `GetConfigFilePath`, `GetDiagnosticLogFilePath`,
  `WriteDiagnosticLog`, `WriteDiagnosticLogWithDevice`, `SaveConfigState`,
  `LoadConfigState`, `EnsureConfigFileExists`.
- **Migration note:** the v2.0 implementation is managed (`StreamReader`/
  `StreamWriter`, UTF-8). Keep the managed implementation for exact behavior
  parity first; a native port is a separate, later, isolated phase with an
  explicit encoding checkpoint (the file content is ASCII today:
  `monitoring=0|1`, ASCII instance IDs).

### `src/events/`

- **Responsibility:** turn raw Win32 messages into semantic events and manage
  notification registration.
- **Belongs:** `SystemEvent` enum, `WinEventDecoder::Decode(msg, wParam, lParam)`
  (including the `PBT_POWERSETTINGCHANGE` lid/button GUID filter), `EventCooldown`
  (1500 ms dedup state, replacing WndProc static locals),
  `NotificationRegistrar` (RAII for power + WTS registration with the 6×500 ms
  retry).
- **Does NOT belong:** hardware calls, `isMonitoring` policy, UI, `Sleep`
  windows that gate hardware.
- **Public interface (intended):**
  - `WinEventDecoder`: `enum class SystemEvent { None, SystemEnding, QueryEnd,
    Suspend, Resume, PowerSettingLid, PowerSettingButton, PowerSettingOther,
    SessionLock, SessionUnlock }`; `Decode(UINT, WPARAM, LPARAM)`.
  - `EventCooldown`: `ShouldSuppress(int code, ULONGLONG now)` / `Record(...)`.
  - `NotificationRegistrar`: register/unregister given an `HWND`.
- **Dependencies:** `utilities/`.
- **From v2.0:** the decode/debounce portion of `WndProc` and the registration
  block in `MyForm_Load`.

### `src/system/`

- **Responsibility:** OS primitives that are not device-specific.
- **Belongs:** `PrivilegeInfo` (elevation + integrity; merges the dead
  `ElevationChecker`), `SingleInstance` (mutex/event handle helpers and
  ownership), `ProcessUtils` (`taskkill` helper for ghost recovery).
- **Does NOT belong:** camera logic, UI, policy.
- **Dependencies:** `utilities/`.
- **From v2.0:** `IsCurrentProcessElevatedNative`, `GetCurrentProcessIntegrityRid`,
  the mutex/event block in `MyForm_Load`, the `system("taskkill ...")` call.

### `src/ui/`

- **Responsibility:** presentation. The Form, its controls, layout, and a WndProc
  that only decodes messages and forwards them.
- **Belongs:** `MyForm` (ref class), `InitializeComponent`, control event
  handlers, bring-to-front, form-closing hide behavior, `UiConstants` (strings,
  layout numbers, colors).
- **Does NOT belong:** hardware calls, Win32 decoding, config parsing, policy.
- **Public interface (intended):** `MyForm` (managed); it consumes
  `ApplicationController` and reports UI events through it.
- **Dependencies:** `application/`, `camera/` (scan only), `events/` (decode
  only).
- **From v2.0:** the presentation portions of `MyForm.h`. After this module is
  complete, the 1,300-line header is gone.

### `src/utilities/`

- **Responsibility:** dependency-free helpers.
- **Belongs:** unified logger (adopts `ProductionLogger`), timing helpers
  (`GetTickCount64`), string helpers (`TrimTrailingChars`).
- **Does NOT belong:** hardware, UI, config policy.
- **Dependencies:** none.
- **From v2.0:** `ProductionLogger` (dead code promoted to live), `TrimTrailingChars`.

## What Is Explicitly NOT in the v2.1 Target

- **No async hardware queue** (`HardwareOperationQueue` from
  `ProductionUtilities.h`) unless it is adopted later as an explicit feature that
  changes behavior. The v2.0 blocking behavior is the compatibility contract.
- **No textual concatenation** of sources. The executable is produced by the
  normal C++ compile+link pipeline.
- **No new build system** (no CMake) in this phase.
- **No new third-party dependencies, frameworks, or package managers.**

## Relationship to the Current Tree

- The root-level v2.0 files remain authoritative and untouched until their
  migration phase runs.
- Each module above lists exactly which v2.0 symbols migrate into it
  (see `migration-map.md` for the granular table).
- During migration, `MyForm.h` shrinks in phases; the final state of `ui/MyForm`
  is a thin presentation shell.

## Related Documents

- `overview.md` — high-level architecture and principles.
- `dependency-map.md` — allowed vs. discouraged dependencies.
- `data-flow.md` — runtime flows.
- `migration-map.md` — current code → future module table.
- `architecture-contract.md` — the binding rules for future code.
- `../refactoring/migration-phases.md` — how to get from here to there.