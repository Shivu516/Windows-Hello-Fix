---

name: hellofix-refactoring
description: Safely refactor Windows Hello Fix while preserving runtime behavior, state ownership, timing, Windows API semantics, and the current known-good architecture. Use for source extraction, module restructuring, architectural cleanup, and behavior-preserving refactors.
compatibility: opencode
-----------------------

# HelloFix Refactoring

## Purpose

This skill governs **architectural and source-level refactoring** of Windows Hello Fix.

The goal is to improve maintainability without accidentally changing runtime behavior.

Windows Hello Fix is a Windows device-control application whose behavior depends on exact:

* object lifetime
* state ownership
* initialization order
* Windows message handling
* SetupAPI behavior
* Configuration Manager behavior
* WTS notification timing
* power-event timing
* synchronization
* sleeps/retries
* command-line modes
* Task Scheduler interaction

Therefore a refactor must be treated as a **behavior-preservation task**, not merely a code-cleanup task.

---

# 1. Core Principle

The default rule is:

> **Refactor structure before changing semantics.**

A refactor should change:

* where code lives
* how files are organized
* how declarations are separated from implementations
* how dependencies are expressed

without changing:

* what the application does
* when it does it
* which thread does it
* what state it uses
* what Windows API it calls
* what order operations occur in
* how long hardware operations wait
* how errors are handled

---

# 2. Current Architecture Is the Baseline

The current source architecture is the known-good baseline.

Preserve it unless the requested task explicitly changes architecture.

In particular, preserve the current `src/core` structure by default:

```text
src/core/
    MyForm.h
    MyForm_Camera.cpp
    MyForm_Config.cpp
    MyForm_Core.cpp
    MyForm_Events.cpp
    MyForm_System.cpp
    MyForm_UI.cpp
```

Do not rename, merge, or split these files merely for stylistic reasons.

Architecture may evolve when a real problem justifies it.

When it does:

```text
Investigate
→ explain problem
→ identify affected behavior
→ design change
→ plan
→ obtain approval
→ implement incrementally
→ build
→ test
```

---

# 3. Behavioral Reference

When available, use:

```text
release-v2.0/
```

as a historical behavioral reference.

Particularly important:

```text
release-v2.0/MyForm.h
```

If the current implementation is being compared against that reference, distinguish:

* intentional improvements
* architectural extraction
* genuine behavioral regressions

Do not blindly revert current code merely because the historical implementation differs.

---

# 4. Preserve State Ownership

This is one of the highest-priority rules.

Before moving code, determine:

* who owns the state
* who initializes it
* who destroys it
* who reads it
* who writes it

Do not accidentally turn:

```text
one authoritative state value
```

into:

```text
two synchronized copies
```

Avoid introducing mirrored state such as:

```cpp
MyForm::isMonitoring
ApplicationController::isMonitoring
```

unless there is an explicit, justified design requiring both.

If state can remain owned by the existing object, prefer that.

---

# 5. Preserve Object Lifetime

Before extracting functionality, determine:

* constructor order
* destructor order
* finalizer behavior
* ownership
* handle lifetime
* thread lifetime
* UI lifetime
* notification-registration lifetime

Do not assume two architectures are equivalent because they eventually call the same functions.

These are potentially behavior-changing:

```text
MyForm owns resource
vs.
Controller owns resource

synchronous lifetime
vs.
shared_ptr lifetime

constructor initialization
vs.
lazy initialization
```

---

# 6. Preserve Execution Order

Never reorder operations during a behavior-preserving refactor unless the task explicitly requires it.

Pay attention to:

* initialization
* configuration loading
* camera discovery
* camera restoration
* event registration
* wake listener startup
* WTS registration
* power notification registration
* UI initialization
* shutdown
* cleanup

A function moved to another file must still execute in the same position in the runtime call graph.

---

# 7. Preserve Thread Context

Do not introduce new concurrency merely to make code "cleaner."

Do not introduce:

* `std::thread`
* task queues
* worker pools
* async wrappers
* detached workers
* callbacks that change execution context

unless the original architecture already used them or the task explicitly requests a concurrency change.

A synchronous operation should remain synchronous.

A UI-thread operation should remain a UI-thread operation.

A WndProc operation should remain in the message-processing context.

---

# 8. Preserve Timing

Timing is behavior in this application.

Do not casually modify:

