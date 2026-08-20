# Current Codebase (v2.0) — Complete Map

This document answers: *"Where does every major piece of the current application
live?"* It documents the pre-refactor baseline. Nothing here has been modified;
this is a reading guide derived from the source as committed on the `v2.1`
branch.

> Line numbers refer to the files as of the analysis baseline. They are an aid
> to navigation, not a contract.

## Build Shape

- `main.cpp` — the single translation unit. Includes `MyForm.h`.
- `MyForm.h` — the entire application (UI, logic, hardware, config, logging).
- `ProductionUtilities.h` — **unused scaffolding** (never `#include`d).
- `Windows_Hello_Fix_v2_0.sln` / `.vcxproj` / `.vcxproj.filters` — the project.
- Resources — see "Resource Files" below.

## main.cpp

| Item | Purpose |
|---|---|
| `main(array<String^>^)` | Managed entry point (`[STAThread]`). Enables visual styles, creates `MyForm`, checks command-line args for background/command modes, hides the form for background launches, runs the message loop. |
| Argument scan (lines 16–25) | Detects `--background`, `/background`, `--disable-camera`, `/disable-camera`, `--enable-camera`, `/restore-camera`, `/repair-camera` to decide whether to hide the window. Note: `/enable-camera` (single dash) is *not* listed here, while the form's command parser does accept it — an existing inconsistency. |

## MyForm.h — Structural Breakdown

`MyForm.h` contains three logical regions:

1. Includes, library pragmas, GUID/constant fallback defines (lines 1–38).
2. Native forward declarations + global state (lines 35–49).
3. The managed `ref class MyForm` inside `namespace Windows_Hello_Fix_v2_0`
   (lines 50–203).
4. Native struct/free-function implementations (lines 205–581).
5. Managed class implementation (lines 583–1359).

### Global / Static State (file scope, lines 35–38)

| Symbol | Type | Purpose | Written by | Read by |
|---|---|---|---|---|
| `g_lastHardwareToggleTick` | `volatile LONG64` | Interlocked timestamp for hardware-toggle cooldown | `RecordHardwareToggleTime` | `TryEnterHardwareToggleCooldown` (never called — dormant) |
| `g_lastSetupApiError` | `volatile LONG` | Last SetupAPI error code (diagnostics) | `ToggleCameraHardware` | diagnostic log formatting |
| `g_lastConfigManagerResult` | `volatile LONG` | Last CFGMGR32 `CONFIGRET` (diagnostics) | `ToggleCameraHardwareCfgMgr` | diagnostic log formatting |
| `g_lastHardwareToggleStage` | `volatile LONG` | Stage marker for the hardware toggle pipeline (10–23) | toggle functions | diagnostic log formatting |

These globals exist solely for the structured diagnostic log lines. They are a
shared-mutable-state problem the refactor should replace with a
`DeviceError`-style result struct.

### Types

| Type | Kind | Responsibility |
|---|---|---|
| `MyForm` | `public ref class` : `System::Windows::Forms::Form` | Everything (the god-object) |
| `CameraDeviceInfo` | native struct | `friendlyName` + `instanceId` for a scanned camera |

### Managed class MyForm — members

| Member | Purpose |
|---|---|
| `cachedCameras` (`void*`) | Owns a native `std::vector<CameraDeviceInfo>*` |
| `selectedInstanceId` (`void*`) | Owns a native `std::wstring*` (current target) |
| `isMonitoring` | Service running flag |
| `isBackgroundMode` | Launched via `/background` |
| `isSystemEnding` | Set on `WM_QUERYENDSESSION`/`WM_ENDSESSION`; changes destructor behavior |
| `cameraStateInitialized` | **Unused** (set in constructor only) |
| `cameraExpectedDisabled` | Tracks expected hardware state (written, diagnostic only) |
| `restartQueuedByMismatch` | **Unused** |
| `lastCameraToggleTick` | **Unused** |
| `hAppMutex` | Single-instance mutex handle |
| `hWakeupEvent` | Cross-process wake-up named event |
| `backgroundWorker`, `keepListening` | Wake-up listener thread + loop flag |
| `hLidNotification`, `hButtonNotification` | `HPOWERNOTIFY` power-registration handles |
| `deviceDrop`, `btnToggle`, `lblTitle`, `lblStatus`, `components` | WinForms controls |
| `diagnosticLogSync` | Lock object for diagnostic log writes |
| `static lastToggleTime`, `static COOLDOWN_MILLISECONDS` | **Set but never read** (dormant cooldown) |

