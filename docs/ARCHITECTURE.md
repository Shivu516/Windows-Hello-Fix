# Windows Hello Fix v2.0 — Architecture

> Documentation-only. Describes the **current** extracted source under `src/ui/`. No code was modified.

## Reference implementation relationship

```
release-v2.0/MyForm.h  (1362-line monolith, known-good)
        │  mechanical extraction
        ▼
src/ui/MyForm.h            declaration only (class, struct, globals `extern`)
src/ui/MyForm_Camera.cpp   native camera pipeline + Disable/Enable/Restore members
src/ui/MyForm_Config.cpp   config + diagnostic logging + target resolution
src/ui/MyForm_Core.cpp     ctor / dtor / finalizer / InitializeComponent / MyForm_Load
src/ui/MyForm_Events.cpp   WndProc (session / power / shutdown)
src/ui/MyForm_System.cpp   command parsing + wake listener
src/ui/MyForm_UI.cpp       FormClosing + btnToggle_Click
```

The extraction kept **`MyForm` as the single, central state owner**. No `ApplicationController`, `CameraController`, `EventController`, or `RecoveryController` was introduced. Only member-function *bodies* moved into separate translation units; the class, its members, its lifetime, and its behavioral authority are unchanged from `release-v2.0/MyForm.h`.

The root `MyForm.h` is a 3-line shim:
```cpp
#pragma once
#include "src/ui/MyForm.h"
```
This preserves the original `#include "MyForm.h"` path used by `main.cpp` without altering it.

## High-level architecture

```mermaid
flowchart TD
    A[main.cpp] --> B[MyForm]
    B --> C[MyForm_Core.cpp<br/>ctor/dtor/Load]
    B --> D[MyForm_Camera.cpp<br/>hardware pipeline]
    B --> E[MyForm_Config.cpp<br/>config + logging]
    B --> F[MyForm_Events.cpp<br/>WndProc]
    B --> G[MyForm_System.cpp<br/>commands + wake]
    B --> H[MyForm_UI.cpp<br/>UI handlers]

    C -->|startup| D
    C -->|startup| E
    C -->|startup| G
    F -->|lock/sleep| D
    F -->|unlock/resume| D
    H -->|stop| D
    G -->|wake signal| B
```

## Runtime lifecycle

```mermaid
flowchart TD
    A[Process start] --> B[main.cpp: MyForm form]
    B --> C[Application::Run]
    C --> D[MyForm ctor → InitializeComponent]
    D --> E[MyForm_Load]
    E --> F[Single-instance mutex]
    F --> G[Restore camera]
    G --> H[Register power + WTS]
    H --> I[Populate dropdown / auto-start]
    I --> J[Start wake listener thread]
    J --> K[Steady state: monitoring]
    K --> L[Lock / Sleep → disable camera]
    K --> M[Unlock / Resume → enable camera]
    K --> N[Shutdown → isSystemEnding → dtor]
```

## State ownership

| State | Owner |
|---|---|
| `isMonitoring` | `MyForm` |
| `isBackgroundMode`, `isSystemEnding` | `MyForm` |
| `cachedCameras`, `selectedInstanceId` (raw pointers) | `MyForm` (manual new/delete) |
| `cameraExpectedDisabled`, `cameraStateInitialized`, `restartQueuedByMismatch` | `MyForm` |
| `hAppMutex`, `hWakeupEvent` | `MyForm` |
| `hLidNotification`, `hButtonNotification` | `MyForm` |
| `backgroundWorker`, `keepListening` | `MyForm` |
| `deviceDrop`, `btnToggle`, `lblTitle`, `lblStatus`, `components` | `MyForm` (WinForms) |
| `diagnosticLogSync` | `MyForm` |
| `g_lastHardwareToggleTick`, `g_lastSetupApiError`, `g_lastConfigManagerResult`, `g_lastHardwareToggleStage` | Defined once in `MyForm_Camera.cpp`, `extern` elsewhere (single authoritative instance) |
| `config.txt`, `diagnostic.log` | Filesystem under `%APPDATA%\Windows Hello Fix` |