```text
Sleep()
WaitForSingleObject()
retry counts
verification intervals
cooldowns
timeouts
startup delays
power-event delays
camera recovery delays
```

Do not:

* remove a sleep because it seems unnecessary
* move a sleep to another function
* combine several waits
* replace a blocking operation with async execution
* shorten retries
* increase/decrease cooldown values

unless the task explicitly concerns that timing.

---

# 9. Protect Camera Operations

Camera code is a protected subsystem.

Do not refactor camera operations together with an unrelated change.

Protect:

* device enumeration
* target selection
* SetupAPI calls
* CfgMgr calls
* disabled-state detection
* verification
* retry logic
* recovery
* hardware-state transitions
* error tracking

When moving camera functions, preserve their bodies and call relationships wherever possible.

---

# 10. Protect Event Semantics

Preserve:

* WTS lock/unlock behavior
* power/lid/button behavior
* Windows message codes
* event filtering
* event deduplication
* event cooldowns
* event ordering

Do not turn a direct event path into an indirect abstraction just because it appears cleaner.

---

# 11. Protect Single-Instance Semantics

Preserve:

```text
Global\WindowsHelloFix_AppMutex
Global\WindowsHelloFix_WakeupEvent
```

Preserve:

* mutex acquisition
* second-instance detection
* wake event
* background behavior
* interactive behavior
* ghost-reset behavior

Do not introduce competing mutex/event systems.

Do not move single-instance state into a second independent subsystem without explicit architectural approval.

---

# 12. Protect UI Visibility

Background and interactive launches have deliberately different behavior.

A refactor must preserve:

### Background

* no visible GUI
* no taskbar button
* no focus stealing

### Interactive

* GUI visible
* normal activation
* existing second-instance behavior

Be especially careful with:

```text
Opacity
Visible
ShowInTaskbar
WindowState
BringToFront
Activate
```

Do not move visibility logic into unrelated abstractions without tracing the full lifecycle.

---

# 13. Preserve Command-Line Contract

Treat existing command-line arguments as API contracts.

Examples:

```text
--background
--disable-camera
--enable-camera
--restore-camera
--repair-camera
```

Do not:

* rename arguments
* change aliases
* change case handling
* change processing order
* merge modes
* move command handling past mutex logic

without explicit task scope.

---

# 14. Preserve Installer/Task Relationships

Refactoring C++ code can affect the executable's interaction with:

* NSIS
* Task Scheduler
* command-line modes
* startup
* uninstall

Do not assume installer behavior is independent of the application.

If a refactor changes an executable entry point, argument, exit code, privilege behavior, or background mode, inspect the installer.

---

# 15. Mechanical Extraction Is Preferred

When splitting a large file:

Prefer:

```text
original function
    ↓
same function
    ↓
different source file
```

over:

```text
original function
    ↓
new architecture
    ↓
new abstraction
    ↓
rewritten equivalent
```

For example:

```cpp
// MyForm_Camera.cpp

#include "MyForm.h"

bool MyForm::DisableTargetCameraHardware(...)
{
    // Preserve original implementation.
}
```

This is safer than creating a new `CameraController` that copies its state out of `MyForm`.

---

# 16. Avoid Premature Abstraction

Do not create interfaces, factories, managers, services, repositories, or controllers simply because they sound architectural.

Every abstraction introduces:

* ownership questions
* lifetime questions
* dependency changes
* possible state duplication
* possible threading changes
* additional call paths

Introduce an abstraction only when it solves a demonstrated problem.

---

# 17. Avoid "Cleanup During Refactoring"

Do not combine refactoring with:

* naming sweeps
* formatting changes
* dead-code removal
* logging redesign
* error-handling redesign
* API replacements
* performance optimization
* dependency upgrades

These should be separate changes.

Otherwise regressions become difficult to attribute.

---

# 18. Function Extraction Checklist

For every extracted function:

1. Record original file/location.
2. Record original function signature.
3. Identify state dependencies.
4. Identify global/static dependencies.
5. Identify callers.
6. Identify called functions.
7. Move implementation.
8. Preserve signature where possible.
9. Preserve body where possible.
10. Build immediately.
11. Verify no duplicate definition exists.
12. Verify no state was duplicated.

---

# 19. Translation-Unit Safety

Moving code between `.cpp` files can change:

* internal linkage
* `static` storage
* anonymous namespaces
* inline behavior
* declaration visibility
* macro visibility
* initialization order

