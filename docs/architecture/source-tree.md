# Target Source Tree (v2.1)

The complete proposed `src/` architecture. Only `src/utilities/` contains
implementation today (Phase 2 of the migration plan). Everything else is
directory scaffolding whose future contents are described below; nothing is
created until its migration phase authorizes it.

```
src/
├── main.cpp                        (future — C++/CLI bootstrap)
├── application/                    C++/CLI — orchestration + command line
│   ├── CommandLine.h
│   ├── CommandLine.cpp
│   ├── ApplicationController.h     (includes IUiSink callback interface)
│   └── ApplicationController.cpp
├── camera/                         native — device discovery + hardware control
│   ├── CameraDevice.h              CameraDeviceInfo, ScanSystemCameras,
│   ├── CameraDevice.cpp            MI_00 target heuristic
│   ├── CameraHardware.h            ToggleCameraHardware, LocateCameraDevInst,
│   ├── CameraHardware.cpp          ToggleCameraHardwareCfgMgr,
│   │                               GetCameraHardwareDisabledState,
│   │                               VerifyCameraHardwareState
│   ├── CameraRecovery.h            SetCameraHardwareStateVerified,
│   ├── CameraRecovery.cpp          RecoverCameraHardware,
│   │                               RestoreAllCameraHardware, cooldown helpers
│   └── DeviceError.h               { setupErr, configRet, stage } result struct
├── config/                         storage — config.txt + diagnostic.log
│   ├── ConfigPaths.h
│   ├── ConfigPaths.cpp
│   ├── ConfigStore.h
│   └── ConfigStore.cpp
├── events/                         native — message decode, dedup, registration
│   ├── SystemEvent.h               SystemEvent enum
│   ├── WinEventDecoder.h
│   ├── WinEventDecoder.cpp
│   ├── EventCooldown.h
│   ├── EventCooldown.cpp
│   ├── NotificationRegistrar.h
│   └── NotificationRegistrar.cpp
├── system/                         native — privileges, single-instance, process
│   ├── PrivilegeInfo.h
│   ├── PrivilegeInfo.cpp
│   ├── SingleInstance.h
│   ├── SingleInstance.cpp
│   ├── ProcessUtils.h
│   └── ProcessUtils.cpp
├── ui/                             C++/CLI — WinForms presentation + dispatch
│   ├── MyForm.h
│   ├── MyForm.cpp
│   └── UiConstants.h
└── utilities/                      native — logging, timing, string helpers
    ├── Logging.h                   ✔ exists (ProductionLogger adopted)
    ├── Logging.cpp                 ✔ exists
    ├── StringHelpers.h             ✔ exists (TrimTrailingChars)
    └── Timing.h                    (future — timing constants + tick helper)
```

---

## File-by-File Explanation

### `src/main.cpp` (future)

The current `main.cpp` migrates here minus its inline arg-scan loop
(`main.cpp:16–25`), which moves to `application/CommandLine`. It keeps:
WinForms bootstrap, form creation, hidden-window setup for
command/background launches, `Application::Run`.

### `src/application/`

- **`CommandLine.h/.cpp`** — the single argument parser (Rule 12). Merges the
  three current sites: `main.cpp:16–25`, `MyForm_Load:939–945`, and
  `IsRestoreCameraCommand`/`IsDisableCameraCommand` (`MyForm.h:827–849`).
  Exposes `ParseArgs(...) → { background, restoreCamera, disableCamera }`.
- **`ApplicationController.h/.cpp`** — the monitoring state machine and
  lifecycle orchestration: `isMonitoring` gate, `isAlreadyDisabled`
  hardware-state lock, event→operation mapping, target resolution order,
  `DisableTargetCameraHardware`, `EnableTargetCameraHardware`,
  `RestoreConfiguredCameraHardware`, startup sequence, shutdown dual-path
  policy, ghost-mutex force-reset orchestration. Contains the narrow
  `IUiSink` interface for UI updates.

### `src/camera/`

- **`CameraDevice.h/.cpp`** — `CameraDeviceInfo` struct and
  `ScanSystemCameras()` (SetupAPI enumeration, `Camera`/`Image` class filter);
  the `MI_00` target heuristic (extracted from
  `TryGetTargetCameraInstanceId:811` and the duplicate at
  `MyForm_Load:1089`).
- **`CameraHardware.h/.cpp`** — SetupAPI toggle with exact +
  case-insensitive instance-ID matching (`ToggleCameraHardware`),
  `LocateCameraDevInst`, CFGMGR32 fallback (`ToggleCameraHardwareCfgMgr`),
  disabled-state query (`GetCameraHardwareDisabledState`), and
  `VerifyCameraHardwareState` (3 × 100 ms).
- **`CameraRecovery.h/.cpp`** — verified state transitions with retry and
  self-healing (`SetCameraHardwareStateVerified`), power-cycle recovery
  (`RecoverCameraHardware`, 350/900/500 ms), `RestoreAllCameraHardware`,
  and the cooldown helpers (`RecordHardwareToggleTime`; the dormant
  `TryEnterHardwareToggleCooldown` is either adopted here or retired in
  cleanup — Phase 4 decision).
