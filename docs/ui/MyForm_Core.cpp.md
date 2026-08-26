# `src/ui/MyForm_Core.cpp` — Lifecycle, Construction, Shutdown & Startup

**Path:** `src/ui/MyForm_Core.cpp`
**Lines:** 407
**Included by:** built directly; `#include "MyForm.h"`

## Purpose

Contains the **lifecycle core** of `MyForm`: constructor, destructor, finalizer, `InitializeComponent`, and `MyForm_Load` (the startup sequence). This is the most behaviorally important file because it establishes the exact initialization and shutdown ordering.

---

## Constructor `MyForm::MyForm(void)` (5–23)

Initializes every member:
- `components = nullptr`
- `cachedCameras = new std::vector<CameraDeviceInfo>()`
- `selectedInstanceId = new std::wstring()`
- All booleans `false` (`isMonitoring`, `isBackgroundMode`, `isSystemEnding`, `cameraStateInitialized`, `cameraExpectedDisabled`, `restartQueuedByMismatch`)
- `lastCameraToggleTick = 0`
- Handles `NULL` (`hAppMutex`, `hWakeupEvent`, `hLidNotification`, `hButtonNotification`)
- `keepListening = true`
- `diagnosticLogSync = gcnew Object()`
- Then calls `InitializeComponent()` (which builds the WinForms UI).

> Raw `std::vector*`/`std::wstring*` pointers are used (not `std::unique_ptr`) because the original code manages them manually in the destructor/finalizer. Ownership is exclusively `MyForm`.

---

## Destructor `~MyForm()` (26–74)

Runs when the CLR finalizes/managed object is destroyed (normal close or `Application::Exit`).

1. `keepListening = false` (stops wake listener loop).
2. Cast `selectedInstanceId` to `std::wstring*`.
3. **If `isSystemEnding`** (set by WndProc on shutdown/logoff):
   - `DisableTargetCameraHardware(true)` — leave camera disabled at system end.
   - If a device is selected: `SaveConfigState(true, id)` (persist monitoring=1).
4. **Else if a device is selected**:
   - `EnableTargetCameraHardware(false)` — guarantee camera re-enabled before exit.
   - `SaveConfigState(true, id)` — revert config to monitoring=1 with live ID.
5. **Else** (nothing selected): `RestoreConfiguredCameraHardware(false)`.
6. Cleanup handles in order: `SetEvent`+`CloseHandle(hWakeupEvent)`; `UnregisterPowerSettingNotification` for lid & button; `delete cachedCameras`; `delete selectedInstanceId`; `delete components`; `CloseHandle(hAppMutex)`.

---

## Finalizer `!MyForm()` (76–106)

Runs during GC finalization. Same cleanup as destructor **minus** the `SaveConfigState` calls (it only toggles the camera and frees handles/native memory). Both destructor and finalizer perform the hardware toggle; the finalizer is the safety net if the destructor was skipped.

> **Observed duplication:** camera disable/enable and handle cleanup appear in *both* `~MyForm` and `!MyForm`. Documented in `docs/KNOWN_ISSUES.md`.

---

## `InitializeComponent()` (108–169)

Standard WinForms designer-generated layout (here hand-written):
- Creates `deviceDrop` (ComboBox, DropDownList), `btnToggle` (Button), `lblTitle`, `lblStatus`, `components`.
- Loads `IDI_ICON1` via `LoadImage`/`FromHandle` (wrapped in try/catch).
- Sets fonts, positions, sizes, `FixedDialog` border, no min/max box, centered start, title "Windows Hello Fix v2.0".
- Wires `btnToggle->Click → btnToggle_Click`, `FormClosing → MyForm_FormClosing`, `Load → MyForm_Load`.

**UI only** — no camera, config, or OS registration here.

---

## `MyForm_Load` (171–405) — the startup sequence

Triggered by the `Load` event after `Application::Run`. Exact order:

