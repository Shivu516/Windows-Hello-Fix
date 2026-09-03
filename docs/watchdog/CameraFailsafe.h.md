# `src/watchdog/CameraFailsafe.h` — Watchdog Declaration & Timing Contract

**Path:** `src/watchdog/CameraFailsafe.h`
**Type:** C++/CLI header (`#pragma once`)
**Included by:** `src/core/MyForm_Core.cpp` (the only TU that constructs/arms it), `src/watchdog/CameraFailsafe.cpp`
**Build relationship:** `ClInclude` in `Windows_Hello_Fix_v2_0.vcxproj`; filter `Header Files\src\watchdog`.

## Purpose

Declares the `CameraFailsafe` managed class — the **long-term auxiliary runtime failsafe** that observes `ExpectedEnabled vs observed Disabled` and, after confirmation, recovers through the existing `src/core` camera pipeline.

Why it exists separately from the camera implementation:

- The authoritative enable/disable implementation lives in `src/core/MyForm_Camera.cpp` (SetupAPI/CfgMgr). That pipeline is behavior-sensitive and must not gain polling/timing responsibilities.
- The failsafe owns the *opposite* concern: **when** to check, **how long** to wait before acting, **how many** retries are allowed, and **when to stay out of the way** (lock/suspend/shutdown/monitoring-off). Splitting this into `src/watchdog/` keeps `src/core/` byte-stable while allowing the recovery policy to evolve.

What it deliberately does **NOT** own:

- No camera selection or `config.txt` parsing (it asks `MyForm::TryGetFailsafeTargetId`).
- No monitoring/lock/power policy (it queries `IsMonitoringActive` / `IsSystemEndingActive` / `IsCameraExpectedEnabled`).
- No hardware calls of its own (no `SetupDi*`, no `CM_*`; recovery is a call to `RecoverCameraHardware(target, false)`).
- No GUI manipulation (no `Show`/`Hide`/`Opacity`), no new threads, no new mutex/event objects.

## Includes

- `windows.h` (for `ULONGLONG`, `GetTickCount64` used by the `.cpp`).
- `string` (for `std::wstring` target IDs).
- No include of `../core/MyForm.h` here — the header only **forward-declares** `ref class MyForm` (line 14) to avoid a circular include. The full core header is included by `CameraFailsafe.cpp` instead.

## The `CameraFailsafe` class (lines 18–56)

`public ref class CameraFailsafe` — a C++/CLI managed class driven by WinForms timers on the UI message pump (same pattern as the rest of the app: no worker thread, no thread pool).

### Private state members

| Member | Type | Meaning |
|---|---|---|
| `owner` | `MyForm^` | Back-reference to the owning form; the only channel to core state and logging. Never null in practice (set by ctor, never reassigned). |
| `pollTimer` | `System::Windows::Forms::Timer^` | Idle poll, every `kIdleIntervalMs`; handler `OnPollTick`. |
| `verifyTimer` | `System::Windows::Forms::Timer^` | One-shot confirmation delay `kVerifyDelayMs`; handler `OnVerifyTick`. Also reused for bounded retry backoff. |
| `state` | `WatchdogState` | `Idle` / `PendingVerification` / `Recovering` — single-active-loop guard. |
| `consecutiveFailures` | `int` | Failed recovery attempts in the current cycle; bounded by `kMaxRetries`. |
| `lastRecoveryTick` | `ULONGLONG` | `GetTickCount64()` stamp of the last recovery attempt; drives the `kCooldownMs` cooldown. `0` = none yet. |
| `startupGraceUntilTick` | `ULONGLONG` | `GetTickCount64()` stamp until which detection is suppressed after `Arm()` (startup race suppression). |
| `isArmed` | `bool` | Master switch; `false` until `Arm()`, cleared by `Disarm()`. Every tick handler checks it first. |

### `WatchdogState` (lines 26–30)

```cpp
enum class WatchdogState { Idle, PendingVerification, Recovering };
```

