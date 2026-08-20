# Code Classification (v2.0 → v2.1)

The master, function-level classification of the existing Windows Hello Fix v2.0
implementation. This document answers: *"every meaningful piece of the current
code — what does it do, who owns it, and where does it go in v2.1?"*

It is the granular companion to `migration-map.md` (which maps at function-group
level) and `target-architecture.md` (which defines module contracts). Use this
document when a migration phase needs the exact symbol list, dependency edges,
and risk of a code region.

> Line numbers refer to the files as of this analysis baseline (MyForm.h is
> 1,354 lines). They are navigation aids, not a contract.

---

## 1. Classification Categories

Each code region is typed by primary nature (section 9 of the task):

| Type | Meaning |
|---|---|
| **A — Application orchestration** | Coordinates other subsystems: policy, ordering, state gates, lifecycle |
| **B — Domain/system logic** | Implements actual Windows Hello Fix functionality (device discovery, camera control, session/power handling, recovery) |
| **C — Presentation/UI** | Form controls, visual state, user-facing messages, window behavior |

Many current functions are mixed; the **future module** column is where the
dominant responsibility lands, and the **coupling notes** flag the parts that
must be separated during extraction.

---

## 2. File-Scope State (MyForm.h:36–39)

| Symbol | Type | Purpose | Written by | Read by | Future owner |
|---|---|---|---|---|---|
| `g_lastHardwareToggleTick` | `volatile LONG64` | Interlocked timestamp for hardware-toggle cooldown | `RecordHardwareToggleTime`, `TryEnterHardwareToggleCooldown` | `TryEnterHardwareToggleCooldown` (dormant) | `camera/DeviceError` + `utilities/Timing` |
| `g_lastSetupApiError` | `volatile LONG` | Last SetupAPI error (diagnostics) | `ToggleCameraHardware` | `DisableTargetCameraHardware`, `EnableTargetCameraHardware` (log formatting) | `camera/DeviceError` |
| `g_lastConfigManagerResult` | `volatile LONG` | Last CFGMGR32 `CONFIGRET` (diagnostics) | `ToggleCameraHardwareCfgMgr` | same as above | `camera/DeviceError` |
| `g_lastHardwareToggleStage` | `volatile LONG` | Stage marker (10–23) for the toggle pipeline (diagnostics) | toggle functions | same as above | `camera/DeviceError` |

All four exist only for the structured diagnostic log lines. They are
shared-mutable state that the refactor replaces with a `DeviceError{ setupErr,
configRet, stage }` result struct returned by the camera functions (see
`architecture-contract.md`, Rule 10).

---

## 3. Master Classification Table — MyForm.h

### 3.1 Native free functions (MyForm.h:225–573)

