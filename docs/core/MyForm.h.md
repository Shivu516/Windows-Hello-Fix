# `src/core/MyForm.h` — Class Declaration & Shared State

**Path:** `src/core/MyForm.h`
**Type:** C++/CLI header (`#pragma once`)
**Included by:** `MyForm.h` (root shim), `main.cpp`, and every `MyForm_*.cpp`
**Build relationship:** `ClInclude` in `Windows_Hello_Fix_v2_0.vcxproj`; filter `Header Files`.

## Purpose

This header is the **single source of truth for the `MyForm` class declaration**. It was extracted from the monolithic `release-v2.0/MyForm.h` so that only declarations (and the `CameraDeviceInfo` struct plus free-function forward declarations) remain here, while all function *bodies* now live in the `MyForm_*.cpp` files.

The root `MyForm.h` is a three-line shim (`#pragma once` + `#include "src/core/MyForm.h"`) so that the existing project include path (`#include "MyForm.h"` in `main.cpp`) keeps resolving without any change to `main.cpp`.

## Includes & Pragmas

- Windows headers: `windows.h`, `wtsapi32.h`, `setupapi.h`, `cfgmgr32.h`, `devguid.h`
- STL: `vector`, `string`, `fstream`
- `msclr\marshal_cppstd.h` for native↔managed string marshaling
- `../../resource.h` (relative path because the header now lives in `src/core/`)
- `#pragma comment(lib, ...)` for `wtsapi32.lib`, `setupapi.lib`, `user32.lib`, `cfgmgr32.lib`, `advapi32.lib`

## Compile-time constants (lines 21–35)

| Constant | Value | Role |
|---|---|---|
| `GUID_LIDSWITCH_STATE_CHANGE` | `{BA3E0F4D-…}` | Identify lid-switch power-setting events |
| `GUID_POWER_BUTTON_TIMESTAMP` | `{A70AFB22-…}` | Identify power-button events |
| `CONFIGFLAG_DISABLED` | `0x00000001` | SPDRP_CONFIGFLAGS bit meaning "disabled by config" |
| `CM_PROB_DISABLED` | `22` | Config Manager problem code meaning "disabled" |

## Shared hardware-toggle state (lines 37–40 here; defined once in `MyForm_Camera.cpp` lines 9–12)

These four globals were `static` in the original monolith (single translation unit). In the modular layout they are declared `extern` in this header and **defined exactly once** in `MyForm_Camera.cpp`:

```cpp
extern volatile LONG64 g_lastHardwareToggleTick;
extern volatile LONG  g_lastSetupApiError;
extern volatile LONG  g_lastConfigManagerResult;
extern volatile LONG  g_lastHardwareToggleStage;
```

They are written with `InterlockedExchange`/`InterlockedCompareExchange` from the camera pipeline and read (after zeroing) only for **diagnostic logging** inside `DisableTargetCameraHardware` / `EnableTargetCameraHardware`. (See `docs/KNOWN_ISSUES.md` re: single-instance reasoning.)

## Forward declarations (lines 44–49)

`SetCameraHardwareStateVerified`, `RecoverCameraHardware`, `GetCameraHardwareDisabledState`, `VerifyCameraHardwareState`, `IsCurrentProcessElevatedNative`, `GetCurrentProcessIntegrityRid`. These let the inline-era member functions reference native helpers that are now defined later in `MyForm_Camera.cpp`.

## Watchdog forward declaration (line 61)

```cpp
ref class CameraFailsafe; // forward-declare watchdog (lives outside src/core)
```

Declares the `src/watchdog/CameraFailsafe` managed class without including its header, so `MyForm` can hold a `CameraFailsafe^` handle while avoiding a circular include (`CameraFailsafe.h` itself forward-declares `MyForm`). The include of the full watchdog header happens only in `MyForm_Core.cpp`, which is the single translation unit that constructs/arms the failsafe.

## The `MyForm` class (lines 63–147)

`public ref class MyForm : public System::Windows::Forms::Form` — a C++/CLI WinForms class. It is the **central and only behavioral owner** of all application state. No `ApplicationController`/`CameraController`/etc. exists; every camera, config, event, UI, and lifecycle operation is a member function of this single class.

### Private state members

| Member | Type | Meaning |
|---|---|---|
| `cachedCameras` | `void*` → `std::vector<CameraDeviceInfo>*` | Live list of discovered camera devices |
| `selectedInstanceId` | `void*` → `std::wstring*` | Currently selected/target camera instance ID |
| `isMonitoring` | `bool` | Whether lock/unlock automation is active |
| `isBackgroundMode` | `bool` | Whether launched with `/background` (minimized, no taskbar) |
| `isSystemEnding` | `bool` | Set in `WndProc` on shutdown/logoff; changes destructor behavior |
| `cameraStateInitialized` | `bool` | Reserved/legacy flag (declared, not heavily used) |
| `cameraExpectedDisabled` | `bool` | Last intended camera state (mirror for logging/UI) |
| `restartQueuedByMismatch` | `bool` | Reserved/legacy flag |
| `lastCameraToggleTick` | `ULONGLONG` | Legacy per-instance tick (separate from global) |
| `hAppMutex` | `HANDLE` | Owned single-instance mutex (`Global\WindowsHelloFix_AppMutex`) |
| `hWakeupEvent` | `HANDLE` | Owned named event for cross-process "show window" signal |
| `backgroundWorker` | `System::Threading::Thread^` | Listener thread for the wake event |
| `keepListening` | `bool` | Loop control for the wake listener |
| `cameraFailsafe` | `CameraFailsafe^` | Owned auxiliary runtime failsafe (see below); `nullptr` until armed in `MyForm_Load` |
| `hLidNotification` | `HPOWERNOTIFY` | Lid-switch power registration handle |
| `hButtonNotification` | `HPOWERNOTIFY` | Power-button power registration handle |
| `deviceDrop` | `ComboBox^` | UI: camera selector |
| `btnToggle` | `Button^` | UI: start/stop monitoring button |
| `lblTitle` | `Label^` | UI |
| `lblStatus` | `Label^` | UI: status text |
| `components` | `Container^` | WinForms component container |
| `diagnosticLogSync` | `Object^` | Monitor lock object for the diagnostic log file |
| `lastToggleTime` (static) | `DateTime` | Per-class cooldown timestamp |
| `COOLDOWN_MILLISECONDS` (static const) | `int = 1500` | Declared cooldown constant (UI/throttling context) |

