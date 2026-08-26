# AGENTS.md — Windows Hello Fix

Practical, persistent instructions for AI agents working on this repository.

## 1. Baseline

This branch (`release-v2.0-restructured`) contains a **behavior-preserving, mechanical extraction** of the original monolithic `release-v2.0/MyForm.h` into a modular `src/ui/` tree. The extraction was verified by static analysis and a successful `Release|x64` build; runtime behavior is intended to be identical to v2.0.

Active source tree (inspect with `git ls-files` before assuming more):

```
main.cpp
MyForm.h                 # SHIM: #include "src/ui/MyForm.h" (do not delete; preserves include path)
ProductionUtilities.h    # legacy/unused helper; not part of build flow
Windows_Hello_Fix_v2_0.{vcxproj,vcxproj.filters,sln}
Windows_Hello_Fix_v2_0.rc / _resources.rc / resource.h / resource1.h / app.manifest
src/ui/
  MyForm.h            # declaration-only: class, CameraDeviceInfo, extern globals, forward decls
  MyForm_Camera.cpp   # native camera pipeline + Disable/Enable/Restore members
  MyForm_Config.cpp   # config.txt + diagnostic.log + save/load + target resolution
  MyForm_Core.cpp     # ctor/dtor/finalizer/InitializeComponent/MyForm_Load
  MyForm_Events.cpp   # WndProc: session/power/shutdown dispatch
  MyForm_System.cpp   # command parsing + wake listener
  MyForm_UI.cpp       # FormClosing + btnToggle_Click
docs/                  # current project documentation (see docs/ rules)
release-v2.0/          # canonical v2.0 reference (gitignored; DO NOT MODIFY)
x64/Release/install_script.nsi   # NSIS installer (load-bearing)
```

`MyForm` remains the **single, central state owner**. No controller classes were introduced.

## 2. Architecture policy

- The `src/ui/` files above are the **known-good modular baseline**. Preserve their exact filenames and current responsibilities unless a task explicitly concerns architecture.
- Do **not** casually rename, split into many smaller files, merge back into a monolith, or introduce a controller/abstraction layer merely for aesthetics.
- Adding new files/folders or improving module boundaries **is allowed** when there is a real engineering reason. Follow the architectural-change rule (§8) for such work.
- New features should go in new appropriate files/folders rather than being forced into the existing seven.

## 3. Behavioral source of truth

- For behavior-sensitive investigation, compare against `release-v2.0/MyForm.h` (known-good reference). It is gitignored and must not be modified.
- If current and reference behavior differ unexpectedly: investigate → determine if intentional → identify exact behavioral impact → do **not** silently revert or rewrite.

## 4. Core principles

1. Behavioral correctness. 2. Preserve known-good behavior. 3. Minimal changes. 4. Clear evidence before modification. 5. Small, reviewable changes. 6. Proper build verification. 7. Clear documentation.

Philosophy: **Reliable behavior first, clean architecture second, optimization third.** Prefer "slightly less elegant but behaviorally proven" over "elegant but behaviorally uncertain."

## 5. Protected behavior (investigate before modifying)

- **Camera hardware** — SetupAPI / Configuration Manager calls, device selection, verification, retries, recovery sequences, `Sleep` timings, error handling. Startup/UI/installer issues must NOT trigger unrelated camera refactoring.
- **Session & power events** — WTS notifications, lock/unlock, suspend/resume, lid/button handling, `WndProc` dispatch, cooldown/dedup (1500 ms windows), `isAlreadyDisabled` static. Preserve ordering and timing.
- **Single-instance** — named objects `Global\WindowsHelloFix_AppMutex` and `Global\WindowsHelloFix_WakeupEvent`. Do not introduce another mutex/event system; preserve wake and second-instance behavior.
- **GUI visibility** — hidden background startup (`--background`, `Opacity=0`), interactive startup, taskbar behavior, wake, `FormClosing` (cancel → minimize to background, `isBackgroundMode`). Do not make background launches visible.
- **Startup / command-line** — arguments `--background`, `--disable-camera`, `--enable-camera`, `--restore-camera`, `--repair-camera` are behavior-sensitive; preserve names and processing order.
- **Installer (NSIS)** — `x64/Release/install_script.nsi`: task names/args, privilege level, triggers, install order, uninstall cleanup, executable deployment, post-install launch. Inspect the full flow before changing.

## 6. No unsafe "cleanup"

Do **not** remove code just because it looks redundant, old, ugly, slow, unused, or duplicated. First determine why it exists. Examples in this repo: `TryEnterHardwareToggleCooldown` (dormant cooldown path), `static`→`extern` globals (required for multi-TU extraction), duplicated dtor/finalizer cleanup (C++/CLI pattern). Document questionable code before removing it.