1. **Read command line** (`Environment::GetCommandLineArgs`), detect `/background` or `--background` → `launchRequestedBackground`.
2. **Log `Startup_Context`** with elevation/integrity/paths (informational, `NoChange`).
3. **Restore-camera command?** (`IsRestoreCameraCommand`): hide window, `RestoreConfiguredCameraHardware(true)`, log begin/end, `Environment::Exit(0)` — no UI, no mutex.
4. **Disable-camera command?** (`IsDisableCameraCommand`): hide window, `DisableTargetCameraHardware(true)`, verify, log, `Environment::Exit(0)`.
5. **Single-instance mutex:** `CreateMutex(NULL, TRUE, L"Global\\WindowsHelloFix_AppMutex")`.
   - If `ERROR_ALREADY_EXISTS`:
     - `OpenEvent` on `Global\WindowsHelloFix_WakeupEvent`; if found: `SetEvent` (wake existing instance), `Sleep(200)`, log, `Exit(0)`.
     - Else if `launchRequestedBackground`: log "WakeEventMissing", `Exit(0)` quietly.
     - Else: show **"Application Already Running"** Yes/No dialog. On **Yes**: load saved device (or scan for `MI_00`), `RecoverCameraHardware(id, true)`, `SaveConfigState(true, id)`, `system("taskkill /F /IM Windows_Hello_Fix_v2_0.exe /T")`, `Sleep(500)`, `Application::Restart()`. Then `Exit(0)` regardless.
   - If mutex acquired (first instance): continue.
6. `hWakeupEvent = CreateEvent(NULL, FALSE, FALSE, L"Global\\WindowsHelloFix_WakeupEvent")`.
7. **Startup camera recovery** (must precede dropdown build): log `Startup_RestoreConfiguredCameraHardware`, `RestoreConfiguredCameraHardware(true)` (re-enables a device left disabled by a prior session).
8. **Register power notifications:** `RegisterPowerSettingNotification` for `GUID_LIDSWITCH_STATE_CHANGE` and `GUID_POWER_BUTTON_TIMESTAMP` (store `hLidNotification`, `hButtonNotification`).
9. `isBackgroundMode = launchRequestedBackground` if applicable.
10. `*pCachedCameras = ScanSystemCameras()`; `LoadConfigState(savedDeviceInstance)` → `shouldAutoStartByConfig`.
11. Populate `deviceDrop` items; pick `savedIdx` (matches saved device), else `autoIdx` (contains `MI_00`), else index 0.
12. Set `*pSelectedInstanceId` from selection; `EnsureConfigFileExists(...)`.
13. If a device is selected: `EnableTargetCameraHardware(shouldAutoStartByConfig)` (force-enable to prevent bricking loops).
14. **Auto-start monitoring** if `(startInBackground || shouldAutoStartByConfig) && selected`:
    - set `isMonitoring=true`, `EnableTargetCameraHardware(false)` (stable start), disable dropdown, set button/status text+color green; if background → hide, no taskbar, minimized.
    - Else: `isMonitoring=false`, dropdown enabled, "Start Monitoring Service", gray status.
15. **Start wake listener:** `backgroundWorker = gcnew Thread(ListenForWakeupSignal)`; `IsBackground=true`; `Start()`.
16. **Register WTS session notifications:** retry up to **6 times** with `Sleep(500)` between attempts (`WTSRegisterSessionNotification`, `NOTIFY_FOR_THIS_SESSION`); log success or last error.

After step 16 the app is in steady state: window shown (or hidden), monitoring possibly active, listener running, OS notifications registered.

## Dependencies
- **Calls:** `IsRestoreCameraCommand`/`IsDisableCameraCommand` (System), `RestoreConfiguredCameraHardware`/`DisableTargetCameraHardware`/`EnableTargetCameraHardware` (Camera), `LoadConfigState`/`SaveConfigState`/`EnsureConfigFileExists`/`TryGetTargetCameraInstanceId`/`GetConfigFilePath` (Config), `ScanSystemCameras` (Camera), `ListenForWakeupSignal` (System), `WriteDiagnosticLog`, `IsCurrentProcessElevatedNative`, `GetCurrentProcessIntegrityRid`.
- **Called by:** WinForms runtime (`Application::Run` → `Load` event).

## Threading
Runs on the **UI thread**. The only other thread (`backgroundWorker`) is created here but executes `ListenForWakeupSignal` in `MyForm_System.cpp`.

## State modified
Owns and initializes *all* `MyForm` state; after Load, sets `hAppMutex`, `hWakeupEvent`, `hLidNotification`, `hButtonNotification`, `isBackgroundMode`, `isMonitoring`, `selectedInstanceId`, `cachedCameras`, and config files.
