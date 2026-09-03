# `src/watchdog/RecoveryLoopFailsafe.cpp` — Startup Check, Poll & Bounded Retry

**Path:** `src/watchdog/RecoveryLoopFailsafe.cpp`
**Lines:** 237
**Included by:** built directly (translation unit); `#include "RecoveryLoopFailsafe.h"`, `#include "../core/MyForm.h"`
**Build relationship:** `ClCompile` in vcxproj, filter `Source Files\src\watchdog`.

## Purpose

Implements the fast-verifier state machine: a one-shot 5 s startup check, a 30 s backup poll, and a 5 s linear-retry recovery loop — all enable-only through `RecoverCameraHardware(target, false)` + `VerifyCameraHardwareState(target, false)`.

Control-flow summary:

```
main.cpp wires form.Load → OnOwnerLoad → Arm()
  → startupTimer 5 s → OnStartupTick → log StartupVerification → RequestRecoveryCheck
  → pollTimer every 30 s → OnPollTick → (same detect-and-schedule inline)
  → detect Disabled while ExpectedEnabled → PendingVerification + retryTimer 5 s → OnRetryTick
    → guards re-checked + still disabled → Recover(false) + Verify
      → success → Recovered log, failures=0, cooldown stamped, Idle
      → failure → RecoveryFailed log, failures++, retry in 5 s → PendingVerification
      → 3 failures → MaxAttempts log, Idle, failures=0 (poll continues at 30 s)
main.cpp wires form.FormClosing → OnOwnerClosing → Disarm()
```

## Constructor (6–24)

### Purpose
Bind the owner, zero state, create the three timers stopped and wired.

### Inputs
`ownerForm` (`MyForm^`).

### Control flow
Initializer list: `state=Idle`, `consecutiveFailures=0`, `lastRecoveryTick=0`, `isArmed=false`. Then create `pollTimer` (30 s → `OnPollTick`), `retryTimer` (5 s → `OnRetryTick`), `startupTimer` (5 s → `OnStartupTick`) — all stopped.

### Side effects
Three managed timers on the CLR heap. No logging, no hardware access.

## Destructor (26–30) & finalizer (32–35)

