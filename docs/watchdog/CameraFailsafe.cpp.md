# `src/watchdog/CameraFailsafe.cpp` — Poll, Confirm & Recover

**Path:** `src/watchdog/CameraFailsafe.cpp`
**Lines:** 217
**Included by:** built directly (translation unit); `#include "CameraFailsafe.h"`, `#include "../core/MyForm.h"`, `#include <msclr\marshal_cppstd.h>`
**Build relationship:** `ClCompile` in vcxproj, filter `Source Files\src\watchdog`.

## Purpose

Implements the `CameraFailsafe` state machine declared in `CameraFailsafe.h`: idle polling, delayed confirmation, enable-only recovery through the existing core pipeline, bounded retries with backoff, and cooldown. Every recovery path funnels through `RecoverCameraHardware(target, false)` + `VerifyCameraHardwareState(target, false)` — the same functions `MyForm::EnableTargetCameraHardware` uses — so there is exactly one hardware implementation.

Control-flow summary:

```
Arm() → pollTimer every 90 s → OnPollTick
  → guards pass + observed Disabled → PendingVerification + verifyTimer 10 s → OnVerifyTick
    → guards re-checked + still disabled → Recover(false) + Verify
      → success → Recovered log, failures=0, cooldown stamped, Idle
      → failure → RecoveryFailed log, failures++, backoff 10/20/40 s → PendingVerification
      → 3 failures → MaxRetries log, poll doubled to 180 s, Idle
```

## Constructor `CameraFailsafe::CameraFailsafe` (7–22)

### Purpose
Bind the owner, zero the state machine, and create the two timers (stopped).

### Inputs
`ownerForm` (`MyForm^`) — the owning form; stored as `owner`.

### Control flow
Initializer list sets `state=Idle`, `consecutiveFailures=0`, `lastRecoveryTick=0`, `startupGraceUntilTick=0`, `isArmed=false`. Then:

1. `pollTimer = gcnew Timer()`; `Interval = kIdleIntervalMs` (90 s); `Tick += OnPollTick`.
2. `verifyTimer = gcnew Timer()`; `Interval = kVerifyDelayMs` (10 s); `Tick += OnVerifyTick`.

Both timers are created **stopped** — `System::Windows::Forms::Timer` does not run until `Start()` is called in `Arm()`.

### Side effects
Two managed `Timer` objects allocated on the CLR heap. No logging, no hardware access.

## `Arm()` (24–38)

### Purpose
Reset the state machine and begin observation. Called once from `MyForm_Load` (`MyForm_Core.cpp:420–428`) after WTS/power registration completes.

### Control flow
1. `if (isArmed) return;` — arming is idempotent; a second `Arm()` is a no-op.
2. Set `isArmed=true`, `state=Idle`, `consecutiveFailures=0`, `lastRecoveryTick=0`.
3. Stamp `startupGraceUntilTick = GetTickCount64() + kStartupGraceMs` (45 s suppression window starts now).
4. Log `Failsafe_Start | Target=Enabled | Verify=PASS` via `owner->LogFailsafe`.
5. `pollTimer->Interval = kIdleIntervalMs; pollTimer->Start();` — begin idle polling.
6. `verifyTimer->Stop();` — ensure no stale confirmation is pending.

### Failure behavior
No failure paths: timer `Start()` on a valid handle does not throw in practice. If `owner` were null this would throw on the `LogFailsafe` call — but `Arm()` is only invoked with `this` from `MyForm_Load`, so this is not guarded (contrast `RecoveryLoopFailsafe::Arm`, which wraps logging in try/catch).

## `Disarm()` (40–47)

### Purpose
Stop all observation so no tick can fire during shutdown teardown. Called first in `~MyForm`/`!MyForm`.

### Control flow
1. Guard: `if (!isArmed && pollTimer == nullptr && verifyTimer == nullptr) return;` — returns early only when fully unarmed **and** both timers are null (in practice only if the ctor never ran). Note this guard is nearly vacuous post-construction: it proceeds to stop timers even when `!isArmed`, which makes `Disarm()` safe to call repeatedly.
2. `isArmed=false; state=Idle;` — any in-flight `PendingVerification`/`Recovering` marker is dropped. (A tick already executing on the UI thread still finishes its synchronous body, but its `!isArmed` checks route it to `Idle` without acting.)
3. `pollTimer->Stop(); verifyTimer->Stop();` (each null-checked).

### Side effects
Timers stopped. No logging (shutdown path must not write ordering-sensitive log noise).

## `IsExpectedEnabled()` (49–55)

### Purpose
Single predicate answering "would HelloFix currently want the camera **enabled**?" Every tick handler consults it before acting.

### Control flow
```cpp
if (owner == nullptr) return false;
if (owner->IsSystemEndingActive()) return false;   // shutdown/logoff
if (!owner->IsMonitoringActive()) return false;    // automation off
return owner->IsCameraExpectedEnabled();           // !cameraExpectedDisabled
```

