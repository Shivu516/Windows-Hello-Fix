# `src/core/MyForm_System.cpp` — Command Parsing & Wake Listener

**Path:** `src/core/MyForm_System.cpp`
**Lines:** 53
**Included by:** built directly; `#include "MyForm.h"`

## Purpose

Holds the **command-line interpretation** helpers and the **cross-process wake listener** used for single-instance behavior. No hardware operations are performed here directly (wake listener only manipulates the UI window).

## Command-line detection

### `IsRestoreCameraCommand` (5–16)
Returns `true` if any arg equals (case-insensitive): `/restore-camera`, `/enable-camera`, `--enable-camera`, `/repair-camera`. Used by `MyForm_Load` to run a one-shot "enable camera" pass and exit.

### `IsDisableCameraCommand` (18–27)
Returns `true` if any arg equals `/disable-camera` or `--disable-camera`. Used by `MyForm_Load` for a one-shot disable + verify + exit.

## Wake listener

### `ListenForWakeupSignal` (29–41)
Runs on `backgroundWorker` thread (started in `MyForm_Load`). Loop:
```
while (keepListening && hWakeupEvent != NULL) {
    DWORD r = WaitForSingleObject(hWakeupEvent, INFINITE);
    if (r == WAIT_OBJECT_0 && keepListening) {
        if (InvokeRequired) Invoke(BringWindowToFrontDelegate);
        else BringWindowToFrontDelegate();
    }
}
```
- Blocks on the named event `Global\WindowsHelloFix_WakeupEvent`.
- When signaled by a second instance's `SetEvent`, it raises the main window. `InvokeRequired` ensures the UI update happens on the UI thread (the listener is background, so `Invoke` is normally taken).
- `keepListening` is set `false` by destructor/finalizer to break the loop.

### `BringWindowToFrontDelegate` (43–51)
Pure UI: `Show`, `Visible=true`, `ShowInTaskbar=true`, `WindowState=Normal`, `BringToFront`, `Activate`, `Refresh`.

## Scenario matrix (current behavior)

| Scenario | Behavior |
|---|---|
| No arguments | Normal UI launch; mutex acquired; recovery; registrations; monitoring per config/background. |
| `--background` | `launchRequestedBackground=true`; window hidden/minimized; monitoring auto-starts if config says so. |
| Restore/enable command | Window hidden; `RestoreConfiguredCameraHardware(true)`; exit. |
| Disable command | Window hidden; `DisableTargetCameraHardware(true)` + verify; exit. |
| Another instance running + wake event present | `OpenEvent` → `SetEvent` (wake existing) → `Sleep(200)` → `Exit(0)`. |
| Another instance running + wake event absent + background | Quiet `Exit(0)`. |
| Another instance running + wake event absent + foreground | "Already Running" dialog; Yes → recover + `taskkill` + `Application::Restart()`. |

## Dependencies
- **Calls:** `BringWindowToFrontDelegate` (self), `WriteDiagnosticLog` (indirect via Load), `WaitForSingleObject`, `SetEvent`/`CloseHandle` (the event belongs to `MyForm`).
- **Called by:** `MyForm_Load` (starts the thread); the OS (wakes via `SetEvent`).

## Threading
`ListenForWakeupSignal` executes on the **background `backgroundWorker` thread** (`IsBackground=true`). It does not touch camera state, only the UI via `Invoke`.

## State read
`keepListening`, `hWakeupEvent` (both owned by `MyForm`).
