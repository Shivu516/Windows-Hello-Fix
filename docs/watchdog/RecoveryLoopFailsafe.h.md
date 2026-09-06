# `src/watchdog/RecoveryLoopFailsafe.h` — Fast-Verifier Declaration

**Path:** `src/watchdog/RecoveryLoopFailsafe.h`
**Type:** C++/CLI header (`#pragma once`)
**Included by:** `main.cpp` (the only TU that constructs it), `src/watchdog/RecoveryLoopFailsafe.cpp`
**Build relationship:** `ClInclude` in `Windows_Hello_Fix_v2_1.vcxproj`; filter `Header Files\src\watchdog`.

## Purpose

Declares the `RecoveryLoopFailsafe` managed class — the **short-term fast recovery coordinator** that closes the latency gap left by `CameraFailsafe` (90 s poll, 45 s startup grace): a **5 s startup verification**, a **30 s periodic backup poll**, and **5 s bounded retries**, all enable-only through the existing core pipeline.

Why a second watchdog instead of just retuning `CameraFailsafe`:

- `CameraFailsafe` is owned by `MyForm` (`src/core` member) and its 45 s grace / 90 s poll are load-bearing for the startup-restore quirk it was built around. Changing them risks the stable long-term backup.
- The fast verifier is owned by **`main.cpp`**, outside `src/core` — it can be armed from the form's `Load`/`FormClosing` events without touching any `src/core` file (extreme core-preservation boundary, see `docs/Plan.md §10`).
- The two loops are complementary, not competing: both are guarded by the same expected-state/cooldown/state-coalescing rules and both call the same idempotent `RecoverCameraHardware(target, false)`. Whichever observes the problem first recovers; the other then sees `AlreadyEnabled` and no-ops.

What it deliberately does **NOT** own — same contract as `CameraFailsafe`, plus:

- No PnP notification handle. The header comment (lines 7–9) states this explicitly: *"PnP notification is optional polling fallback; current implementation uses timers only to avoid native/managed interop complexity."* A `CM_Register_Notification` accelerator was investigated (`docs/Plan.md §18`) but deferred after native-callback interop (`HCMNOTIFICATION`, `__stdcall` vs `__clrcall`) broke the `/clr` build; the timer-only contract still meets the startup target via the scheduled-task helper plus the 5 s startup check.
- No shutdown/lock/suspend policy of its own (queries core accessors every tick).
- No GUI manipulation, no new threads, no new mutex/event objects (single-instance stays `Global\WindowsHelloFix_AppMutex` / `Global\WindowsHelloFix_WakeupEvent` only).

## Includes

- `windows.h` (for `ULONGLONG`, `GetTickCount64`).
- `string` (for `std::wstring` target IDs).
- Forward-declares `ref class MyForm` (line 17) for the same circular-include reason as `CameraFailsafe.h`; the full core header is included only by the `.cpp`.

## The `RecoveryLoopFailsafe` class (lines 19–65)

`public ref class RecoveryLoopFailsafe`, driven by **three** WinForms timers on the UI message pump.

### Private state members

| Member | Type | Meaning |
|---|---|---|
| `owner` | `MyForm^` | Back-reference to the form (never reassigned after ctor). |
| `pollTimer` | `Timer^` | Periodic backup, every `kPollIntervalMs`; handler `OnPollTick`. |
| `retryTimer` | `Timer^` | One-shot retry delay `kRetryIntervalMs`; handler `OnRetryTick`. Started when Disabled is detected; re-armed after each failed attempt. |
| `startupTimer` | `Timer^` | One-shot `kStartupVerifyMs` after `Arm()`; handler `OnStartupTick`. The fast startup path. |
| `state` | `RecoveryState` | `Idle` / `PendingVerification` / `Recovering` — single-active-loop guard shared by all three timers plus `RequestRecoveryCheck`. |
| `consecutiveFailures` | `int` | Failed attempts in the current cycle; bounded by `kMaxRetries`. |
| `lastRecoveryTick` | `ULONGLONG` | Stamp of the last recovery attempt; drives the `kCooldownMs` cooldown. |
| `isArmed` | `bool` | Master switch. |

There is intentionally **no** `startupGraceUntilTick` here — the startup timer (5 s) *replaces* the 45 s grace: instead of suppressing everything for 45 s, the loop checks once at 5 s and only acts if the device is actually Disabled while ExpectedEnabled (already-enabled fast-exit avoids churn).

### `RecoveryState` (lines 28–32)

Identical shape to `CameraFailsafe::WatchdogState` (separate enum because the classes are independent):

- `Idle` — no suspected problem.
- `PendingVerification` — Disabled detected; `retryTimer` running toward `OnRetryTick`.
- `Recovering` — synchronous `Recover + Verify` in progress inside `OnRetryTick`.

