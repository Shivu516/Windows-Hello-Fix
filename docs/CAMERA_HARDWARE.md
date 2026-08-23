# Camera Hardware Path (as-built)

Baseline: branch `test`, commit `acc37d8`. This document traces the complete
camera control pipeline: discovery → target identification → enable/disable →
SetupAPI/Configuration Manager calls → verification → logging. **No code was
modified to produce this document.**

Files involved (all native C++, no managed types):

- `src/camera/CameraDevice.cpp/.h` — discovery
- `src/camera/CameraHardware.cpp/.h` — low-level toggle/state/verify
- `src/camera/CameraRecovery.cpp/.h` — retry/recovery orchestration
- `src/camera/DeviceError.h` — global diagnostic slots
- Managed orchestration: `ApplicationController::DisableTargetCameraHardware`,
  `EnableTargetCameraHardware`, `RestoreConfiguredCameraHardware`

## 1. Discovery

`ScanSystemCameras()` (CameraDevice.cpp):

```
SetupDiGetClassDevs(NULL, NULL, NULL, DIGCF_ALLCLASSES | DIGCF_PRESENT)
   ↓ enumerate all present devices (SetupDiEnumDeviceInfo)
   ↓ keep device where SPDRP_CLASS == "Camera" or "Image"
   ↓ instanceId = SetupDiGetDeviceInstanceId
   ↓ friendlyName = SPDRP_DEVICEDESC
   → std::vector<CameraDeviceInfo>{friendlyName, instanceId}
```

On enumeration failure it returns an empty list silently.

## 2. Target identification

`ApplicationController::TryGetTargetCameraInstanceId(id, preferCurrentSelection)`
resolves the target in this exact priority order:

1. Current UI selection (`m_selectedInstanceId`) — only if
   `preferCurrentSelection` is true.
2. `device=` entry loaded from `%APPDATA%\Windows Hello Fix\config.txt`.
3. Current selection again (when called with `preferCurrentSelection=false`).
4. First scanned camera whose `instanceId` contains the substring `MI_00`
   (USB composite-interface heuristic for the RGB sensor).
5. First camera of any kind.
6. Fail (`NoTarget` logged, action returns false).

## 3. Enable/disable requests and who calls what

| Caller intent | Function chain |
|---|---|
| Lock / suspend / lid / button / system-end disable | `HandleSessionEvent`/`HandlePowerEvent`/`HandleSystemEnd` → `DisableTargetCameraHardware(retry)` → `SetCameraHardwareStateVerified(id, false, retry)` |
| Unlock / resume enable; monitoring stop; shutdown cleanup | `HandleSessionEvent`/… → `EnableTargetCameraHardware(cycleDevice)` → `RecoverCameraHardware(id, cycleDevice)` → `SetCameraHardwareStateVerified(id, true, false)` |
| Startup restore | `Initialize` → `RestoreConfiguredCameraHardware(true)`: recover configured device, else `RestoreAllCameraHardware(true)` (recover every scanned camera) |

## 4. The two hardware toggle mechanisms

### 4.1 Primary: SetupAPI property change — `ToggleCameraHardware(id, enable)`

```
stage=10; g_lastSetupApiError=ERROR_SUCCESS
SetupDiGetClassDevs(NULL,NULL,NULL, DIGCF_ALLCLASSES)      ← no PRESENT flag
   ↓ SetupDiEnumDeviceInfo loop
   ↓ match instance id exactly OR case-insensitively (_wcsicmp)
   ↓ SP_PROPCHANGE_PARAMS:
   │    ClassInstallHeader.InstallFunction = DIF_PROPERTYCHANGE
   │    StateChange = DICS_ENABLE | DICS_DISABLE
   │    Scope = DICS_FLAG_GLOBAL, HwProfile = 0
   ↓ SetupDiSetClassInstallParams      (fail → stage=12, save GetLastError)
   ↓ SetupDiCallClassInstaller(DIF_PROPERTYCHANGE, …)
                                          (fail → stage=13, save GetLastError)
   success → stage=14, changed=true
device never matched / no change → stage=15
always: SetupDiDestroyDeviceInfoList
```

Stage markers stored in `g_lastHardwareToggleStage`: 10 start, 11
GetClassDevs failed, 12 SetClassInstallParams failed, 13 CallClassInstaller
failed, 14 success, 15 no matching device or no change performed.

### 4.2 Fallback: Configuration Manager — `ToggleCameraHardwareCfgMgr(id, enable)`

```
stage=20
LocateCameraDevInst(id, devInst)     (same enumeration + match as above)
   not found → stage=21, return false
cr = CM_Enable_DevNode(devInst, 0)            when enabling
   | CM_Disable_DevNode(devInst, CM_DISABLE_UI_NOT_OK)  when disabling
g_lastConfigManagerResult = cr
   cr != CR_SUCCESS → stage=22, return false
CM_Reenumerate_DevNode(devInst, 0)   ← result code ignored
success → stage=23
```

## 5. How "already enabled/disabled" is determined

`GetCameraHardwareDisabledState(id, &isDisabled)` finds the device (exact or
case-insensitive match, `DIGCF_ALLCLASSES`, no PRESENT flag — so disabled
devices are still visible) and reports disabled when **either**:

- `CM_Get_DevNode_Status(&status, &problem, devInst, 0)` succeeds and
  `problem == CM_PROB_DISABLED` (=22), or
- `SetupDiGetDeviceRegistryProperty(SPDRP_CONFIGFLAGS)` yields a DWORD whose
  `CONFIGFLAG_DISABLED` (=0x00000001) bit is set.

Returns `found=false` if the handle can't be created or the id doesn't match;
callers treat unknown-found as "not verifiably in state".

Both managed wrappers short-circuit on this check:

- `DisableTargetCameraHardware`: already disabled → log
  `DisableTargetCameraHardware_AlreadyDisabled`, mark expected state, return true.
- `EnableTargetCameraHardware`: already enabled → log
  `EnableTargetCameraHardware_AlreadyEnabled`, return true.
- `SetCameraHardwareStateVerified` performs its own check-before-change first
  (verify desired state → skip all hardware calls).

## 6. Verification

`VerifyCameraHardwareState(id, shouldBeDisabled)`:

```
up to 3 attempts:
    GetCameraHardwareDisabledState → isDisabled
    if found && isDisabled == shouldBeDisabled → return true
    Sleep(100 ms)
return false
```

"Verified" therefore means *the device node reports the requested problem
code/config flag within ~300 ms*. There is no functional test of the stream,
no FrameServer probe, no driver reload wait beyond these sleeps.

## 7. Retry / recovery policy

`SetCameraHardwareStateVerified(id, enable, reinitializeOnMismatch)`
(CameraRecovery.cpp):

```
check-before-change (verify desired state → true, no I/O)
repeat up to 3 times:
    ToggleCameraHardware(id, enable)          // SetupAPI attempt
    verify → done?
    ToggleCameraHardwareCfgMgr(id, enable)    // CfgMgr attempt
    verify → done?
    if reinitializeOnMismatch:
        ToggleCameraHardware(id, !enable)     // force opposite state…
        ToggleCameraHardwareCfgMgr(id, !enable)
        Sleep(250)                            // …so next pass re-applies cleanly
    Sleep(250)
final attempt: ToggleCameraHardware(id, enable); return Verify(…)
```

`reinitializeOnMismatch` is true only from `DisableTargetCameraHardware` when
its caller passed `retryOnFailure=true` (all production call sites do).

`RecoverCameraHardware(id, cycleDevice)`:

```
restored = SetCameraHardwareStateVerified(id, true, false)
if cycleDevice:
    Sleep(350) → SetVerified(disable) → Sleep(900)
    restored = SetVerified(enable) || restored
    Sleep(500) → SetVerified(enable) again (redundant second enable)
return restored
```

`cycleDevice=true` produces a full off/on power cycle; used at startup restore,
`/restore-camera` commands, ghost reset, and `RestoreAllCameraHardware`.
Monitoring unlock path uses `EnableTargetCameraHardware(false)` → recover
without cycling.

## 8. Error codes handled / recorded

| Slot | Producer | Meaning |
|---|---|---|
| `g_lastSetupApiError` (volatile LONG) | SetupAPI stages | Win32 `GetLastError()` captured at each failure point (11/12/13 above) |
| `g_lastConfigManagerResult` | CfgMgr path | `CONFIGRET` from `CM_Enable_DevNode`/`CM_Disable_DevNode` (`CR_SUCCESS`=0) |
| `g_lastHardwareToggleStage` | both paths | Pipeline marker 10–15 (SetupAPI), 20–23 (CfgMgr) as listed above |

These three globals live in `CameraDevice.cpp` (declared in `DeviceError.h`)
and are read back via `InterlockedCompareExchange(...)` by
`ApplicationController::Disable/EnableTargetCameraHardware` when composing the
`*_Result` diagnostic lines:

```
Event = DisableTargetCameraHardware_Result | Elevated={0|1} | IntegrityRid=<rid>
      | SetupErr=<win32> | CfgMgr=<CONFIGRET> | Stage=<10..23>
```

The functions treat `result && verified` as overall success. No other error
taxonomy exists; empty-target failures log `*_NoTarget` with `Verify=FAIL`.

## 9. Logging produced by this path

See `LOGGING.md` for format. Events emitted by the camera path:

- `DisableTargetCameraHardware_NoTarget` / `_AlreadyDisabled` / `_Result`
- `EnableTargetCameraHardware_NoTarget` / `_AlreadyEnabled` / `_Result`
- (via `WriteDiagnosticLogWithDevice`) each carries `| Device=<instance id>`.

Native camera functions themselves write nothing to `diagnostic.log`.

## 10. Threading & timing notes

- All camera calls execute synchronously on the UI thread (see
  `ARCHITECTURE.md §5`); worst-case wall time per disable is roughly
  3 × (verify ≤300 ms + toggles + 250 ms reinit + 250 ms) ≈ several seconds.
- `RecoverCameraHardware` with cycling adds fixed 350/900/500 ms sleeps.
- The three diagnostic globals make the module thread-tolerant but racy by
  design: last writer wins between concurrent processes (e.g., daemon plus
  scheduled failsafe task).