| Current Location | Function | Responsibility | Type | Future Module | Future File | Depends on | Risk |
|---|---|---|---|---|---|---|---|
| 227–239 | `IsCurrentProcessElevatedNative` | Token elevation check (`OpenProcessToken` + `TokenElevation`) | B (system) | `system/` | `PrivilegeInfo.h/.cpp` | advapi32 | Low |
| 241–269 | `GetCurrentProcessIntegrityRid` | Integrity level RID from token (`TokenIntegrityLevel`, SID sub-authority) | B (system) | `system/` | `PrivilegeInfo.h/.cpp` | advapi32, LocalAlloc/LocalFree | Low |
| 271–305 | `ScanSystemCameras` | Enumerate `DIGCF_PRESENT` devices, filter class `Camera`/`Image`, collect `{friendlyName, instanceId}` | B (camera discovery) | `camera/` | `CameraDevice.h/.cpp` | setupapi | Medium (enumeration semantics) |
| 309–368 | `ToggleCameraHardware` | SetupAPI disable/enable by instance ID (exact + case-insensitive match); writes `g_lastSetupApiError`/`g_lastHardwareToggleStage` | B (camera hardware) | `camera/` | `CameraHardware.h/.cpp` | setupapi | Medium (wrong-device protection lives here) |
| 370–395 | `LocateCameraDevInst` | Find `DEVINST` for an instance ID (SetupAPI enumeration) | B (camera hardware) | `camera/` | `CameraHardware.h/.cpp` | setupapi | Low |
| 397–417 | `ToggleCameraHardwareCfgMgr` | CFGMGR32 fallback (`CM_Enable/Disable_DevNode` + `CM_Reenumerate_DevNode`); writes `g_lastConfigManagerResult`/stage | B (camera hardware) | `camera/` | `CameraHardware.h/.cpp` | cfgmgr32 | Medium (fallback path) |
| 419–464 | `GetCameraHardwareDisabledState` | Detect disabled via `CM_Get_DevNode_Status` (`CM_PROB_DISABLED`) or `SPDRP_CONFIGFLAGS` (`CONFIGFLAG_DISABLED`) | B (camera state query) | `camera/` | `CameraHardware.h/.cpp` | setupapi, cfgmgr32 | Medium (dual-path state check) |
| 466–477 | `VerifyCameraHardwareState` | Up to 3 attempts × 100 ms polling of disabled state | B (verification) | `camera/` | `CameraHardware.h/.cpp` | `GetCameraHardwareDisabledState` | Low |
| 479–498 | `TryEnterHardwareToggleCooldown` | Interlocked cooldown gate over `g_lastHardwareToggleTick` — **dormant, never called** | B (safety/timing) | `camera/` or retire | `CameraRecovery.h/.cpp` (or drop in cleanup) | kernel32 | Low (dormant) |
| 500–502 | `RecordHardwareToggleTime` | Stores `GetTickCount64()` into the global | B (timing) | `camera/` | `CameraRecovery.h/.cpp` | kernel32 | Low |
| 504–546 | `SetCameraHardwareStateVerified` | Check-before-change; up to 3 attempts: SetupAPI → CFGMGR32 → optional reinit-by-opposite-toggle + 250 ms sleeps; final retry | B (camera recovery) | `camera/` | `CameraRecovery.h/.cpp` | `ToggleCameraHardware`, `ToggleCameraHardwareCfgMgr`, `VerifyCameraHardwareState`, `RecordHardwareToggleTime` | **High** (retry/self-healing contract) |
| 548–565 | `RecoverCameraHardware` | Enable; optional power cycle with fixed sleeps (350/900/500 ms) | B (camera recovery) | `camera/` | `CameraRecovery.h/.cpp` | `SetCameraHardwareStateVerified` | **High** (timing contract) |
| 567–573 | `RestoreAllCameraHardware` | Recover every scanned camera (no configured target fallback) | B (camera recovery) | `camera/` | `CameraRecovery.h/.cpp` | `ScanSystemCameras`, `RecoverCameraHardware` | Medium |

**Camera subsystem grouping note:** discovery (`ScanSystemCameras`) +
identification heuristic + hardware ops + verification + recovery form **one
coherent `camera/` subsystem** of three files. They are not split into five
separate modules: they share `DEVINST`/`HDEVINFO` semantics and the
verification loop, and the target heuristic is small enough to live with
`CameraDevice`. This follows the "do not over-engineer" requirement.

### 3.2 Managed class — inline policy methods (MyForm.h:107–177)

| Current Location | Function | Responsibility | Type | Future Module | Future File | Depends on | Risk |
|---|---|---|---|---|---|---|---|
| 107–141 | `DisableTargetCameraHardware(bool retryOnFailure)` | Resolve target → skip if already disabled → verified disable → diagnostic line (`..._AlreadyDisabled` / `..._Result`) | A (policy) | `application/` | `ApplicationController.h/.cpp` | `TryGetTargetCameraInstanceId`, `GetCameraHardwareDisabledState`, `SetCameraHardwareStateVerified`, `VerifyCameraHardwareState`, privilege helpers, `g_last*` globals, config logging | **High** (policy + diagnostics coupled) |
| 143–177 | `EnableTargetCameraHardware(bool cycleDevice)` | Resolve target → skip if already enabled → recover → verified enable → diagnostic line | A (policy) | `application/` | `ApplicationController.h/.cpp` | same set | **High** |

Both methods currently read the `g_last*` globals and elevation state inline for
log formatting — coupling that must move into `camera/DeviceError` +
`system/PrivilegeInfo` and be passed back to the caller.

### 3.3 Managed class — lifecycle (MyForm.h:579–680, 935–1169, 1171–1212)