### The `cameraFailsafe` member (lines 85–87)

```cpp
CameraFailsafe^ cameraFailsafe;
// Auxiliary runtime failsafe — observes ExpectedEnabled vs observed Disabled;
// never performs camera operations itself. Lives outside src/core.
```

The single owned watchdog instance, created and armed at the end of `MyForm_Load` (`MyForm_Core.cpp:420–428`) and disarmed first in `~MyForm`/`!MyForm` before any shutdown camera handling. It is the **only** `MyForm` member whose type lives outside `src/core`. The watchdog holds a back-reference (`MyForm^ owner`) and may only call the six read-only accessors below plus the logging helpers — it must never mutate core state directly. A second, independent watchdog (`RecoveryLoopFailsafe`) is owned by `main.cpp`, not by `MyForm`, so it has no member here.

### Member function declarations

- `GetConfigFilePath`, `GetDiagnosticLogFilePath`
- `WriteDiagnosticLog`, `WriteDiagnosticLogWithDevice`
- `SaveConfigState`, `LoadConfigState`, `EnsureConfigFileExists`
- `TryGetTargetCameraInstanceId`
- `DisableTargetCameraHardware(bool)`, `EnableTargetCameraHardware(bool)` — declared here, body in `MyForm_Camera.cpp`
- `IsRestoreCameraCommand`, `IsDisableCameraCommand`, `RestoreConfiguredCameraHardware`
- `ListenForWakeupSignal`, `BringWindowToFrontDelegate`
- Failsafe integration (public, read-only — bodies in `MyForm_Core.cpp:433–461`):
  - `IsMonitoringActive()` — returns `isMonitoring`
  - `IsSystemEndingActive()` — returns `isSystemEnding`
  - `IsCameraExpectedEnabled()` — returns `!cameraExpectedDisabled`
  - `TryGetFailsafeTargetId(std::wstring&)` — forwards to `TryGetTargetCameraInstanceId(target, true)`
  - `LogFailsafe(...)` / `LogFailsafeWithDevice(...)` — forward to `WriteDiagnosticLog` / `WriteDiagnosticLogWithDevice`
- `MyForm(void)` (public ctor), `~MyForm()` / `!MyForm()` (protected dtor & finalizer)
- `InitializeComponent`, `MyForm_Load`, `MyForm_FormClosing`, `btnToggle_Click`
- `WndProc(Message%)` (protected override)

## The `CameraDeviceInfo` struct (lines 153–156)

```cpp
struct CameraDeviceInfo { std::wstring friendlyName; std::wstring instanceId; };
```
Holds the display name and the device-instance ID used by SetupAPI/CfgMgr.

## Free-function declarations (lines 158–170)

Forward declarations for the native camera pipeline: `ScanSystemCameras`, `ToggleCameraHardware`, `LocateCameraDevInst`, `ToggleCameraHardwareCfgMgr`, `GetCameraHardwareDisabledState`, `VerifyCameraHardwareState`, `SetCameraHardwareStateVerified`, `TryEnterHardwareToggleCooldown`, `RecordHardwareToggleTime`, `RecoverCameraHardware`, `RestoreAllCameraHardware`, and `TrimTrailingChars`.

## Why the architecture keeps `MyForm` as the central owner

The extraction deliberately preserved *one class, one state model, one lifetime, one behavioral authority*. Splitting member-function bodies into separate `.cpp` files did **not** split ownership: the `MyForm` object still owns the mutex, the wake event, the power-notification handles, the camera target selection, the monitoring flag, and the UI controls. This minimizes the risk of behavioral regressions versus the known-good `release-v2.0/MyForm.h`.

## Dependencies

- **Depends on:** Windows SDK headers, `resource.h`, the C++/CLI runtime, .NET Framework 4.7.2.
- **Depended on by:** all six `MyForm_*.cpp` files (they each `#include "MyForm.h"`), `main.cpp`, and both `src/watchdog/*.cpp` files (which include `../core/MyForm.h` for the `MyForm` accessors and the native camera-pipeline declarations).

## Threading / synchronization

- `diagnosticLogSync` guards `WriteDiagnosticLog` only.
- The four `g_last*` globals use interlocked operations; they are *not* protected by `diagnosticLogSync`.
- `lastToggleTime`/`COOLDOWN_MILLISECONDS` are `static` on the class (shared across all instances, of which there is effectively one).