### Managed methods (grouped by responsibility)

**Configuration / logging**
- `GetConfigFilePath` / `GetDiagnosticLogFilePath` — `%APPDATA%\Windows Hello Fix\config.txt` / `diagnostic.log`.
- `WriteDiagnosticLog` / `WriteDiagnosticLogWithDevice` — append timestamped `Event=... | Target=... | Verify=PASS/FAIL` lines (thread-safe via `Monitor`).
- `SaveConfigState` / `LoadConfigState` / `EnsureConfigFileExists` — `monitoring=0|1` + `device=<instance id>`.

**Target resolution**
- `TryGetTargetCameraInstanceId` — selection → saved config → first `MI_00` device → first device.

**Command parsing**
- `IsRestoreCameraCommand` / `IsDisableCameraCommand` — recognize the `/restore-camera`/`/enable-camera`/`/repair-camera` and `/disable-camera`/`--disable-camera` families.

**Camera policy (managed orchestration)**
- `DisableTargetCameraHardware(bool retryOnFailure)` — resolve target, skip if already disabled, call native verified-disable, write diagnostic.
- `EnableTargetCameraHardware(bool cycleDevice)` — resolve target, skip if already enabled, call native recover, write diagnostic.
- `RestoreConfiguredCameraHardware(bool cycleDevice)` — enable saved device; fall back to restoring all cameras.

**Lifecycle / events**
- Constructor — initializes members, calls `InitializeComponent`.
- Destructor (`~MyForm`) / Finalizer (`!MyForm`) — shutdown cleanup; duplicates cleanup logic.
- `MyForm_Load` — startup sequence (see Data Flow doc).
- `MyForm_FormClosing` — hides the form instead of closing; shows the "background service active" notice once.
- `btnToggle_Click` — start/stop monitoring.
- `WndProc` — message handling (see below).
- `ListenForWakeupSignal` / `BringWindowToFrontDelegate` — background wake-up listener thread + UI-thread invoker.

**UI construction**
- `InitializeComponent` — builds controls, loads the icon via `LoadImage(IDI_ICON1)`.

### Native free functions (lines 205–581)

| Function | Category | Purpose |
|---|---|---|
| `TrimTrailingChars` | utility | Strips trailing `\r`/`\n`/space from a `std::wstring` |
| `IsCurrentProcessElevatedNative` | system | Token elevation check |
| `GetCurrentProcessIntegrityRid` | system | Integrity level RID from token |
| `ScanSystemCameras` | camera | Enumerate `DIGCF_PRESENT` devices of class `Camera`/`Image` |
| `ToggleCameraHardware` | camera | SetupAPI disable/enable by instance ID (exact + case-insensitive match) |
| `LocateCameraDevInst` | camera | Find `DEVINST` for an instance ID |
| `ToggleCameraHardwareCfgMgr` | camera | CFGMGR32 fallback (`CM_Enable/Disable_DevNode` + re-enumerate) |
| `GetCameraHardwareDisabledState` | camera | Detect disabled via `CM_Get_DevNode_Status` (`CM_PROB_DISABLED`) or `SPDRP_CONFIGFLAGS` (`CONFIGFLAG_DISABLED`) |
| `VerifyCameraHardwareState` | camera | Up to 3 attempts × 100 ms |
| `TryEnterHardwareToggleCooldown` | timing | **Dormant** (never called) |
| `RecordHardwareToggleTime` | timing | Stores `GetTickCount64()` into the global |
| `SetCameraHardwareStateVerified` | camera | Check-before-change; up to 3 attempts; SetupAPI → CFGMGR → optional reinit-by-opposite-toggle; final retry |
| `RecoverCameraHardware` | camera | Enable; optional power cycle with fixed sleeps (350/900/500 ms) |
| `RestoreAllCameraHardware` | camera | Recover every scanned camera |