| Current Location | Function | Responsibility | Type | Future Module | Future File | Depends on | Risk |
|---|---|---|---|---|---|---|---|
| 579–597 | `MyForm::MyForm` (ctor) | Init all members (native buffers as `void*`, flags, `diagnosticLogSync`), `InitializeComponent` | C (UI init) | `ui/` | `MyForm.cpp` | — | Low |
| 600–648 | `MyForm::~MyForm` | Shutdown: `isSystemEnding` → disable camera + save `monitoring=1`; else enable camera (or restore all) + save; release wake event, power notifications, native buffers, mutex | A (shutdown orchestration) + B (handle cleanup) | `application/` (policy) + `system/` + `events/` (RAII release) | `ApplicationController::Shutdown` + `SingleInstance` + `NotificationRegistrar` | `DisableTargetCameraHardware`, `EnableTargetCameraHardware`, `RestoreConfiguredCameraHardware`, `SaveConfigState`, `SetEvent`/`CloseHandle`/`UnregisterPowerSettingNotification` | **High** (dual-path shutdown semantics) |
| 650–680 | `MyForm::!MyForm` (finalizer) | Duplicate of destructor cleanup (no `SaveConfigState`) | A + B | merge into `ui/MyForm` finalizer delegating to controller | `MyForm.cpp` | same | Medium (duplication hazard) |
| 935–1169 | `MyForm_Load` | Full startup sequence (see §6.1 for sub-blocks) | A + C + B | `application/` (orchestration) + `ui/` (presentation) + `system/` + `events/` (registration) | `ApplicationController::Start` + `MyForm.cpp` + `SingleInstance` + `NotificationRegistrar` | everything | **High** (startup ordering) |
| 1171–1183 | `ListenForWakeupSignal` | Background thread: wait on wake event → `Invoke` bring-to-front | A (single-instance) + C | `ui/` (needs `Invoke`) | `MyForm.cpp` | kernel32, `BringWindowToFrontDelegate` | Medium (thread + handle lifetime) |
| 1185–1193 | `BringWindowToFrontDelegate` | Show/activate/refresh the form on the UI thread | C | `ui/` | `MyForm.cpp` | — | Low |
| 1195–1212 | `MyForm_FormClosing` | Hide-on-close; one-time "background service active" notice | C | `ui/` | `MyForm.cpp` | `isBackgroundMode` | Low |

### 3.4 Managed class — configuration (MyForm.h:682–785)

| Current Location | Function | Responsibility | Type | Future Module | Future File | Depends on | Risk |
|---|---|---|---|---|---|---|---|
| 682–690 | `GetConfigFilePath` | `%APPDATA%\Windows Hello Fix\config.txt` (creates dir) | B (config) | `config/` | `ConfigPaths.h/.cpp` | `Environment`, `Path` | Low |
| 692–705 | `GetDiagnosticLogFilePath` | `%APPDATA%\Windows Hello Fix\diagnostic.log` (creates dir) | B (config) | `config/` | `ConfigPaths.h/.cpp` | `GetConfigFilePath` | Low |
| 707–728 | `WriteDiagnosticLog` | Append `timestamp \| Event=.. \| Target=.. \| Verify=PASS/FAIL`, `Monitor`-locked | B (config/logging) | `config/` | `ConfigStore.h/.cpp` | `GetDiagnosticLogFilePath` | Low |
| 730–737 | `WriteDiagnosticLogWithDevice` | Wraps the above with `Device=` suffix | B (config/logging) | `config/` | `ConfigStore.h/.cpp` | `WriteDiagnosticLog` | Low |
| 739–748 | `SaveConfigState` | Write `monitoring=0\|1` + `device=` (overwrite) | B (config) | `config/` | `ConfigStore.h/.cpp` | `GetConfigFilePath` | Low |
| 750–775 | `LoadConfigState` | Read two lines; `monitoring=1` gate; sanitize `device=` via `TrimTrailingChars` | B (config) | `config/` | `ConfigStore.h/.cpp` | `GetConfigFilePath`, `utilities/StringHelpers` | Low (encoding note: managed UTF-8 impl kept for parity) |
| 777–785 | `EnsureConfigFileExists` | Create default config if absent | B (config) | `config/` | `ConfigStore.h/.cpp` | `SaveConfigState` | Low |

### 3.5 Managed class — target resolution & command parsing (MyForm.h:787–870)

