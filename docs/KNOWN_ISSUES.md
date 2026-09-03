# Known Issues & Observations

> Documentation only. Nothing here was fixed or changed in source. Each item is labeled **Observed** (verifiable in code) or **Potential** (inferred, unverified).

## Observed

1. **Duplicated cleanup in destructor and finalizer.** `MyForm::~MyForm` (lines 26–74) and `MyForm::!MyForm` (lines 76–106) both perform camera disable/enable (`DisableTargetCameraHardware`/`EnableTargetCameraHardware`) and free the same handles/native memory (`hWakeupEvent`, `hLidNotification`, `hButtonNotification`, `cachedCameras`, `selectedInstanceId`, `hAppMutex`). In C++/CLI, the destructor calls the finalizer, so on a deterministic dispose both run. This can cause the camera toggle to execute twice at shutdown. Impact: an extra (idempotent) enable/disable and double handle closing; the `CloseHandle` on an already-closed handle is guarded by the member being set to `NULL` only in the destructor, while the finalizer also closes — potential double-close is mitigated only if the runtime skips the finalizer after the destructor. **Not fixed.**

2. **`TryEnterHardwareToggleCooldown` / `RecordHardwareToggleTime` are effectively unused in the main flow.** `SetCameraHardwareStateVerified` calls `RecordHardwareToggleTime` (stamps `g_lastHardwareToggleTick`), but `TryEnterHardwareToggleCooldown` is never invoked anywhere in the current code. The `COOLDOWN_MILLISECONDS`/`lastToggleTime` `static` members are also unused for control flow (logging/UI only). This is a dormant code path inherited from v2.0. **Not fixed.**

3. **`cameraStateInitialized`, `restartQueuedByMismatch`, `lastCameraToggleTick` are declared but not meaningfully used.** Legacy/placeholder members retained from the original. **Not fixed.**

4. **Globals changed from `static` to `extern`.** In the original monolith the four `g_last*` variables were file-`static` (single TU). To support multiple `.cpp` translation units they are now `extern` in `MyForm.h` and defined once in `MyForm_Camera.cpp`. Behavior is preserved (single authoritative instance), but the change is a structural requirement of the extraction, not a behavioral one. **By design.**

5. **`system("taskkill /F /IM Windows_Hello_Fix_v2_0.exe /T")` in the ghost-reset path.** Shells out to kill all instances before `Application::Restart()`. Works but is a heavy-handed self-termination; inherited from v2.0. **Not fixed.**

6. **Build warnings:** `warning C4793` for `TryEnterHardwareToggleCooldown` and `RecordHardwareToggleTime` ("function compiled as native: found an intrinsic not supported in managed code"). These are expected for interlocked/`GetTickCount64` usage inside a `/clr` compile and match the original baseline. **No change.**

7. **`RecoveryLoopFailsafe` is disarmed by hide-to-background and never re-armed.** `OnOwnerClosing` runs on *every* `FormClosing` — including `UserClosing`, which `MyForm_FormClosing` cancels (hide-to-background, the normal "close" path). The form lives on but the fast verifier stays disarmed for the rest of the process lifetime, because `Load` never fires again. Traceable in source (`main.cpp:54` subscription + `MyForm_UI.cpp:6-8` cancel). After the user closes the window once, only `CameraFailsafe` (90 s poll) remains. **Not fixed** (documentation pass only).

8. **`CameraFailsafe` is timer-only; `docs/Plan.md §3` still describes a 60 s poll + `CM_Register_Notification` layer.** The current source polls every **90 s** (`kIdleIntervalMs = 90000`, `CameraFailsafe.h:39`) and contains no PnP notification registration; `MyForm_Events.cpp` has no corresponding `WM_APP+0x20` handler. A disable inside the 45 s startup grace, or between 90 s polls, waits for the next poll + 10 s confirmation (100 s worst). `RecoveryLoopFailsafe` (5 s / 30 s / 5 s) exists to cover this gap. **Not fixed** — recorded so the plan is not mistaken for the implementation.

9. **Two watchdogs keep independent cooldown/failure state.** `CameraFailsafe` and `RecoveryLoopFailsafe` each hold their own `consecutiveFailures`/`lastRecoveryTick`/`state` and never call each other. Simultaneous detection resolves to two back-to-back enable-only `Recover(false)` calls — idempotent, no disable involved — but `diagnostic.log` will show interleaved `Failsafe_*` and `RecoveryLoop_*` lines for the same incident. **By design** (no shared mutable state = no cross-watchdog race).

10. **`RequestRecoveryCheck(reason)` ignores its `reason` parameter.** All call sites pass `L"StartupVerification"`; the label is reserved for future event sources (PnP, resume). Harmless. **Not fixed.**

11. **`CameraFailsafe::Disarm` guard is nearly vacuous.** `if (!isArmed && pollTimer == nullptr && verifyTimer == nullptr) return;` only returns early when fully unarmed *and* both timers are null (i.e. the ctor never ran). Post-construction it always proceeds to stop timers — which is exactly the safe behavior, just expressed oddly. **Not fixed.**

## Potential / unverified

7. **Possible race on `selectedInstanceId` / `cachedCameras`.** These are raw pointers accessed on the UI thread and the (background) wake thread only touches UI, so no live race in practice; but there is no lock protecting the `std::wstring*`/`std::vector*` from concurrent access if a future change calls camera members off-thread. Currently safe.

8. **`isAlreadyDisabled` is a `static` inside `WndProc`.** It persists for the lifetime of the process and is shared across all messages. If a suspend event is missed (no resume received), the flag could remain `true` and suppress a later legitimate disable. Observed design; only a concern if Windows omits the resume message (unverified).

9. **Debounce windows are wall-clock `GetTickCount64` based.** A system clock jump (rare) could cause a missed or duplicated event. Inherited from v2.0.

10. **Config file has no locking.** `SaveConfigState` truncates and rewrites; concurrent writes (e.g., toggle + Load) are not guarded beyond the single-threaded UI model. Potential only under unexpected multithreading.

## Things that are NOT problems (clarified)

- The root `MyForm.h` shim is intentional and does not change behavior; `main.cpp` still includes the same logical header.
- Multiple `.cpp` files defining `MyForm` member functions is valid C++/CLI; there is exactly one class definition (in `MyForm.h`) and many out-of-line member definitions.
- No controller classes were introduced; `MyForm` remains the sole state owner (per the extraction rules).
- The two watchdogs are not a "second camera driver": neither `src/watchdog` file contains `SetupDi*`/`CM_*` device-state calls, neither parses `config.txt`, and neither mutates `MyForm` state — all hardware changes flow through `src/core/MyForm_Camera.cpp`.
- `CameraFailsafe` doubling its poll interval to 180 s after `MaxRetries`, and `RecoveryLoopFailsafe` keeping 30 s, is intentional asymmetry (long-term backoff vs persistent fast backup), not a bug.
