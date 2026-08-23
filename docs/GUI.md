# GUI Visibility (as-built)

Baseline: branch `test`, commit `acc37d8`. This document inventories **every**
piece of code that can make the main window visible or invisible. Nothing was
changed.

## 1. Window basics

`MyForm` (src/ui/MyForm.h/.cpp, namespace `Windows_Hello_Fix_v2_0`) is the only
top-level window. `InitializeComponent` sets: `FixedDialog`, no max/min box,
430×240 client size, `StartPosition=CenterScreen`, text "Windows Hello Fix
v2.0". Default WinForms visibility applies (visible when the message loop
shows it). There is **no** `SetVisibleCore` override and **no** notify/tray
icon anywhere in the codebase.

## 2. Startup-time visibility decision (main.cpp)

```
bool runHidden = CommandLine::ShouldHideWindow(args);
if (runHidden) {
    form.Opacity = 0;
    form.ShowInTaskbar = false;
    form.WindowState = FormWindowState::Minimized;
}
Application::Run(%form);
```

Hidden when args contain (case-sensitive): `/background`, `--background`,
`/disable-camera`, `--disable-camera`, `/enable-camera`, `--enable-camera`,
`/restore-camera`, `/repair-camera`.

For a normal launch (no args) nothing is touched → window shows normally.
`main.cpp` never restores visibility after hiding it; only the delegate below
can.

## 3. Functions that change visibility

| Function | File | Direction | What it does |
|---|---|---|---|
| `main()` hide block | main.cpp:16-23 | hide | Opacity 0 + taskbar off + minimized (see above) |
| `MyForm::BringWindowToFrontDelegate` | src/ui/MyForm.cpp:95-104 | **show** | `Opacity = 1.0; Show(); Visible = true; ShowInTaskbar = true; WindowState = Normal; BringToFront(); Activate(); Refresh();` — the **only** code path that restores a hidden window |
| `MyForm::BringWindowToFront` | src/ui/MyForm.cpp:68-74 | show (marshals) | If `InvokeRequired`, `Invoke`s the delegate on the UI thread; else calls it directly. Thread-safe entry point for non-UI threads |
| `IUiSink::BringWindowToFront` implementations | interface in src/application/IUiSink.h | show | The abstraction through which `ApplicationController` reaches the GUI |
| `MyForm::SetWindowVisibleForBackground(true)` | src/ui/MyForm.cpp:61-67 | hide | `Visible = false; ShowInTaskbar = false; WindowState = Minimized;`. Called from `MyForm_Load` when a background autostart happened. (Called with `false` it does nothing.) |
| `MyForm::MyForm_FormClosing` | src/ui/MyForm.cpp:235-245 | hide | On `UserClosing`: cancels the close (`e->Cancel = true`), `Hide()`, `ShowInTaskbar = false`, shows the one-time background notice. The app therefore never exits via the X button |

## 4. Who can trigger a *show*

1. **Wake event from a second instance** (primary mechanism):
   - Second process runs without `--background`, finds mutex held, calls
     `SingleInstance::TrySignalExistingInstance()` → `SetEvent` on
     `Global\WindowsHelloFix_WakeupEvent`.
   - The daemon's listener thread
     (`ApplicationController::ListenForWakeupSignal`, blocked in
     `WaitForSingleObject`) wakes and calls `m_sink->BringWindowToFront()`
     → marshals to UI thread → `BringWindowToFrontDelegate` (opacity restored,
     shown, activated).
   - Loop exits only when `m_keepListening` flips false or handle is NULL.
2. **`ApplicationController::OnWakeupSignal`** also calls
   `m_sink->BringWindowToFront()`, but as of this baseline **nothing calls
   it** — dormant API kept public.
3. There is deliberately no other un-hide: no tray icon, no hotkey, no
   command-line "show" verb. A user who closed the window must relaunch the
   exe (path #7 in STARTUP.md) to summon it.

## 5. Opacity / taskbar state machine

```
Normal launch            : Opacity 1, taskbar ON , Normal  (visible)
Background/command launch: Opacity 0, taskbar OFF, Minimized (invisible)
    └─ background autostart additionally calls SetWindowVisibleForBackground(true)
Wake signal received     : Opacity 1, taskbar ON , Normal  (delegate restores all three)
User closes (X)          : Hidden(), taskbar OFF        (process still alive)
```

Because the wake delegate is the only restorer, a hidden daemon whose wake
event was somehow broken would be unreachable except by killing the process.

## 6. Single-instance interaction with visibility

`PromptGhostReset` (MessageBox Yes/No) can appear during `MyForm_Load` of a
second instance when the wake-signal attempt fails — i.e., a modal dialog can
be visible while the underlying form is still opacity-hidden. Same for
`ShowBackgroundNotice` (first user-close) and `ShowNoDeviceSelectedMessage`
(toggle with empty dropdown). These are ordinary modal dialogs owned by the
hidden/shown form.

## 7. Uncertainties (documented, not fixed)

- `SetWindowVisibleForBackground(false)` is part of the `IUiSink` contract but
  the implementation ignores non-background values; no caller passes false
  today.
- `UiConstants.h` defines all strings/geometry used by the form, but several
  literals duplicate hard-coded strings inside `InitializeComponent` /
  message boxes; the constants class is only partially wired in.
