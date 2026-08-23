# Module: `src/camera/` — native hardware control

Pure native C++ (no managed types). Free functions only; no logging of its
own. Full pipeline documentation: `../CAMERA_HARDWARE.md`.

---

## DeviceError.h

| Field | Content |
|---|---|
| Purpose | Declares the three global diagnostic slots shared between camera files and managed log formatting |
| Contents | `extern volatile LONG g_lastSetupApiError; g_lastConfigManagerResult; g_lastHardwareToggleStage;` and struct `DeviceError {setupErr; configRet; stage;}` |
| Note | The `DeviceError` struct itself is **never instantiated** anywhere — dead declaration kept for a planned refactor |

## CameraDevice.h / CameraDevice.cpp

| Field | Content |
|---|---|
| Purpose | Camera discovery + owns the diagnostic globals' storage |
| Types | `struct CameraDeviceInfo { std::wstring friendlyName; std::wstring instanceId; }` |
| Functions | `ScanSystemCameras()` |
| Inputs | Live device tree |
| Outputs | Vector of present `Camera`/`Image` class devices |
| Side effects | Defines/resets `g_last*` globals; destroys device info list |
| Windows APIs | `SetupDiGetClassDevs(DIGCF_ALLCLASSES\|DIGCF_PRESENT)`, `SetupDiEnumDeviceInfo`, `SetupDiGetDeviceRegistryProperty(SPDRP_CLASS, SPDRP_DEVICEDESC)`, `SetupDiGetDeviceInstanceId`, `SetupDiDestroyDeviceInfoList` |
| Error handling | Enumeration failure → empty vector, silent |
| Callers | ApplicationController (target fallback, ghost reset), CameraRecovery (`RestoreAllCameraHardware`), MyForm_Load (dropdown population) |
| Threading | Called on UI thread only today; function itself is stateless/thread-safe except for the global slots defined here |

Execution: enumerate all present devices → keep class `Camera`/`Image` →
capture instance id + description. No filtering by interface substring here;
the `MI_00` heuristic lives in callers.

## CameraHardware.h / CameraHardware.cpp

| Field | Content |
|---|---|
| Purpose | Low-level toggle, locate, state query, verification against SetupAPI/Configuration Manager |
| Functions | `ToggleCameraHardware(id, enable)`; `LocateCameraDevInst(id, DEVINST&)`; `ToggleCameraHardwareCfgMgr(id, enable)`; `GetCameraHardwareDisabledState(id, bool&)`; `VerifyCameraHardwareState(id, shouldBeDisabled)` |
| Windows APIs | SetupDi* family incl. `SetupDiSetClassInstallParams` + `SetupDiCallClassInstaller(DIF_PROPERTYCHANGE)` with `DICS_ENABLE/DICS_DISABLE`, `DICS_FLAG_GLOBAL`; `CM_Enable_DevNode`, `CM_Disable_DevNode(CM_DISABLE_UI_NOT_OK)`, `CM_Reenumerate_DevNode`, `CM_Get_DevNode_Status`; property `SPDRP_CONFIGFLAGS` |
| State written | Stage markers 10–15 (SetupAPI path), 20–23 (CfgMgr path); last Win32 error; last CONFIGRET |
| Matching | Instance id exact match **or** `_wcsicmp` case-insensitive fallback |
| Disabled detection | `problem == CM_PROB_DISABLED (22)` OR `CONFIGFLAG_DISABLED (0x1)` bit in SPDRP_CONFIGFLAGS |
| Verification | ≤3 attempts × 100 ms sleep comparing reported vs expected disabled state |
| Error handling | Per-stage failure capture into globals + boolean returns; CfgMgr re-enumeration result ignored by design |
| Threading | Synchronous; UI thread in current call sites |

## CameraRecovery.h / CameraRecovery.cpp

| Field | Content |
|---|---|
| Purpose | Retry/recovery orchestration above raw toggles |
| Functions | `SetCameraHardwareStateVerified(id, enable, reinitializeOnMismatch)`; `RecoverCameraHardware(id, cycleDevice)`; `RestoreAllCameraHardware(cycleDevices)` |
| Execution | See CAMERA_HARDWARE.md §7 verbatim: check-before-change → up to 3 × {SetupAPI toggle → verify → CfgMgr toggle → verify → optional opposite-toggle reinit + 250 ms} → final attempt; recover adds fixed sleeps 350/900/500 with redundant second enable when cycling; restore-all iterates every scanned camera |
| Timing side effects | Multiple blocking `Sleep`s; total worst case several seconds per call |
| Error handling | Boolean composition only (`restored = X || restored`) |
| Callers | ApplicationController wrappers, ghost-reset path, installer-invoked `/restore-camera` command path |