### Truth table
| Monitoring | SystemEnding | cameraExpectedDisabled | Result | Meaning |
|---|---|---|---|---|
| off | * | * | `false` | user stopped automation — never recover |
| * | true | * | `false` | shutdown — never recover |
| on | false | true (lock/suspend/shutdown-disable) | `false` | intentional disabled — never recover |
| on | false | false | `true` | recovery candidate |

## `TryGetTargetId()` (57–61)

### Purpose
Resolve which device to observe, delegating 100% of target policy to core.

### Control flow
Null-guards `owner`, then `return owner->TryGetFailsafeTargetId(targetId)` — which forwards to `TryGetTargetCameraInstanceId(target, true)` (live selection → `config.txt device=` → `MI_00` → first camera). The watchdog never parses `config.txt` itself.

## `OnPollTick` (63–118) — idle detection

### Purpose
The 90 s safety net: if the camera is unexpectedly Disabled while ExpectedEnabled, enter `PendingVerification` and schedule confirmation.

### Control flow (execution order)
1. `if (!isArmed) return;` — disarmed watchdog is fully inert.
2. `nowTick = GetTickCount64()`.
3. **Startup grace:** `if (nowTick < startupGraceUntilTick) return;` — silent (no log) suppression for 45 s after `Arm()`. Covers the ~2.8 s startup restore plus the ~431 ms power-event quirk with wide margin, at the cost of ignoring genuine disables in that window (see Known limitation below).
4. **Expected-state guards** (each silent `return`): `IsSystemEndingActive()` → return; `!IsMonitoringActive()` → return; `!IsExpectedEnabled()` → return. A lock/suspend/shutdown that lands between polls is therefore never even queried.
5. **Cooldown:** `if (lastRecoveryTick != 0 && nowTick - lastRecoveryTick < kCooldownMs) return;` — 30 s quiet period after any recovery attempt.
6. **Coalescing:** `if (state == PendingVerification || state == Recovering) return;` — a second poll while confirmation/recovery is in flight does nothing (single active loop).
7. `TryGetTargetId` — empty/missing target → silent `return` (retry next poll).
8. `GetCameraHardwareDisabledState(target, isDisabled)` — query failure → silent `return` (retry next poll). This is the only per-tick hardware read (~2 ms).
9. `if (!isDisabled) { consecutiveFailures = 0; return; }` — already enabled: reset the failure counter, nothing to do.
10. Otherwise: `state = PendingVerification`; log `Failsafe_DetectDisabled | Device=<id> | Target=Disabled | Verify=FAIL`; `verifyTimer->Interval = kVerifyDelayMs; verifyTimer->Start();` — wait 10 s, then `OnVerifyTick` decides.

### Timing
Worst-case detection latency after arming (outside grace): up to 90 s (one full poll period) + 10 s confirmation = **100 s**.

## `OnVerifyTick` (120–215) — confirm & recover

### Purpose
Re-check everything after the 10 s delay (a legitimate lock/suspend/shutdown may have occurred meanwhile), then recover or back off.

### Control flow (execution order)
1. `verifyTimer->Stop();` — one-shot semantics; re-armed explicitly only on the retry path.
2. `if (!isArmed) { state=Idle; return; }`.
3. Re-check guards, now with state reset to `Idle` and (for one path) logging:
   - `IsSystemEndingActive()` → `Idle`, silent return.
   - `!IsMonitoringActive()` → `Idle`, silent return.
   - `!IsExpectedEnabled()` → log `Failsafe_Skipped_ExpectedDisabled | NoChange | PASS`, `Idle`. (This is the lock-during-delay path: detection was valid 10 s ago, but the user locked since — correctly abandoned.)
   - `nowTick < startupGraceUntilTick` → `Idle`, silent return.
   - cooldown active → `Idle`, silent return.