## 7. Investigation-first & minimal-change rules

For bugs:
```
INVESTIGATE → TRACE ACTUAL CODE PATH → ROOT CAUSE → EVIDENCE → MINIMAL FIX → BUILD → TEST → REPORT
```
Distinguish **Confirmed / Likely / Possible / Unverified**. Static compilation ≠ behavioral correctness. Use source tracing, git history, diagnostics, runtime logs, Windows state inspection, targeted tests.

Each functional change should touch the smallest reasonable set of files and preserve unrelated behavior. If more files than expected are needed, explain why each is required.

## 8. Architectural-change rule

```
Investigate → map responsibilities → identify problem → docs/Plan.md → propose → approval → implement incrementally → build → test
```
Do not bundle architecture refactor + camera redesign + installer redesign + startup redesign into one unbounded task. Do not hide unrelated bug fixes inside an architecture change.

> `docs/Plan.md` does not currently exist. Create it only when starting substantial implementation/architecture work, and keep it focused on active/upcoming work, assumptions, and blockers.

## 9. Build & verification

- Use the existing Visual Studio / MSBuild system. **Do not** introduce other build systems (npm, custom generators, JS concatenation) unless explicitly requested.
- Primary verification config: **Release | x64**.
- Clean verify:
  ```
  MSBuild Windows_Hello_Fix_v2_0.vcxproj /p:Configuration=Release /p:Platform=x64 /t:Rebuild
  ```
- After source changes: rebuild, report errors **and** warnings (note baseline `C4793` for `TryEnterHardwareToggleCooldown`/`RecordHardwareToggleTime`), identify the output exe (`Windows_Hello_Fix_v2_0.exe`), and confirm you tested the newly built binary. Compilation success alone is not proof.

## 10. Git safety

- Inspect `git status --short`, active branch, and relevant commit before changes.
- **Do NOT** commit, reset, revert, checkout another branch, discard user changes, run destructive cleanup, or auto-resolve binary conflicts unless explicitly instructed.
- Always report what was modified and show the final diff.

## 11. Release-artifact safety

Treat `x64/Release/` as sensitive. Do not modify binaries/release artifacts unnecessarily. When a task concerns only source, build as needed but distinguish rebuilt local artifacts from tracked source changes. The installer `x64/Release/install_script.nsi` may be modified when the task concerns the installer.

## 12. Documentation rules

- Documentation is first-class but intentional. Read existing `docs/` before modifying behavior.
- When explicitly documenting `src/`, document every source file (`.h`/`.cpp`) and follow the **actual** structure (see §1). Explain purpose, responsibilities, functions, state, dependencies, callers, Windows APIs, side effects, threading, lifecycle, error handling, logging, and module interactions. Explain in logical blocks and execution order; do not dump full source.
- Documentation-only tasks must **not** modify `.cpp`, `.h`, project/build, or installer files. If a bug is found while documenting, record it as a finding; do not fix unless explicitly asked.
- Use the existing `docs/` directory; avoid duplicates; preserve useful existing docs; mark historical docs as historical. Do not rewrite everything for one addition.

## 13. Testing

Test the behavior affected by a change:
- GUI/startup: manual launch, background launch, second-instance behavior, hidden/visible.
- Camera/events: lock, unlock, suspend/resume, camera state, `diagnostic.log`.
- Installer: install, startup registration, task creation, uninstall, residue cleanup.

Do not claim runtime behavior was tested if only static inspection was performed.

## 14. Reporting

Every implementation report must include: files changed (exact paths); why each changed; behavioral impact (what changed vs unchanged); build (config, platform, errors, warnings, exe path); tests performed + results; remaining uncertainty.

## Agent Skills

Project-specific skills are stored under `.claude/skills/`.

When a task clearly matches an available skill, inspect and follow the relevant
skill's `SKILL.md` and its referenced supporting resources before performing
the task.

Do not assume a skill applies when it does not match the task.

When a skill references additional files such as `references/`, `examples/`,
or `scripts/`, load or use those resources when the skill instructs you to do so.

## Quick reference

| Item | Value |
|---|---|
| Application | `Windows_Hello_Fix_v2_0.exe` |
| Build | `Release` \| `x64` (MSBuild) |
| Config | `%APPDATA%\Windows Hello Fix\config.txt` |
| Diagnostic log | `%APPDATA%\Windows Hello Fix\diagnostic.log` |
| Reference impl | `release-v2.0/` (do not modify) |
| Primary source | `main.cpp`, `src/` |
| Installer | `x64/Release/install_script.nsi` |
| Sync objects | `Global\WindowsHelloFix_AppMutex`, `Global\WindowsHelloFix_WakeupEvent` |
| Docs | `docs/` (per-file under `docs/files/`, architecture under `docs/`) |
