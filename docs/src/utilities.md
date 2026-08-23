# Module: `src/utilities/` — shared helpers

Native C++.

---

## Logging.h / Logging.cpp (`ProductionLogger`)

| Field | Content |
|---|---|
| Purpose | Structured debug-output logger (design artifact of the refactor plan) |
| Functions | `LogHardwareOperation(op, deviceId, result, lastError, configRet, durationMs)`; `LogError(context, msg)`; `LogInfo(context, msg)`; private `GetTimestamp()` via `localtime_s` |
| Output channel | `OutputDebugStringW` only — **no file I/O** |
| Format | `[ts] OP=… \| DEVICE=… \| RESULT=SUCCESS\|FAILED [| WIN_ERROR=0x..] [| CONFIGRET=n] [| DURATION=nms] \| PID=n \| TID=n` / `[ts] ERROR\|INFO \| ctx \| msg` |
| Status | Compiled into the binary but **never called** by production code (verified by search). The real log is ConfigStore's diagnostic.log — see ../LOGGING.md §3 |
| Threading | Stateless; each call independent |

## StringHelpers.h

| Field | Content |
|---|---|
| Purpose | One inline sanitizer: `TrimTrailingChars(std::wstring)` strips trailing `\r`, `\n`, spaces in a loop |
| Consumers | ConfigStore::LoadConfigState (device-id sanitization after managed Trim), MyForm.h include chain |
| Side effects | None; pure function |

---

## File inventory check

Every file under `src/` as compiled today:

| File | Documented in |
|---|---|
| src/application/ApplicationController.h/.cpp | application.md |
| src/application/CommandLine.h/.cpp | application.md |
| src/application/IUiSink.h | application.md |
| src/camera/CameraDevice.h/.cpp | camera.md |
| src/camera/CameraHardware.h/.cpp | camera.md |
| src/camera/CameraRecovery.h/.cpp | camera.md |
| src/camera/DeviceError.h | camera.md |
| src/config/ConfigPaths.h/.cpp | config.md |
| src/config/ConfigStore.h/.cpp | config.md |
| src/events/SystemEvent.h | events.md |
| src/events/NotificationRegistrar.h/.cpp | events.md |
| src/events/WinEventDecoder.h/.cpp | events.md |
| src/events/EventCooldown.h/.cpp | events.md |
| src/system/PrivilegeInfo.h/.cpp | system.md |
| src/system/ProcessUtils.h/.cpp | system.md |
| src/system/SingleInstance.h/.cpp | system.md |
| src/ui/MyForm.h/.cpp | ui.md |
| src/ui/UiConstants.h | ui.md |
| src/utilities/Logging.h/.cpp | utilities.md |
| src/utilities/StringHelpers.h | utilities.md |

Root-level sources documented in ../ARCHITECTURE.md and ../STARTUP.md:
`main.cpp` (entry point) and `MyForm.h` (shim → src/ui).