### Timing constants (lines 39–44, per `docs/Plan.md §12–14`)

| Constant | Value | Role |
|---|---|---|
| `kStartupVerifyMs` | `5000` (5 s) | Delay from `Arm()` to the first startup check. Arm happens on `form.Load` (~3–4 s after daemon start), so first verification lands ~8–9 s after process start. |
| `kPollIntervalMs` | `30000` (30 s) | Periodic backup poll — 3× faster than `CameraFailsafe`'s 90 s, still negligible (~2 ms per tick, 2 queries/min). |
| `kRetryIntervalMs` | `5000` (5 s) | Fixed linear retry delay (not exponential — predictable within the 5–15 s target window). |
| `kCooldownMs` | `30000` (30 s) | Quiet period after any recovery attempt. |
| `kMaxRetries` | `3` | Bounded attempts per cycle. |

Derived worst cases: startup-disabled → detected at 5 s, recovered by ~5 + 5 + ~2 = **~12 s**; runtime-disabled → detected at next 30 s poll, recovered 5 s later = **35 s worst** without PnP.

### Private methods (lines 46–52)

- `OnPollTick` — 30 s backup detection (same guard chain as `RequestRecoveryCheck`, inline).
- `OnRetryTick` — confirmation + recovery + bounded linear retry (the only place that calls `RecoverCameraHardware`).
- `OnStartupTick` — one-shot: stop `startupTimer`, log `RecoveryLoop_StartupVerification`, delegate to `RequestRecoveryCheck`.
- `IsExpectedEnabled()` — same four-condition predicate as `CameraFailsafe`.
- `TryGetTargetId(std::wstring&)` — forwards to `owner->TryGetFailsafeTargetId`.
- `RequestRecoveryCheck(const wchar_t* reason)` — shared detect-and-schedule entry used by the startup tick (the `reason` parameter is currently unused — reserved for logging context; all call sites pass `L"StartupVerification"`).

### Public API (lines 54–64)

- `RecoveryLoopFailsafe(MyForm^ ownerForm)` — ctor; stores owner, zeroes state, creates the three timers stopped and wired.
- `~RecoveryLoopFailsafe()` — deterministic dtor: `Disarm()` then run the finalizer (C++/CLI pattern, mirrors `MyForm`).
- `!RecoveryLoopFailsafe()` — finalizer: best-effort `Disarm()` in try/catch (GC safety net).
- `Arm()` — idempotent start: refuse if already armed or if `IsSystemEndingActive()`; reset state; log `RecoveryLoop_Start`; start `pollTimer` + `startupTimer`; stop `retryTimer`.
- `Disarm()` — unconditional: clear `isArmed`, reset `Idle`, stop all three timers (each in its own try/catch).
- `OnOwnerLoad(Object^, EventArgs^)` — `form.Load` hook for `main.cpp`: `Arm()` in try/catch. Wiring through the public `Load` event avoids any `src/core` edit.
- `OnOwnerClosing(Object^, FormClosingEventArgs^)` — `form.FormClosing` hook for `main.cpp`: `Disarm()` in try/catch.

## Dependencies

- **Depends on:** `MyForm` read-only accessors + `LogFailsafe*`, native `GetCameraHardwareDisabledState` / `VerifyCameraHardwareState` / `RecoverCameraHardware` (same authority as `CameraFailsafe`), WinForms `Timer`, `GetTickCount64`.
- **Depended on by:** `main.cpp` only (owns the single instance, wires `Load`/`FormClosing`).

## Threading / synchronization

All three timers are `System::Windows::Forms::Timer` on the **UI thread** — every state transition is single-threaded with the rest of the app; no locks. `OnOwnerLoad`/`OnOwnerClosing` run on the UI thread via the WinForms event pump. The only cross-thread touchpoint is `main.cpp`'s post-`Application::Run` `Disarm()` call, which runs after the pump has exited (timers already dead) and is additionally wrapped in try/catch at both ends.

## Lifetime / ownership

Owned exclusively by `main.cpp` (`recoveryLoop` local, alive for the duration of `Application::Run`), **not** by `MyForm` — this is the architectural point: the fast verifier can exist without adding a member to the frozen `MyForm` class. Created only for the long-lived daemon (`!isCommandWorker`); short-lived `--enable/--disable-camera` workers never get an instance, so no watchdog can outlive a worker's `Environment::Exit(0)`. Disarmed on `FormClosing` and again after `Application::Run` returns (belt and suspenders). Unlike `CameraFailsafe`, every logging call in this class is wrapped in try/catch — the watchdog must never crash the daemon from a logging failure.
