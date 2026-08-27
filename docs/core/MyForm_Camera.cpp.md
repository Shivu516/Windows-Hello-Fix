# `src/core/MyForm_Camera.cpp` — Camera Discovery, Toggle & Recovery

**Path:** `src/core/MyForm_Camera.cpp`
**Lines:** 470
**Included by:** built directly (translation unit); `#include "MyForm.h"`
**Build relationship:** `ClCompile` in vcxproj, filter `Source Files\src\core`.

## Purpose

Owns the entire **native camera hardware pipeline** plus the `MyForm` member functions `DisableTargetCameraHardware`, `EnableTargetCameraHardware`, and `RestoreConfiguredCameraHardware`. It also defines the four shared `g_last*` globals (lines 9–12) so they have a single authoritative definition.

## Global state (lines 9–12)

```cpp
volatile LONG64 g_lastHardwareToggleTick = 0;
volatile LONG  g_lastSetupApiError = ERROR_SUCCESS;
volatile LONG  g_lastConfigManagerResult = CR_SUCCESS;
volatile LONG  g_lastHardwareToggleStage = 0;
```
These are the *only* definitions. `TryEnterHardwareToggleCooldown` and `RecordHardwareToggleTime` use the tick; the toggle/verify functions record stage/error codes for diagnostic logging. They are written with `InterlockedExchange`/`InterlockedCompareExchange` so concurrent access does not corrupt them (though in practice all hardware calls happen on the UI thread or the wake thread, never truly concurrently).

## Native helper functions

### `TrimTrailingChars` (16–23)
Strips trailing `\r`, `\n`, and spaces from a `std::wstring`. Used to sanitize config-file device-instance strings that may contain CRLF. Pure, no side effects.

### `IsCurrentProcessElevatedNative` (25–37)
Calls `OpenProcessToken` + `GetTokenInformation(TokenElevation)`. Returns `true` if the process token is elevated. Used only for diagnostic logging (not for control flow).

### `GetCurrentProcessIntegrityRid` (39–67)
Opens the process token, queries `TokenIntegrityLevel`, allocates a `TOKEN_MANDATORY_LABEL`, and reads the lowest sub-authority of the integrity SID. Returns the Integrity Level RID for logging. Returns `0` on any failure.

## Camera discovery

### `ScanSystemCameras` (69–103)
- `SetupDiGetClassDevs(NULL, NULL, NULL, DIGCF_ALLCLASSES | DIGCF_PRESENT)` enumerates **present** devices.
- For each device, reads `SPDRP_CLASS`. If it equals `L"Camera"` or `L"Image"`, it reads the instance ID (`SetupDiGetDeviceInstanceId`) and description (`SPDRP_DEVICEDESC`) and pushes a `CameraDeviceInfo`.
- Returns the vector (empty on failure). **This is the authoritative device list** used for dropdown population and "restore all".

## Toggle (disable/enable) operations

### `ToggleCameraHardware` (107–166) — SetupAPI path
1. Resets `g_lastSetupApiError = SUCCESS`, sets stage `10`.
2. `SetupDiGetClassDevs(DIGCF_ALLCLASSES)` (note: **no** `DIGCF_PRESENT` here, so it can act on non-present nodes).
3. Iterates devices; matches target by exact `==` or case-insensitive `_wcsicmp`.
4. Builds `SP_PROPCHANGE_PARAMS` with `DIF_PROPERTYCHANGE`, `DICS_ENABLE`/`DICS_DISABLE`, `DICS_FLAG_GLOBAL`, `HwProfile=0`.
5. `SetupDiSetClassInstallParams` → stage `12` on fail; `SetupDiCallClassInstaller` → stage `13` on fail.
6. On success sets stage `14`, `changed=true`, breaks.
7. After loop, if nothing changed and stage still `10`, sets stage `15`.
8. Returns `changed`.

Stages are purely diagnostic (read then zeroed in the member logging functions).

### `LocateCameraDevInst` (168–193) — CfgMgr dev-inst lookup
Like `ToggleCameraHardware` but only resolves the matching device to a `DEVINST` via `SetupDiEnumDeviceInfo` + `SetupDiGetDeviceInstanceId`. Used by the CfgMgr fallback.

### `ToggleCameraHardwareCfgMgr` (195–215) — Configuration Manager path
1. Resets `g_lastConfigManagerResult = CR_SUCCESS`, stage `20`.
2. `LocateCameraDevInst` → stage `21` on failure.
3. `enable ? CM_Enable_DevNode : CM_Disable_DevNode(devInst, CM_DISABLE_UI_NOT_OK)` → records `cr`, stage `22` on failure.
4. `CM_Reenumerate_DevNode(devInst, 0)` → stage `23`.
5. Returns `cr == CR_SUCCESS`.

## State inspection

### `GetCameraHardwareDisabledState` (217–262)
Determines whether a device is disabled by **either**:
- Config flag: `SPDRP_CONFIGFLAGS & CONFIGFLAG_DISABLED`, or
- Problem code: `CM_Get_DevNode_Status` returns problem `CM_PROB_DISABLED`.
Sets `isDisabled` and returns `true` if the device was found. This is the ground-truth check used everywhere for verification.

