<p align="center">
  <!-- BANNER IMAGE PLACEHOLDER: HelloFix banner/logo (GitHub-hosted image) goes here -->
</p>

# Windows Hello Fix v2.1 📸

**Windows Hello keeps picking the wrong camera. This makes sure it doesn't.**

[![Version: v2.1](https://img.shields.io/badge/version-v2.1-blue)](https://github.com/Shivu516/Windows-Hello-Fix/releases)
[![Platform: Windows 10/11 x64](https://img.shields.io/badge/platform-Windows_10%2F11_x64-lightgrey)](https://github.com/Shivu516/Windows-Hello-Fix/releases)
[![License: MIT](https://img.shields.io/badge/license-MIT-green)](LICENSE)

Windows Hello Fix is a small native Windows app that runs quietly in the background and manages your RGB camera around lock, unlock, sleep, and startup — so face sign-in uses the IR sensor and just works, even in the dark.

<!-- SHOWCASE: STARTUP CAMERA RECOVERY -->

## 🚀 What is Windows Hello Fix?

It is a single lightweight background program, started automatically at sign-in by Task Scheduler. Once running, it listens for Windows session and power events and toggles the selected RGB camera accordingly: off when you step away, back on when you return. A recovery layer watches for states where the camera should be on but isn't — after boot, or after something else interfered — and restores it through the same verified hardware path.

There is no cloud component, no account, and no settings panel to babysit. You pick the camera once, start monitoring, and forget about it.

## 💡 The Problem

Many Hello-capable laptops have two front sensors: a normal RGB (color) camera and an infrared (IR) sensor. Windows, apparently, has decided that two cameras cooperating is an unreasonable request — after sleep or a lock cycle it often tries to recognize you with the slower, light-dependent RGB camera instead of the instant IR sensor, and you end up staring at an endless "Looking for you" loop before giving up and typing your PIN.

The fix is almost insultingly simple: disable the RGB camera just before the system locks. With its favorite distraction gone, Windows Hello falls back to the IR sensor and unlocks near-instantly, even in pitch darkness. This app just automates that, in both directions.

## 🛠️ What Changed in v2.1

v2.1 is not just a feature update — the codebase itself was rebuilt into a more maintainable modular structure while preserving the behavior that made v2.0 reliable. v2.0 was the original native C++ overhaul; v2.1 continues that implementation, reorganized.

The first restructuring attempt was… ambitious. It also broke things. So it was abandoned in favor of a simpler extraction that keeps the proven v2.0 behavior intact — Windows was already providing enough debugging opportunities on its own.

Concretely, v2.1 brings:

- **Modular architecture** — the old monolith is now `src/core/` (camera, config, lifecycle, events, UI) plus `src/watchdog/` (recovery), with one clear owner per job.
- **A real recovery system** — unexpected camera disables are detected and repaired automatically instead of lingering until the next lock cycle.
- **Startup hardening** — the camera is restored during launch, backed by a sign-in helper task that survives flaky boot-time triggers.
- **[Issue #2](https://github.com/Shivu516/Windows-Hello-Fix/issues/2) fixed** — summoning the background app can no longer leave you staring at an invisible window (details below).

## 🛡️ Reliability & Failsafe

HelloFix normally expects the RGB camera to be enabled while you are actively using Windows. If Windows or another component leaves it disabled unexpectedly, the recovery system checks the actual hardware state and attempts to restore it — then verifies the result, retrying a few times before backing off.

Legitimate states are respected, not fought:

```text
Locked / suspended / shutting down:
    camera may intentionally be disabled — left alone.


Active unlocked session:
    camera should be enabled — recovered if found disabled.
```

Two watchdogs cover different time scales: a fast verifier that checks seconds after startup and polls every 30 seconds, and a long-term backstop polling every 90 seconds. Both are enable-only — neither can ever turn your camera *off* — and both stay silent when everything is healthy. The full design is documented in [`docs/watchdog/watchdog.md`](docs/watchdog/watchdog.md).

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
- **Diagnostic log** — every startup, event, camera operation, and recovery lands timestamped with PASS/FAIL in `%APPDATA%\Windows Hello Fix\diagnostic.log`.

## 🧩 Architecture

```text
src/core/       → authoritative camera/session behavior (the app itself)
src/watchdog/   → recovery and safety mechanisms (two enable-only observers)
main.cpp        → entry point, hidden launch, fast-watchdog wiring
```

The core owns every state decision and is the only code that touches camera hardware. The watchdogs observe the expected-vs-actual state and request recoveries through the core's own pipeline — they implement no driver logic of their own. Details: [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) and the [`docs/core/`](docs/core/core.md) / [`docs/watchdog/`](docs/watchdog/watchdog.md) overviews.

## 🖥️ Compatibility

- **Windows 10 / 11, 64-bit.** Built with the v143 toolset against the Windows 10 SDK, running on the .NET Framework 4.7.2 already present on these systems — no extra runtime to install.
- **Administrator rights are required.** Toggling camera hardware via the Windows device-management interfaces needs elevation, so the app runs elevated (via its manifest and Highest-privilege scheduled tasks). Without elevation, operations fail and the log says so.
- A laptop (or device) with Windows Hello face sign-in — an RGB camera plus an IR sensor — is the entire point; without that hardware there is nothing to fix.

## 📦 Installation

### Graphical installer (recommended)

1. Download `Windows_Hello_Fix_Setup.exe` from the [**Releases**](https://github.com/Shivu516/Windows-Hello-Fix/releases) page.
2. Run it and follow the wizard (license → components → install folder). It registers four scheduled tasks (background launch at logon, lock/unlock helpers, daily log cleanup), performs a warm-up camera restore, and offers to launch the app.
3. On first launch from the Start Menu, select your **RGB camera** from the drop-down and click **Start Monitoring Service**. That's the one-time setup — automation runs from then on.

> ⚠️ **Pick the RGB camera, not the IR sensor.** Disabling the wrong device will break Hello entirely — this is the one step worth doing carefully.

### WinGet

```powershell
winget install hellofix
```

This resolves to the community `Shivu516.WindowsHelloFix` package, which at last check serves **v2.0.0** — use the Releases installer above for v2.1. After a WinGet install, launch the app from the Start Menu and complete the same one-time camera binding.

## 🧹 Uninstallation

Uninstall via `Uninstall.exe` in the install folder, the Start Menu link, or **Settings → Installed Apps**. The uninstaller restores the camera first, stops the background program, deletes all scheduled tasks, removes the program files, shortcuts, and the `%APPDATA%\Windows Hello Fix\` data (`config.txt` and `diagnostic.log`), and cleans up its own Add/Remove Programs entries.

## ⚠️ Limitations

- **Lock/unlock scope.** The app acts on sign-in session and power events. It does not intervene in Hello prompts that appear mid-session (browser passkeys, app logins) — for those, the cameras are on their own.
- **Recovery takes seconds, not milliseconds.** Detection rides on polls (30 s fast loop, 90 s backstop), so an unexpected disable can linger briefly before repair.
- **Closing the window hides it.** The app keeps running in the background; to quit fully, stop the monitoring service or end the process in Task Manager.
- **Windows may force-disable frequently-toggled cameras** (observed upstream behavior, tracked in [Issue #1](https://github.com/Shivu516/Windows-Hello-Fix/issues/1)) — toggling thrash is exactly what the debounce and cooldowns exist to avoid.
- Full technical details live in [`docs/KNOWN_ISSUES.md`](docs/KNOWN_ISSUES.md).

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
| [`docs/core/core.md`](docs/core/core.md) | Plain-language core overview |
| [`docs/watchdog/watchdog.md`](docs/watchdog/watchdog.md) | Plain-language failsafe overview |
| [`docs/Plan.md`](docs/Plan.md) | Engineering history and design record |

## 📄 License

**MIT** — see [`LICENSE`](LICENSE).
