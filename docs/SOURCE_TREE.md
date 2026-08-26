# Source Tree

Current `src/` layout (documented as-is):

```
src/
└── ui/
    ├── MyForm.h            Declaration-only: MyForm class, CameraDeviceInfo, extern globals, forward decls
    ├── MyForm_Camera.cpp   Native camera pipeline + Disable/Enable/Restore members (470 lines)
    ├── MyForm_Config.cpp   Config paths, diagnostic log, save/load, target resolution (148 lines)
    ├── MyForm_Core.cpp     ctor/dtor/finalizer/InitializeComponent/MyForm_Load (407 lines)
    ├── MyForm_Events.cpp   WndProc: session/power/shutdown dispatch (110 lines)
    ├── MyForm_System.cpp   Command parsing + wake listener (53 lines)
    └── MyForm_UI.cpp       FormClosing + btnToggle_Click (56 lines)
```

Other relevant files (outside `src/`):

```
MyForm.h                              Root shim -> #include "src/ui/MyForm.h"
main.cpp                             Entry point (MyForm form + Application::Run)
ProductionUtilities.h                Legacy/unused helper header (not part of the build flow)
Windows_Hello_Fix_v2_0.vcxproj       MSBuild project (lists all src/ui files)
Windows_Hello_Fix_v2_0.vcxproj.filters
release-v2.0/MyForm.h                Canonical reference (untouched)
```

## Per-file responsibility (one line)

| File | Responsibility |
|---|---|
| `src/ui/MyForm.h` | Class/struct/extern declarations; compile-time GUID/constant definitions. |
| `src/ui/MyForm_Camera.cpp` | Camera discovery, SetupAPI & CfgMgr enable/disable, verification, retry/recovery, cooldown globals. |
| `src/ui/MyForm_Config.cpp` | `%APPDATA%` config + diagnostic log, save/load/parse, target-device resolution. |
| `src/ui/MyForm_Core.cpp` | Object lifetime: constructor, destructor, finalizer, UI init, full startup sequence. |
| `src/ui/MyForm_Events.cpp` | `WndProc`: deduplicated handling of shutdown, suspend/resume, lid/button, lock/unlock. |
| `src/ui/MyForm_System.cpp` | Command-line verb detection; background wake-listener thread. |
| `src/ui/MyForm_UI.cpp` | Form close (minimize-to-background) and start/stop monitoring button. |