### `VerifyCameraHardwareState` (264–275)
Retries up to **3 times**: calls `GetCameraHardwareDisabledState` and returns `true` if `isDisabled == shouldBeDisabled`. Sleeps **100 ms** between attempts. Total worst-case ~300 ms.

## Cooldown (declared, lightly used)

### `TryEnterHardwareToggleCooldown` (277–296)
Spins up to 8 times using `InterlockedCompareExchange64` on `g_lastHardwareToggleTick` to atomically claim a cooldown window of `cooldownMs`. Returns `false` if still within the window, `true` if it successfully stamped the tick. **Note:** in the current code path this function is *not called* by the main flow (see `docs/KNOWN_ISSUES.md`).

### `RecordHardwareToggleTime` (298–300)
Stamps `g_lastHardwareToggleTick = GetTickCount64()` (interlocked). Called by `SetCameraHardwareStateVerified` after a successful verified transition.

## Verified set + recovery

### `SetCameraHardwareStateVerified` (302–344) — core orchestrator
1. Empty target → `false`.
2. `shouldBeDisabled = !enable`.
3. **Check-before-change:** if `VerifyCameraHardwareState(target, shouldBeDisabled)` already true, return `true` (no churn).
4. Loop **3 attempts**:
   - `ToggleCameraHardware(target, enable)` (SetupAPI). If verify passes → record tick, return `true`.
   - `ToggleCameraHardwareCfgMgr(target, enable)` (CfgMgr). If verify passes → record tick, return `true`.
   - If `reinitializeOnMismatch`: toggle both paths with `!enable` then `Sleep(250)`.
   - `Sleep(250)`.
5. Final attempt outside loop: `ToggleCameraHardware` + verify; record tick only if verified; return verified.

Timing budget per call (worst case): 3 × (SetupAPI + 100×3 verify + CfgMgr + 100×3 verify + 250) + 250 ≈ up to ~2.5 s, plus reinitialize sleep.

### `RecoverCameraHardware` (346–363)
1. Empty target → `false`.
2. First ensures enabled via `SetCameraHardwareStateVerified(target, true, false)`.
3. If `cycleDevice`: `Sleep(350)` → disable (`false`) → `Sleep(900)` → enable (`false`) → `Sleep(500)` → enable again. The extra enables `||`-accumulate `restored`.
   - Total added sleeps: 350 + 900 + 500 = **1750 ms** when cycling.

### `RestoreAllCameraHardware` (365–371)
`ScanSystemCameras()` then `RecoverCameraHardware` on every camera with the given `cycleDevices` flag. Used as a fallback when no configured device is known.

## `MyForm` member camera operations

### `MyForm::DisableTargetCameraHardware(bool retryOnFailure)` (377–411)
1. `TryGetTargetCameraInstanceId(target, true)`; if none, log `…_NoTarget`, return `false`.
2. Stamp `lastToggleTime = DateTime::Now`.
3. If already disabled (via `GetCameraHardwareDisabledState`), set `cameraExpectedDisabled=true`, log `…_AlreadyDisabled`, return `true`.
4. Otherwise `SetCameraHardwareStateVerified(target, false, retryOnFailure)` and `VerifyCameraHardwareState(target, true)`; set `cameraExpectedDisabled=result`; log `…_Result` with elevated/integrity/setup/cfgmgr/stage readouts; return `result && verified`.

### `MyForm::EnableTargetCameraHardware(bool cycleDevice)` (413–447)
Symmetric to disable but uses `RecoverCameraHardware(target, cycleDevice)` and `cameraExpectedDisabled = !result`.

### `MyForm::RestoreConfiguredCameraHardware(bool cycleDevice)` (449–468)
Loads the saved device from config; if present, `RecoverCameraHardware(nativeId, cycleDevice)`. On any exception or empty config, falls back to `RestoreAllCameraHardware(cycleDevice)`. **Catches all exceptions** and treats as "not restored".

## Dependencies
- **Calls (native):** SetupAPI, CfgMgr32, `kernel32` (`Sleep`, `GetTickCount64`, `Interlocked*`).
- **Calls (members):** `WriteDiagnosticLog`, `WriteDiagnosticLogWithDevice`, `TryGetTargetCameraInstanceId`, `LoadConfigState`, `IsCurrentProcessElevatedNative`, `GetCurrentProcessIntegrityRid`.
- **Called by:** `MyForm_Core.cpp` (Load/dtor), `MyForm_Events.cpp` (WndProc), `MyForm_UI.cpp` (toggle), `MyForm_System` indirectly via Load.

## Threading context
All hardware calls run on **whatever thread invoked the member**: UI thread for Load/toggle/WndProc; background wake thread never calls these directly (it only brings the window forward). No new worker threads are spawned for camera work.

## Error handling
Each API failure is recorded into the `g_last*` diagnostic globals; the functions return `bool` success. No exceptions are thrown from native code (the managed members catch `...` only in `RestoreConfiguredCameraHardware`).
