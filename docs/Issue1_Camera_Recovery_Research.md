# Issue #1 Research — "PnP Enabled but Camera Unusable" and Same-Session Camera Recovery

> **Type:** Investigation & planning (read-only). No source, build, installer, or system state was modified.
> **Date:** 2026-10-03
> **Branch inspected:** `v2.2` (clean tree at `8857efa`)
> **Issue:** https://github.com/Shivu516/Windows-Hello-Fix/issues/1 ("Anomalous Windows Behaviour")
> **Evidence labels:** **Confirmed** = verified against official Microsoft documentation, Windows SDK headers, or the repository source during this investigation. **Likely** = consistent, credible secondary evidence. **Possible / Speculative** = plausible, not yet evidenced. **Unresolved** = needs runtime data.

---

## 1. Problem

Issue #1 reports that after several days of running HelloFix (which toggles the selected RGB camera on lock/unlock/power events), Windows enters a state where the camera stops working. Observed:

- The Windows Camera app fails with **`0xA00F4241 (0xC00D7167)`**; the failure appears "randomly after a few days of using this program."
- A **system restart fixes it** (the only reliable fix).
- **Reinstalling HelloFix has sometimes appeared to fix it** (anecdotal).
- The issue author's working theory was that Windows "forcefully disables the selected camera hardware if it is toggled on and off frequently," and proposed a periodic reset timer.

