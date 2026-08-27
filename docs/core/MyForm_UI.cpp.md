# `src/core/MyForm_UI.cpp` — UI Event Handlers

**Path:** `src/core/MyForm_UI.cpp`
**Lines:** 56
**Included by:** built directly; `#include "MyForm.h"`

## Purpose

Houses the two WinForms UI event handlers: `MyForm_FormClosing` and `btnToggle_Click`. These translate user interaction into `MyForm` state changes and camera operations.

## `MyForm_FormClosing` (5–22)

Fired when the form is being closed.
- If `CloseReason == UserClosing`:
  - `e->Cancel = true` (prevent actual closure — app stays in background).
  - `Hide()` + `ShowInTaskbar = false`.
  - If `!isBackgroundMode`: show the "Background Service Active" info box and set `isBackgroundMode = true` (so the box only appears once).
- If the close is due to **system shutdown**, nothing is done here; the destructor/finalizer handles camera reset (because `isSystemEnding` was set in `WndProc`).

> **Pure UI/lifecycle logic** — does not call camera APIs. It only manages window visibility and the background-mode flag.

## `btnToggle_Click` (24–54)

The "Start/Stop Monitoring Service" button.
- Casts `cachedCameras` → `vector<CameraDeviceInfo>*` and `selectedInstanceId` → `wstring*`.
- If no device selected (`deviceDrop->SelectedIndex == -1`): warn and return.
- **Start monitoring** (`!isMonitoring`):
  - `*pSelectedInstanceId = selected camera instance ID`.
  - `isMonitoring = true`.
  - Disable dropdown, set button text "Stop Monitoring Service", status "Service Running" (green).
  - `SaveConfigState(true, id)` — persists monitoring + device.
- **Stop monitoring** (`isMonitoring`):
  - `isMonitoring = false`.
  - `EnableTargetCameraHardware(false)` — **real camera operation** (re-enables device).
  - `SaveConfigState(false, id)`.
  - `pSelectedInstanceId->clear()`.
  - Re-enable dropdown, "Start Monitoring Service", status "Service Stopped" (gray).

## UI ↔ application state mapping

| UI action | Application state change | Camera effect |
|---|---|---|
| Click toggle (start) | `isMonitoring=true`, save config | none directly (config only) |
| Click toggle (stop) | `isMonitoring=false`, save config | `EnableTargetCameraHardware(false)` |
| User closes window | `Cancel=true`, hidden, `isBackgroundMode=true` | none |
| System shutdown | (handled in WndProc/dtor) | disable or enable per `isSystemEnding` |

## Dependencies
- **Calls:** `EnableTargetCameraHardware` (Camera), `SaveConfigState` (Config), `MessageBox`.
- **Called by:** WinForms event subscriptions made in `InitializeComponent`.

## Threading
Runs on the **UI thread** (button click handler).