| Current Location | Function | Responsibility | Type | Future Module | Future File | Depends on | Risk |
|---|---|---|---|---|---|---|---|
| 787–823 | `TryGetTargetCameraInstanceId` | Target order: selection → saved config → first `MI_00` device → first device | A (policy) | `application/` | `ApplicationController.h/.cpp` (or `camera/TargetResolver` for the pure heuristic) | `LoadConfigState`, `ScanSystemCameras` | Medium (selection order is the contract) |
| 827–838 | `IsRestoreCameraCommand` | Recognize `/restore-camera`, `/enable-camera`, `--enable-camera`, `/repair-camera` | A (command line) | `application/` | `CommandLine.h/.cpp` | — | Low |
| 840–849 | `IsDisableCameraCommand` | Recognize `/disable-camera`, `--disable-camera` | A (command line) | `application/` | `CommandLine.h/.cpp` | — | Low |
| 851–870 | `RestoreConfiguredCameraHardware` | Enable saved device from config; fall back to `RestoreAllCameraHardware` | A (orchestration) | `application/` | `ApplicationController.h/.cpp` | `LoadConfigState`, `RecoverCameraHardware`, `RestoreAllCameraHardware` | Medium (fallback chain) |

### 3.6 Managed class — UI construction & interaction (MyForm.h:872–933, 1214–1244)

| Current Location | Function | Responsibility | Type | Future Module | Future File | Depends on | Risk |
|---|---|---|---|---|---|---|---|
| 872–933 | `InitializeComponent` | Build controls, wire events, load icon via `LoadImage(IDI_ICON1)` | C | `ui/` | `MyForm.cpp` | WinForms, `LoadImage` | Low (icon ID mismatch is a known resource bug, not touched here) |
| 1214–1244 | `btnToggle_Click` | Start/stop monitoring; **mixes** UI state + policy + config save + hardware enable | C with heavy A/B coupling | `ui/` (delegates to controller) | `MyForm.cpp` → `ApplicationController` | `SaveConfigState`, `EnableTargetCameraHardware`, UI controls | Medium (coupling to break) |

### 3.7 WndProc (MyForm.h:1246–1349) — the second monolith inside the monolith

WndProc is a mix of **event decoding**, **dedup**, **policy**, and **hardware
calls**. The classification below splits it into the sub-blocks a future
`events/` + `application/` split will use.

| Current Location | Block | Responsibility | Type | Future Module | Future File | Depends on | Risk |
|---|---|---|---|---|---|---|---|
| 1247–1252 | Static locals `isAlreadyDisabled`, `lastSessionEventTick/Code`, `lastPowerEventTick/Code`; `nowTick` | Hardware-state lock + 1500 ms dedup state | B (safety) | `events/` (`EventCooldown`) + `application/` (lock ownership) | `EventCooldown.h/.cpp` + `ApplicationController` | kernel32 `GetTickCount64` | Medium (debounce contract) |
| 1255–1263 | `WM_QUERYENDSESSION`/`WM_ENDSESSION` handling | Set `isSystemEnding`; disable if monitoring; unregister WTS | A (policy) | `application/` (decision) + `events/` (decode) | `WinEventDecoder::SystemEnding` + `ApplicationController::HandleSystemEnd` | `DisableTargetCameraHardware`, WTS | **High** (shutdown path) |
| 1266–1275 | `WM_POWERBROADCAST` dedup + code capture | 1500 ms dedup of power events | B (safety) | `events/` | `EventCooldown` | — | Medium |
| 1278–1303 | Suspend (`0x0004`) / power-setting (`0x8013`) handling | GUID filter (lid/button vs irrelevant); `isAlreadyDisabled` lock; disable + 500 ms window | A (policy) + B (decode) | `events/` (GUID filter) + `application/` (policy) | `WinEventDecoder` + `ApplicationController` | `DisableTargetCameraHardware`, `IsEqualGUID` | **High** (lid/button timing) |
| 1306–1313 | Resume (`0x0007`/`0x0012`) handling | 1000 ms delay; enable; release lock | A (policy) | `application/` | `ApplicationController` | `EnableTargetCameraHardware` | **High** (device-tree rebuild timing) |
| 1318–1346 | `WM_WTSSESSION_CHANGE` handling | Log code; dedup; monitoring-off gate; lock→disable; unlock→enable | A (policy) + B (decode) | `events/` (decode) + `application/` (policy) | `WinEventDecoder::SessionLock/Unlock` + `ApplicationController` | `DisableTargetCameraHardware`, `EnableTargetCameraHardware` | **High** (core feature path) |

---

## 4. MyForm_Load Sub-Block Classification (MyForm.h:935–1169)

