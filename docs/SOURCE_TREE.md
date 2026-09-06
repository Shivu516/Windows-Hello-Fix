# Source Tree

Current `src/` layout (documented as-is):

```
src/
├── core/
│   ├── MyForm.h              Declaration-only: class, CameraDeviceInfo, extern globals, forward decls (170 lines)
│   ├── MyForm_Camera.cpp     Native camera pipeline + Disable/Enable/Restore members (470 lines)
│   ├── MyForm_Config.cpp     Config paths, diagnostic log, save/load, target resolution (148 lines)
│   ├── MyForm_Core.cpp       ctor/dtor/finalizer/InitializeComponent/MyForm_Load + failsafe accessors (463 lines)
│   ├── MyForm_Events.cpp     WndProc: session/power/shutdown dispatch (110 lines)
│   ├── MyForm_System.cpp     Command parsing + wake listener (54 lines)
│   └── MyForm_UI.cpp         FormClosing + btnToggle_Click (56 lines)
│
└── watchdog/
    ├── CameraFailsafe.h          Long-term failsafe declaration: 90 s poll / 10 s verify / 45 s grace (58 lines)
    ├── CameraFailsafe.cpp        Poll, confirm & enable-only recover, bounded backoff (217 lines)
    ├── RecoveryLoopFailsafe.h    Fast-verifier declaration: 5 s startup / 30 s poll / 5 s retry (67 lines)
    └── RecoveryLoopFailsafe.cpp  Startup check, poll & bounded linear retry (237 lines)
```

> Historical note: an earlier revision of this document showed a `src/ui/` directory. The tree was renamed to `src/core/` (`6df22d0 Renamed Folders`) and `src/watchdog/` was added for the failsafe work. The per-file docs live alongside the tree: `docs/core/` mirrors `src/core/`, `docs/watchdog/` mirrors `src/watchdog/`.

Other relevant files (outside `src/`):

```
MyForm.h                              Root shim -> #include "src/core/MyForm.h"
main.cpp                             Entry point (MyForm form + Application::Run + RecoveryLoopFailsafe wiring, 67 lines)
ProductionUtilities.h                Legacy/unused helper header (not part of the build flow)
Windows_Hello_Fix_v2_1.vcxproj       MSBuild project (lists all src/core + src/watchdog files; per-platform <TargetName> v2_1_x86/x64)
Windows_Hello_Fix_v2_1.vcxproj.filters
installer/                           NSIS source: common.nsh (shared logic) + install_x86/x64/universal .nsi (thin wrappers)
reference/                           Canonical references: release-v2.0 (camera truth), legacy-v1.0 (untouched)
```

## Per-file responsibility (one line)

| File | Responsibility |
|---|---|
| `src/core/MyForm.h` | Class/struct/extern declarations; compile-time GUID/constant definitions; watchdog forward-decl + read-only accessor declarations. |
| `src/core/MyForm_Camera.cpp` | **Authoritative camera implementation:** discovery, SetupAPI & CfgMgr enable/disable, verification, retry/recovery, cooldown globals. |
| `src/core/MyForm_Config.cpp` | `%APPDATA%` config + diagnostic log, save/load/parse, target-device resolution. |
| `src/core/MyForm_Core.cpp` | Object lifetime: constructor, destructor, finalizer, UI init, full startup sequence; owns/arms `CameraFailsafe`; implements the failsafe accessors. |
| `src/core/MyForm_Events.cpp` | `WndProc`: deduplicated handling of shutdown, suspend/resume, lid/button, lock/unlock. |
| `src/core/MyForm_System.cpp` | Command-line verb detection; background wake-listener thread. |
| `src/core/MyForm_UI.cpp` | Form close (minimize-to-background) and start/stop monitoring button. |
| `src/watchdog/CameraFailsafe.h` | Long-term failsafe declaration: state enum, 90 s / 10 s / 45 s / 30 s timing contract. |
| `src/watchdog/CameraFailsafe.cpp` | Long-term observation/recovery: 90 s poll, 10 s confirm, enable-only `Recover(false)` + verify, 10→20→40 s backoff, 3-attempt bound. |
| `src/watchdog/RecoveryLoopFailsafe.h` | Fast-verifier declaration: 5 s startup / 30 s poll / 5 s retry contract; `Load`/`FormClosing` hooks for `main.cpp`. |
| `src/watchdog/RecoveryLoopFailsafe.cpp` | Fast observation/recovery: one-shot 5 s startup check, 30 s poll, enable-only `Recover(false)` + verify, linear 5 s retry, 3-attempt bound. |
| `main.cpp` | Process entry: hidden-launch opacity, `isCommandWorker` guard, owns `RecoveryLoopFailsafe` (`Load` → Arm, `FormClosing` → Disarm). |

## Camera authority (read this before touching camera code)

- **Hardware changes happen in exactly one place:** `src/core/MyForm_Camera.cpp` (`ToggleCameraHardware`, `ToggleCameraHardwareCfgMgr`, orchestrated by `SetCameraHardwareStateVerified` / `RecoverCameraHardware`).
- **Observation + recovery requests** live in `src/watchdog/` (`CameraFailsafe`, `RecoveryLoopFailsafe`). Both call `GetCameraHardwareDisabledState` (observe), `RecoverCameraHardware(target, false)` (enable-only recover), `VerifyCameraHardwareState` (confirm). Neither contains `SetupDi*`/`CM_*` device-state calls.
- **Target/config authority** is `MyForm::TryGetTargetCameraInstanceId` (`src/core/MyForm_Config.cpp`); both watchdogs resolve targets through `TryGetFailsafeTargetId` and never parse `config.txt` themselves.