4. `TryGetTargetId` — failure → `Idle`, silent.
5. `GetCameraHardwareDisabledState(target, stillDisabled)` — if the query fails **or** the device is no longer disabled → `Idle`, silent cancel ("either query failed or device recovered").
6. **Retry budget:** `if (consecutiveFailures >= kMaxRetries)` → log `Failsafe_MaxRetries`, double the idle interval (`pollTimer->Interval = kIdleIntervalMs * 2` = 180 s backoff), `Idle`, reset `consecutiveFailures=0`, return. Note this pre-check is nearly unreachable in the normal flow (the counter is also checked after each failure at step 8) — it covers a counter that reached 3 without being reset.
7. **Recovery** (lines 177–184): `state=Recovering`; log `Failsafe_RecoveryQueued`; stamp `recoverStart`; call `RecoverCameraHardware(target, false)` (**enable-only, no cycle**) then `VerifyCameraHardwareState(target, false)`; `durationMs = now - recoverStart`; stamp `lastRecoveryTick` (starts the 30 s cooldown **regardless of outcome**).
8. **Success** (`recoverResult && verified`): log `Failsafe_Recovered | DurationMs=<ms>`; `consecutiveFailures=0`; restore `pollTimer->Interval = kIdleIntervalMs`; `Idle`.
9. **Failure**: `consecutiveFailures++`; log `Failsafe_RecoveryFailed | DurationMs=<ms> | Attempt=<n>`; if `consecutiveFailures < kMaxRetries`, compute exponential backoff `kVerifyDelayMs * 2^(n-1)` capped at 40 s (i.e. **10 s → 20 s → 40 s**), restart `verifyTimer`, `state=PendingVerification` (another `OnVerifyTick` will run after the backoff); else log `Failsafe_MaxRetries`, double poll to 180 s, `Idle`, reset counter.

### Important branches
- The `!stillDisabled` early-cancel (step 5) is what makes the loop **stop-when-enabled**: if the user re-enabled the camera (or the core pipeline did) during the delay, no recovery is attempted and no failure is counted.
- `lastRecoveryTick` is stamped **before** the success/failure branch, so even a failed attempt gets the 30 s cooldown — but the retry path re-arms `verifyTimer` for 10–40 s, which is longer than the cooldown, so retries are not blocked by it. The cooldown instead blocks *new poll detections* for 30 s after any attempt.
- After `MaxRetries`, the poll interval stays doubled (180 s) until the next *successful* recovery restores it to 90 s — so a persistently failing device is polled half as often (by design, to avoid churning a broken stack).

### Dependencies
Calls `RecoverCameraHardware` / `VerifyCameraHardwareState` / `GetCameraHardwareDisabledState` (core camera authority), `TryGetFailsafeTargetId` + `LogFailsafe*` (core accessors). Calls nothing in `RecoveryLoopFailsafe` — the two watchdogs are independent and coordinate only implicitly (same guards, same idempotent pipeline, separate cooldown stamps).

### Side effects
Camera hardware state change (enable-only), `diagnostic.log` lines (all `Failsafe_*` events), timer interval mutation, `consecutiveFailures`/`lastRecoveryTick`/`state` transitions.

### Failure behavior
Query failures cancel silently (retry next tick). Recovery failures are counted, logged with `DurationMs` and attempt number, and retried with backoff up to 3 attempts, then backed off to 180 s polling. The watchdog never disables, never throws (no try/catch here — exceptions from `LogFailsafe` would propagate to the WinForms pump; in practice `WriteDiagnosticLog` swallows internally).

### Timing
Per-cycle worst case: 10 s confirm + (recover ~1–2 s + verify ~0.3 s) + 10 s + attempt + 20 s + attempt ≈ **~45 s** for 3 attempts, then 180 s poll backoff.

### Behavioral sensitivity
- `kIdleIntervalMs`, `kVerifyDelayMs`, `kStartupGraceMs` jointly determine detection latency (currently 90/10/45 s). Shortening the poll or grace speeds recovery but risks fighting the startup restore and the 431 ms power-event quirk.
- Removing the `!IsExpectedEnabled()` re-check in `OnVerifyTick` would let the watchdog re-enable the camera **during lock/suspend** — the single most dangerous change in this file.
- The `RecoverCameraHardware(target, false)` argument must stay `false` (enable-only). Passing `true` would run the 1.75 s disable→enable cycle on every recovery.

## Logging events (all via `owner->LogFailsafe*` → `diagnostic.log`)

| Event | When |
|---|---|
| `Failsafe_Start` | `Arm()` |
| `Failsafe_DetectDisabled` | poll observed unexpected Disabled |
| `Failsafe_RecoveryQueued` | about to call `Recover(false)` |
| `Failsafe_Recovered \| DurationMs=<ms>` | recover + verify both passed |
| `Failsafe_RecoveryFailed \| DurationMs=<ms> \| Attempt=<n>` | recover or verify failed |
| `Failsafe_MaxRetries` | budget exhausted (logged in two places: pre-check and post-failure) |
| `Failsafe_Skipped_ExpectedDisabled` | confirmation delay overlapped a legitimate disable |

Idle polls that find the camera enabled, and all guard rejections except the expected-disabled confirmation path, are deliberately **not** logged.

## Known limitation (observed, not fixed)

- **No event-driven path.** Despite `docs/Plan.md §3` describing a `CM_Register_Notification` accelerator, the current source is **timer-only**: an unexpected disable is detected at the next 90 s poll at the earliest (100 s worst with confirmation), and any disable inside the 45 s startup grace is ignored outright. `RecoveryLoopFailsafe` (5 s startup check, 30 s poll) exists precisely to cover this gap.
