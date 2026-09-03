# `src/watchdog/` — How It All Works Together

**Folder:** `src/watchdog/`
**Files:** `CameraFailsafe.h`, `CameraFailsafe.cpp`, `RecoveryLoopFailsafe.h`, `RecoveryLoopFailsafe.cpp`
**Detail docs:** one per file in this same folder (`docs/watchdog/*.md`)
**Core docs:** `docs/core/core.md` (the main program these watchdogs protect)

## The one-sentence version

`src/watchdog/` contains two safety nets that periodically check "is the camera supposed to be on but actually off?" — and if so, ask the main program to turn it back on.

## Why watchdogs exist at all

The main program (`src/core/`) already handles every *expected* situation: lock → disable, unlock → enable, sleep → disable, resume → enable. But sometimes the camera ends up disabled for *unexpected* reasons — a missed startup, a driver hiccup, someone toggling it in Device Manager. Nobody sends the program a message about those. So the watchdogs go and *look*, on a timer, instead of waiting to be told.

## The golden rule: they observe, they never operate

This is the most important thing to understand about this folder:

- The **only** code allowed to flip camera hardware lives in `src/core/MyForm_Camera.cpp`.
- Watchdogs **never** call those low-level Windows functions themselves. They only:
  1. **Observe** — "is the camera currently disabled?" (a harmless read),
  2. **Ask** — "please run your normal enable routine on this camera",
  3. **Confirm** — "did it actually come back on?"
- They also never decide *which* camera or *whether monitoring is on* — they ask `MyForm` for both, every single time.

So even if both watchdogs malfunctioned at once, the worst they could do is request extra *enable* operations (which are harmless no-ops when the camera is already on). They contain no "disable" call anywhere.

## The three questions before every action

Before a watchdog lifts a finger, it always checks the same three things:

1. **Is monitoring on?** If the user turned automation off → do nothing.
2. **Is the system shutting down?** If yes → do nothing.
3. **Is the camera *expected* to be enabled?** The main program tracks this flag: it is set when the camera is intentionally disabled (locked, asleep, shutting down). If disabled is the *correct* state → do nothing, and do it silently.

Only when all three say "yes, the camera should be on" does the watchdog even look at the actual device — and only if the device disagrees does it schedule a recovery.

## The two watchdogs: slow-and-steady + fast-and-eager

There are two of them because they cover different time scales:

### `CameraFailsafe` — the patient guardian (owned by `MyForm`)

- **Polls every 90 seconds:** "is the camera unexpectedly off?"
- **Waits 10 more seconds** to confirm it was not a passing glitch (a lock that happened a moment ago, for instance).
- **Recovers** with up to 3 attempts, spaced 10 → 20 → 40 seconds apart.
- **Rests 30 seconds** after any recovery before looking again.
- **Ignores everything for 45 seconds after startup**, so it never fights the startup recovery that is still settling.
- If all 3 attempts fail, it slows its own polling to every 3 minutes rather than hammering a broken device.

Worst case: a problem is found after ~90 s + 10 s confirmation ≈ **100 seconds**. Slow, but rock-solid and nearly free (one ~2 ms check every minute and a half).

### `RecoveryLoopFailsafe` — the quick responder (owned by `main.cpp`)

- **Checks once, 5 seconds after startup** — this is its main reason to exist: catch a camera that boot left disabled, fast.
- **Polls every 30 seconds** afterward as a backup.
- **Retries every 5 seconds** (fixed rhythm, not growing), up to 3 attempts.
- **Rests 30 seconds** after any recovery, same as its sibling.

Worst case: startup problem fixed in ~12 seconds; runtime problem in ~35 seconds. It lives outside `src/core` on purpose — `main.cpp` attaches it to the window's load/close events, so the frozen core files never had to change to gain a fast watchdog.

## How they avoid stepping on each other (and on you)

- **One active job each.** Both use a tiny state machine (`Idle` → `suspect a problem` → `recovering`). A second alarm arriving mid-recovery is simply absorbed into the running job — there is never a pile-up of overlapping recoveries.
- **They do not talk to each other — and do not need to.** Each keeps its own attempt counter and cooldown clock. If both spot the same outage, both request the same harmless enable routine; whoever runs second finds the camera already on and stands down.
- **They never fight legitimate states.** Lock your PC mid-recovery and the next check sees "expected disabled now" and abandons the job, logging a `SkippedExpectedDisabled` line. Same for suspend and shutdown.
- **Steady state is silent.** A healthy camera produces no log spam — just one `StartupVerification` line from the fast watchdog at boot. Only real detections, recoveries, failures, and give-ups are logged (all with `Failsafe_*` / `RecoveryLoop_*` names in `diagnostic.log`).

## A walkthrough: unexpected disable while you work

1. You are logged in, monitoring is on, camera is on.
2. Something outside the program disables the camera.
3. Within 30 s (fast watchdog's poll) or 90 s (patient guardian's poll), a watchdog notices: expected = on, actual = off.
4. It waits 5 s (or 10 s) and looks again — still off, and you have not locked or suspended meanwhile.
5. It asks core to run the standard enable routine on the target camera, then verifies the result.
6. Success → `Recovered` log line, counters reset, 30 s rest. Failure → `RecoveryFailed` with attempt number, wait, try again (max 3). Still failing → `MaxAttempts` logged, back to quiet polling, which will start a fresh cycle later if the camera is still off.

## Timing cheat sheet

| | `CameraFailsafe` | `RecoveryLoopFailsafe` |
|---|---|---|
| Owner | `MyForm` | `main.cpp` |
| Startup behavior | 45 s grace (looks away) | one check after 5 s |
| Regular poll | every 90 s | every 30 s |
| Confirmation delay | 10 s | 5 s |
| Retry rhythm | 10 → 20 → 40 s | 5 s flat |
| Attempts per cycle | 3 | 3 |
| Rest after recovery | 30 s | 30 s |
| After giving up | poll slows to 180 s | poll stays 30 s |

## Known quirks (documented, not fixed)

- The fast watchdog is disarmed whenever the window closes — including "close" that only hides it to the background — and is not re-armed afterward, since the load event never fires again. From then on only the 90-second guardian is watching.
- Both watchdogs check on timers only; there is no instant hardware notification wired up, so detection always waits for the next poll.
- The design documents once planned a 60-second poll and instant notifications for the guardian — the actual code polls every 90 seconds with timers only. The code is the truth.