| Lines | Block | Responsibility | Type | Future Module | Future File |
|---|---|---|---|---|---|
| 936–945 | Arg scan for `/background` | Detect background launch (third arg-parse site in the app) | A | `application/` | `CommandLine.h/.cpp` |
| 947–959 | `Startup_Context` log | Elevation, integrity, exe, cwd, config path | A | `application/` | `ApplicationController::Start` |
| 961–969 | Restore command mode | Hide form, restore configured camera (cycle), exit | A | `application/` | `ApplicationController` + `CommandLine` |
| 971–981 | Disable command mode | Hide form, disable + verify target, exit | A | `application/` | `ApplicationController` + `CommandLine` |
| 984–1001 | Single-instance mutex + wake path | `CreateMutex(Global\...)`; wake existing instance via `SetEvent`, exit | A + B | `system/` | `SingleInstance.h/.cpp` |
| 1003–1008 | Background quiet exit | No wake event + background launch → silent exit | A | `application/` | `ApplicationController` |
| 1010–1050 | **Ghost-mutex force-reset** | Prompt; recover camera from config (or `MI_00`); `SaveConfigState(true)`; `taskkill`; restart | A (self-healing) + B | `application/` + `system/` | `ApplicationController` + `ProcessUtils.h/.cpp` (`taskkill` helper) |
| 1053 | Wake event creation | `CreateEvent(Global\WindowsHelloFix_WakeupEvent)` | B | `system/` | `SingleInstance.h/.cpp` |
| 1055–1058 | Startup recovery | `RestoreConfiguredCameraHardware(true)` before dropdown build | A | `application/` | `ApplicationController` |
| 1060–1066 | Power notification registration | `RegisterPowerSettingNotification` (lid + button GUIDs) | B | `events/` | `NotificationRegistrar.h/.cpp` |
| 1068–1111 | Scan + dropdown population + selection | Fill dropdown; select saved → `MI_00` → first; sync `selectedInstanceId`; `EnsureConfigFileExists` | C + B | `ui/` (dropdown) + `camera/` (scan) | `MyForm.cpp` + `CameraDevice` |
| 1113–1116 | Force-enable target | First hardware action after startup | A | `application/` | `ApplicationController` |
| 1118–1142 | Auto-start vs stopped state | `isMonitoring` decision; UI labels; background hiding | A + C | `application/` + `ui/` | `ApplicationController` + `MyForm.cpp` |
| 1144–1146 | Wake-up listener thread | `backgroundWorker` start | A | `ui/` | `MyForm.cpp` |
| 1148–1168 | WTS registration with 6×500 ms retry | Session notification; success/failure log | B | `events/` | `NotificationRegistrar.h/.cpp` |

---

## 5. Safety / Fail-Safe Mechanisms (first-class inventory)

Every safety mechanism in the application, its protector, its current
implementation, its future owner, and its dependencies. This is the reference
for the `safety` concern — see `module-catalog.md` for why these are owned by
their domain modules rather than a standalone `safety/` directory.