The issue discussion (and this project's own roadmap in `README.md` §Known Issues) frames the target state as:

```text
PnP state = Enabled
Camera actually usable = NO
```

This is the failure mode the existing failsafes cannot see (see §3).

## 2. Symptoms

| Signal | Meaning |
|---|---|
| `0xA00F4241` | Windows Camera app error family — camera start/initialization failure (see §5) |
| `0xC00D7167` | Sub-code carried by 0xA00F4241; see §5 |
| `CameraSwitchFailed` | Name used in the README/roadmap for this class of mid-session camera-switch failure (not present in the issue text itself) |
| Device Manager shows the camera **enabled and problem-free** while the camera is unusable | Distinguishes this failure from the states the existing watchdogs already repair |
| Reboot fixes | The failure lives in volatile state (driver stack / USB device state / camera stack), not in persistent configuration |

Important negative symptom: the camera remains **listed and enabled** in Device Manager / PnP. If Windows actually performed a PnP-level disable (`CM_PROB_DISABLED` / `CONFIGFLAG_DISABLED`), the existing `RecoveryLoopFailsafe` (30 s poll) or `CameraFailsafe` (90 s poll) would detect and re-enable it within seconds to minutes. The reported failure persists for days — so it is **below the resolution of the PnP state checks** this project currently performs.

## 3. Current architecture — what exists, and the exact gap

### 3.1 The single camera authority (`src/core/MyForm_Camera.cpp`)

All device mutation lives here (per the architecture rule enforced for the watchdogs). The relevant primitives:

| Function | Mechanism | Verification |
|---|---|---|
| `ToggleCameraHardware` | SetupAPI: `SetupDiSetClassInstallParams(DIF_PROPERTYCHANGE, DICS_ENABLE/DICS_DISABLE, DICS_FLAG_GLOBAL)` + `SetupDiCallClassInstaller` | none inline |
| `ToggleCameraHardwareCfgMgr` | CfgMgr: `CM_Enable_DevNode`/`CM_Disable_DevNode(CM_DISABLE_UI_NOT_OK)` then **`CM_Reenumerate_DevNode(devInst, 0)` on the target devnode itself** | none inline |
| `GetCameraHardwareDisabledState` | `SPDRP_CONFIGFLAGS & CONFIGFLAG_DISABLED` OR `CM_Get_DevNode_Status` problem == `CM_PROB_DISABLED (22)` | — |
| `VerifyCameraHardwareState` | 3× (`GetCameraHardwareDisabledState` + `Sleep(100)`) — **PnP-level only** | — |
| `SetCameraHardwareStateVerified` | check-before-change, up to 3 attempts alternating SetupAPI → CfgMgr paths, optional opposite-toggle "reinitialize" | PnP-level |
| `RecoverCameraHardware(target, cycle)` | ensure-enabled; if `cycle`: enable → Sleep 350 → **disable** → Sleep 900 → enable → Sleep 500 → enable | PnP-level |

**Key structural fact:** every check in the entire codebase answers only *"is the devnode PnP-enabled?"*. Nothing anywhere exercises the camera pipeline (driver → DMFT → frame server → capture). A grep confirms there is **no Media Foundation, DirectShow, or KS code anywhere** in the source tree.

### 3.2 The existing watchdogs (`src/watchdog/`)

| | `CameraFailsafe` (owned by `MyForm`) | `RecoveryLoopFailsafe` (owned by `main.cpp`) |
|---|---|---|
| Detects | "Expected enabled, observed PnP-Disabled" | same |
| Poll | 90 s | 5 s startup check + 30 s |
| Confirm / retry | 10 s confirm, 10/20/40 s backoff, 3 attempts | 5 s retry, 3 attempts |
| Recovery action | `RecoverCameraHardware(target, false)` (enable-only) | same |
| Blind to | **enabled-but-broken** (this issue), in-use, privacy, driver-stack failure | same |

Both watchdogs are correctly built as *observers* of the expected-vs-actual **PnP** state. They share the same blind spot because their only sensor is `GetCameraHardwareDisabledState` (§3.1). Confirmed by `README.md` §Known Issues: *"a camera-switch failure there (of the `0xA00F4241` / `CameraSwitchFailed` variety) is not something v2.2 currently repairs. The failsafes only fix unexpected-disabled states they poll for."*

### 3.3 Full-cycle restore already runs frequently — and does not prevent the failure

The disable→enable cycle is not a rare event in the current design:

- **Every sign-in:** the `WindowsHelloFix_Unlock` scheduled task (AtLogOn, PT10S delay, `--enable-camera`) hits the `IsRestoreCameraCommand` early-exit (`src/core/MyForm_Core.cpp:210-222`) → `RestoreConfiguredCameraHardware(true)` → **full cycle** (`RecoverCameraHardware(target, true)`).
- **Every daemon startup:** `MyForm_Load` → `RestoreConfiguredCameraHardware(true)` (`src/core/MyForm_Core.cpp:320`) → full cycle.
- **Every install:** `installer/common.nsh:142` and `:249` run `/restore-camera` (full cycle) twice, plus `Sleep 3000/2500`.
- **Every uninstall:** `installer/common.nsh:267` (pre-kill) and `:280` (post-kill) run `/restore-camera` — i.e. a full cycle with **all** HelloFix processes killed and (typically) no camera consumer running.

### 3.4 Findings about the existing recovery path (evidence from source + docs)

**F1 — Confirmed: `CM_Reenumerate_DevNode` is called on the wrong tree level.**
`ToggleCameraHardwareCfgMgr` calls `CM_Reenumerate_DevNode(devInst, 0)` with `devInst` = the target camera interface devnode itself (e.g. `USB\VID_04F2&PID_B829&MI_00\...`). Per the official documentation, the function "enumerates the devices identified by a specified device node **and all of its children**" and acts as a bus rescan only when the node "represents a hardware or software **bus** device." A USB interface devnode (`MI_00`) is a leaf — it has no children, so the call is effectively a no-op. Genuine re-detection of the USB device requires re-enumerating the **composite parent** (e.g. `USB\VID_04F2&PID_B829\0001`) or higher. (https://learn.microsoft.com/en-us/windows/win32/api/cfgmgr32/nf-cfgmgr32-cm_reenumerate_devnode)

**F2 — Confirmed: re-enumeration never restarts a started device's driver stack.**
Docs: re-enumeration detects arrival/removal and starts unconfigured children; "it does **not** stop/restart the driver stack of an already-started devnode." So none of the existing operations performs what Device Manager's "restart device" does. (Same doc.)

**F3 — Confirmed: the documented device *restart* mechanism is `DICS_PROPCHANGE`.**
`SP_PROPCHANGE_PARAMS` docs: for `DICS_PROPCHANGE`, "Windows ignores the Scope information as long it is a valid value, and **stops and restarts the device**." Microsoft's docs repeatedly direct application developers to this mechanism — `CM_Setup_DevNode` and `CM_Query_And_Remove_SubTreeW` remarks both say to prefer `DIF_PROPERTYCHANGE`; `DICS_STOP`/`DICS_START` are explicitly not for applications. This exact sequence (`DIF_PROPERTYCHANGE` + `DICS_PROPCHANGE` + `SetupDiCallClassInstaller`) is what `devcon restart` implements and what `pnputil /restart-device` (Win10 2004+) is documented to do. (https://learn.microsoft.com/en-us/windows/win32/api/setupapi/ns-setupapi-sp_propchange_params, https://learn.microsoft.com/en-us/windows-hardware/drivers/devtest/pnputil-command-syntax)

**F4 — Confirmed: DEVINST handles are not stable across state changes; the device instance ID is.**
`SetupDiChangeState` documents `DeviceInfoData` as IN-OUT because "`DeviceInfoData.DevInst` might be updated with a new handle value upon return." The device instance ID (e.g. `USB\VID_04F2&PID_B829&MI_00\6&321DD860&1&0000`) is the persistent identity (re-locatable via `CM_Locate_DevNodeW`). Any recovery implementation must re-resolve the DEVINST from the instance ID and must rediscover the device if re-enumeration recreates the subtree. (https://learn.microsoft.com/en-us/windows/win32/api/setupapi/nf-setupapi-setupdichangestate)

**F5 — Confirmed: the current design has no way to even *observe* the Issue #1 failure.**
Consequence of §3.1–3.3: on the failure signature of Issue #1, `EnableTargetCameraHardware` logs `AlreadyEnabled`, both watchdogs log nothing, the installer's `/restore-camera` sees "enabled," and the diagnostic log reports a fully healthy system. The only observable is the Camera app's error. **This is the primary instrumentation gap to close.**

## 4. Root-cause investigation

The exact trigger is not reproducible on demand (appears "randomly after a few days"). The findings below are what the evidence supports; the failure state itself has not yet been captured live — capturing it (log + PnP + MF state at failure time) is a first-order goal of the diagnostics proposed in §9.

| # | Hypothesis | Status | Rationale |
|---|---|---|---|
| H0 | **Media Foundation machine-level wedge — `MF_E_REBOOT_REQUIRED` state** (the confirmed meaning of `0xC00D7167`, §5): MF/frame-server machine state flags the capture environment as needing a reboot; the devnode and driver can be perfectly healthy. | **Confirmed as the reported error; the underlying wedged component (likely Frame Server) is Likely** | The sub-code is an MF `FACILITY_MEDIASERVER` HRESULT whose literal text is "We need to reboot the machine." Matches every field observation: PnP green, reboot fixes, device-level toggles unreliable. Microsoft-acknowledged frame-server wedging exists as a failure class (24H2 known issue "Camera use might cause some applications to become unresponsive", affected facial-recognition scenarios). |
| H1 | **Driver/DMFT wedge below PnP resolution**: after many disable/enable cycles, the UVC driver stack (`usbvideo` + Realtek RGB DMFT per the live capture in `docs/Anomaly_Investigation.md §C.4`) wedges at streaming-init time while the devnode starts fine. | **Possible (downgraded)** | Would produce start-time failures (0xC00D3704-class) invisible to PnP status — but the Issue #1 sub-code is `MF_E_REBOOT_REQUIRED`, a machine-level MF state, not a driver start failure. H1 may coexist as a trigger that *pushes* MF into the reboot-required state. |
| H2 | **USB device-level stall** requiring re-detection (the composite device's children need a bus re-enumeration to recover, which the current code never performs at the right level — F1). | **Possible (downgraded)** | Explains why a reboot (full re-enumeration) fixes it and why the existing cycle may not — but again, the confirmed error is above the device level; H2 is now a secondary contributor hypothesis. |
| H3 | **Frame Server stale state / ghost client**: the Frame Server service (svchost, session 0) holds wedged state or a dead client's reference after hundreds of enable/disable cycles; every new activation is refused until the service (or the machine) restarts. | **Likely — most probable concrete carrier of H0** | The frame server mediates all modern camera activations; community reports specifically tie 0xC00D7167-class failures to it and report service restart as the working non-reboot fix (§5.4). Microsoft-acknowledged frame-server wedging (24H2 known issue) proves the layer can wedge with healthy devices. |
| H4 | **PnP-level disable** (the issue author's original theory, "Windows forcefully disables the camera"). | **Mostly excluded** | A persistent PnP disable (`CM_PROB_DISABLED`/`CONFIGFLAG_DISABLED`) would be detected and repaired by the existing watchdogs within ≤ 100 s; the reported failure persists for days. A *transient* problem code (e.g. 43 `CM_PROB_FAILED_POST_START`, 10 `CM_PROB_FAILED_START`, 55 `CM_PROB_CONSOLE_LOCKED`) at the moment of failure is still possible and would be caught by the richer status logging proposed in §9. |
| H5 | **Privacy gate** (Settings → Privacy → Camera off, or a hardware shutter). | **Distinguishable, not the target** | Produces `E_ACCESSDENIED` at MF activation while PnP remains OK — superficially similar. Must be recognized and explicitly *not* "repaired" (see §6.4). A physical privacy shutter on the RGB camera also leaves PnP OK; note the health check would still stream successfully in the shutter case (sensor enumerates, frames may be black), so it does not confound detection. |

**Plausible unified mechanism (Speculative — coherent with all confirmed facts, unverified at code level):** hundreds of PnP enable/disable cycles leave the Frame Server holding stale per-device-instance state (ghost references to defunct device instances); after enough cycles MF classifies the capture environment as unrecoverable and returns `MF_E_REBOOT_REQUIRED` to every new activation, while PnP keeps reporting the (perfectly functional) device. Reboot restarts the Frame Server service from scratch. No public source or KB pinpoints the exact raise-site — this remains the key runtime question the §9 diagnostics will answer.

**Why a reboot fixes it — Confirmed mechanically:** reboot re-enumerates the entire device tree, rebuilds every driver stack from `AddDevice`, and restarts the Frame Server service. Both H1/H2/H3 predict reboot recovery.

**Why reinstalling sometimes appears to fix it — Unresolved, attribution suspect.**
What reinstall actually changes (`installer/common.nsh`): kills all HelloFix processes, runs `/restore-camera` (full camera cycle) up to 3 times (twice during install, once pre/post-kill during uninstall), recreates scheduled tasks, rewrites config. But §3.3 shows the *same full cycle already runs at every sign-in* — if a cycle fixed the wedge, the sign-in cycle would fix it too. Therefore either (a) the reinstall got credit for a recovery that was going to happen anyway (reboots commonly accompany reinstalls) — **most plausible**; (b) back-to-back cycles in a no-consumer window (uninstall kills everything first) occasionally succeed where a single cycle fails; or (c) deleting/recreating the tasks clears some stuck scheduler state — no mechanism identified, speculative. **Conclusion: do not build the recovery strategy on the reinstall observation; treat it as noise with a possible weak signal for "multiple cycles + closed consumers".**

**What the issue's "reset timer" idea maps to:** a periodic unverified enable/cycle. Rejected as a primary mechanism: it adds *more* of the very transitions suspected of causing the problem (Plan B concern), it cannot verify success (no health check), and it would fight the intentional `ExpectedDisabled` (lock/suspend/shutdown) states unless heavily guarded. The researched alternative — verify, then recover on evidence — supersedes it.

## 5. Error-code research — `0xA00F4241` / `0xC00D7167`

### 5.1 `0xC00D7167` = **`MF_E_REBOOT_REQUIRED`** — Confirmed (Windows SDK, `um/Mferror.h` line 2668, SDK 10.0.26100)

```c
// MessageId: MF_E_REBOOT_REQUIRED
// MessageText:
// We need to reboot the machine.%0
#define MF_E_REBOOT_REQUIRED             _HRESULT_TYPEDEF_(0xC00D7167L)
```

- Facility `FACILITY_MEDIASERVER` (0x00D) — **Media Foundation**, not a driver or vendor code. The `0xC00D71xx` block sits in mferror.h's protected-media / Input-Trust-Authority section (neighbors: `MF_E_NON_PE_PROCESS 0xC00D7165`). Historically raised when the protected-media/MF machine state needs a reboot after a component change.
- **Literal meaning: Media Foundation's machine state is wedged and MF itself demands a reboot.** This matches the field evidence exactly: Device Manager stays green (the failure is *above* the driver), and a reboot fixes it — the error name is the fix.
- Caution: do not confuse with the **transposed** code `0xC00D7176` (`MF_E_INCOMPATIBLE_SAMPLE_PROTECTION`) which appears in some mirrored forum threads.
- No Microsoft support page documents `0xA00F4241` or `0xC00D7167` (checked "Camera app shows error 0xA00F4244" and "Camera doesn't work in Windows"). The exact pair `0xA00F4241 (0xC00D7167)` is confirmed in the wild in RGB+IR / Windows Hello contexts (r/Surface "Windows Hello just plain doesn't work"; Microsoft Q&A threads; and this project's Issue #1). The same underlying `0xC00D7167` also appears wrapped as `0xA00F429F <WindowShowFailed>` — the 0xA00F42xx wrapper varies with which Camera-app operation failed; the parenthesized HRESULT is the substance.

### 5.2 What the wrapper is — Likely (no official documentation exists)

The Windows Camera app reports its own `0xA00F42xx` wrapper + the underlying HRESULT. `0xA00F4241` carries the tag `<CameraSwitchFailed>` in real reports — but the wrapper is **not** one specific cause: `0xA00F4241 (CameraSwitchFailed)` has been observed wrapping `0xC00D7167` (`MF_E_REBOOT_REQUIRED`) and `0xC00DABE0` (`MF_E_NO_CAPTURE_DEVICES_AVAILABLE`) alike. The sub-code is diagnostic; the wrapper is not.

### 5.3 Neighboring-code map (authoritative symbols from local `Mferror.h`; wrappers from support pages/community)

| Camera app shows | Underlying HRESULT | Symbol | Meaning |
|---|---|---|---|
| 0xA00F4244 (NoCamerasAreAttached) | 0xC00D36D5 | `MF_E_NOT_FOUND` | device not found at all (PnP/enumeration level) |
| 0xA00F4243 / 0xA00F4271 | 0xC00D3704 | `MF_E_HW_MFT_FAILED_START_STREAMING` | hardware MFT (e.g. Realtek DMFT) failed to start streaming / no resources |
| (various) | 0xC00D36FA | `MF_E_CANNOT_CREATE_SINK` | MF could not build the capture pipeline |
| 0xA00F4241 (CameraSwitchFailed) | 0xC00DABE0 | `MF_E_NO_CAPTURE_DEVICES_AVAILABLE` | no capture device available at MF level |
| **0xA00F4241 (CameraSwitchFailed)** | **0xC00D7167** | **`MF_E_REBOOT_REQUIRED`** | **the Issue #1 signature** |
| 0xA00F429F (WindowShowFailed) | 0xC00D7167 | `MF_E_REBOOT_REQUIRED` | same underlying code, different wrapper |
| (camera in use) | 0xC00D4E24 | `MF_E_VIDEO_DEVICE_LOCKED` | device locked by another path |
| (privacy block) | 0xC00DB798 | `MF_E_CAMERA_PRIVACY_NOT_ALLOWED` | camera access denied by privacy policy |

### 5.4 Interpretation for this project — Likely

`0xA00F4241 (0xC00D7167)` = the Camera app asked Media Foundation to start/switch the camera source and **MF returned "reboot required"**: a Media Foundation / Frame Server **machine-level software-stack failure** with the PnP device healthy. Which component exactly holds the wedged state is not publicly documented (no KB or source pinpoints what raises `MF_E_REBOOT_REQUIRED` in the capture path), but the Frame Server (`FrameServer` service, svchost) mediates every camera activation on modern Windows and is the layer with Microsoft-acknowledged wedging history — including the confirmed Windows 11 24H2 known issue "Camera use might cause some applications to become unresponsive" (affected facial-recognition scenarios; tracked in Windows Release Health 2024–2025). Community-reported **non-reboot remedies** for this signature, in confidence order:

1. **Restart the "Windows Camera Frame Server" service** — Likely (most-cited non-reboot remedy for 0xA00F429F/0xC00D7167-class reports; one MS Q&A user had the service running yet still broken, so not universally sufficient).
2. **Full shutdown / hard power drain** — Likely (a "reboot in disguise"; consistent with the error's semantics).
3. Device Manager disable/enable, device uninstall, "scan for hardware changes", driver reinstall — Possible (standard advice for 0xA00F4241 generally; **unverified** whether a device-level toggle clears a machine-level MF flag — plausibly explains why "reinstalling HelloFix" only *sometimes* appears to fix it).
4. `EnableFrameServerMode=0` registry bypass — Possible but requires a restart to take effect (not a same-session fix; global system-behavior change; not appropriate for this project).

## 6. Windows API research

### 6.1 Configuration Manager (cfgmgr32)

All verified against learn.microsoft.com (and constant values against the Windows SDK 10.0.26100 headers).

| API | Documented semantics | What it does NOT guarantee |
|---|---|---|
| `CM_Enable_DevNode` / `CM_Disable_DevNode` | Enable/disable a devnode. Plain disable is **not persistent** across reboot unless `CM_DISABLE_PERSIST` (Win10+). | Docs list no privilege; operationally requires elevation (`CR_ACCESS_DENIED` otherwise). Does not by itself re-detect hardware. |
| `CM_Reenumerate_DevNode` | Bus rescan of the node **and its children**; starts unconfigured children; initiates surprise-removal of absent ones. Flags: `CM_REENUMERATE_NORMAL`/`SYNCHRONOUS` (equivalent), `ASYNCHRONOUS`, `RETRY_INSTALLATION` (dangerous: can prompt user; only DM/HW wizard use). Requires **SeLoadDriverPrivilege**. | Does **not** restart an already-started devnode's driver stack. Even SYNCHRONOUS completion "does not guarantee that the drivers … have rescanned their bus" nor that new devices are started. |
| `CM_Get_DevNode_Status` | `pulStatus` (DN_ flags: `DN_STARTED 0x8`, `DN_HAS_PROBLEM 0x400`, `DN_DISABLEABLE 0x2000`, …) + `pulProblemNumber` (valid only when `DN_HAS_PROBLEM`). | A clean status says nothing about whether the camera stack can stream (the core blind spot). |
| `CM_Locate_DevNodeW` | Resolve a device instance ID → DEVINST. `CM_LOCATE_DEVNODE_NORMAL` = only if present/configured (`CR_NO_SUCH_DEVNODE` otherwise); `PHANTOM` also matches registry-only nodes. | — |
| `CM_Setup_DevNode` | "Restarts a device instance that is not running because there is a problem with the device configuration" (`CM_SETUP_DEVNODE_READY`); no-op if already started. Remarks direct apps to `DIF_PROPERTYCHANGE` instead. | Only helps problem-state devnodes; the Issue #1 device has **no** problem state. |
| `CM_Query_And_Remove_SubTreeW` / `CM_Request_Device_EjectW` | Prepared removal of subtree / safe eject. Remarks direct apps to `DIF_PROPERTYCHANGE` for enable/disable/restart. Eject is wrong for a non-ejectable internal camera. | Heavier than needed; veto semantics; require SeLoadDriverPrivilege / SeUndockPrivilege. |

**Relevant `CM_PROB` codes (Confirmed, cfg.h + Device Manager error messages page):** 10 `CM_PROB_FAILED_START`, 14 `CM_PROB_NEED_RESTART`, 22 `CM_PROB_DISABLED` (the only one current code checks), 43 `CM_PROB_FAILED_POST_START` (Code 43 — note: there is no `CM_PROB_STOPPED`), 45 `CM_PROB_PHANTOM`, 54 `CM_PROB_DEVICE_RESET`, 55 `CM_PROB_CONSOLE_LOCKED` (notably relevant: devices can be blocked *while the console is locked* — exactly when HelloFix toggles).

**Common `CONFIGRET` errors:** `CR_SUCCESS 0`, `CR_NO_SUCH_DEVNODE 0x0D`, `CR_REMOVE_VETOED 0x17`, `CR_NOT_DISABLEABLE 0x28`, `CR_ACCESS_DENIED 0x33`.

### 6.2 SetupAPI device restart — the documented mechanism

- `SP_PROPCHANGE_PARAMS` with `DIF_PROPERTYCHANGE`: `DICS_ENABLE 0x1`, `DICS_DISABLE 0x2`, **`DICS_PROPCHANGE 0x3` — "stops and restarts the device"** (Scope ignored for PROPCHANGE). `DICS_STOP`/`DICS_START` exist but docs say applications "should not specify" them and should use `DICS_PROPCHANGE` instead.
- Dispatch: `SetupDiSetClassInstallParams` → `SetupDiCallClassInstaller(DIF_PROPERTYCHANGE, …)`; default handler `SetupDiChangeState` — **caller must be a member of Administrators**.
- Post-call: check `SetupDiGetDeviceInstallParams` for `DI_NEEDREBOOT`/`DI_NEEDRESTART` — a restart can be **deferred** (typically because handles are open).
- Prior art — all three Windows mechanisms converge on this exact sequence:
  - **devcon restart** (`microsoft/Windows-driver-samples`, `setup/devcon/cmds.cpp`): `DICS_PROPCHANGE`, `DICS_FLAG_CONFIGSPECIFIC`, then honors `DI_NEEDRESTART|DI_NEEDREBOOT`.
  - **pnputil /restart-device** (in-box, Win10 2004+, documented; Microsoft explicitly recommends pnputil over devcon).
  - **Device Manager "restart/disable/enable"**: property pages set `DI_FLAGSEX_PROPCHANGE_PENDING` → same `DIF_PROPERTYCHANGE` dispatch. At kernel level disable = `IRP_MN_QUERY_REMOVE_DEVICE` (+ remove when handles close), enable/re-enumeration = `AddDevice` + `IRP_MN_START_DEVICE` ("Understanding when Remove IRPs are Issued").
- **Open-handle semantics:** a disable/restart with open handles can be vetoed or deferred (`CR_REMOVE_VETOED`, `DI_NEEDRESTART`). In practice HelloFix's existing disable/enable of this camera succeeds routinely, so handle-veto is not expected to block the RGB camera in practice (the Frame Server releases the device between clients).
- **WOW64 caveat (Confirmed):** `SetupDiCallClassInstaller` fails with `ERROR_IN_WOW64` for a 32-bit process on 64-bit Windows — the x64 OS must run the x64 payload (the installer already deploys per-arch binaries).

**Conclusion:** a *new*, currently-unused primitive — devnode restart via `DICS_PROPCHANGE` — is the documented, supported, in-process equivalent of Device Manager's "restart device" and is the strongest candidate for same-session recovery (§7). `pnputil /restart-device` exists as an external-process fallback but spawning a process adds no capability over the in-process API and loses structured error reporting; in-process is preferred.

### 6.3 Device enumeration & identity across recovery

- **Stable identity:** the device **instance ID** (`USB\VID_…&PID_…&MI_00\…`) — persistent in `HKLM\SYSTEM\CurrentControlSet\Enum`, re-locatable with `CM_Locate_DevNodeW`. **Confirmed.**
- **Unstable:** the `DEVINST` handle (F4). Never cache it across a restart/re-enumeration.
- **Rediscovery:** if the composite parent is re-enumerated, the PnP manager re-creates child devnodes with the **same** instance IDs (Likely — universal observed behavior; not contractually worded). Recovery code must therefore: re-locate by ID after each rung; if `CR_NO_SUCH_DEVNODE`, re-enumerate the parent and re-locate again before declaring failure.
- **MF-side identity:** `MFEnumDeviceSources` returns `MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK` per device; for USB cameras this interface path embeds the device instance ID (lowercase, `\`→`#`). Matching the configured instance ID against the symbolic link selects *exactly* the configured RGB device. (Likely — standard device-interface path format; verify against live values during implementation.)

### 6.4 Camera health check — what "actually usable" means and how to test it

Three levels were researched (details and sources in the companion research notes):

| Level | Proves | Misses | Cost |
|---|---|---|---|
| 0. PnP status (current code) | devnode enabled | **everything user-mode** | ~2 ms |
| 1. `MFEnumDeviceSources` | device enumerated by the camera stack | driver/DMFT/frame-server health, run-state, privacy effects | low |
| 2. `ActivateObject` / `MFCreateDeviceSource` | driver + DMFT load, device opens through the Frame Server | **run-state failures** (documented HLK failures occur at Start, not activation) | medium |
| 3. `Start` + first frame (`IMFSourceReader::ReadSample` or SampleGrabber sink) | end-to-end: KS run state → sensor → DMFT → frame server → user-mode frame | nothing (for "can this camera produce frames") | ~1–3 s real capture (LED on) |

**Confirmed facts that shape the design:**
- `IMFMediaSource::Start` is **asynchronous** — a bare `S_OK` proves nothing; success must be judged by `MESourceStarted` event status or the first `OnReadSample(S_OK)` with a non-null sample.
- MF has **no built-in timeouts** (`GetEvent(…, 0)` blocks indefinitely); Microsoft's own HLK camera tests budget **9–10 s per operation** and treat timeout as failure. The health check must run on a dedicated thread with its own deadline; a thread stuck past a hard deadline is abandoned (leaked, never killed mid-call) and counted as wedged.
- Mandatory cleanup: `IMFMediaSource::Shutdown` (docs: failure to call it leaks/holds device references) — `Stop → Shutdown → release → MFShutdown` on every exit path.
- **Privacy gate (Confirmed):** `E_ACCESSDENIED` at `ActivateObject` when camera access is disabled in Settings → Privacy → Camera. This is **not** a wedge — the watchdog must classify it "privacy, do not repair" (log and stand down).
- **"In use" is not a failure:** with the Frame Server (default since ~Win10 1703) access is shared; in-use conditions surface as `0xC00D3704`/`MF_E_VIDEO_DEVICE_LOCKED` and mean the device pipeline *responds* — classify as healthy, never recover over it.
- **Windows Hello safety (Confirmed via Chromium source + KS category docs):** the IR/Hello sensor is enumerated under `KSCATEGORY_SENSOR_CAMERA`; default VIDCAP enumeration (`MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID` without a category attribute) sees only normal (`KSCATEGORY_VIDEO_CAMERA`) devices — i.e. the RGB camera. A health check that (a) uses default enumeration and (b) matches the symbolic link against the configured target instance ID **cannot touch the IR camera or any other camera**, even on multi-camera systems.
- DirectShow (`IAMCameraControl` etc.) is officially legacy and does not exercise streaming; rejected.

**Minimum viable health check (recommendation):** Level 2+3 — enumerate (default category), match target by symbolic link, activate, `ReadSample` once via `MFCreateSourceReaderFromMediaSource`, bounded ~10 s total on a dedicated native thread, full cleanup, classify `{Healthy, PrivacyDenied, BusyHealthy, Wedged, Timeout, NotFound}`.

## 7. Candidate recovery strategies

| Strategy | Assessment | Verdict |
|---|---|---|
| **A. Detect + same-session recovery** (health check; on failure perform controlled device restart; re-verify; bounded) | Directly addresses the gap; uses documented mechanisms (§6.2); matches the issue's Plan A intent. | **Adopt** (design in §8) |
| **B. Reduce transitions** (debounce, skip redundant toggles, cross-event cooldowns) | Real prevention value, but the transitions HelloFix performs are the product's core function (lock → disable is the point), cooldowns already exist (1500 ms dedup, 30 s watchdog cooldowns), and every enable already passes a check-before-change gate. B is a complement, not a fix — and must not touch `src/core` per the task constraints. | Note for future; not this mechanism |
| **C. Enable RGB only on demand** (watch camera access, enable only while in use) | Requires reliable user-mode detection of *arbitrary* camera access. No documented mechanism: camera-stack activation events are not exposed to third parties; polling MF enumeration adds load and still misses usage start/stop. Impractical and architecturally invasive. | **Rejected** (research-only, as instructed) |
| **D. Registry/state spoofing** | Undocumented, creates exactly the inconsistent-state hazard the issue warns about (`Registry=Enabled / PnP=Disabled / Driver=Error`). No benefit over A. | **Rejected — never implement** |
| Periodic unverified reset timer (issue author's original idea) | Adds unverified toggles; cannot confirm success; risks fighting `ExpectedDisabled` states. Superseded by A's verify→recover-on-evidence design. | **Rejected** |

## 8. Chosen recovery strategy

### 8.1 Concept

```text
Expected-enabled moment (post-unlock / post-resume / post-enable / periodic low-freq)
        ↓
PnP check (existing, ~2 ms)  ── disabled → existing watchdogs handle it (unchanged)
        ↓ enabled
NEW: camera-stack health check (MF, RGB target only, ≤ ~10 s, dedicated thread)
        ↓
Healthy / PrivacyDenied / Busy → resume normal operation (log result)
        ↓ Wedged / Timeout / NotFound
CameraRecoveryFailsafe escalation ladder (bounded, cooled-down):
   1. DICS_PROPCHANGE restart of the target devnode   (documented "restart device";
                                                       forces the frame server to drop/re-acquire the device)
   2. Restart the Windows Camera Frame Server service (stack-level reset — the layer the
                                                       confirmed MF_E_REBOOT_REQUIRED state most likely lives in;
                                                       community-reported non-reboot fix)
   3. Full disable→enable cycle via existing pipeline (RecoverCameraHardware(target, true))
   4. CM_Reenumerate_DevNode on the composite parent  ("scan for hardware changes" at the right tree level)
   after each rung: re-locate device by instance ID → health check again
        ↓
Recovered → log, resume
Still broken → log RecoveryExhausted (with the health-check HRESULT — if MF_E_REBOOT_REQUIRED
               persists, the log says so), recommend reboot in the log + UI status, park.
               Never reboot automatically — reboot remains the user's final fallback.
```

### 8.2 Why this ladder (and in this order)

- **Rung 1 (`DICS_PROPCHANGE`) is the only mechanism in the codebase's vocabulary that is genuinely new:** it stops/restarts the driver stack *in place* without touching the enabled/disabled state, it is the documented restart primitive (F3), and it targets device/driver-level wedging (H1) directly while forcing the Frame Server to drop and re-acquire the device (partially targeting H3). It is device-scoped — the least invasive rung.
- **Rung 2 (Frame Server service restart)** targets the *confirmed* failure layer head-on: `MF_E_REBOOT_REQUIRED` is an MF machine-state error, and the Frame Server service is the most probable carrier (H3, §5.4). Restarting a documented Windows service via the SCM is a standard elevated operation. It is placed **above** the device restarts because it is stack-wide rather than device-scoped: it momentarily disrupts *every* camera client on the system (any app streaming any camera loses its session), so it should only run when device-level recovery has already failed. Also honest about its limits: one community report had the service running and still broken — the wedged state may not always live in the service process.
- **Rung 3 (existing cycle)** is proven-safe in this codebase and forces a full device teardown/rebuild — but §3.3/§4 show it is not reliably sufficient alone (it runs at every sign-in without preventing the wedge), hence it is not higher.
- **Rung 4 (parent re-enumeration)** addresses H2 (USB-level re-detection). It is the heaviest device-level rung because re-enumerating the composite parent briefly disturbs the **sibling** interfaces (IR `MI_02`, DFU `MI_04`) — acceptable only near the end of the ladder, only while expected-enabled (session unlocked; Hello face auth does not run mid-session against the RGB camera), with re-verification of both the target and the IR devnode state afterwards.
- **Honest caveat carried through the design:** the error's literal semantics ("We need to reboot the machine") mean **no non-reboot recovery is guaranteed** to clear the state. The ladder is built from the documented mechanisms, ordered least→most invasive, and *instrumented to learn*: the log will record, for the first time in this project, the exact health-check HRESULT before and after every rung — so the first real-world incident turns speculation into evidence. If `MF_E_REBOOT_REQUIRED` persists after all rungs, the failsafe says so and recommends the reboot.
- **Device identity** is preserved through the ladder by re-resolving `CM_Locate_DevNodeW` from the instance ID at every step (F4); the MF health check re-matches the symbolic link so it can never act on a different camera (§6.4).

### 8.3 Failure handling, retry/cooldown policy (no recovery loops)

- **Bounded per incident:** one health-check failure opens an *incident*; at most one pass per rung per incident (≤ 4 recovery actions + ≤ 5 health checks), then `RecoveryExhausted` and park. A new incident may open only after a state change (expected-disabled episode, re-enable event) or a long cooldown (e.g. ≥ 30 min) — never a tight loop.
- **Cooldowns:** ≥ 60 s between health checks triggered by different sources; recovery actions separated by settle sleeps consistent with the existing pipeline style (the codebase already sleeps 350/500/900 ms around toggles for device settle).
- **Guards before every action** (same chain the existing watchdogs use): `isArmed` && monitoring on && `!isSystemEnding` && expected-enabled && not already recovering. While locked/suspended/shutting down (`ExpectedDisabled`), the failsafe never acts — this is load-bearing for Windows Hello and for the shutdown-leave-disabled contract.
- **All results logged** to `diagnostic.log` via the existing `LogFailsafe*` wrappers with the standard format (`Op=` correlation, `HRESULT=`/`CR=`/`Stage=` fields), so a real-world failure finally produces evidence (closing F5).

## 9. Diagnostics to add with the mechanism (evidence-gathering is a first-class goal)

Since the failure cannot be reproduced on demand, the implementation must log enough to *characterize* it the first time it happens in the wild:

1. Health-check result lines: classification, MF HRESULTs at each step (activation, media-type, ReadSample), `DurationMs`, device instance ID + symbolic link.
2. PnP context at detection time: `CM_Get_DevNode_Status` full `status`/`problem` words (not just the disabled check) — catches transient codes 10/14/43/54/55.
3. Each ladder rung: API result (`CONFIGRET`, Win32 error, `DI_NEEDRESTART` flag), pre/post devnode status, pre/post health-check verdict.
4. Explicit `PrivacyDenied`/`Busy` classifications so support can distinguish them from wedges in user-submitted logs.

## 10. Interaction with existing watchdogs and architecture policy

- `CameraFailsafe` and `RecoveryLoopFailsafe` remain **unchanged** and continue to own the "unexpected PnP-disabled" contract. The new mechanism owns a **different sensor** (camera-stack health) and **escalation authority** (restart/re-enumerate), so there is no overlapping authority and no second camera-control implementation.
- The "golden rule" (documented in `docs/watchdog/watchdog.md`: only `src/core/MyForm_Camera.cpp` flips device state) is preserved by adding the two new primitives — devnode restart and parent re-enumeration — **to `src/core`** (new functions alongside `ToggleCameraHardwareCfgMgr`, same `g_last*` attribution pattern), with the watchdog merely *requesting* them through the same accessor pattern it already uses (`RecoverCameraHardware` etc.).
- This requires a minimal, explicitly-justified `src/core` addition (two functions + two forward declarations) — permitted under `AGENTS.md` §2 ("bug fixes may modify existing files in src/core when the affected behavior belongs there") because device-state operations belong in the single camera authority; keeping them in `src/watchdog` would create exactly the second camera authority the rules forbid.
- Naming: **`CameraRecoveryFailsafe`** (files `src/watchdog/CameraRecoveryFailsafe.{h,cpp}`, log prefix `FAILSAFE`/Op prefix `CAMREC`). Evaluated candidates: `HardwareHero` (rejected: inconsistent with the `*Failsafe` convention; the mechanism restarts a Windows devnode, it does not repair "hardware"), `DeviceRecoveryFailsafe`/`HardwareRecoveryFailsafe` (rejected: broader than the camera-scoped reality), `CameraGuardian` (rejected: breaks convention).

## 11. Testing plan (for the implementation phase)

| # | Scenario | Method |
|---|---|---|
| 1 | Normal camera operation, health check passes | Launch daemon, unlock, inspect log: `HealthCheck OK` no churn, LED blip only |
| 2 | Normal lock/unlock | Win+L cycles: existing behavior unchanged; health check only when expected-enabled |
| 3 | Repeated toggling | Automated lock/unlock loop (the Issue #1 accelerant) — verify no new transitions added beyond existing design |
| 4 | Camera PnP-disabled unexpectedly | Device Manager disable → existing watchdogs recover (regression guard: new failsafe must not double-act; cooldown/coalescing) |
| 5 | PnP enabled + camera init failure | Inject by breaking the stack without PnP disable: e.g. stop/break the DMFT chain or simulate with a filter driver / bad registry of the DMFT; or approximate by holding the camera in a failed start state. Verify detection + ladder. (If not injectable: verify detection only, mark recovery unverified.) |
| 6 | Recovery success path | Follows from 5 when the injection is restartable |
| 7 | Recovery failure path | Non-recoverable injection → `RecoveryExhausted`, parked, no loop, log complete |
| 8 | Multi-camera systems | External USB webcam + virtual camera present: health check and all rungs touch only the configured instance ID (verify by log + PnP state of others) |
| 9 | Windows Hello usage | Face sign-in at lock screen during/around health-check windows; verify no interference (expected-disabled guard + post-unlock scheduling) |
| 10 | Reboot comparison | Capture diagnostic.log + PnP status at failure time before rebooting; compare pre/post — produces the real-world evidence §4 lacks |
| 11 | Privacy gate | Disable camera in Settings → Privacy → Camera: verify `PrivacyDenied` classification and **no** recovery attempt |
| 12 | x86/x64 | Both payloads build; WOW64 restriction honored (x64 binary on x64 OS) |

## 12. Open questions / remaining uncertainty

1. **The actual failure state has never been captured live** (PnP status word + health-check HRESULT at failure time). H0 is confirmed as *the reported error*; which component carries the state (Frame Server vs deeper MF state vs device) is not, and the unified mechanism is speculative. The §9 diagnostics are designed to answer this on the first real-world occurrence.
2. **Does any non-reboot recovery actually clear `MF_E_REBOOT_REQUIRED`?** Community evidence says the Frame Server service restart usually does (Likely); the error's literal semantics say only a reboot is guaranteed. The ladder treats this as the empirical question: every rung is followed by a health check and logged with HRESULTs, so the first incident produces a definitive answer for this machine class.
3. Whether Windows Hello's own RGB/IR access routes through the Frame Server on all builds (retired 1703 hardware doc claimed Hello coexists with the frame server; no surviving primary source). The design does not depend on it: the health check runs only when expected-enabled (session unlocked), when Hello face auth is not streaming.
4. Whether `DICS_PROPCHANGE` restart of `MI_00` also resets the shared USB composite device's state (H2) — testable in scenarios 5/6 of the test plan; if insufficient, rungs 3–4 cover it.
5. Symbolic-link ↔ instance-ID matching format should be verified against live values on the first implementation run (§6.3).
6. Whether restarting the Frame Server service is safe while Windows Hello is mid-enrollment or another app is streaming an unrelated camera — the implementation must check for active clients where feasible or accept and log the disruption window (rung 2 is last-resort-adjacent by design).

## 13. Sources

**Microsoft documentation:** CM_Reenumerate_DevNode, CM_Enable/Disable_DevNode, CM_Get_DevNode_Status, CM_Locate_DevNodeW, CM_Get_Device_IDW, CM_Get_Child, CM_Setup_DevNode, CM_Query_And_Remove_SubTreeW, CM_Request_Device_EjectW, SetupDiCallClassInstaller, SP_PROPCHANGE_PARAMS, DIF_PROPERTYCHANGE, SetupDiChangeState, Device Manager error messages, Understanding when Remove IRPs are Issued, pnputil command syntax, DevCon Restart, MFEnumDeviceSources, MFCreateDeviceSource, ActivateObject, IMFMediaSource::Start, MFCreateSampleGrabberSinkActivate, IMFSourceReader::ReadSample, IMFMediaEventGenerator::GetEvent, MFStartup, audio-video capture in MF, Frame Server Custom Media Source, KSCATEGORY_VIDEO_CAMERA / KSCATEGORY_SENSOR_CAMERA, camera privacy setting, Windows Hello face authentication, camera privacy controls, HLK camera tests, security guidelines for disabling system services (FrameServer row), Windows Release Health / Windows 11 24H2 status — all under learn.microsoft.com / support.microsoft.com (principal ones cited inline above).

**SDK headers verified locally (10.0.26100):** `cfg.h`, `cfgmgr32.h`, `setupapi.h`, `RegStr.h`, `Mferror.h` (incl. `MF_E_REBOOT_REQUIRED 0xC00D7167` at line 2668), `mfidl.h/.idl`.

**Error-code research:** hresult.info/FACILITY_MEDIASERVER/0xC00D7167; Josh Poley's Microsoft HRESULT reference (joshpoley.blogspot.com, FACILITY_MEDIASERVER); NAudio `MediaFoundationErrors.cs`; Microsoft Q&A threads 5747541 ("HOW TO FIX ERROR CODE ON MY PC CAMERA" — `0xA00F429F <WindowShowFailed> 0xC00D7167`) and "Cant run my integrated camera" (0xC00D7167, hard-reset advice); support.microsoft.com camera-app error pages (0xA00F4244 / general); r/Surface "Windows Hello just plain doesn't work" (0xA00F4241(0xC00D7167) in Hello context); kapilarya.com 0xA00F429F frame-server-restart fix; r/techsupport frame-server fix thread; borncity.com 24H2 camera-bug coverage (Oct 2024 / Sep 2025); mymce 0xA00F4241 disable-IR/rescan writeup; docam.io frame-server failure-mode guide.

**Prior art:** microsoft/Windows-driver-samples `setup/devcon/cmds.cpp` (devcon restart implementation); wireguard-nt `api/adapter.c` (production DICS_ENABLE/DISABLE pattern); Chromium `video_capture_device_factory_win.cc` (sensor-camera enumeration exclusion); first-party: Shivu516/Windows-Hello-Fix issues #1 and #6.

---

*Companion planning document: `docs/Issue1_Failsafe_Plan.md` (implementation design). This document records investigation only; no code was changed.*
