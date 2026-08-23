# Known Architectural Inconsistencies (documented, NOT fixed)

Baseline: branch `test`, commit `acc37d8`. This file lists oddities discovered
while writing the as-built documentation. **Per project rules none of these
have been changed.** Each entry states where the code stands today and what is
uncertain — investigation and approval are required before any fix.

## 1. Stale documentation contradicts the build

- `src/README.md` says `src/` is "architectural scaffolding … not compiled"
  and that root-level files are authoritative. Reality: every `src/` file is
  compiled (vcxproj), root `MyForm.h` is a shim to `src/ui/MyForm.h`, and no
  monolithic implementation exists anymore.
- `docs/architecture/current-codebase.md` describes the pre-migration monolith
  (`MyForm.h` ~1300 lines, `ProductionUtilities.h`) that no longer exists.
- Impact: agents trusting these docs will look for code that isn't there. The
  docs in `docs/*.md` + `docs/src/*` supersede them.

## 2. Startup camera call passes config flag into cycleDevice

`MyForm_Load` (src/ui/MyForm.cpp:214):

```cpp
m_controller->EnableTargetCameraHardware(autoStart);
```

The parameter is `cycleDevice`. When config has `monitoring=1` (`autoStart`
true), every launch performs a full off/on power cycle (350/900/500 ms sleeps)
instead of a plain enable. Whether this was intentional ("aggressive recovery
on autostart") or a mistake cannot be determined from the code.

## 3. Double/triple shutdown execution

- `~MyForm` calls `m_controller->Shutdown(...)`; then `delete m_controller`
  runs `~ApplicationController` which calls `Shutdown(...)` **again**.
- `!MyForm` calls `Shutdown` once more if the finalizer path runs.
- `!ApplicationController` duplicates cleanup logic inline instead of calling
  Shutdown (asymmetric with its destructor).
Consequences: enable/disable runs multiple times at teardown (cheap due to
already-state short-circuits) and handle release relies on NULL guards.

## 4. Case-sensitivity mismatch between command-line checks

`CommandLine::IsBackgroundLaunch` compares case-insensitively;
`CommandLine::ShouldHideWindow` compares case-sensitively (`String::operator==`).
A mixed-case argument could start background monitoring without hiding the
window.

## 5. Failsafe tasks race the daemon

`--disable-camera` / `--enable-camera` command handling occurs **before** mutex
acquisition in `Initialize`, so scheduled lock/unlock tasks run concurrently
with the daemon and both may toggle hardware for the same event. Per-process
cooldowns can't coordinate across processes. Appears intentional (failsafe
design), but it is undocumented in code comments and doubles device-node churn.

## 6. Ghost reset kills by image name including itself

`ProcessUtils::KillHelloFixProcess` → `taskkill /F /IM Windows_Hello_Fix_v2_0.exe /T`
matches every process of that name, including the caller performing the reset.
The subsequent `Sleep(500)` + `Application::Restart()` sequence races process
termination. Works in practice per commit history, but ordering is not
guaranteed.

## 7. Resource/icon ID mismatch persists

Compiled resources: `Windows_Hello_Fix_v2_0_resources.rc` + `resource1.h`
(`IDI_ICON1 = 114`). Form includes `resource.h` (`IDI_ICON1 = 102`) and loads
`MAKEINTRESOURCE(IDI_ICON1)` → requests ID 102 while the compiled resource
provides 114 → icon load likely fails silently (default icon shown).
Uncompiled legacy `Windows_Hello_Fix_v2_0.rc` references absolute machine
paths and is not part of the build.

## 8. Dormant / dead code kept in tree

| Item | Location | Status |
|---|---|---|
| `ApplicationController::OnWakeupSignal` | ApplicationController.cpp:273 | No callers |
| `ApplicationController::UnregisterNotifications` | ApplicationController.cpp:395 | No callers (cleanup done via Shutdown/finalizer) |
| `m_cachedCamerasPlaceholder` | ApplicationController.h:50 | Unused placeholder |
| `cameraExpectedDisabled` (both classes) | MyForm.h:66 / ApplicationController.h:47 | Written, never read |
| `SingleInstance::CloseHandleIfValid` | SingleInstance.cpp:39 | Unused helper |
| `ProductionLogger` (entire class) | src/utilities/Logging.* | Compiled, never invoked; OutputDebugStringW only |

## 9. Installer/uninstaller mismatches

- Uninstall deletes `$INSTDIR\README.rtf`; install deploys `README.html` →
  README.html remains after uninstall.
- Uninstall deletes task `WindowsHelloFix_Wake`, never created by the current
  installer (legacy-name cleanup; harmless but confusing).
- LogCleanup task's `cmd.exe` argument uses `$APPDATA` syntax cmd.exe does not
  expand (see TASK_SCHEDULER.md §1.4) — daily log truncation may not work.
- Installer mutex lacks `Global\` prefix → per-session installer singletons.

## 10. Session registration timing

WTS session notification registration happens after startup restore, power
registration, and wake-thread start, with up to ~3 s of retries. Lock events
during early startup are missed silently (no queued replay).

## 11. UI thread blocking during events

All camera operations and their sleeps run on the UI thread inside WndProc /
Load handlers (up to several seconds). The async queue proposed by
`WndProc_Redesign.txt` was never integrated. Any change here alters event
timing semantics and needs careful testing.

## 12. Lid/button state ignored

Any lid-switch or power-button notification triggers the suspend-style disable
regardless of the payload's new state (e.g., opening the lid also disables).
Documented in SESSION_MONITORING.md §7; unclear whether intentional.

---

Reminder for future agents: fixing any item above requires the full
investigation-first workflow from `AGENTS.md`. This file is a map of hazards,
not a to-do list.