| # | Mechanism | Protects against | Current implementation | Future owner | Depends on |
|---|---|---|---|---|---|
| SF-1 | Exact + case-insensitive instance-ID matching | Disabling the wrong camera | `ToggleCameraHardware:331`, `LocateCameraDevInst:385`, `GetCameraHardwareDisabledState:435` | `camera/CameraHardware` | setupapi |
| SF-2 | Class filter (`Camera`/`Image` only) | Targeting non-camera devices | `ScanSystemCameras:288` | `camera/CameraDevice` | setupapi |
| SF-3 | `MI_00` target heuristic | Auto-selecting the wrong sensor (prefer IR-conflict device) | `TryGetTargetCameraInstanceId:811`, `MyForm_Load:1089`, ghost-reset `:1029` | `camera/CameraDevice` (heuristic) + `application/` (ordering) | scan |
| SF-4 | Selection-order contract | Target drift (selection → config → `MI_00` → first) | `TryGetTargetCameraInstanceId:787–823` | `application/ApplicationController` | config, camera |
| SF-5 | Check-before-change | Hardware churn when state already matches | `DisableTargetCameraHardware:118`, `EnableTargetCameraHardware:154`, `SetCameraHardwareStateVerified:512` | `application/` (skip) + `camera/` (verify) | camera state query |
| SF-6 | Verification (3 × 100 ms) | False-positive state changes | `VerifyCameraHardwareState:466–477` | `camera/CameraHardware` | cfgmgr32 |
| SF-7 | Retry loop (3 attempts, SetupAPI → CFGMGR32 → reinit) | Flaky PnP state transitions | `SetCameraHardwareStateVerified:516–545` | `camera/CameraRecovery` | hardware ops |
| SF-8 | Reinit-on-mismatch (+250 ms) | Stuck device node | `SetCameraHardwareStateVerified:530–535` | `camera/CameraRecovery` | hardware ops |
| SF-9 | `isAlreadyDisabled` hardware-state lock | Double-disable on overlapping suspend/lid events | `WndProc:1247, 1297, 1312` | `application/ApplicationController` | policy |
| SF-10 | 1500 ms event dedup | Duplicate lock/unlock/power spam | `WndProc:1269, 1327` | `events/EventCooldown` | kernel32 |
| SF-11 | 500 ms post-disable safety window | Suspend race after disable | `WndProc:1302` | `application/ApplicationController` | — |
| SF-12 | 1000 ms resume delay | Device tree not yet rebuilt on wake | `WndProc:1309` | `application/ApplicationController` | — |
| SF-13 | Recovery cycle timings (350/900/500 ms) | Half-recovered device | `RecoverCameraHardware:555–562` | `camera/CameraRecovery` | hardware ops |
| SF-14 | Single-instance mutex (`Global\WindowsHelloFix_AppMutex`) | Two monitoring processes | `MyForm_Load:984` | `system/SingleInstance` | kernel32 |
| SF-15 | Wake-event signaling | Second instance silently ignored | `MyForm_Load:987–993`, `ListenForWakeupSignal:1173` | `system/SingleInstance` + `ui/` | kernel32 |
| SF-16 | Ghost-process force-reset | Frozen instance bricking the camera | `MyForm_Load:1010–1050` | `application/` + `system/ProcessUtils` | process, camera, config |
| SF-17 | Shutdown dual-path (system-ending vs normal) | Camera left disabled (or enabled) wrongly | `~MyForm:604–622`, `!MyForm:653–658`, `WndProc:1255–1263` | `application/ApplicationController::Shutdown` | camera, config |
| SF-18 | `TrimTrailingChars` config sanitization | `\r\n` corruption breaking device matching | `LoadConfigState:767` | `utilities/StringHelpers` (done) + `config/ConfigStore` | — |
| SF-19 | `catch (...) {}` guards around I/O | Crash on config/log I/O failure | `WriteDiagnosticLog`, `SaveConfigState`, `LoadConfigState`, `RestoreConfiguredCameraHardware`, `InitializeComponent` | retained at the managed boundary (`config/` + `application/`) | — |
| SF-20 | Target-empty guards | Hardware call with no target | `SetCameraHardwareStateVerified:505`, `RecoverCameraHardware:549`, `Disable/EnableTargetCameraHardware` | `camera/CameraRecovery` + `application/` | — |

---

## 6. Global State & Ownership Map

| State | Current owner | Written by | Read by | Lifetime | Future owner |
|---|---|---|---|---|---|
| `cachedCameras` (`void*` → `std::vector<CameraDeviceInfo>*`) | MyForm | `MyForm_Load:1075`, `btnToggle_Click` | dropdown fill, selection sync | ctor → dtor/finalizer | `camera/CameraDevice::ScanResult` returned by value; no stored native buffer |
| `selectedInstanceId` (`void*` → `std::wstring*`) | MyForm | `MyForm_Load`, `btnToggle_Click` | policy methods, dtor | ctor → dtor/finalizer | `application/ApplicationController` (target state) |
| `isMonitoring` | MyForm | ctor, `MyForm_Load`, `btnToggle_Click` | `WndProc`, dtor policy | app lifetime | `application/ApplicationController` |
| `isBackgroundMode` | MyForm | ctor, `MyForm_Load`, `FormClosing` | `FormClosing` | app lifetime | `ui/MyForm` |
| `isSystemEnding` | MyForm | ctor, `WndProc:1256` | dtor, finalizer | app lifetime | `application/ApplicationController` |
| `cameraExpectedDisabled` | MyForm | policy methods | **unused (diagnostic intent)** | app lifetime | retire or fold into controller state |
| `cameraStateInitialized`, `restartQueuedByMismatch`, `lastCameraToggleTick`, `lastToggleTime`, `COOLDOWN_MILLISECONDS` | MyForm | ctor (never meaningfully) | **never read** | — | retire (Phase 11) |
| `hAppMutex` | MyForm | `MyForm_Load:984` | dtor cleanup | app lifetime | `system/SingleInstance` (RAII) |
| `hWakeupEvent` | MyForm | `MyForm_Load:1053` | `ListenForWakeupSignal`, dtor | app lifetime | `system/SingleInstance` (RAII) |
| `hLidNotification`, `hButtonNotification` | MyForm | `MyForm_Load:1065–1066` | dtor/finalizer unregister | app lifetime | `events/NotificationRegistrar` (RAII) |
| `backgroundWorker`, `keepListening` | MyForm | ctor, `MyForm_Load:1144` | wake listener | app lifetime | `ui/MyForm` |
| `diagnosticLogSync` | MyForm | ctor | `WriteDiagnosticLog` | app lifetime | `config/ConfigStore` (lock internal) |
| WndProc statics (`isAlreadyDisabled`, `last*EventTick/Code`) | `WndProc` function statics | `WndProc` | `WndProc` | process lifetime | `events/EventCooldown` + `application/` lock |
| `g_last*` globals | file scope | camera functions | policy methods (log) | process lifetime | `camera/DeviceError` result struct |
| Controls (`deviceDrop`, `btnToggle`, `lblTitle`, `lblStatus`, `components`) | MyForm | `InitializeComponent` | UI handlers | form lifetime | `ui/MyForm` |