- `Idle` — no suspected problem; polls may schedule verification.
- `PendingVerification` — a disabled observation is waiting out the `verifyTimer` delay (or a retry backoff). New poll observations coalesce (early-return) instead of starting a second loop.
- `Recovering` — set only around the synchronous `RecoverCameraHardware` + `Verify` call inside `OnVerifyTick`. Because recovery runs synchronously on the UI thread, this state is transient, but it still guards against re-entrancy.

### Timing constants (lines 38–43)

| Constant | Value | Role |
|---|---|---|
| `kIdleIntervalMs` | `90000` (90 s) | Idle poll period. Steady-state cost is one `GetCameraHardwareDisabledState` query (~2 ms) every 90 s. |
| `kVerifyDelayMs` | `10000` (10 s) | Confirmation delay between first detecting Disabled and acting on it; also the base unit for retry backoff (`10→20→40 s`). |
| `kCooldownMs` | `30000` (30 s) | Quiet period after a recovery attempt during which new detections are ignored. |
| `kMaxRetries` | `3` | Bounded recovery attempts per cycle before backing off. |
| `kStartupGraceMs` | `45000` (45 s) | Post-`Arm()` suppression window covering the ~2.8 s startup restore plus the ~431 ms power-event quirk, with wide margin. |

> **Source-wins note:** `docs/Plan.md §3` describes this failsafe with a "60 s poll" and an event-driven `CM_Register_Notification` layer. The **current source has neither**: the poll constant is `90000` (90 s) and there is no PnP notification registration anywhere in `CameraFailsafe.*` — detection is timer-only (`pollTimer` + `verifyTimer`). The 90 s / 10 s / 45 s / 30 s values above are the implementation of record. `Plan.md` also references `docs/Plan.md §7-8` in the header comment (line 38); those section numbers refer to an earlier revision of the plan and do not align with the current `Plan.md` — the constants themselves are as tabulated here.

### Private methods (lines 45–49)

- `OnPollTick(Object^, EventArgs^)` — idle-poll handler; detects unexpected Disabled and enters `PendingVerification`.
- `OnVerifyTick(Object^, EventArgs^)` — confirmation/recovery handler; re-verifies, then recovers or backs off.
- `IsExpectedEnabled()` — `owner != null && !IsSystemEndingActive() && IsMonitoringActive() && IsCameraExpectedEnabled()`.
- `TryGetTargetId(std::wstring&)` — forwards to `owner->TryGetFailsafeTargetId`.

### Public API (lines 51–55)

- `CameraFailsafe(MyForm^ ownerForm)` — ctor; stores owner, zeroes state, creates both timers wired to their tick handlers (timers created stopped; `Arm()` starts them).
- `Arm()` — reset state, set the 45 s grace stamp, log `Failsafe_Start`, start `pollTimer`, stop `verifyTimer`. Called once from `MyForm_Load` after WTS registration.
- `Disarm()` — clear `isArmed`, reset `state` to `Idle`, stop both timers. Called first in `~MyForm`/`!MyForm` so no tick can fire during shutdown teardown.

## Dependencies

- **Depends on:** `MyForm` read-only accessors + `LogFailsafe*` (declared in `src/core/MyForm.h`), native `GetCameraHardwareDisabledState` / `VerifyCameraHardwareState` / `RecoverCameraHardware` (declared in `src/core/MyForm.h`, defined in `MyForm_Camera.cpp`), WinForms `Timer`, `GetTickCount64`.
- **Depended on by:** `MyForm` (owns one instance as `cameraFailsafe`).

## Threading / synchronization

Both timers are `System::Windows::Forms::Timer` — they fire on the **UI thread** message pump, so all state transitions are single-threaded with the rest of the app. No locks are used or needed. The `ULONGLONG` tick comparisons use plain reads; `GetTickCount64()` is monotonic, so the grace/cooldown windows are immune to wall-clock jumps.

## Lifetime / ownership

Owned exclusively by `MyForm` (`cameraFailsafe` member). Created in `MyForm_Load`, disarmed in `~MyForm`/`!MyForm` **before** any shutdown camera handling. The failsafe never outlives its owner: every method guards `owner == nullptr` via `IsExpectedEnabled`/`TryGetTargetId`, returning "not expected / no target" (i.e., do nothing) rather than crashing.