## Threading model

1. **UI thread** — message pump, `MyForm_Load`, `WndProc`, `btnToggle_Click`, `MyForm_FormClosing`, all camera members when invoked from UI/Load/WndProc.
2. **Background wake listener** (`backgroundWorker`, `IsBackground=true`) — runs `ListenForWakeupSignal`, blocks on `WaitForSingleObject(hWakeupEvent)`. Only updates the window via `Invoke`; never touches camera hardware.

No thread pool, no task queue, no async camera operations. This matches the original v2.0 threading model.

## Dependency map

```mermaid
flowchart LR
    MFH[MyForm.h] --> CAM[MyForm_Camera.cpp]
    MFH --> CFG[MyForm_Config.cpp]
    MFH --> CORE[MyForm_Core.cpp]
    MFH --> EVT[MyForm_Events.cpp]
    MFH --> SYS[MyForm_System.cpp]
    MFH --> UI[MyForm_UI.cpp]
    CAM --> CFG[TrimTrailingChars, ScanSystemCameras used by Config]
    CORE --> CAM
    CORE --> CFG
    CORE --> SYS
    EVT --> CAM
    EVT --> CFG
    UI --> CAM
    UI --> CFG
```

`MyForm.h` is the hub; every `.cpp` includes it. `MyForm_Camera.cpp` defines the shared globals and is the only place that calls SetupAPI/CfgMgr.

## Camera operation flow (high level)

```mermaid
flowchart TD
    A[Event / command / UI] --> B[TryGetTargetCameraInstanceId]
    B --> C[GetCameraHardwareDisabledState]
    C --> D{Already in target state?}
    D -->|yes| Z[Return true]
    D -->|no| E[ToggleCameraHardware - SetupAPI]
    E --> F[VerifyCameraHardwareState]
    F -->|ok| Z
    F -->|fail| G[ToggleCameraHardwareCfgMgr]
    G --> H[Verify]
    H -->|ok| Z
    H -->|fail| I{reinitializeOnMismatch?}
    I -->|yes| J[Revert toggle + Sleep 250]
    J --> E
    I -->|no| K[Retry x3 then final attempt]
```

## Windows APIs used (summary)

- **SetupAPI:** `SetupDiGetClassDevs`, `SetupDiEnumDeviceInfo`, `SetupDiGetDeviceInstanceId`, `SetupDiGetDeviceRegistryProperty`, `SetupDiSetClassInstallParams`, `SetupDiCallClassInstaller`, `SetupDiDestroyDeviceInfoList`.
- **Configuration Manager:** `CM_Get_DevNode_Status`, `CM_Enable_DevNode`, `CM_Disable_DevNode`, `CM_Reenumerate_DevNode`.
- **WTS:** `WTSRegisterSessionNotification`, `WTSUnRegisterSessionNotification`.
- **Power:** `RegisterPowerSettingNotification`, `UnregisterPowerSettingNotification`.
- **Sync/IPC:** `CreateMutex`, `OpenEvent`, `CreateEvent`, `SetEvent`, `WaitForSingleObject`, `GetTickCount64`, `Interlocked*`, `Sleep`.
- **Process:** `CreateProcess` family not used directly; `system("taskkill ...")` only in the ghost-reset path.
- **Token:** `OpenProcessToken`, `GetTokenInformation` (elevation/integrity, logging only).
- **WinForms:** `Application::Run`, `MessageBox`, `StreamWriter`/`StreamReader`, `Monitor`, `Invoke`.

See `docs/CAMERA_FLOW.md`, `docs/EVENT_FLOW.md`, `docs/LIFECYCLE.md`, `docs/DEBUGGING.md`, and `docs/KNOWN_ISSUES.md` for detail.