---

## 7. Event Sources Map

| Event source | Raw message/trigger | Current handler | Current processing | Business logic invoked | Hardware/device effect | Future home |
|---|---|---|---|---|---|---|
| System shutdown/logoff | `WM_QUERYENDSESSION` (0x0011), `WM_ENDSESSION` (0x0016) | `WndProc:1255–1263` | mark `isSystemEnding`, disable if monitoring, unregister WTS | `DisableTargetCameraHardware(true)` | RGB camera disabled for next boot | `events/WinEventDecoder` (decode) + `application/ApplicationController` (policy) |
| Sleep | `WM_POWERBROADCAST` (0x0218) `PBT_APMSUSPEND` (0x0004) | `WndProc:1278–1303` | dedup, `isAlreadyDisabled` lock, disable, 500 ms window | `DisableTargetCameraHardware(true)` | camera disabled during sleep | same |
| Lid close / power button | `WM_POWERBROADCAST` (0x0218) `PBT_POWERSETTINGCHANGE` (0x8013) + GUID | `WndProc:1281–1294` | GUID filter (lid/button vs irrelevant), then disable path | `DisableTargetCameraHardware(true)` | camera disabled | `events/WinEventDecoder` (GUID filter) + `application/` |
| Resume | `WM_POWERBROADCAST` `PBT_APMRESUMESUSPEND` (0x0007) / `PBT_APMRESUMEAUTOMATIC` (0x0012) | `WndProc:1306–1313` | 1000 ms delay, enable, release lock | `EnableTargetCameraHardware(false)` | camera re-enabled | `application/` |
| Session lock/unlock | `WM_WTSSESSION_CHANGE`, `WTS_SESSION_LOCK`/`WTS_SESSION_UNLOCK` | `WndProc:1318–1346` | log code, dedup, monitoring gate, disable/enable | `DisableTargetCameraHardware(true)` / `EnableTargetCameraHardware(false)` | camera disabled on lock, enabled on unlock | `events/` + `application/` |
| Power notification registration | `RegisterPowerSettingNotification` (lid + button GUIDs) | `MyForm_Load:1060–1066` | register on the form HWND | — | — | `events/NotificationRegistrar` |
| Session notification registration | `WTSRegisterSessionNotification` (6 × 500 ms retry) | `MyForm_Load:1148–1168` | retry until success or fail log | — | — | `events/NotificationRegistrar` |
| Cross-process wake | named event `Global\WindowsHelloFix_WakeupEvent` | `ListenForWakeupSignal:1171–1183` | wait → `Invoke` bring-to-front | — | — | `system/SingleInstance` (signal) + `ui/` (present) |
| UI events | `Load`, `FormClosing`, `Click` | `MyForm_Load`, `MyForm_FormClosing`, `btnToggle_Click` | startup, hide-on-close, start/stop | policy + config + hardware via methods | depends on action | `ui/MyForm` (delegating) |
| Application end | destructor/finalizer | `~MyForm`/`!MyForm` | shutdown policy + handle release | disable/enable per `isSystemEnding` | depends on path | `application/` (policy) + RAII owners |

---

## 8. Couplings That the Target Architecture Must Eliminate

