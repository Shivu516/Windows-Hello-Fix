# MyForm.h Decomposition

How the current 1,354-line `MyForm.h` (plus `main.cpp`) will eventually be
dismantled into the `src/` modules. This is the narrative companion to the
master table in `code-classification.md` and the per-module detail in
`module-catalog.md`.

> Line numbers refer to the analysis baseline. They are navigation aids.

## The Shape of the Problem

`MyForm.h` is not one file — it is **five layers of application squeezed into
one header**:

1. A managed WinForms form class (the `MyForm` ref class, lines 51–204
   declaration, 575–1351 implementation).
2. A native camera-hardware library (lines 208–573).
3. A Win32 message decoder + policy engine (the `WndProc`, lines 1246–1349).
4. A config/log store (lines 682–785).
5. OS primitives (privileges, single-instance) spread across the form
   (lines 227–269, 984–1053).

The decomposition below cuts along those lines. Every group maps to exactly
one future module — no group is split across two modules *within a phase*, so
each extraction phase leaves the build green.

## The Decomposition Tree

```
MyForm.h (1,354 lines)
│
├── 1. UI responsibilities
│       MyForm ctor, InitializeComponent, FormClosing, BringWindowToFrontDelegate,
│       ListenForWakeupSignal, btnToggle_Click (shell), dropdown population,
│       WndProc dispatch shell
│       ──► src/ui/MyForm.h/.cpp (+ UiConstants.h)
│
├── 2. Event handling (decode + dedup + registration)
│       WndProc raw-message classification + GUID filter,
│       1500 ms dedup statics, power/WTS registration blocks
│       ──► src/events/ (WinEventDecoder, EventCooldown, NotificationRegistrar)
│
├── 3. Application lifecycle & policy
│       DisableTargetCameraHardware, EnableTargetCameraHardware,
│       TryGetTargetCameraInstanceId, RestoreConfiguredCameraHardware,
│       MyForm_Load orchestration, destructor/finalizer shutdown policy,
│       WndProc policy blocks (isMonitoring gate, isAlreadyDisabled lock),
│       ghost-reset orchestration
│       ──► src/application/ApplicationController (+ IUiSink)
│
├── 4. Camera/device management
│       CameraDeviceInfo, ScanSystemCameras, ToggleCameraHardware,
│       LocateCameraDevInst, ToggleCameraHardwareCfgMgr,
│       GetCameraHardwareDisabledState
│       ──► src/camera/CameraDevice + CameraHardware
│
├── 5. Safety/failsafe logic
│       VerifyCameraHardwareState, SetCameraHardwareStateVerified,
│       RecoverCameraHardware, RestoreAllCameraHardware,
│       cooldown helpers, check-before-change, event windows
│       ──► src/camera/CameraRecovery
│       (policy-level safety: isAlreadyDisabled, shutdown dual-path
│        ──► src/application/ApplicationController)
│
├── 6. System helpers
│       IsCurrentProcessElevatedNative, GetCurrentProcessIntegrityRid,
│       mutex/event block, taskkill call
│       ──► src/system/ (PrivilegeInfo, SingleInstance, ProcessUtils)
│
├── 7. Configuration & logging
│       GetConfigFilePath, GetDiagnosticLogFilePath, WriteDiagnosticLog,
│       WriteDiagnosticLogWithDevice, SaveConfigState, LoadConfigState,
│       EnsureConfigFileExists
│       ──► src/config/ (ConfigPaths, ConfigStore)
│
└── 8. Generic utilities
        TrimTrailingChars (already moved), timing constants
        ──► src/utilities/ (StringHelpers ✔, Timing)
```

## Group-by-Group Extraction Narrative

### Group 1 — UI responsibilities (──► `src/ui/`)

What stays with the form: construction (`MyForm::MyForm:579–597`),
`InitializeComponent:872–933`, hide-on-close (`MyForm_FormClosing:1195–1212`),
bring-to-front (`BringWindowToFrontDelegate:1185–1193`), the wake-listener
thread (`ListenForWakeupSignal:1171–1183` — it needs `Invoke`), dropdown
population (`MyForm_Load:1068–1111`), and the final thin WndProc that only
decodes and forwards.

**What must NOT stay in the UI:** hardware calls (`btnToggle_Click:1234` calls
`EnableTargetCameraHardware`; WndProc policy blocks), config writes
(`btnToggle_Click:1230,1236`), and `isMonitoring` policy. These become
delegations to `application/ApplicationController` (Rule 1 of the
architecture contract). The `btnToggle_Click` handler ends up as:
read dropdown → tell controller start/stop → controller returns → update
labels. This is the coupling that "UI logic that performs non-UI work"
refers to in the classification.

### Group 2 — Event handling (──► `src/events/`)

The `WndProc` (`MyForm.h:1246–1349`) is a decoder + policy engine fused.
Phase 7 extracts only the *classification* portion: raw message values
(`0x0011/0x0016/0x0218/0x0004/0x8013/0x0007/0x0012`,
`WM_WTSSESSION_CHANGE`) → `SystemEvent`; the `PBT_POWERSETTINGCHANGE` GUID
filter (lid/button vs irrelevant); and the 1500 ms dedup statics into an
`EventCooldown` object. The registration blocks
(`MyForm_Load:1060–1066, 1148–1168`) move to `NotificationRegistrar` in
Phase 8. The policy blocks (isMonitoring gate, op selection) remain in WndProc
until Phase 9 — the decode boundary is what makes Phase 7 verifiable.