### WndProc message handling (lines 1254–1357)

Raw message values are used directly (not symbolic constants):

| Message | Raw value | Behavior |
|---|---|---|
| `WM_ENDSESSION` / `WM_QUERYENDSESSION` | `0x0016` / `0x0011` | `isSystemEnding = true`; if monitoring, disable camera; `WTSUnRegisterSessionNotification` |
| `WM_POWERBROADCAST` (suspend, power intercept) | `0x0218`, `0x0004`, `0x8013` | If `0x8013`, filter to lid/button GUIDs; if monitoring and not already disabled: set `isAlreadyDisabled`, disable camera, `Sleep(500)` |
| `WM_POWERBROADCAST` (resume) | `0x0007`, `0x0012` | If monitoring: `Thread::Sleep(1000)`, enable camera, clear `isAlreadyDisabled` |
| `WM_WTSSESSION_CHANGE` (lock/unlock) | `WTS_SESSION_LOCK` / `WTS_SESSION_UNLOCK` | Disable on lock, enable on unlock |

Static locals inside `WndProc`: `isAlreadyDisabled` (hardware-state lock),
`lastSessionEventTick/Code` and `lastPowerEventTick/Code` (1500 ms debounce).

### Application lifecycle summary

**Startup (`MyForm_Load`)**: log context → handle restore/disable command modes
(exit) → acquire single-instance mutex (or wake existing / ghost-reset / exit) →
create wakeup event → restore configured camera (cycle) → register power
notifications → scan + populate dropdown (saved → `MI_00` → first) → force-enable
target → start monitoring if background or config says so → start wake-up thread →
register `WTSRegisterSessionNotification` (6 × 500 ms retry).

**Shutdown**: normal exit → enable camera, save `monitoring=1`; system-ending →
disable camera, save `monitoring=1`; release wake event, power notifications,
native buffers, mutex.

## ProductionUtilities.h (unused)

| Class | Purpose (as designed) | Status |
|---|---|---|
| `ProductionLogger` | Structured `OutputDebugStringW` logging | Dead (design artifact) |
| `HardwareOperationQueue` | Async hardware op queue with worker thread | Dead; proposed in `WndProc_Redesign.txt`; `ProcessOperations` was to live in `MyForm.h` (latent circular dependency) |
| `SingleInstanceManager` | `Local\WindowsHelloFix_AppMutex_v15` mutex | Dead; the live code uses `Global\WindowsHelloFix_AppMutex` directly |
| `ElevationChecker` | Elevation check | Dead; duplicates `IsCurrentProcessElevatedNative` |
| `EnhancedSetupAPI` | SetupAPI + CFGMGR fallback wrappers | Dead; the live code implements the same strategy in `ToggleCameraHardware` + `ToggleCameraHardwareCfgMgr` |
| `ShutdownManager` | Static shutdown flag | Dead |

## Resource Files

| File | Role | Issues |
|---|---|---|
| `resource.h` | IDs for `Windows_Hello_Fix_v2_0.rc` (`IDI_ICON1` = 102, `IDI_ICON2` = 103, `IDI_ICON3` = 104, `IDB_PNG1` = 101) | The form loads `IDI_ICON1` = 102, but this header is not the one compiled into the build |
| `resource1.h` | IDs for `Windows_Hello_Fix_v2_0_resources.rc` (`IDI_ICON1` = 114) | The compiled resource header |
| `Windows_Hello_Fix_v2_0.rc` | **Not compiled** (not in the vcxproj) | References absolute machine paths (`C:\Users\gupta\...`) and a missing `icon1.ico` |
| `Windows_Hello_Fix_v2_0_resources.rc` | The compiled resource script | References `x64\Release\WindowsHelloFix.ico` — a gitignored build output; a clean checkout cannot build it |

