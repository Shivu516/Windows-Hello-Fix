# Camera Flow (Pipeline Detail)

This document describes the camera hardware pipeline exactly as implemented in `src/ui/MyForm_Camera.cpp` and the `MyForm` members that drive it. It is descriptive only — no optimizations are proposed.

## 1. Discovery — `ScanSystemCameras`

1. `SetupDiGetClassDevs(NULL, NULL, NULL, DIGCF_ALLCLASSES | DIGCF_PRESENT)` — present devices only.
2. For each device: read `SPDRP_CLASS`. Keep only `Camera` or `Image`.
3. Read `SPDRP_DEVICEDESC` (friendly name) and instance ID.
4. Return `vector<CameraDeviceInfo>`.

> `DIGCF_PRESENT` is used here. The toggle functions use `DIGCF_ALLCLASSES` **without** `DIGCF_PRESENT` so they can still act on a device node that is currently not present (e.g., disabled).

## 2. Target selection — `TryGetTargetCameraInstanceId`

Priority (see `MyForm_Config.cpp`):
1. Live `selectedInstanceId` if `preferCurrentSelection` and non-empty.
2. Saved device from `config.txt` (`device=`).
3. (Only when `!preferCurrentSelection`) live selection again.
4. Scan and pick first instance ID containing `MI_00` (RGB/IR sub-device).
5. Else first scanned camera.
6. Else `false`.

## 3. Disabled-state detection — `GetCameraHardwareDisabledState`

A device counts as disabled if **either**:
- `SPDRP_CONFIGFLAGS & CONFIGFLAG_DISABLED` is set, **or**
- `CM_Get_DevNode_Status` returns problem code `CM_PROB_DISABLED` (22).

This dual check is the ground truth used by all verification.

## 4. Disable — `MyForm::DisableTargetCameraHardware(retryOnFailure)`

1. Resolve target; if none → log `…_NoTarget`, return `false`.
2. `lastToggleTime = DateTime::Now`.
3. If already disabled → set `cameraExpectedDisabled=true`, log `…_AlreadyDisabled`, return `true`.
4. `SetCameraHardwareStateVerified(target, false, retryOnFailure)`.
5. `VerifyCameraHardwareState(target, true)`.
6. `cameraExpectedDisabled = result`; log `…_Result` with elevated/integrity/setup/cfgmgr/stage; return `result && verified`.

## 5. Enable — `MyForm::EnableTargetCameraHardware(cycleDevice)`

Symmetric. Uses `RecoverCameraHardware(target, cycleDevice)` instead of the direct verified set, and logs `…_Result` / `…_AlreadyEnabled`.

## 6. Verified set — `SetCameraHardwareStateVerified`

```
if target empty -> false
shouldBeDisabled = !enable
if Verify(target, shouldBeDisabled) -> true        // already correct, no churn
for attempt 0..2:
    ToggleCameraHardware(target, enable)           // SetupAPI
    if Verify(target, shouldBeDisabled): RecordTick; return true
    ToggleCameraHardwareCfgMgr(target, enable)      // CfgMgr
    if Verify(target, shouldBeDisabled): RecordTick; return true
    if reinitializeOnMismatch:
        ToggleCameraHardware(target, !enable)
        ToggleCameraHardwareCfgMgr(target, !enable)
        Sleep(250)
    Sleep(250)
// final attempt
ToggleCameraHardware(target, enable)
verified = Verify(target, shouldBeDisabled)
if verified: RecordTick
return verified
```

## 7. SetupAPI toggle — `ToggleCameraHardware`

- `SetupDiGetClassDevs(DIGCF_ALLCLASSES)`.
- Match instance ID (exact or `_wcsicmp`).
- `SP_PROPCHANGE_PARAMS` with `DIF_PROPERTYCHANGE`, `DICS_ENABLE`/`DICS_DISABLE`, `DICS_FLAG_GLOBAL`.
- `SetupDiSetClassInstallParams` then `SetupDiCallClassInstaller`.
- Records diagnostic stage `10→14` (or `12/13` on failure, `15` if not found).

## 8. CfgMgr toggle — `ToggleCameraHardwareCfgMgr`

- `LocateCameraDevInst` (resolves DEVINST).
- `CM_Enable_DevNode` / `CM_Disable_DevNode(devInst, CM_DISABLE_UI_NOT_OK)`.
- `CM_Reenumerate_DevNode(devInst, 0)`.
- Records stage `20→23`.

## 9. Verification — `VerifyCameraHardwareState`

- Up to **3 attempts**, `Sleep(100)` between, checking `GetCameraHardwareDisabledState == shouldBeDisabled`.

## 10. Recovery — `RecoverCameraHardware`

- `SetCameraHardwareStateVerified(target, true, false)` (ensure enabled).
- If `cycleDevice`:
  - `Sleep(350)`
  - `SetCameraHardwareStateVerified(target, false, false)` (disable)
  - `Sleep(900)`
  - `SetCameraHardwareStateVerified(target, true, false)` (enable) — OR-accumulated
  - `Sleep(500)`
  - `SetCameraHardwareStateVerified(target, true, false)` (enable) — OR-accumulated

## 11. Restore all — `RestoreAllCameraHardware`

`ScanSystemCameras()` → `RecoverCameraHardware(each, cycleDevices)`.

## 12. Why operations take the time they do

| Step | Sleep | Cumulative contribution |
|---|---|---|
| `VerifyCameraHardwareState` | 100 ms × up to 3 | ≤ 300 ms per verify call |
| `SetCameraHardwareStateVerified` per attempt | 250 ms (+250 if reinitialize) | up to ~2.5 s worst case |
| `RecoverCameraHardware` cycle | 350 + 900 + 500 | 1750 ms extra |
| `WndProc` suspend | 500 ms | post-disable |
| `WndProc` resume | 1000 ms | pre-enable |

A single lock event can therefore take ~1 s (disable + 500 ms). A startup "restore + enable" with cycling can take several seconds. These timings are intentional in the original v2.0 to allow the Windows device stack to settle; they are preserved exactly in the extraction.

## 13. Error reporting

All API failures are captured into the four `g_last*` globals (`g_lastSetupApiError`, `g_lastConfigManagerResult`, `g_lastHardwareToggleStage`, plus the toggle tick). The `Disable/EnableTargetCameraHardware` functions read these (zeroing them via `InterlockedCompareExchange`) and embed them in the `…_Result` diagnostic log line. No error is surfaced to the user via MessageBox; logging is the only diagnostic channel.

## 14. Shutdown behavior

On `isSystemEnding`, the destructor calls `DisableTargetCameraHardware(true)` (leave disabled). On a normal close, the destructor calls `EnableTargetCameraHardware(false)` (leave enabled). See `docs/LIFECYCLE.md`.
