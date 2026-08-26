# `src/ui/MyForm_Config.cpp` — Configuration & Diagnostic Logging

**Path:** `src/ui/MyForm_Config.cpp`
**Lines:** 148
**Included by:** built directly; `#include "MyForm.h"`

## Purpose

Implements all `MyForm` members related to **persistence of configuration** (`config.txt`) and **diagnostic logging** (`diagnostic.log`), plus the **target-device resolution** helper `TryGetTargetCameraInstanceId`. No hardware operations occur here except calling `ScanSystemCameras` (in the fallback branch).

## Config storage location

`GetConfigFilePath` (5–13) builds:
```
%APPDATA%\Windows Hello Fix\config.txt
```
using `Environment::SpecialFolder::ApplicationData` + `Path::Combine(..., L"Windows Hello Fix")`, then `Directory::CreateDirectory` (idempotent). `GetDiagnosticLogFilePath` (15–28) places `diagnostic.log` in the **same directory**.

## Logging

### `WriteDiagnosticLog` (30–51)
- Enters `Monitor::Enter(diagnosticLogSync)` (the `Object^` lock in `MyForm`).
- Opens the log with `StreamWriter(logPath, true)` (append).
- Writes one line:
  ```
  yyyy-MM-dd HH:mm:ss.fff | Event=<eventName> | Target=<targetState> | Verify=PASS|FAIL
  ```
- All exceptions swallowed (`catch (...) {}`). `finally` always exits the monitor.
- **This is the single source of all diagnostic output.** Every camera/event operation calls it.

### `WriteDiagnosticLogWithDevice` (53–60)
Convenience wrapper that marshals the `std::wstring` instance ID to `String^` and prepends ` | Device=<id>` to the event name, then calls `WriteDiagnosticLog`.

## Config file format

Two lines:
```
monitoring=1        (or monitoring=0)
device=<instanceId>
```
- `monitoring=1` ⇒ auto-start monitoring on next launch.
- `device=` ⇒ the target camera instance ID persisted across runs.

`SaveConfigState` (62–71) overwrites the file (`append:false`) with these two lines. Never throws (swallows exceptions).

`LoadConfigState` (73–98):
- Returns `false` if file absent.
- Reads `line1`, `line2`.
- `monitoringActive = (line1 trimmed == "monitoring=1")`.
- If `line2` starts with `device=`, takes the substring after 7 chars, trims, and runs `TrimTrailingChars` (from `MyForm_Camera.cpp`) to strip CRLF corruption before returning it via the `[Out]` parameter.
- Swallows exceptions, returning `false`.

`EnsureConfigFileExists` (100–108) writes a default `monitoring=0` config only if the file does not yet exist (never overwrites an existing file).

## Target-device resolution

### `TryGetTargetCameraInstanceId(std::wstring& target, bool preferCurrentSelection)` (110–146)
Resolution priority:
1. If `preferCurrentSelection` and `selectedInstanceId` is non-empty → use the live selection.
2. Else `LoadConfigState` → if a saved device exists → use it.
3. Else (only when `!preferCurrentSelection`) fall back to live selection.
4. Else `ScanSystemCameras()` and pick the first whose instance ID contains `MI_00` (the RGB/IR sensor sub-device), else the first camera.
5. Else return `false`.

This is the central "which camera do we act on?" decision used by every toggle/restore/verify path.

## Dependencies
- **Calls:** `ScanSystemCameras`, `TrimTrailingChars` (both in `MyForm_Camera.cpp`), `WriteDiagnosticLog`.
- **Called by:** `MyForm_Camera.cpp` (disable/enable/restore), `MyForm_Core.cpp` (Load), `MyForm_System` indirectly, `MyForm_UI.cpp`.

## Threading / sync
`WriteDiagnosticLog` is the only synchronized region (monitor on `diagnosticLogSync`). File writes are buffered and closed each call. The `target` resolution reads `selectedInstanceId` (a raw `std::wstring*`) directly; no lock, but it is only ever touched on the UI thread.

## State modified
- Filesystem: `config.txt`, `diagnostic.log`.
- Out-parameter `deviceInstanceId` (managed) in `LoadConfigState`.
- No `MyForm` member state is written here except transient local reads.
