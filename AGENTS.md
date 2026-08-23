# AGENTS.md — Instructions for AI Coding Agents

This repository is actively maintained with the help of AI agents. Previous
agent-driven changes introduced regressions in startup behavior, GUI
visibility, Task Scheduler integration, NSIS installation, and camera control.
The rules below exist because of that history. **They are mandatory.**

---

## Project overview

**Windows Hello Fix v2.0** is a C++/CLI (.NET 4.7.2, WinForms, x64) utility
that disables the RGB camera's PnP device node on workstation lock / suspend /
shutdown and re-enables it on unlock / resume, forcing Windows Hello to use
the IR sensor.

- Single executable: `Windows_Hello_Fix_v2_0.exe`, always elevated
  (`requireAdministrator`).
- Runs as: visible GUI, hidden background daemon (`--background`), or
  short-lived command worker (`--disable-camera`, `/enable-camera`,
  `/restore-camera`, `/repair-camera`).
- Live code: `main.cpp` + everything under `src/` (application, camera,
  config, events, system, ui, utilities). Root `MyForm.h` is a shim only.
- Authoritative documentation: `docs/ARCHITECTURE.md`,
  `docs/STARTUP.md`, `docs/CAMERA_HARDWARE.md`,
  `docs/SESSION_MONITORING.md`, `docs/GUI.md`, `docs/INSTALLER.md`,
  `docs/TASK_SCHEDULER.md`, `docs/LOGGING.md`, `docs/KNOWN_ISSUES.md`,
  per-module references in `docs/src/*.md`.
- Older docs under `docs/architecture/`, `docs/refactoring/`, and
  `src/README.md` describe a pre-migration state and are historical; do not
  treat them as descriptions of current code.

## Architecture rules — who owns what

| Responsibility | Owner | Nobody else may… |
|---|---|---|
| Message pump, controls, WndProc dispatch | `src/ui/MyForm.*` | …call SetupAPI/CfgMgr directly |
| Startup policy, monitoring state, camera decisions, single-instance/wake | `src/application/ApplicationController.*` | …be bypassed by UI doing hardware work |
| Argument classification | `src/application/CommandLine.*` | …parse args ad hoc elsewhere |
| Device discovery / toggle / verify / recovery | `src/camera/*` (native) | …log or touch managed types inside these files |
| config.txt + diagnostic.log I/O | `src/config/ConfigStore.*`, `ConfigPaths.*` | …write those files from other modules |
| WTS/power registration, message decode, dedup | `src/events/*` | …register notifications ad hoc |
| Mutex/wake event, taskkill, elevation queries | `src/system/*` | …create other named kernel objects casually |
| Dependency direction | `ui → application → {camera, config, events, system} → utilities`; UI reached back only via `IUiSink` | …introduce reverse dependencies |

## Protected components — investigate before touching

Changes in these areas have historically caused regressions. Before editing
any of them you must investigate, explain the blast radius in your report, and
get explicit human approval:

1. **Camera hardware control** — `src/camera/*`. Retry/stage/verification
   semantics are load-bearing; see `docs/CAMERA_HARDWARE.md`.
2. **WTS session handling & lock/unlock logic** — `NotificationRegistrar`,
   `WinEventDecoder::DecodeSessionEvent`, `MyForm::WndProc`,
   `HandleSessionEvent`. Raw codes: lock=7, unlock=8.
3. **Power/lid/button handling** — same files plus GUID registration;
   suspend/resume latching (`m_isAlreadyDisabled`) exists for a reason.
4. **Startup architecture & command-line contract** — `main.cpp`,
   `ApplicationController::Initialize`, `CommandLine`. Installer tasks,
   finish-page launch, and manual launches all depend on exact argument
   strings and ordering (command check happens BEFORE mutex check).
5. **Single-instance behavior** — mutex/event names
   (`Global\WindowsHelloFix_AppMutex`, `Global\WindowsHelloFix_WakeupEvent`),
   wake-listener thread, ghost-reset flow.
6. **GUI visibility** — `main.cpp` hide block,
   `BringWindowToFrontDelegate` (the ONLY un-hide), `SetWindowVisibleForBackground`,
   FormClosing hide-on-close. There is no tray icon; breaking the wake path
   makes a hidden daemon unreachable.
7. **Task Scheduler setup** — installer-generated PowerShell block; task names
   `WindowsHelloFix*`, trigger type 11 StateChange 7/8, RunLevel Highest.
   Renames break uninstall cleanup.
8. **NSIS installer** — `x64/Release/install_script.nsi`; ordering of
   taskkill → file deploy → `/restore-camera` passes → task registration →
   warm-up is deliberate.