### Group 3 — Application lifecycle & policy (──► `src/application/`)

The hardest group, extracted last. It includes the inline policy methods
(`DisableTargetCameraHardware:107–141`,
`EnableTargetCameraHardware:143–177`), target resolution
(`TryGetTargetCameraInstanceId:787–823`), restore orchestration
(`RestoreConfiguredCameraHardware:851–870`), the startup sequence
(`MyForm_Load:935–1169` — orchestration portions), the shutdown dual-path
policy (destructor `600–648`, finalizer `650–680`), WndProc policy blocks
(`1255–1346`), and ghost-recovery orchestration. The controller exposes
`Start/Stop/HandleEvent/HandleSystemEnd/Shutdown` and reports to the UI only
through `IUiSink`.

### Group 4 — Camera/device management (──► `src/camera/CameraDevice` + `CameraHardware`)

Discovery (`ScanSystemCameras:271–305` + `CameraDeviceInfo:208–211`) and the
raw hardware operations (`ToggleCameraHardware:309–368`,
`LocateCameraDevInst:370–395`, `ToggleCameraHardwareCfgMgr:397–417`,
`GetCameraHardwareDisabledState:419–464`) move first — they are
self-contained native code with no managed dependencies. The `MI_00`
heuristic currently duplicated in `TryGetTargetCameraInstanceId:811` and
`MyForm_Load:1089` consolidates into `CameraDevice`.

### Group 5 — Safety/failsafe logic (──► `src/camera/CameraRecovery` + `application/`)

The verification/recovery functions (`VerifyCameraHardwareState:466–477`,
`SetCameraHardwareStateVerified:504–546`, `RecoverCameraHardware:548–565`,
`RestoreAllCameraHardware:567–573`, cooldown `479–502`) form the
**hardware-state safety layer** and move as one unit with the camera hardware
(Phase 4) — they must stay verbatim with the operations they verify.
The **policy-level safety** (`isAlreadyDisabled` lock, 500/1000 ms windows,
shutdown dual-path) moves with the controller (Phase 9). Splitting these two
layers across phases is deliberate: hardware-state safety moves with the
hardware, policy safety moves with the policy.

### Group 6 — System helpers (──► `src/system/`)

Privilege checks (`227–269`) are the first system extraction (Phase 3 — they
have zero dependencies and are used by policy logging). The single-instance
block (`MyForm_Load:984–1053`) and `taskkill` (`1044`) move in Phase 8.

### Group 7 — Configuration & logging (──► `src/config/`)

The seven config/log methods (`682–785`) move as one unit (Phase 5). The
`diagnosticLogSync` lock becomes internal to `ConfigStore`. `LoadConfigState`
keeps its `TrimTrailingChars` sanitization — dependency on
`utilities/StringHelpers` (already extracted) is the only cross-module edge.

### Group 8 — Generic utilities (──► `src/utilities/`)

`TrimTrailingChars` is **already moved** (Phase 2 complete). Remaining:
timing constants (the 1500/100/250/500/1000/350/900/500 ms contract) into
`utilities/Timing.h`, extracted when `camera/` moves.

## What Happens to Each Original Section

| MyForm.h section | Lines | Becomes |
|---|---|---|
| Includes, pragmas, GUID defines | 1–34 | distributed to owning modules (`#pragma comment(lib)` travels with the code) |
| File-scope globals (`g_last*`) | 36–39 | `camera/DeviceError` result struct (Rule 10) |
| Native forward declarations | 41–48 | real declarations in `camera/`/`system/` headers |
| Class declaration (inline policy) | 60–202 | `ui/MyForm` + `application/ApplicationController` |
| Native types + decls | 206–224 | `camera/` headers |
| Native implementations | 225–573 | `system/PrivilegeInfo`, `camera/*` |
| Managed implementations | 575–1351 | `ui/`, `application/`, `config/`, `events/` |

## The End State

After Phase 10, `MyForm.h` does not exist. `src/ui/MyForm` is a thin
presentation shell:

```cpp
// (sketch — not implementation)
private: ApplicationController^ controller;
WndProc(Message% m) {  // dispatch only
    SystemEvent ev = events::WinEventDecoder::Decode(m.Msg, m.WParam.ToInt32(), m.LParam.ToPointer());
    if (ev != SystemEvent::None) controller->HandleEvent(ev);
    Form::WndProc(m);
}
btnToggle_Click(...) { controller->ToggleMonitoring(); }  // policy lives elsewhere
```

No presentation, policy, hardware, config, or logging decision lives in it.

## Phase-to-Group Mapping

| Group | Future module | Migration phase |
|---|---|---|
| 8. Utilities | `utilities/` | 2 ✔ done |
| 6. System helpers (privileges) | `system/PrivilegeInfo` | 3 |
| 4 + 5. Camera hardware + recovery | `camera/` | 4 |
| 7. Configuration & logging | `config/` | 5 |
| 3. Command-line (part) | `application/CommandLine` | 6 |
| 2. Event decode + dedup | `events/` | 7 |
| 2 + 6. Registration + single-instance | `events/` + `system/` | 8 |
| 3. Controller (policy, lifecycle, shutdown) | `application/` | 9 |
| 1. UI shell | `ui/` | 10 |

## Related Documents

- `code-classification.md` — the master function table with line numbers.
- `source-tree.md` — the concrete file tree.
- `module-catalog.md` — per-module contracts.
- `../refactoring/modular-extraction-order.md` — why this order is safe.
- `../refactoring/migration-phases.md` — the operational phase plan.