### Purpose
Deterministic + GC-safety-net teardown (C++/CLI pattern, mirrors `MyForm`'s dtor/finalizer duplication deliberately).

### Control flow
- `~RecoveryLoopFailsafe()`: `Disarm();` then `this->!RecoveryLoopFailsafe();` — stop timers deterministically, then run the finalizer body.
- `!RecoveryLoopFailsafe()`: `try { Disarm(); } catch (...) {}` — best effort only; a finalizer must never throw.

### Behavioral sensitivity
The dtor's explicit finalizer call matches the codebase's existing C++/CLI idiom (see `MyForm`). Do not "simplify" one away without understanding CLR dispose semantics — the double-Disarm is idempotent by design (`Disarm()` is unconditional and safe to repeat).

## `Arm()` (37–54)

### Purpose
Begin fast verification. Invoked via `OnOwnerLoad` when the daemon form finishes loading.

### Control flow
1. `if (isArmed) return;` — idempotent.
2. `if (owner != nullptr && owner->IsSystemEndingActive()) return;` — never arm during shutdown (contrast `CameraFailsafe::Arm`, which has no such guard because it is armed from inside `MyForm_Load` before `isSystemEnding` can be set; here the event-driven wiring makes the guard worthwhile).
3. `isArmed=true; state=Idle; consecutiveFailures=0; lastRecoveryTick=0;`
4. `owner->LogFailsafe("RecoveryLoop_Start", "Enabled", true)` in try/catch.
5. `pollTimer->Interval = kPollIntervalMs; pollTimer->Start();` — 30 s backup begins.
6. `retryTimer->Stop();` — no retry pending.
7. `startupTimer->Interval = kStartupVerifyMs; startupTimer->Start();` — first check in 5 s.

### Failure behavior
All logging is try/caught; timer ops on valid handles do not throw in practice. A null `owner` is tolerated (step 2 null-checks; step 4 would throw inside try and be swallowed — leaving the loop armed but inert, since every tick re-checks `IsExpectedEnabled()`, which returns `false` for null owner).

## `Disarm()` (56–63)

### Purpose
Stop everything. Called from `OnOwnerClosing`, the dtor/finalizer, and `main.cpp` after `Application::Run`.

### Control flow
Unconditional: `isArmed=false; state=Idle;` then stop each timer in its own `try { } catch (...) { }`. No `isArmed` early-return (unlike `Arm`), no logging — safe to call any number of times from any teardown path.

## `OnOwnerLoad` (65–68) & `OnOwnerClosing` (70–73)

### Purpose
Public event-handler hooks so `main.cpp` can subscribe with `form.Load += ...` / `form.FormClosing += ...` — the seam that avoids editing `src/core`.

### Control flow
Each is a one-line `try { Arm()/Disarm(); } catch (...) {}` forwarder. Parameters are unnamed (`/*sender*/`, `/*e*/`) because they carry no information. Note `OnOwnerClosing` disarms on **every** close reason, including `UserClosing` (hide-to-background): polling simply resumes never — wait, no: on `UserClosing` the form is *not* destroyed (`e->Cancel=true` in `MyForm_FormClosing`), but this watchdog is still disarmed and never re-armed (Load does not fire again). See Known limitation below.

## `IsExpectedEnabled()` (75–81) & `TryGetTargetId()` (83–87)

Identical contract to `CameraFailsafe`'s versions: the four-condition expected-enabled predicate (`owner`, `!IsSystemEndingActive`, `IsMonitoringActive`, `IsCameraExpectedEnabled`) and pure delegation of target resolution to `owner->TryGetFailsafeTargetId` (live selection → config → `MI_00` → first). The watchdog owns no target policy.

## `RequestRecoveryCheck()` (89–115) — shared detect-and-schedule

### Purpose
Single entry point for "something suggests we should verify now" (currently only the startup tick). Checks cheaply, schedules `retryTimer` only when action may be needed.

### Inputs
`const wchar_t* reason` — reserved context label; currently unused (all call sites pass `L"StartupVerification"`). Kept so future event sources (PnP, resume) can share this path with a logged reason.

### Control flow (execution order)
1. `if (!isArmed) return;`
2. `IsSystemEndingActive()` → return; `!IsMonitoringActive()` → return; `!IsExpectedEnabled()` → return. (Note: unlike `OnPollTick`, these three are **not** individually null-guarded on `owner` — but `IsExpectedEnabled()` itself returns `false` for null owner, so the net effect is identical.)
3. **Coalescing:** `PendingVerification`/`Recovering` → return (one active loop).
4. **Cooldown:** `lastRecoveryTick != 0 && now - lastRecoveryTick < 30 s` → return.
5. `TryGetTargetId` — empty/missing → return.
6. `GetCameraHardwareDisabledState` — query failure → return (retry at next poll/startup path).
7. `if (!isDisabled) { consecutiveFailures = 0; return; }` — already enabled fast-exit (the common case at startup: no churn, counter reset).
8. Otherwise: `state=PendingVerification`; log `RecoveryLoop_DisabledDetected | Device=<id> | Disabled | FAIL` (try/catch); `retryTimer->Interval = 5 s; retryTimer->Start();`.

### Side effects
At most one log line + one timer start. No hardware call here — the actual `Recover` happens 5 s later in `OnRetryTick`, giving transient states time to settle.

## `OnStartupTick` (117–125) — the 5 s fast path

### Purpose
One-shot startup verification: the reason this class exists.

### Control flow
1. `startupTimer->Stop();` — one-shot; it never restarts (per `Arm()` lifetime there is exactly one startup check).
2. `if (!isArmed) return;` — disarmed during the 5 s window (e.g., immediate close) → nothing.
3. Log `RecoveryLoop_StartupVerification | NoChange | PASS` (try/catch) — marks in `diagnostic.log` that the startup check ran, regardless of outcome.
4. `RequestRecoveryCheck(L"StartupVerification")` — delegate to the shared path.

### Timing
Fires 5 s after `Arm()`, which `main.cpp` triggers on `form.Load` — i.e. after the full `MyForm_Load` sequence (restore, WTS registration, `CameraFailsafe::Arm`) has completed. First verification therefore lands ~8–10 s after process start, well inside the 5–15 s post-sign-in target when combined with the `PT10S` scheduled-task helper.

## `OnPollTick` (127–153) — 30 s backup

### Purpose
Steady-state safety net after the startup check is spent.

### Control flow
Duplicates the `RequestRecoveryCheck` guard chain inline (steps 1–8 above, `isArmed` → shutdown → monitoring → expected → cooldown → coalesce → target → query → fast-exit → `PendingVerification` + `RecoveryLoop_DisabledDetected` + 5 s `retryTimer`), except every `owner` access is individually null-guarded (`owner != nullptr && ...`) and the `reason` concept does not apply.

### Timing
One `GetCameraHardwareDisabledState` (~2 ms) every 30 s = ~4 ms/min steady-state CPU. Worst-case runtime detection: 30 s (missed the poll by epsilon) + 5 s retry delay + ~2 s recover/verify = **~37 s**.

## `OnRetryTick` (155–235) — confirm, recover, bound retries

### Purpose
The only method that changes hardware state. Re-validates everything after the 5 s delay, then recovers or schedules the next bounded retry.

### Control flow (execution order)
1. `retryTimer->Stop();` — one-shot per attempt.
2. `if (!isArmed) { Idle; return; }`.
3. Guard re-checks with **logged** skips (contrast the silent returns in detect paths — by the time we reach the retry tick we had a live detection, so abandoning it is worth one log line):
   - `IsSystemEndingActive()` → `RecoveryLoop_SkippedShutdown`, `Idle`.
   - `!IsMonitoringActive()` → `RecoveryLoop_SkippedMonitoringOff`, `Idle`.
   - `!IsExpectedEnabled()` → `RecoveryLoop_SkippedExpectedDisabled`, `Idle`. (The lock-during-delay path: user pressed Win+L in the 5 s window — correctly abandoned, never fights the lock disable.)
   - cooldown active → `Idle`, **silent** (a sibling recovery just ran; nothing to report).
4. `TryGetTargetId` — failure → `Idle`, silent.
5. `GetCameraHardwareDisabledState(target, stillDisabled)` — query failure **or** no-longer-disabled → `consecutiveFailures=0`, `Idle`, silent. This is the stop-when-enabled cancel: the camera came back on its own (or via `CameraFailsafe`), so the cycle ends with the counter reset.
6. **Budget:** `consecutiveFailures >= kMaxRetries` → log `RecoveryLoop_MaxAttempts`, `Idle`, reset counter, return. (Unlike `CameraFailsafe`, the poll interval is **not** doubled here — the 30 s backup keeps its cadence; see Complement section.)
7. **Recovery** (lines 200–207): `state=Recovering`; log `RecoveryLoop_EnableAttempt`; stamp `recoverStart`; `RecoverCameraHardware(target, false)` (**enable-only**, `cycleDevice=false` — never the 1.75 s disable→enable cycle) + `VerifyCameraHardwareState(target, false)`; `durationMs`; stamp `lastRecoveryTick` (cooldown starts whether or not recovery worked).
8. **Success:** log `RecoveryLoop_Recovered | DurationMs=<ms>`; `consecutiveFailures=0`; `Idle`.
9. **Failure:** `consecutiveFailures++`; log `RecoveryLoop_RecoveryFailed | DurationMs=<ms> | Attempt=<n>`; if `consecutiveFailures < kMaxRetries`, restart `retryTimer` at the fixed 5 s and return to `PendingVerification` (**linear** retry — 5 s, 5 s, not 10/20/40); else log `RecoveryLoop_MaxAttempts`, `Idle`, reset counter.

### Important branches
- The step-5 cancel resets `consecutiveFailures` — a transient that self-heals between attempts does not consume budget.
- The step-6 budget check sits **before** the recovery call, and the post-failure check repeats the `MaxAttempts` log — so the log appears exactly once per exhausted cycle (post-failure branch), with the pre-check covering a counter that somehow reached 3 without reset.
- Linear 5 s retries (vs `CameraFailsafe`'s 10/20/40 s exponential) are deliberate: three attempts span 15 s, keeping the whole cycle inside a predictable ~21 s envelope (5 detect + 3×(5 wait + ~2 recover)).

### Dependencies
`RecoverCameraHardware` / `VerifyCameraHardwareState` / `GetCameraHardwareDisabledState` (core authority), `TryGetFailsafeTargetId` + `LogFailsafe*` (core accessors). No calls into `CameraFailsafe`.

### Side effects
Hardware enable (only here), `diagnostic.log` lines (all `RecoveryLoop_*`), `consecutiveFailures`/`lastRecoveryTick`/`state`/timer transitions.

### Failure behavior
Query failures cancel silently to `Idle` (next 30 s poll retries). Recovery failures count toward the bound of 3, then the cycle parks until the next poll starts a fresh cycle — recovery is **bounded per cycle but persistent across cycles** (a device that stays disabled while ExpectedEnabled is retried 3× every ~30 s indefinitely, 5 s apart within each cycle).

### Timing
Per-cycle worst case: 5 s (retry delay) + ~2.3 s (recover + verify) per attempt × 3 ≈ **~22 s**; per-cycle best case (first attempt works): **~7 s** after detection.

### Behavioral sensitivity
- The three `Skipped*` guards are the lock/suspend/shutdown protection — removing any of them turns the watchdog into a second camera authority that fights intentional disables.
- `RecoverCameraHardware(target, false)` must stay `false`. The scheduled-task helper (`--enable-camera`) goes through `RestoreConfiguredCameraHardware(true)` (cycle) by contrast — that asymmetry is intentional and documented in `docs/Plan.md §30`: the in-process fast path avoids the disable-first cycle; the out-of-process helper reuses the existing command pipeline.
- `kRetryIntervalMs` / `kMaxRetries` / `kPollIntervalMs` jointly set the recovery envelope; sub-second retries would busy-loop the device stack (PnP re-enumeration needs seconds to settle).

## Complement to `CameraFailsafe` (observed, from source)

| Aspect | `CameraFailsafe` | `RecoveryLoopFailsafe` |
|---|---|---|
| Owner | `MyForm` (`cameraFailsafe`) | `main.cpp` (`recoveryLoop`) |
| Startup check | none (45 s grace suppresses) | one-shot 5 s `startupTimer` |
| Steady poll | 90 s | 30 s |
| Confirm delay | 10 s `verifyTimer` | 5 s `retryTimer` |
| Retry spacing | exponential 10→20→40 s | linear 5 s |
| Post-exhaustion | poll doubled to 180 s | poll stays 30 s |
| Failure log | `Failsafe_*` | `RecoveryLoop_*` |
| Cooldown | 30 s, own stamp | 30 s, own stamp |

The two loops never call each other and share no mutable state; each keeps its **own** `consecutiveFailures`/`lastRecoveryTick`/`state`. They coordinate implicitly: identical expected-state guards prevent fighting intentional disables, and the shared idempotent core pipeline (`Recover` checks before changing; `Verify` confirms) makes whichever-loop-recovers-first win harmlessly — the other observes `AlreadyEnabled` and no-ops. Worst-case double recovery (both timers firing simultaneously on different phases) resolves to two back-to-back enable-only `Recover(false)` calls, both verified — idempotent, no disable involved.

## Logging events (all try/caught → `diagnostic.log`)

| Event | When |
|---|---|
| `RecoveryLoop_Start` | `Arm()` |
| `RecoveryLoop_StartupVerification` | startup tick ran (always, even if nothing follows) |
| `RecoveryLoop_DisabledDetected` | detect path observed Disabled while ExpectedEnabled |
| `RecoveryLoop_EnableAttempt` | about to call `Recover(false)` |
| `RecoveryLoop_Recovered \| DurationMs=<ms>` | recover + verify passed |
| `RecoveryLoop_RecoveryFailed \| DurationMs=<ms> \| Attempt=<n>` | recover or verify failed |
| `RecoveryLoop_MaxAttempts` | budget exhausted |
| `RecoveryLoop_SkippedShutdown` / `SkippedMonitoringOff` / `SkippedExpectedDisabled` | retry tick abandoned for the named reason |

Detect-path guard rejections (poll/startup/cooldown/coalescing/target/query/already-enabled) are deliberately **not** logged — steady state stays silent except for the single `StartupVerification` line.

## Known limitation (observed, not fixed)

- **Disarmed by hide-to-background, never re-armed.** `OnOwnerClosing` disarms on *every* `FormClosing`, but `MyForm_FormClosing` cancels `UserClosing` closes (hide-to-background, the normal "close" path) — the form lives on, yet the fast verifier stays disarmed for the rest of the process lifetime because `Load` never fires again. After the user closes the window once, only `CameraFailsafe` (90 s poll) remains. A re-arm on `BringWindowToFrontDelegate` (or scoping `OnOwnerClosing` to non-`UserClosing` reasons) would fix it, but that is a code change and out of scope for this documentation pass.
- **`reason` parameter of `RequestRecoveryCheck` is unused.** Reserved for future event sources; currently always `L"StartupVerification"`.
