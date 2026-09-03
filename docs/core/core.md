# `src/core/` — How It All Works Together

**Folder:** `src/core/`
**Files:** `MyForm.h`, `MyForm_Camera.cpp`, `MyForm_Config.cpp`, `MyForm_Core.cpp`, `MyForm_Events.cpp`, `MyForm_System.cpp`, `MyForm_UI.cpp`
**Detail docs:** one per file in this same folder (`docs/core/*.md`)

## The one-sentence version

`src/core/` is the entire application: a small Windows program that keeps your camera enabled while you are using the PC, and automatically disables it when you lock the PC or it goes to sleep — so Windows Hello keeps working without you thinking about it.

## The main idea: one owner for everything

Everything revolves around a single object called `MyForm` — the program's window. It owns all the state:

- whether monitoring is on or off,
- which camera is the target,
- whether the camera is *supposed* to be disabled right now,
- the handles Windows gives it (single-instance lock, wake-up signal, power notifications),
- and the on-screen controls (camera picker, start/stop button, status text).

There are no separate "manager" or "controller" classes. Each `.cpp` file is just a chapter of `MyForm`'s behavior, grouped by topic. That is deliberate: it keeps behavior identical to the original proven version of the program.

## What each file does (plain language)

### `MyForm.h` — the table of contents

This is the declaration file. It lists *what exists* without containing the actual logic:

- the `MyForm` class: its true/false flags, handles, UI controls, and the names of all its functions,
- the `CameraDeviceInfo` struct: just a camera's display name plus its device ID,
- shared diagnostic counters (so logs can say *why* a camera operation failed),
- the names of the low-level camera helper functions.

Every other file includes this header. Think of it as the contract they all agree on.

### `MyForm_Camera.cpp` — the hands that touch the camera

This is the **only file allowed to actually enable or disable camera hardware**. It talks to Windows through two official channels:

1. **SetupAPI** — the general "change a device's state" mechanism.
2. **Configuration Manager** — a lower-level fallback if the first attempt does not stick.

Its most important habits:

- **Check before acting.** Before touching anything, it asks Windows "is the camera already in the state I want?" If yes, it does nothing. This avoids pointless on/off flickering.
- **Try, verify, retry.** Each operation is followed by a check ("did it really change?"), retried a few times with short pauses so the device has time to settle.
- **Recovery routine.** `RecoverCameraHardware` makes sure the camera ends up *enabled*. With the "cycle" option it briefly turns the camera off and back on — a stronger reset used at startup.
- **Targeted.** It always acts on exactly one camera (the selected one), matched by its device ID.

The watchdog code in `src/watchdog/` never touches hardware directly — it always asks *this* file's functions to do it.

### `MyForm_Config.cpp` — memory and diary