Before moving a global/static helper, determine whether it was:

* file-local
* class-local
* translation-unit-local
* globally shared

Do not blindly replace `static` with `extern`.

If a shared variable is required, verify that exactly one storage instance exists.

---

# 20. Header Discipline

Headers should contain declarations and required type definitions.

Avoid putting substantial runtime implementation into headers unless there is a concrete reason.

However, do not move inline code merely for aesthetic reasons when doing so could change semantics.

When moving an inline member function out-of-line:

* preserve its signature
* preserve access level
* preserve behavior
* ensure exactly one definition exists

---

# 21. Project File Discipline

When adding files:

Update the actual Visual Studio project:

```text
Windows_Hello_Fix_v2_1.vcxproj
Windows_Hello_Fix_v2_1.vcxproj.filters
```

Do not rely on files merely existing in the directory.

After modifying project membership:

```text
Rebuild Release|x64
```

---

# 22. Incremental Refactoring

For large architectural work:

### Phase 1

Inventory.

### Phase 2

Dependency mapping.

### Phase 3

Extract a small subsystem.

### Phase 4

Build.

### Phase 5

Verify.

### Phase 6

Extract next subsystem.

### Phase 7

Build.

### Phase 8

Final comparison.

Do not perform a massive blind restructuring and only build at the end.

---

# 23. Regression Detection

A successful compilation is necessary but insufficient.

After a refactor, inspect:

* git diff
* call graph
* state ownership
* initialization order
* thread context
* timing
* Windows API calls
* command-line flow
* installer assumptions

If practical, perform runtime tests appropriate to the changed subsystem.

---

# 24. Behavioral Comparison

When a reference implementation is available, compare:

### Startup

* normal launch
* background launch
* command-worker launch
* second instance

### Camera

* enumeration
* disable
* enable
* recovery
* verification

### Events

* lock
* unlock
* suspend
* resume
* shutdown

### UI

* hidden startup
* GUI launch
* second-instance wake
* FormClosing

### System

* mutex
* wake event
* process exit
* cleanup

A refactor should preserve these paths unless the task explicitly changes them.

---

# 25. Documentation During Refactoring

When documentation is not part of the task:

> Do not rewrite documentation merely because the architecture changed internally.

When documentation is explicitly requested:

* document the current real structure
* preserve exact filenames
* preserve exact folder paths
* document every source file under `src/`
* explain important functions and dependencies
* document runtime flows
* distinguish observed behavior from inferred behavior

Documentation should not dictate an architecture that the source does not actually use.

---

# 26. Plan.md

For substantial refactoring:

Read:

```text
docs/Plan.md
```

before starting.

For major architecture work, maintain it through:

```text
Investigate
→ Plan
→ Review
→ Implement
→ Build
→ Test
```

Do not use `Plan.md` as a changelog.

Do not hide failed experiments.

---

# 27. Git Safety

Before work:

```text
git status --short
git branch --show-current
git rev-parse HEAD
```

Never:

* commit without instruction
* reset
* revert
* checkout another branch
* discard work
* clean untracked files

unless explicitly authorized.

---

# 28. Final Refactoring Report

Every significant refactoring task should report:

## Files added

Exact paths.

## Files modified

Exact paths.

## Files deleted

Exact paths.

## State ownership

What owns important state before and after.

## Behavioral changes

Explicitly state whether any behavior changed.

## Timing changes

Explicitly state whether any timing changed.

## Threading changes

Explicitly state whether threading changed.

## Project changes

Which `.vcxproj`/`.filters` files changed.

## Build

Configuration, platform, warnings, errors, executable.

## Tests

Actual tests performed.

## Remaining risks

Anything not verified.

---

# 29. Stop Condition

If a proposed refactor requires:

* changing camera semantics
* changing event ordering
* changing thread context
* changing state ownership
* changing startup behavior
* changing Task Scheduler
* changing installer logic

STOP the refactor and report the conflict unless those changes are explicitly included in the requested task.

Do not silently expand scope.

---

# FINAL PRINCIPLE

The safest refactor is:

> **same behavior, same state, same timing, same APIs, better file organization.**

Do not optimize while extracting.

Do not redesign while extracting.

Do not "fix" unrelated bugs while extracting.

Make the smallest structural change that produces a cleaner source organization, then verify the resulting program.