- **`DeviceError.h`** — `{ DWORD setupErr; CONFIGRET configRet; LONG stage; }`
  returned by toggle functions, replacing the `g_last*` globals
  (`MyForm.h:36–39`).

### `src/config/`

- **`ConfigPaths.h/.cpp`** — `GetConfigFilePath`/`GetDiagnosticLogFilePath`
  (`%APPDATA%\Windows Hello Fix\...`, directory creation).
- **`ConfigStore.h/.cpp`** — `WriteDiagnosticLog`,
  `WriteDiagnosticLogWithDevice` (Monitor-locked append), `SaveConfigState`,
  `LoadConfigState` (with `TrimTrailingChars` sanitization),
  `EnsureConfigFileExists`. Starts as the managed implementation for exact
  parity; the native port is a separate isolated phase (encoding checkpoint).

### `src/events/`

- **`SystemEvent.h`** — `enum class SystemEvent { None, SystemEnding,
  QueryEnd, Suspend, Resume, PowerSettingLid, PowerSettingButton,
  PowerSettingOther, SessionLock, SessionUnlock }`.
- **`WinEventDecoder.h/.cpp`** — `Decode(UINT, WPARAM, LPARAM)`; the raw
  message classification and `PBT_POWERSETTINGCHANGE` GUID filter currently
  inline in `WndProc` (`MyForm.h:1246–1346`).
- **`EventCooldown.h/.cpp`** — the 1500 ms dedup semantics
  (`WndProc:1269–1275, 1327–1333`): `ShouldSuppress(code, now)` /
  `Record(code, now)`.
- **`NotificationRegistrar.h/.cpp`** — RAII for
  `RegisterPowerSettingNotification` (lid + button GUIDs) and
  `WTSRegisterSessionNotification` with the 6 × 500 ms retry
  (`MyForm_Load:1060–1066, 1148–1168`).

### `src/system/`

- **`PrivilegeInfo.h/.cpp`** — `IsCurrentProcessElevatedNative` +
  `GetCurrentProcessIntegrityRid`; merges the dead `ElevationChecker`.
- **`SingleInstance.h/.cpp`** — mutex + wake-event creation/ownership
  (`Global\WindowsHelloFix_AppMutex`, `Global\WindowsHelloFix_WakeupEvent`),
  wake signaling.
- **`ProcessUtils.h/.cpp`** — the `taskkill /F /IM Windows_Hello_Fix_v2_0.exe
  /T` ghost-recovery invocation (`MyForm_Load:1044`).

### `src/ui/`

- **`MyForm.h/.cpp`** — the managed form: `InitializeComponent`, control
  handlers, hide-on-close, bring-to-front, wake-listener thread, dropdown
  population (scan only), and a WndProc that only decodes and forwards.
- **`UiConstants.h`** — strings and layout numbers (button text, status
  labels, sizes, colors) currently hard-coded in `InitializeComponent` and
  handlers.

### `src/utilities/` (existing, partially populated)

- **`Logging.h/.cpp`** ✔ — `ProductionLogger` (adopted from dead code;
  Phase 2 complete).
- **`StringHelpers.h`** ✔ — `TrimTrailingChars` (Phase 2 complete).
- **`Timing.h`** (future) — timing constants (1500/100/250/500/1000,
  350/900/500 ms — the behavioral contract from `data-flow.md`) and any
  generic tick helper. Extracted together with `camera/` when the cooldown
  helpers move.

---

## What Is NOT in This Tree

- **No `safety/` module** — see `module-catalog.md` §"Safety as a First-Class
  Concern": safety mechanisms live inside the domain functions that implement
  them; a separate folder would be an empty placeholder.
- **No per-function folders** — `camera/` is one directory of three files, not
  `detection/enumeration/helpers/...`.
- **No `ProductionUtilities.h` remnants** — dead classes are retired in Phase
  11, except `ProductionLogger` (already adopted) and the semantics merged
  into `system/`/`camera/`.
- **No async hardware queue** — explicitly out of scope
  (`target-architecture.md`).

## Migration Phases per Directory

| Directory | Populated by | Status |
|---|---|---|
| `src/utilities/` | Phase 2 | ✔ done |
| `src/system/` | Phases 3, 8 | empty |
| `src/camera/` | Phase 4 | empty |
| `src/config/` | Phase 5 | empty |
| `src/application/` | Phases 6, 9 | empty |
| `src/events/` | Phases 7, 8 | empty |
| `src/ui/` | Phase 10 | empty |
| `src/main.cpp` | Phase 9–10 | empty |

## Related Documents

- `module-catalog.md` — per-module specification.
- `code-classification.md` — function-level mapping into these files.
- `myform-decomposition.md` — the dismantling narrative.
- `../refactoring/modular-extraction-order.md` — the sequence.
- `target-architecture.md` — the module contracts this tree implements.