Two jobs, both about files stored under `%APPDATA%\Windows Hello Fix\`:

- **Memory (`config.txt`).** Two lines: is monitoring on (`monitoring=1/0`) and which camera (`device=<id>`). Saved whenever things change, read at startup so the program resumes where it left off.
- **Diary (`diagnostic.log`).** Every important event is appended here with a timestamp: startup, lock, unlock, camera operations, errors, watchdog activity. This log is how you figure out what the program did overnight.

It also contains the **"which camera?" decision**: live selection first, then the saved `device=` value, then a guess (the sub-device containing `MI_00`), then simply the first camera found. Every camera operation uses this one decision-maker, so there is never disagreement about the target.

### `MyForm_Core.cpp` — birth, startup, and death

This file handles the object's whole life:

- **Constructor:** sets every flag to a safe default, creates empty storage for the camera list and selection, then builds the window.
- **Startup (`MyForm_Load`)** — the most important sequence in the program, in strict order:
  1. Read command-line arguments (was it launched hidden? is this a one-shot `--enable-camera` job?).
  2. Write a startup line to the log (including whether it runs as admin).
  3. If it is a one-shot command (enable/disable camera), do that job and exit immediately — no window, no background activity.
  4. Single-instance check: if the program is already running, wake the old window (or quietly exit for background launches) instead of starting twice.
  5. Run the startup camera recovery, so a camera left disabled by a previous session comes back.
  6. Register for power notifications (lid, power button) and build the camera picker from currently present cameras.
  7. Re-enable the camera and, if background mode or the config says so, switch monitoring on (hiding the window for background launches).
  8. Start the background wake-listener thread and register for lock/unlock notifications.
  9. Arm the long-term watchdog (`CameraFailsafe`).
- **Destructor & finalizer:** mirror-image cleanup. If Windows is shutting down, leave the camera disabled; otherwise make sure it is re-enabled, save the config, and release all handles. The watchdog is disarmed *first* so it can never fire in the middle of shutdown.
- **Read-only accessors:** six tiny getters (`IsMonitoringActive`, `IsSystemEndingActive`, `IsCameraExpectedEnabled`, `TryGetFailsafeTargetId`, `LogFailsafe`, `LogFailsafeWithDevice`) that let the watchdogs observe state and write logs without being able to change anything.

### `MyForm_Events.cpp` — ears on Windows

A single function, `WndProc`, receives every Windows notification and sorts it into three buckets:

- **Shutdown/logoff:** set the "system is ending" flag, disable the camera if monitoring, unsubscribe from notifications.
- **Power (sleep, resume, lid, power button):** on suspend-like signals, disable the camera (once — a latch prevents repeats) plus a short safety pause; on resume, wait a second for devices to reappear, then re-enable.
- **Session lock/unlock:** lock → disable; unlock → enable.

Two protections keep this sane:

- **Debounce:** the same signal repeated within 1.5 seconds is ignored (Windows often sends duplicates).
- **Monitoring check:** if the user turned monitoring off, events are logged and ignored — the program never touches the camera unprompted.

### `MyForm_System.cpp` — command words and the wake-up call

- **Command words:** recognizes `--enable-camera` (and `/restore-camera`, `/repair-camera`) and `--disable-camera` (plus `/`-variants) so scheduled tasks and scripts can trigger one-shot camera jobs.
- **Wake listener:** a background thread that sleeps until another copy of the program signals the shared wake-up event — then it brings the main window back on screen. This is how double-clicking the app while it runs in the background shows the window instead of starting a duplicate.
- **Bring-to-front:** restores the hidden window (including resetting `Opacity` back to visible) and activates it.

### `MyForm_UI.cpp` — the two things the user can do

- **Close the window:** it does not actually quit — it hides to the background (with a one-time explanatory message). Real shutdown handling lives in the destructor.
- **Start/Stop Monitoring button:** start → remember the chosen camera, switch monitoring on, save config. Stop → switch monitoring off, re-enable the camera (so stopping never strands it disabled), save config, clear the selection.

## How a typical day flows through these files

1. **Sign-in:** `MyForm_Core` starts up, recovers the camera, arms monitoring and the watchdog.
2. **You lock (Win+L):** `MyForm_Events` hears it, `MyForm_Camera` disables the camera, `MyForm_Config` logs it.
3. **You unlock:** `MyForm_Events` hears it, `MyForm_Camera` re-enables, log line written.
4. **Something unexpected disables the camera:** the watchdogs in `src/watchdog/` notice and ask `MyForm_Camera` to re-enable it (see `docs/watchdog/watchdog.md`).
5. **Shutdown:** `MyForm_Events` flags it, `MyForm_Core` disarms the watchdog, leaves the camera disabled, saves config, cleans up.

## Why it is built this way

- **One authority per job:** hardware → Camera file; memory/logs → Config file; lifetime → Core file; Windows signals → Events file. If something goes wrong, you know exactly where to look.
- **Never act blindly:** check-before-change, verification after every operation, and the "expected disabled" flag mean the program does not fight legitimate states (locked, asleep, shutting down, user turned it off).
- **Everything is logged:** `diagnostic.log` records each step with PASS/FAIL, so behavior can always be reconstructed afterward.
