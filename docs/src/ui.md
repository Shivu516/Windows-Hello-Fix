# Module: `src/ui/` — WinForms presentation

C++/CLI. The only module allowed to touch controls. Also documents the root
`MyForm.h` shim and `UiConstants.h`.

---

## MyForm.h / MyForm.cpp (namespace `Windows_Hello_Fix_v2_0`)

| Field | Content |
|---|---|
| Purpose | Main (only) window: device dropdown, toggle button, status label; WndProc event dispatch; implements `IUiSink` |
| Base | `System::Windows::Forms::Form, public IUiSink` |

**Important state**

| Member | Meaning |
|---|---|
| `m_controller` (`ApplicationController^`) | Orchestration owner; created in constructor, deleted in destructor |
| `cachedCameras` (`void*`) | Owns native `std::vector<CameraDeviceInfo>*` (deleted in dtor + finalizer) |
| `isBackgroundMode` | Set when launched with `--background`; gates the one-time background notice on user close |
| `cameraExpectedDisabled` | Written in ctor; never read (placeholder) |
| Controls | `deviceDrop`, `btnToggle`, `lblTitle`, `lblStatus`, `components` |

**Execution flow: `MyForm_Load`** (UI thread, after `Application::Run`):
1. Capture HWND from `this->Handle`; args from `Environment::GetCommandLineArgs`.
2. `m_controller->SetHwnd(hwnd)` → `Initialize(hwnd, args)`; bail if it
   returned false (command mode / duplicate instance already exited).
3. Scan cameras into cached vector; populate dropdown; selection priority:
   saved config device → first `MI_00` instance → first device.
4. Second `LoadConfigState` for autostart flag; `EnsureConfigFileExists`.
5. If a device is selected: `EnableTargetCameraHardware(autoStart)`
   ("first enable to prevent bricking" — note cycleDevice semantics,
   KNOWN_ISSUES #2).
6. If `(isBackground \|\| autoStart) && selected`: set selection again,
   `IsMonitoring=true`, `EnableTargetCameraHardware(false)`, lock dropdown,
   button→"Stop Monitoring Service", status green; background also
   `SetWindowVisibleForBackground(true)`. Else stopped-state UI.

**WndProc dispatch** (all camera work happens synchronously here):

| Message | Handling |
|---|---|
| `0x0011`/`0x0016` | IsSystemEnding=true → HandleSystemEnd → base |
| `WM_POWERBROADCAST` | dedup (`PowerEvent_DedupIgnored` log) → decode (`PowerSetting_IrrelevantGuid` log) → HandlePowerEvent → base |
| `WM_WTSSESSION_CHANGE` | log raw code always → dedup → decode → HandleSessionEvent → base |

**Other handlers**
- `btnToggle_Click`: require selection (message box), sync selection to
  controller, `ToggleMonitoring()`, update UI accordingly.
- `MyForm_FormClosing`: UserClosing → cancel + hide + one-time notice. App has
  no real exit via UI.
- Destructor/finalizer: controller Shutdown+delete, free cached cameras.
  Finalizer calls Shutdown again without delete (asymmetry documented).

**IUiSink implementations**: thin control wrappers; `BringWindowToFront`
marshals to UI thread then runs `BringWindowToShowDelegate` (Opacity=1, Show,
Visible, taskbar, Normal, BringToFront, Activate, Refresh); dialogs:
`PromptGhostReset`, `ShowNoDeviceSelectedMessage`, `ShowBackgroundNotice`.

**Threading**: everything UI-thread except wake listener callback which uses
Invoke.

**Includes**: pulls nearly every src header + `resource.h`; pragma-links
wtsapi32/setupapi/user32/cfgmgr32/advapi32.

## UiConstants.h

| Field | Content |
|---|---|
| Purpose | Presentation literals (form text/name, labels, button texts, messages, control geometry) as `literal` properties |
| Status | **Partially wired**: `InitializeComponent` and the message boxes hard-code identical strings instead of referencing the constants; constants exist but are mostly unreferenced today |

## Root `MyForm.h` (compatibility shim)

4-line shim: `#include "src/ui/MyForm.h"` — kept so `main.cpp`'s historical
include path keeps working. Contains no code of its own.