The two header/script pairs are inconsistent (`IDI_ICON1` = 102 vs 114), which
means the form's window icon load silently fails at runtime. See the risk
register.

## Project Files & Manifest

- `Windows_Hello_Fix_v2_0.sln` — VS 17 solution; `Debug|Win32`, `Release|Win32`,
  `Debug|x64`, `Release|x64`.
- `Windows_Hello_Fix_v2_0.vcxproj` — `CLRSupport=true`, `CharacterSet=Unicode`,
  toolset v143, SDK `10.0` (latest), .NET Framework v4.7.2, `SubSystem=Windows`,
  `EntryPointSymbol=main`, `RequireAdministrator`; x64 configs list
  `setupapi.lib;user32.lib;wtsapi32.lib;advapi32.lib`; Win32 configs rely on
  `#pragma comment(lib)`.
- `Windows_Hello_Fix_v2_0.vcxproj.filters` — flat filters; references many
  missing doc files as `None`/`Text`/`Xml` items (harmless but stale).
- `app.manifest` — `requireAdministrator`, `uiAccess=false`, identity
  `Windows_Hello_Fix_v2_0` v2.0.0.0.
- `CppProperties.json` — IntelliSense config (x64-Debug, `msvc_x64`,
  `UNICODE`/`_UNICODE`).

## Important Windows APIs Used

| API family | Used for |
|---|---|
| `SetupDiGetClassDevs` / `SetupDiEnumDeviceInfo` / `SetupDiGetDeviceInstanceId` / `SetupDiGetDeviceRegistryProperty` / `SetupDiSetClassInstallParams` / `SetupDiCallClassInstaller` / `SetupDiDestroyDeviceInfoList` | Device enumeration + property-based disable/enable |
| `CM_Locate_DevNodeW` / `CM_Enable_DevNode` / `CM_Disable_DevNode` / `CM_Reenumerate_DevNode` / `CM_Get_DevNode_Status` | Configuration Manager fallback + state query |
| `WTSRegisterSessionNotification` / `WTSUnRegisterSessionNotification` | Session lock/unlock notification |
| `RegisterPowerSettingNotification` / `UnregisterPowerSettingNotification` | Lid switch + power button notification |
| `OpenProcessToken` / `GetTokenInformation` (`TokenElevation`, `TokenIntegrityLevel`) | Elevation + integrity checks |
| `CreateMutexW` / `CreateEventW` / `OpenEvent` / `SetEvent` / `WaitForSingleObject` | Single instance + wake-up |
| `GetTickCount64` | Timing |
| `OutputDebugStringW` | Native logging (via `ProductionLogger`) |
| `LoadImage` | Form icon |
| `taskkill` (via `system()`) | Ghost-process force reset |

## Generated vs. Source-Controlled

| Class | Files |
|---|---|
| SOURCE | `main.cpp`, `MyForm.h`, `ProductionUtilities.h` |
| CONFIGURATION | `.sln`, `.vcxproj`, `.vcxproj.filters`, `CppProperties.json`, `.gitignore`, `.gitattributes`, `app.manifest` |
| RESOURCES | `resource.h`, `resource1.h`, both `.rc` |
| DOCUMENTATION | `README.md`, `LICENSE`, `WndProc_Redesign.txt` |
| GENERATED BUILD OUTPUT | `x64\*`, `Windows_.6BD38D8C\*` (MSBuild intermediates), `.vs\*` |
| MISSING (referenced, absent) | `icon1.ico`, `WindowsHelloFix_TaskSchedule.xml`, several stale `.md`/`.txt`/`.nsi` files, and `x64\Release\WindowsHelloFix.ico` on a clean checkout |