| # | Coupling | Where today | Target fix |
|---|---|---|---|
| 1 | **UI → hardware** | `btnToggle_Click:1234`, `WndProc` policy blocks call `Disable/EnableTargetCameraHardware` directly | `ui/` delegates to `application/ApplicationController`; Rule 1 |
| 2 | **UI → config** | `btnToggle_Click:1230,1236`, `MyForm_Load` config calls, `FormClosing` | config routed through `application/`; `config/` owns storage |
| 3 | **Events → hardware** | `WndProc` decodes and performs hardware in one function | `events/` decodes only; `application/` decides; `camera/` executes |
| 4 | **Events → policy** | `WndProc` carries `isMonitoring`, `isAlreadyDisabled` | policy moves to `application/ApplicationController` |
| 5 | **Policy → globals** | `Disable/EnableTargetCameraHardware` read `g_last*` for logs | `camera/` returns `DeviceError`; controller logs it |
| 6 | **Policy → privilege internals** | same methods call `IsCurrentProcessElevatedNative`/`GetCurrentProcessIntegrityRid` inline | `system/PrivilegeInfo` queried once at startup, passed along |
| 7 | **Destructor duplication** | `~MyForm` and `!MyForm` duplicate cleanup | single `ApplicationController::Shutdown` + RAII owners |
| 8 | **Three arg-parse sites** | `main.cpp:16–25`, `MyForm_Load:939–945`, `IsRestore/IsDisableCameraCommand` | one `application/CommandLine` |
| 9 | **Native buffers as `void*`** | `cachedCameras`, `selectedInstanceId` cast in every use | native types owned inside modules; marshaling only at the `ui/` boundary |

---

## 9. main.cpp Classification

| Location | Item | Responsibility | Type | Future Module | Future File |
|---|---|---|---|---|---|
| 10–11 | `EnableVisualStyles` / `SetCompatibleTextRenderingDefault` | WinForms bootstrap | C | `ui/` | `src/main.cpp` |
| 13 | `MyForm form;` | Form creation | C | `ui/` | `src/main.cpp` |
| 16–25 | Arg scan for hidden launch | Second arg-parse site (background/command detection for `Opacity=0` + hidden) | A | `application/` | `CommandLine.h/.cpp` |
| 27–32 | Hidden-window setup | `Opacity=0`, no taskbar, minimized | C | `ui/` | `src/main.cpp` |
| 34 | `Application::Run` | Message loop | A | `ui/` | `src/main.cpp` |

---

## 10. ProductionUtilities.h (dead code) Disposition

| Class | Status | Future |
|---|---|---|
| `ProductionLogger` | **Adopted** into `src/utilities/Logging` (already extracted, Phase 2) | `utilities/Logging.h/.cpp` |
| `HardwareOperationQueue` | Dead (async design; behavioral change) | retire — see `target-architecture.md` "Not in v2.1" |
| `SingleInstanceManager` | Dead; live code uses `Global\` mutex directly | reimplement live semantics in `system/SingleInstance` |
| `ElevationChecker` | Dead; duplicates live native functions | merge into `system/PrivilegeInfo` |
| `EnhancedSetupAPI` | Dead; live code implements the same fallback strategy | retire |
| `ShutdownManager` | Dead | retire |

---

## 11. Resource / Build Files (classified, not to be migrated now)

| File | Role | Future |
|---|---|---|
| `resource.h` (`IDI_ICON1`=102) | IDs for the **orphan** `.rc` | consolidate with `resource1.h` (Phase 11) |
| `resource1.h` (`IDI_ICON1`=114) | IDs for the **compiled** `.rc` | consolidate (Phase 11) |
| `Windows_Hello_Fix_v2_0.rc` | Orphan (not compiled; absolute paths) | decide fold/delete (Phase 11) |
| `Windows_Hello_Fix_v2_0_resources.rc` | Compiled resource script (icon from build output) | keep + relative path fix (Phase 11) |
| `app.manifest` | `requireAdministrator` | kept; identity rename in Phase 12 |
| `.sln` / `.vcxproj` / `.vcxproj.filters` | build | primary build system; gains `src/` items during migrations |

---

## Related Documents

- `migration-map.md` — function-group-level mapping + phase numbers.
- `target-architecture.md` — module contracts.
- `myform-decomposition.md` — how the 1,354-line header is dismantled.
- `module-catalog.md` — per-module purpose/interface/difficulty.
- `dependency-map.md` — allowed dependencies.
- `data-flow.md` — runtime flows and timing contract.