9. **Shutdown/end-session behavior** — `HandleSystemEnd`, `Shutdown(bool)`;
   destructor/finalizer asymmetry is documented in KNOWN_ISSUES #3 — do not
   "simplify" it without analysis.

## Change-management rules

Every functional change must follow ALL of these:

1. **Investigate before modifying.** Read the relevant `docs/*.md`, then the
   actual source. Docs first, code second, opinions last.
2. **Identify the exact root cause** before proposing any edit. Symptom-level
   patches are rejected.
3. **Minimize modified files.** If a fix needs more than a handful of files,
   stop and present a plan first.
4. **Preserve working architecture.** Do not move responsibilities between
   components.
5. **No unrelated refactoring.** No drive-by renames, formatting sweeps, or
   "modernization". Dead code listed in `docs/KNOWN_ISSUES.md` stays until
   explicitly approved for removal.
6. **Build after changes.** Build the solution (`Windows_Hello_Fix_v2_0.sln`,
   Release|x64 preferred) and report success/failure verbatim.
7. **Report exact files changed** (full paths).
8. **Report exact behavioral changes** — startup paths affected, log events
   added/removed/renamed, timing changes.
9. **Test affected startup paths**: at minimum manual GUI launch,
   `--background` launch, second-instance wake, and one command mode; add
   lock/unlock when session code changed. Record results in your report.
10. **Never silently modify unrelated architecture.** Anything unexpected you
    had to touch gets its own paragraph in the report.

## Git safety rules

- **Never commit unless explicitly instructed** in the current conversation.
- **Never reset, revert, checkout-away, or force-push branches** without
  explicit permission naming the branch/ref.
- **Never discard user changes** (`git restore`, `git clean`) without
  confirmation, even if they look accidental.
- **Never resolve binary conflicts automatically** (icons, .rc, exe, .metagen).
- **Never modify release artifacts** under `x64/Release/` except
  `install_script.nsi` when the task is installer work.
- **Always report dirty working-tree state** (`git status --short`) before and
  after your work so the user knows exactly what you left behind.

## Investigation-first rule (bugs)

```
INVESTIGATE        read docs + trace the actual code path (cite file:line)
→ REPORT ROOT CAUSE
→ PROPOSE MINIMAL FIX
→ WAIT FOR APPROVAL      ← hard stop; no implementation before this
→ IMPLEMENT
→ BUILD
→ TEST (affected startup paths)
→ REPORT (files, behavior, test results)
```

Do not jump from symptom to implementation. If the root cause cannot be proven
from code reading, say so and instrument via `diagnostic.log`
(`%APPDATA%\Windows Hello Fix\diagnostic.log` — catalog in `docs/LOGGING.md`)
rather than guessing.

## Plan.md Requirement

Every AI agent working on this repository MUST:

1. Read `docs/Plan.md` before making changes.
2. Treat `docs/Plan.md` as the current active implementation plan.
3. Update `docs/Plan.md` when:
   - the active implementation strategy changes
   - a phase is completed
   - a major assumption is disproven
   - a new major blocker is discovered
4. Keep `docs/Plan.md` focused on CURRENT and UPCOMING work.
5. Do not use Plan.md as a changelog or historical dump.
6. Do not rewrite unrelated sections merely for stylistic reasons.
7. Never modify Plan.md to hide failed experiments or unsuccessful changes.
8. After completing a planned phase, mark it complete and record the resulting state before starting the next major phase.

### Planning-before-implementation rule

For architectural or behavioral changes:

```
INVESTIGATE
→ DOCUMENT
→ PLAN
→ REVIEW
→ IMPLEMENT
→ BUILD
→ TEST
→ COMMIT
```

Agents must not skip directly from a bug report to a broad implementation.

### Minimal-change rule

An agent must:
- modify the minimum number of files necessary
- avoid unrelated refactoring
- preserve existing working behavior
- explicitly justify every additional file touched

### Protected-camera rule

The camera hardware path is a protected subsystem.

Agents must NOT modify camera hardware/recovery code merely because it is adjacent to a startup/GUI issue.

## Quick reference

- Config: `%APPDATA%\Windows Hello Fix\config.txt` (`monitoring=0|1` /
  `device=<id>`)
- Log: `%APPDATA%\Windows Hello Fix\diagnostic.log`
- Named objects: mutex + auto-reset wake event (names above)
- Scheduled tasks: `WindowsHelloFix` (logon daemon), `_Lock`/`_Unlock`
  (session-state failsafes), `_LogCleanup` (daily truncate; suspected path
  defect — see TASK_SCHEDULER.md)
- Known hazards list: `docs/KNOWN_ISSUES.md` — read it before assuming
  anything odd-looking is a bug you should fix.
