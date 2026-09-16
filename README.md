<p align="center">
  <!-- BANNER IMAGE PLACEHOLDER: HelloFix banner/logo (GitHub-hosted image) goes here -->
  <img width="210" src="x64/Release/WindowsHelloFix.ico" alt="HelloFix Logo">
</p>

# Windows Hello Fix v2.1 📸

**Windows Hello keeps picking the wrong camera. This makes sure it doesn't.**

[![Version: v2.1](https://img.shields.io/badge/version-v2.1-blue)](https://github.com/Shivu516/Windows-Hello-Fix/releases)
[![Platform: Windows 10/11 x86+x64](https://img.shields.io/badge/platform-Windows_10%2F11_x86%2Bx64-lightgrey)](https://github.com/Shivu516/Windows-Hello-Fix/releases)
[![License: MIT](https://img.shields.io/badge/license-MIT-green)](LICENSE)

Windows Hello Fix is a small native Windows app that runs quietly in the background and manages your RGB camera around lock, unlock, sleep, and startup — so face sign-in uses the IR sensor and just works, even in the dark.

<!-- SHOWCASE: STARTUP CAMERA RECOVERY -->

## 💡 The Problem

Many Hello-capable laptops have two front sensors: a normal RGB (color) camera and an infrared (IR) sensor. Windows, apparently, has decided that two cameras cooperating is an unreasonable request — after sleep or a lock cycle it often tries to recognize you with the slower, light-dependent RGB camera instead of the instant IR sensor, and you end up staring at an endless "Looking for you" loop before giving up and typing your PIN. Hello says hello to the wrong camera, every time.

The fix is almost insultingly simple: disable the RGB camera just before the system locks. With its favorite distraction gone, Windows Hello falls back to the IR sensor and unlocks near-instantly, even in pitch darkness. This app just automates that, in both directions — no more camera roulette at the lock screen.

## 🛠️ What Changed in v2.1

v2.1 is not just a feature update — the codebase itself was rebuilt into a more maintainable modular structure while preserving the behavior that made v2.0 reliable. v2.0 was the original native C++ overhaul; v2.1 continues that implementation, reorganized.

The first restructuring attempt was… ambitious. It also broke things. So it was abandoned in favor of a simpler extraction that keeps the proven v2.0 behavior intact — Windows was already providing enough debugging opportunities on its own.

Concretely, v2.1 brings:

- **Modular architecture** — the old monolith is now `src/core/` (camera, config, lifecycle, events, UI) plus `src/watchdog/` (recovery), with one clear owner per job.
- **x86 + x64 from one shared tree** — the same source builds both architectures (`Windows_Hello_Fix_v2_1_x86.exe` and `Windows_Hello_Fix_v2_1_x64.exe`); no forked codebases, no `src/x86` versus `src/x64` split.
- **Universal + standalone installers** — one `Windows_Hello_Fix_Setup.exe` that detects your architecture and installs the matching payload, plus pinned `Windows_Hello_Fix_Setup_x86.exe` / `Windows_Hello_Fix_Setup_x64.exe` for anyone who wants a specific build.
- **A real recovery system** — unexpected camera disables are detected and repaired automatically instead of lingering until the next lock cycle.
- **Startup hardening** — the camera is restored during launch, backed by a sign-in helper task that survives flaky boot-time triggers.
- **Professional diagnostics** — the log now records severity, category, per-operation correlation IDs, and trigger-to-action timings, and nightly cleanup only truncates it past 512 KB instead of wiping every midnight.
- **[Issue #2](https://github.com/Shivu516/Windows-Hello-Fix/issues/2) fixed** — summoning the background app can no longer leave you staring at an invisible window (details below).

## ✨ Features

### 🔒 Session automation

- **Lock / unlock handling** — disables the selected RGB sensor on session lock, re-enables it on unlock via native session notifications.
- **Sleep, lid, and power-button handling** — suspend-like signals disable the camera (once, via a latch); resume waits for the device tree to settle, then re-enables.
- **Shutdown awareness** — the camera is intentionally left disabled at system shutdown/logoff so the next boot starts from a clean biometric state, and re-enabled on normal exit so quitting the app never strands it off.

### 🛡️ Recovery & verification

- **Startup recovery** — the configured camera is restored during launch, before the UI is even built; a scheduled sign-in helper re-verifies shortly after logon.
- **Runtime recovery** — unexpected disables are detected on low-frequency polls and repaired through the standard enable path, with confirmation and bounded retries.
- **Check-before-change** — the current device state is always queried first; if the camera is already where it should be, nothing happens. No flicker, no redundant driver calls.
- **Triple-attempt verification** — every state change is confirmed against the hardware, retrying across both the SetupAPI and Configuration Manager paths with re-enumeration if Windows ignores the request.

### 🧰 Safety mechanisms

- **Duplicate-signal debounce** — Windows loves sending the same event twice; repeats within 1.5 seconds are ignored so drivers are never thrashed.
- **Single instance** — a system-wide mutex guarantees one manager; launching again simply wakes the running window, and a frozen ghost can be force-reset with the camera restored.
- **Background daemon** — scheduled launches run fully hidden (no window, no taskbar) and idle on OS event signals plus a few lightweight timer checks — no busy polling.
- **Diagnostic log** — every startup, event, camera operation, and recovery lands timestamped with severity and PASS/FAIL in `%APPDATA%\Windows Hello Fix\diagnostic.log`.

## 🧩 Architecture

```text
src/core/       → authoritative camera/session behavior (the app itself)
src/watchdog/   → recovery and safety mechanisms (two enable-only observers)
main.cpp        → entry point, hidden launch, fast-watchdog wiring
```

The core owns every state decision and is the only code that touches camera hardware. The watchdogs observe the expected-vs-actual state and request recoveries through the core's own pipeline — they implement no driver logic of their own. One shared tree compiles to both architectures via per-platform build targets, so x86 and x64 can never drift apart the way two forks would. Details: [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) and the [`docs/core/`](docs/core/core.md) / [`docs/watchdog/`](docs/watchdog/watchdog.md) overviews.

## 📦 Installation

### Universal installer (recommended for most users)

1. Download `Windows_Hello_Fix_Setup.exe` from the [**Releases**](https://github.com/Shivu516/Windows-Hello-Fix/releases) page.
2. Run it and follow the wizard (license → components → install folder). It detects whether Windows is x86 or x64, installs exactly one matching payload (`Windows_Hello_Fix_v2_1_x86.exe` or `Windows_Hello_Fix_v2_1_x64.exe`), registers four scheduled tasks (background launch at logon, lock helper, sign-in recovery helper, daily log cleanup), performs a warm-up camera restore, and offers to launch the app.
3. On first launch from the Start Menu, select your **RGB camera** from the drop-down and click **Start Monitoring Service**. That's the one-time setup — automation runs from then on.

> ⚠️ **Pick the RGB camera, not the IR sensor.** Disabling the wrong device will break Hello entirely — this is the one step worth doing carefully.

### Standalone installers

Prefer a fixed build instead of auto-detection? Grab the one that matches your system:

| Installer | Payload | Installs to |
|---|---|---|
| `Windows_Hello_Fix_Setup_x64.exe` | `Windows_Hello_Fix_v2_1_x64.exe` | `Program Files\WindowsHelloFix` |
| `Windows_Hello_Fix_Setup_x86.exe` | `Windows_Hello_Fix_v2_1_x86.exe` | `Program Files (x86)\WindowsHelloFix` |

Behavior — tasks, shortcuts, warm-up restore, uninstall — is identical to the universal installer. Native ARM64 builds are not shipped yet (see Roadmap); on ARM PCs the x64 build may run under emulation, which is not native support.

### WinGet

```powershell
winget install hellofix
```

This resolves to the community `Shivu516.WindowsHelloFix` package, which at last check serves **v2.0.0** — use the Releases installer above for v2.1. After a WinGet install, launch the app from the Start Menu and complete the same one-time camera binding.

## 🧹 Uninstallation

Uninstall via `Uninstall.exe` in the install folder, the Start Menu link, or **Settings → Installed Apps**. The uninstaller restores the camera first, stops the background program, deletes all scheduled tasks, removes the program files, shortcuts, and the `%APPDATA%\Windows Hello Fix\` data (`config.txt` and `diagnostic.log`), and cleans up its own Add/Remove Programs entries.

## 🖥️ Usage

Normal use needs no commands — pick the camera once, start monitoring, forget about it. The command line exists for Task Scheduler, troubleshooting, and the curious:

| Command | What it does |
|---|---|
| *(no arguments)* | Normal GUI mode — full window, camera picker, monitoring toggle. |
| `--background` (or `/background`) | Hidden daemon mode — no window, no taskbar; exits silently if the app is already running. Used by the logon task. |
| `--disable-camera` (or `/disable-camera`) | Disables the configured camera once, verifies, exits. Used by the lock helper task. |
| `--enable-camera` (or `/enable-camera`) | Enables the configured camera once, verifies, exits. Used by the sign-in recovery task. |
| `/restore-camera`, `/repair-camera` | Full restore pass (disable→enable cycle) then exit. Used by the installer warm-up and uninstall camera-restore steps. |

Note the slash-only pair: `/restore-camera` and `/repair-camera` have no `--` forms — the rest accept either prefix. Short-lived command runs never arm the fast watchdog; only the long-lived daemon gets one.

## 🛡️ Reliability & Failsafe

HelloFix normally expects the RGB camera to be enabled while you are actively using Windows. If Windows or another component leaves it disabled unexpectedly, the recovery system checks the actual hardware state and attempts to restore it — then verifies the result, retrying a few times before backing off.

Legitimate states are respected, not fought:

```text
Locked / suspended / shutting down:
    camera may intentionally be disabled — left alone.


Active unlocked session:
    camera should be enabled — recovered if found disabled.
```

Two watchdogs cover different time scales: a fast verifier that checks seconds after startup and polls every 30 seconds, and a long-term backstop polling every 90 seconds. Both are enable-only — neither can ever turn your camera *off* — and both stay silent when everything is healthy. Critically, neither is the lock/unlock authority: the native session listener makes those calls, and the watchdogs merely notice when reality disagrees with expectation. The full design is documented in [`docs/watchdog/watchdog.md`](docs/watchdog/watchdog.md).

<!-- SHOWCASE: FAILSAFE RECOVERY -->

### 🖥️ A note on Issue #2 — "HelloFix not Opening"

The background instance runs with a fully transparent window by design. Previously, waking it (by launching the app again) could show the window while leaving it transparent — the app was there, just invisible. v2.1 restores visibility whenever the window is summoned, and background/scheduled launches now exit silently instead of disturbing the running instance. Reported in [Issue #2](https://github.com/Shivu516/Windows-Hello-Fix/issues/2).

<!-- SHOWCASE: GUI -->

## ⚙️ How It Works

Normal day:

```text
PC running normally
        ↓
RGB camera enabled


User locks PC (Win+L)
        ↓
RGB camera disabled
        ↓
Windows Hello uses IR sensor


User unlocks
        ↓
RGB camera re-enabled
```

Recovery:

```text
Unexpected camera disable
        ↓
Failsafe detects state mismatch
        ↓
Camera recovery via the standard enable path
        ↓
Verified enabled state (or bounded retry, then quiet polling)
```

<!-- SHOWCASE: LOCK / UNLOCK BEHAVIOR -->

## 🩺 Diagnostic Logs

Everything lands in `%APPDATA%\Windows Hello Fix\diagnostic.log`, one line per event:

```text
[2026-09-06 22:41:03.191] [INFO ] [LOCK    ] SessionLock_Disable | Op=LOCK-000123 | TriggerToComplete=67ms | Target=Disabled | Verify=PASS
```

Each line carries a severity (`DEBUG`/`INFO`/`WARN`/`ERROR`), a category (`STARTUP`, `COMMAND`, `LOCK`, `POWER`, `SESSION`, `CAMERA`, `FAILSAFE`, `SYSTEM`), and — for multi-step operations — an `Op=` correlation ID so you can follow one lock, unlock, or recovery from trigger to verified completion. Camera operations add durations, attempt counts, which driver path won, and human-readable error text. A nightly task trims the log only once it passes 512 KB, so history survives across reboots. When reporting a problem, attach this file (plus which installer and architecture you used) — it usually answers the question before anyone has to ask a follow-up. Field guide: [`docs/DEBUGGING.md`](docs/DEBUGGING.md).

## ⚠️ Known Issues

- **Mid-session Hello prompts are out of scope.** The app acts on sign-in session and power transitions. Hello invocations that happen while the system is already running — browser passkeys, in-app Hello logins — are not intercepted, and a camera-switch failure there (of the `0xA00F4241` / `CameraSwitchFailed` variety) is not something v2.1 currently repairs. The failsafes only fix unexpected-disabled states they poll for; they do not watch the switch path itself. Improving this is on the roadmap, not in the release.
- **Lock/unlock scope.** Related to the above: automation follows session and power events, full stop.
- **Recovery takes seconds, not milliseconds.** Detection rides on polls (30 s fast loop, 90 s backstop), so an unexpected disable can linger briefly before repair.
- **Closing the window hides it.** The app keeps running in the background; to quit fully, stop the monitoring service or end the process in Task Manager.
- **Windows may force-disable frequently-toggled cameras** (observed upstream behavior, tracked in [Issue #1](https://github.com/Shivu516/Windows-Hello-Fix/issues/1)) — toggling thrash is exactly what the debounce and cooldowns exist to avoid.
- Full technical details live in [`docs/KNOWN_ISSUES.md`](docs/KNOWN_ISSUES.md).

## 🗺️ Roadmap

Planned, not promised — and definitely not already shipped:

- **Runtime camera-switch recovery** — extend the safety net toward failures like `0xA00F4241` when Hello fires mid-session. Current status: the failsafe infrastructure (poll → confirm → enable-only recover → verify) exists and works for unexpected-disabled states; watching the switch path itself is future work.
- **In-app updater** — a GUI-integrated client that checks for new HelloFix releases and notifies you, so upgrading stops meaning manual downloads. No updater code exists in the repo today.
- **Native ARM64 support** — the source is kept portable and the installer is already structured for a third payload, but C++/CLI plus .NET Framework 4.7.2 blocks a native ARM64 build for now, so v2.1 ships x86+x64. When the toolchain story changes, ARM64 slots in additively.
- **Event-driven detection** — the long-term watchdog is timer-only today; reacting to PnP notifications instead of polling is a possible future tightening.

## 🖥️ Compatibility

- **Windows 10 / 11, x86 and x64.** Built with the v143 toolset against the Windows 10 SDK, running on the .NET Framework 4.7.2 already present on these systems — no extra runtime to install.
- **Administrator rights are required.** Toggling camera hardware via the Windows device-management interfaces needs elevation, so the app runs elevated (via its manifest and Highest-privilege scheduled tasks). Without elevation, operations fail and the log says so.
- A laptop (or device) with Windows Hello face sign-in — an RGB camera plus an IR sensor — is the entire point; without that hardware there is nothing to fix.

## 🎬 Showcase

*Demo slots — media landing here as it is produced (see also the inline `SHOWCASE` slots above):*

- Startup camera recovery (boot → sign-in → verified enabled)
- Failsafe recovery (unexpected disable → detect → repair)
- GUI walkthrough (camera picker → Start Monitoring Service)
- Lock / unlock behavior (Win+L → RGB off → IR Hello → RGB back on)

## 📚 Documentation

| Document | Contents |
|---|---|
| [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) | Module map, state ownership, threading, API surface |
| [`docs/CAMERA_FLOW.md`](docs/CAMERA_FLOW.md) | Hardware pipeline: discovery, toggle, verify, recovery |
| [`docs/EVENT_FLOW.md`](docs/EVENT_FLOW.md) | Session/power/shutdown dispatch and debounce |
| [`docs/LIFECYCLE.md`](docs/LIFECYCLE.md) | Exact startup and shutdown ordering |
| [`docs/SOURCE_TREE.md`](docs/SOURCE_TREE.md) | File-by-file responsibility map |
| [`docs/FUNCTION_INDEX.md`](docs/FUNCTION_INDEX.md) | Every function and its callers |
| [`docs/DEBUGGING.md`](docs/DEBUGGING.md) | Reading `diagnostic.log`, symptom table |
| [`docs/KNOWN_ISSUES.md`](docs/KNOWN_ISSUES.md) | Observed issues and non-issues |
| [`docs/build/architecture-support.md`](docs/build/architecture-support.md) | x86/x64 builds, ARM64 stance and blockers |
| [`docs/core/core.md`](docs/core/core.md) | Plain-language core overview |
| [`docs/watchdog/watchdog.md`](docs/watchdog/watchdog.md) | Plain-language failsafe overview |
| [`docs/Plan.md`](docs/Plan.md) | Engineering history and design record |

## 📄 License

**MIT** — see [`LICENSE`](LICENSE).
