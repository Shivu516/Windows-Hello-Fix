# Issue #1 Deep Device Recovery — Investigation (why the failsafe ladder can't clear `MF_E_REBOOT_REQUIRED`, and what can)

> **Type:** Investigation & planning (read-only). No source was modified.
> **Date:** 2026-10-04 (evening — live wedge specimen on the machine during the investigation)
> **Builds on:** `docs/Issue1_Camera_Recovery_Research.md` (mechanism research), `docs/CameraRecoveryFailsafe.md` (implemented ladder), `docs/Issue1_Failsafe_Plan.md`
> **Trigger for this investigation:** the implemented 4-rung ladder ran against a real, harness-reproduced wedge and **did not recover the camera** — `CameraRecovery_RecoveryExhausted … MF_E_REBOOT_REQUIRED_persists | Recommendation=RebootWindows`.
> **Evidence labels:** **Confirmed** = Microsoft docs / SDK headers / repository source / live diagnostic evidence. **Likely / Possible / Unknown** as marked.

---

## A. Current failure — what we now know

### A.1 The failure signature, reproduced on demand

The stress harness (`tools/CameraStressRepro.ps1 -Mode MidStreamToggle`) reproduces Issue #1 by disabling the RGB camera **while a live Media Foundation frame-reader session is attached**, then re-enabling across the still-open session. After enough cycles the machine enters the state:

```text
PnP:        USB\VID_04F2&PID_B829&MI_00\…  Status=OK, CM_PROB_NONE, started
Media Foundation: every activation fails 0xC00D7167 (MF_E_REBOOT_REQUIRED)
Camera app: 0xA00F4241 (0xC00D7167)
```

A live specimen existed throughout this investigation (probe at 19:35:50: `PnP=OK/CM_PROB_NONE CameraStack=RebootRequired HRESULT=0xC00D7167`).

### A.2 The failsafe's ladder run (diagnostic.log, 2026-10-04 19:18–19:19) — the decisive evidence

| Rung | Action | Result | Diagnostic evidence |
|---|---|---|---|
| detect | MF health check | `Result=Wedged HRESULT=0xC00D7167` | `CAMREC-000005` |
| 1 | `DICS_PROPCHANGE` restart | API **succeeded** (Stage=36) **but the restart was DEFERRED** | **`PropFlags=0x00000100` = `DI_NEEDREBOOT`** |
| 2 | Frame Server service restart | succeeded (`ServiceState=4` RUNNING) | wedge persisted |
| 3 | full disable→enable cycle | succeeded PnP-level (Stage=14) | wedge persisted |
| 4 | composite-parent re-enumeration | succeeded (Stage=65); siblings healthy | wedge persisted; **sibling status snapshot proves `DN_NEED_RESTART`** |
| — | `RecoveryExhausted` | parked 30 min, recommended reboot | `MF_E_REBOOT_REQUIRED_persists` |

### A.3 The three decisive new facts

1. **`PropFlags=0x00000100` = `DI_NEEDREBOOT`.** The `DICS_PROPCHANGE` device restart — the rung expected to rebuild the driver stack — **was never executed**. Windows deferred it because open handles existed on the device. The driver stack that the wedge lives in was never torn down.
2. **The RGB devnode carries `DN_NEED_RESTART` (status bit 0x100, alias `DN_LIAR` — "System needs to be restarted for this Devnode to work properly", Confirmed from `cfg.h`).** Rung 4's sibling snapshot shows MI_00 = `0x0180610A` vs IR sibling MI_02 = `0x0180600A` — differing by exactly bit 0x100. The device is in a **pending-configuration-restart state**.
3. **The state does not live in the Frame Server service.** Rung 2 restarted the service (RUNNING afterwards) with no effect; later local inspection found the service **stopped** (idle auto-stop) while the wedge persisted. Whatever holds the `0xC00D7167` state is re-derived at activation time from device state, or lives outside the service (research pending on the exact carrier — see §E).

**Working model (final):** two parallel symptoms of the same PnP churn, at two different layers:

1. **PnP layer (Confirmed):** the mid-stream device removal left a device configuration change pending; the devnode is flagged `DN_NEED_RESTART`; Windows deferred the restart (`DI_NEEDREBOOT`) because handles were open. This is why rung 1 was a silent no-op.
2. **Media Foundation / trust layer (the wedge's owner — Likely, with Confirmed raise-mechanism):** `0xC00D7167` is **not** a camera error at all. It is raised by **`mfcore.dll` translating a kernel verdict from `peauth.sys`** — the Protected Environment Authentication driver of the Protected Media Path (mapping Confirmed in Microsoft's own clearkey CDM sample: `PEAUTH_REBOOT_REQUIRED` flag 0x20 → `MF_E_REBOOT_REQUIRED`). The camera stack reaches this machinery because the machine's camera DMFTs are **Windows Hello–signed trusted components** (`SignatureAttributes.WindowsHello` countersignatures on `RsDMFTIR64.dll`/`RsDMFTNB.dll`; the IR path even runs a UMDF `SecureUSBVideo.dll`). The sticky state is most probably the **per-boot PEAuth trust session** ("a trusted component changed while running → refuse activations until the machine reboots") — the only known mechanism that survives FrameServer restart, device disable/enable and re-enumeration, is cleared by reboot, and has a code path in mfcore raising exactly this HRESULT. (Full analysis: §E.)

The failure of rungs 2–4 is then mechanical (see §B): none of them completes the deferred restart, and — more fundamentally — none of them can reset a kernel trust session that is keyed to the boot, not to the devnode.

## B. Why the current failsafe fails

| Rung | Why it cannot clear this wedge |
|---|---|
| 1 — `DICS_PROPCHANGE` | Belongs to the **handle-vetoed** operation class. With open handles (frame-server session / any camera client) Windows defers it (`DI_NEEDREBOOT`) and the driver stack is never rebuilt. It "succeeded" as an API call while doing nothing. |
| 2 — FrameServer restart | The wedge is not (only) frame-server session state — the flag survives a full service restart and even service idle-stop. |
| 3 — disable/enable cycle | Executed at PnP level, but **the log cannot prove the disable actually completed**: `RecoverCameraHardware(cycle)` verifies only the PnP enabled-state and OR-accumulates its steps, so a handle-vetoed disable (query-remove tracks open handles → veto) is invisible in the result while the final enable still "passes" on the already-enabled devnode. The `DN_NEED_RESTART` bit still being set at rung 4 (after rung 3) is consistent with the disable never completing — or with a completed disable/re-enable that simply does not clear a per-boot trust verdict (§E). |
| 4 — parent re-enumeration | `CM_Reenumerate_DevNode` re-enumerates a bus node's **children**: it starts unconfigured children and surprise-removes absent ones — it does **not** restart already-started children (documented). The flagged MI_00 was already started, so nothing happened to it. |

**Conclusion:** every current rung either (a) is handle-vetoed and silently no-ops, or (b) operates at a level that cannot clear the wedge's owner. What is missing is an operation that either **completes the deferred restart** (requires the handle holders to let go, or force) or **destroys and recreates the devnode** (removal + re-enumeration — not vetoed by handles) — and, if the owner is the per-boot PEAuth trust session (§E), *no* device-level operation can clear it, in which case reboot genuinely is the only cure and the failsafe's honest recommendation is the correct terminal behavior.

## C. Device topology (live, Confirmed)

```text
USB\VID_04F2&PID_B829\0001          USB Composite Device [Started]   ← composite parent
 ├─ USB\…&MI_02\…                   Integrated IR Camera  [Started]   ← Windows Hello sensor
 ├─ USB\…&MI_04\…                   Camera DFU Device     [Started]   ← firmware interface
 └─ USB\…&MI_00\…                   Integrated Camera     [Started]   ← RGB target (HelloFix)
                                        status 0x0180610A — includes DN_NEED_RESTART (0x100)
```

- Removing **MI_00 only** recreates just the RGB interface (IR/DFU untouched).
- Removing the **composite parent** and re-enumerating its parent hub recreates all three interfaces in one shot — the closest software equivalent to physically unplugging the whole camera unit.
- Driver: Realtek UVC DMFT (`oem21.inf RS_RGB_DMFT_CAMERA_26080.NT`, `usbvideo` stack) — installed from the **driver store**, which removal does not touch.

## D. Candidate recovery mechanisms (deep tier)

### D.1 Device-instance removal + re-enumeration — **the "software unplug/replug"** ⭐

**Mechanism (Confirmed from Microsoft docs + devcon source):**

```text
SP_REMOVEDEVICE_PARAMS { InstallFunction = DIF_REMOVE, Scope = DI_REMOVEDEVICE_GLOBAL, HwProfile = 0 }
  → SetupDiSetClassInstallParams
  → SetupDiCallClassInstaller(DIF_REMOVE, …)        ← what devcon `remove` / pnputil /remove-device do
  → devnode + hardware/software registry keys deleted; device leaves the tree
  → CM_Reenumerate_DevNode(root/hub parent, CM_REENUMERATE_SYNCHRONOUS)   ← devcon `rescan`
  → bus re-reports the device → PnP "handles the device as a new instance and installs
    the driver package from the driver store" (MS: How devices and driver packages are uninstalled)
  → same device instance ID string, brand-new PDO + driver stack, clean status bits
```

Key properties (researched):

- **Not vetoed by open handles** (Confirmed): removal deletes the devnode/registry keys immediately; holders' I/O fails; the final kernel teardown (`IRP_MN_REMOVE_DEVICE`) is deferred to the last handle close — *deferred, not aborted*. This is the property rung 1 lacked. (OSR practitioner evidence: `DIF_REMOVE` with an open handle succeeds.)
- **Identity is preserved** (Confirmed): device instance IDs are persistent; re-enumeration recreates the *same* instance ID (`USB\…&MI_00\6&321DD860&1&0000`) — `config.txt` targeting keeps working; no re-selection needed.
- **Driver package is kept** (Confirmed): uninstall removes the device↔driver association, not the driver store package; re-enumeration auto-installs the same driver. Never pair with `pnputil /delete-driver`.
- **`DN_NEED_RESTART` cannot survive recreation** (Confirmed by mechanism; Likely as literal claim): the flagged devnode and its registry keys are deleted; the fresh instance starts clean.
- **Scope choice**: remove MI_00 only (RGB) — IR/DFU untouched; or remove the composite parent (`/subtree` equivalent) to recreate all three interfaces. Removal flows **downward only** (removing a devnode removes its children).
- **Prior art / Device-Manager equivalence**: this is exactly Device Manager **"Uninstall device" → "Scan for hardware changes"** — Microsoft's own camera troubleshooting and OEM guidance (e.g., HP for 0xA00F4246-class errors) prescribe precisely this when disable/enable fails. devcon `remove` + `rescan` and `pnputil /remove-device` + `/scan-devices` (Win10 2004+, documented) are the CLI forms.
- **Honest limitation (from §E):** recreation is a *PnP/devnode-level* reset. A USB port cycle would additionally reset the device itself, and the PEAuth trust session sits above both. Whether a fresh devnode clears `0xC00D7167` is the central open question — Likely if the MF state is keyed to the devnode/pending-restart condition, **Unknown if it is the per-boot PEAuth trust session**. The discriminating experiment is cheap (§H) and must be run before this rung is trusted.

**Rejected siblings:** `CM_Query_And_Remove_SubTreeW` and `CM_Request_Device_EjectW` are the *safe-removal* family — **handle-vetoed** (`PNP_VetoOutstandingOpen`), exactly like rung 1; wrong for an internal non-ejectable camera. Do not use.

### D.2 Completing the deferred restart (free the handles, then restart again) — the lightest possible fix

Rung 1 failed only because handles were open *at that moment*. If the holders release (frame-server idle-stops, transient client exits), a **second** `DICS_PROPCHANGE` attempt may complete immediately and clear `DN_NEED_RESTART` — possibly fixing the wedge with the existing code and no new rung. Supporting observation: the failsafe parked 30 minutes; the wedge is still live; a fresh incident (lock/unlock) would re-attempt rung 1 with today's handle landscape. **Unknown** whether completing the restart actually clears `0xC00D7167` (it is the state MF is presumed to be objecting to, per §A.3 — test is cheap and already implementable).

### D.3 Disable-first then enable — the user's proposed sequence

Researched precisely:

- **Disable is a remove path, not a stop** (Confirmed): on Windows 2000+ the PnP manager sends remove IRPs when Device Manager disables a device; a *successful* disable tears down the FDO and filter DOs, and the subsequent enable performs AddDevice + `IRP_MN_START_DEVICE` — a **fuller rebuild than the propchange restart** (which stops/restarts within the existing stack and was the rung that got deferred).
- **But disable can also be vetoed by open handles** (Confirmed semantics): disable proceeds through `IRP_MN_QUERY_REMOVE_DEVICE`; "Windows 2000 and later versions of Windows track open handles and fail the query if there are open handles." The veto vocabulary is documented (`PNP_VetoOutstandingOpen`, `PNP_VetoNonDisableable`…). `CM_DISABLE_ABSOLUTE` ("don't ask the driver", `cfgmgr32.h`) bypasses the *driver* veto but there is **no flag that bypasses the PnP manager's outstanding-open-handle check**. Practical expectation: the Frame Server is a documented query-remove notification recipient and likely releases handles on request (which is why Device Manager can usually disable an in-use camera) — **Likely**, not guaranteed.
- **Critical telemetry gap (Confirmed from our log):** the rung-3 disable/enable "passed" at PnP level, yet `DN_NEED_RESTART` was still set afterward — so either the disable was vetoed and masked by the pipeline's OR-accumulation + PnP-only verification, or a completed disable/enable genuinely does not clear the wedge. Current telemetry cannot distinguish the two.
- **Verdict: Possible.** Cheap to test manually (`pnputil /disable-device "<id>"` prints a clear failure on veto), worth one harness experiment before being added as a rung; strictly weaker than D.1 if the holders refuse.

### D.4 `CM_Setup_DevNode` (`CM_SETUP_DEVNODE_READY` / `CM_SETUP_DEVNODE_RESET`) — rejected with new evidence

- `CM_SETUP_DEVNODE_READY` "restarts a device instance **that is not running** because of a problem with the device configuration", and the doc states explicitly: "**If a device instance does not have a problem and is already started, CM_Setup_DevNode returns without changing the status of the device instance.**" Our RGB devnode is Started with no problem → READY is a **documented no-op** here. (Confirmed)
- `CM_SETUP_DEVNODE_RESET` (0x4) applies exclusively to the "no restart" state created by `CM_Query_And_Remove_SubTree` with `CM_REMOVE_NO_RESTART` — it does not touch `DN_NEED_RESTART`. (Confirmed)
- The doc itself redirects applications to `DIF_PROPERTYCHANGE` — the very call that gets deferred on this machine. There is **no supported user-mode "complete the deferred restart" call**; a reconfiguration only completes when the handle holders release or the devnode is removed/recreated.

### D.5 USB port/device reset from user mode — no documented mechanism (Confirmed verdict)

- `IOCTL_INTERNAL_USB_CYCLE_PORT` ("simulate a plug/unplug on the upstream port — the device is disconnected and reconnected in software; the PnP Manager rebuilds the device node") is a **kernel-mode internal IOCTL**; user-mode `DeviceIoControl` cannot issue it. The documented wrapper (`WdfUsbTargetDeviceCyclePortSynchronously`) is KMDF-only. (Confirmed)
- `WinUSB` deliberately exposes nothing above pipe level (`WinUsb_ResetPipe` = endpoint-halt clear only; no ResetDevice/CyclePort in the current SDK headers). libusb documents the boundary verbatim: cycle-port "is only available in kernel mode"; real resets exist only behind kernel drivers (libusbK/UsbDk). (Confirmed)
- `IOCTL_USB_HUB_CYCLE_PORT` exists in the public header but is undocumented and was removed from Vista onward per libusb — not a supportable mechanism. (Confirmed)
- Composite-device caveat: a port cycle resets the **entire composite device** (RGB + IR + DFU together) — the right granularity, but kernel-territory only. **Verdict: the only in-session approximation of a port cycle available to a user-mode app is the PnP-level remove + re-enumerate of §D.1** — which is likely strictly weaker (the USB device itself is never reset), but is the documented toolset we have.

### D.6 Driver reinstallation via existing package

Not needed if D.1 works (re-enumeration auto-reinstalls from the driver store — same effect without `UpdateDriverForPlugAndPlayDevices` machinery). Kept as a documented last resort before reboot only if D.1 proves insufficient.

## E. Where `MF_E_REBOOT_REQUIRED` lives

Researched via web sources **plus read-only binary-level static analysis of the affected machine** (72 mf*/camera* System32 DLLs, usbvideo.sys, ksthunk.sys, SecureUSBVideo.dll, the Realtek DMFTs, mfpmp.exe, FrameServer.dll byte-scanned for the constant):

### E.1 The raise mechanism (Confirmed)

- `0xC00D7167` sits in mferror.h's **Protected Media Path / PEAuth / GRL block** (neighbors: `MF_E_KERNEL_UNTRUSTED`, `MF_E_PEAUTH_UNTRUSTED`, `MF_E_NON_PE_PROCESS`, `MF_E_PROCESS_RESTART_REQUIRED`…).
- Microsoft's own clearkey CDM sample contains the canonical mapping: `IF_FALSE_GOTO(0 == (PEAUTH_REBOOT_REQUIRED & ulResponseFlags), MF_E_REBOOT_REQUIRED)`. **`PEAUTH_REBOOT_REQUIRED` (flag 0x20) is a verdict returned by `peauth.sys`** — the kernel Protected Environment Authentication driver — during the trust handshake of a protected environment. mfcore merely translates it.
- A byte-scan of the machine found the constant in **exactly two** camera-stack binaries: **`mfcore.dll`** (a genuine raise site near `CPMPHost::CreateSource` — PMP source creation — plus a PMP/GRL error handler) and **`FrameServer.dll`** (which only *consumes* it: `FSToastManager::HandlePost` maps `0xC00D7167` to a "Camera Troubleshooter" toast; never constructs it). No driver, DMFT, ksproxy, mfpmp, or mfplat code contains it — **no in-box component maps PnP pending-restart state (`DN_NEED_RESTART`) to this HRESULT**.

### E.2 Why the camera stack touches the Protected Media Path (Likely)

The camera's device MFTs are **Windows Hello–signed trusted components** (this machine's `oem18.inf`/`oem21.inf` carry `SignatureAttributes.WindowsHello` for `RsDMFTIR64.dll`/`RsDMFTNB.dll`; the IR path runs a UMDF `SecureUSBVideo.dll`). Windows Hello DMFT activation is therefore a **PEAuth-trusted ("protected") activation**, routed through mfcore's PMP logic — which is where the reboot-required verdict is produced. A precise third-party corroboration exists: an IR-camera project documents the black-IR-view "We need to reboot" (0xC00D7167) state occurring **after an IR-camera driver change**, fixed only by restarting Windows.

### E.3 Where the state survives (Likely)

- **Not per-process client state** — all activations are intercepted by the FrameServer service, so every client inherits it.
- **Not FrameServer service state** — the failsafe's rung 2 restarted it (RUNNING confirmed) with no effect, and the service later idle-stopped while the wedge persisted.
- **Not any registry flag we can find** — `HKLM\SOFTWARE\Microsoft\Windows Media Foundation\PEAuth` (referenced by mfcore) is absent on the healthy machine; no documented "PMP reboot required" registry flag exists.
- **Most probable owner: the per-boot PEAuth trust session in `peauth.sys`** — kernel trust state established at boot ("trusted component set validated at boot; components changing while running → refuse until reboot"). This is the only candidate consistent with every measurement: survives service restart, device toggles, and re-enumeration; cleared by reboot; produced through mfcore.
- **`DN_NEED_RESTART` and `0xC00D7167` are probably parallel symptoms**, not cause and effect: the devnode-level "restart pending" flag and the trust-layer "reboot required" verdict both stem from the same PnP churn, but no code path maps one to the other. Untested — the discriminating experiment below settles it.

### E.4 Reported non-reboot fixes, catalogued

| Fix | Confidence for 0xC00D7167 |
|---|---|
| Restart FrameServer service | **Disconfirmed by our own controlled experiment** (rung 2) |
| `EnableFrameServerMode=0` registry bypass (Rafael Rivera's classic workaround) | **Plausible** — bypasses the frame-server activation path where the error is produced; unverified for the sticky state; needs app/service restart; breaks sharing (and possibly Hello) — diagnostic value exceeds product value |
| Device-Manager uninstall → scan | Unverified for this HRESULT; mechanism of the *parallel symptom* only |
| Cold shutdown (vs warm restart) | Unverified; resets the USB controller — may help USB-level wedges, not PEAuth state |
| Privacy toggles, Camera-app reset, troubleshooters | Unlikely to matter |
| **Reboot** | **Confirmed — the only consistently successful fix anywhere, matching the HRESULT's own message** |

### E.5 The discriminating experiment (recommended, cheap)

1. Enumerate handle holders (`handle.exe -a | findstr VID_04F2`, admin) — expect FrameServer svchost / WUDFHost / stray clients.
2. Close them (or let them release), then complete the deferred `DICS_PROPCHANGE` restart; verify `DN_NEED_RESTART` (0x100) cleared via `DEVPKEY_Device_DevNodeStatus`.
3. Test MF activation **without reboot**. Cleared → the wedge was re-derived from PnP pending-restart state (Possible branch). Persisted → the owner is the per-boot PEAuth trust session (Likely branch), and **no user-mode recovery exists** — the failsafe's honest reboot recommendation is the correct terminal behavior.
4. Optional discriminator: `EnableFrameServerMode=0` + exclusive-mode activation — distinguishes frame-server-activation-bound from mfcore/PMP-bound.

## F. Recommended recovery ladder (proposed v2)

```text
Level 0   health check (existing)
Level 1   DICS_PROPCHANGE restart            (existing rung 1 — completes only when handles are free)
Level 2   Frame Server service restart       (existing rung 2 — proven insufficient for this wedge alone)
Level 3   existing full cycle                (existing rung 3 — disable may itself be vetoed; telemetry gap noted in §B)
Level 4   composite-parent re-enumeration    (existing rung 4 — no-op for started children)
Level 5   NEW: DIF_REMOVE device-instance removal (MI_00 scope first) + wait-for-gone
          + root/hub re-enumeration → fresh devnode, same instance ID
          (escalate scope: composite-parent removal if MI_00-only is insufficient)
Level 6   reboot (user action; logged recommendation only — never automatic)
```

Two framing corrections from this investigation:

1. **Level 5 is the deepest *PnP-level* tool, but it is a discriminator, not a guaranteed cure.** If the wedge's owner is the per-boot PEAuth trust session (§E, Likely), no PnP-level operation — including Level 5 — clears it, and Level 6 is genuinely required. The Level-5 experiment is still worth implementing because it is the only remaining in-session mechanism, and its outcome (logged with the fresh devnode's status) permanently answers the §E open question for this machine class.
2. **A cheap pre-check improves Level 1:** before declaring the ladder exhausted, the failsafe could re-attempt the `DICS_PROPCHANGE` restart once more after the FrameServer service has been restarted and allowed to go idle (the original rung-1 blocker may have been the transient frame-server session handles). This costs nothing and may complete the deferred restart in favorable conditions.

Additional diagnostic value worth adding alongside Level 5: attempt `CM_Query_And_Remove_SubTree` (vetoable) first purely to **capture the veto type** (`PNP_VetoOutstandingOpen` vs `PNP_VetoWindowsService` etc.) and log it — it identifies *who* is holding the device, which is otherwise invisible to our telemetry. Escalation policy stays bounded (one Level-5 attempt per incident, parking, no loops); Level 5 inherits strict guards (expected-enabled only, IR-sibling re-verification after parent-scope use, all stages/HRESULTs logged).

## G. Rejected approaches

| Approach | Why rejected |
|---|---|
| `CM_Query_And_Remove_SubTreeW` / `CM_Request_Device_EjectW` | Handle-vetoed (`PNP_VetoOutstandingOpen`) — same failure class as rung 1; eject meaningless for internal camera |
| `pnputil /delete-driver` (or any driver-package deletion) | Destroys the driver store package; unnecessary and dangerous — re-enumeration reinstalls from the store |
| Registry spoofing (`CONFIGFLAG_*`, fake device state) | Hides the failure instead of fixing it; creates inconsistent states; no legitimate reinitialization path triggered |
| `CM_REMOVE_NO_RESTART` flows | Deliberately blocks restart until manually reset — strictly worse for recovery; (if ever encountered: clear with `CM_Setup_DevNode(dn, CM_SETUP_DEVNODE_RESET)`) |
| Killing arbitrary camera client processes | Only justified if a specific, identified holder is proven to block completion of the deferred restart; indiscriminate process kills are unsafe |
| Kernel-mode USB cycle-port IOCTLs | Not exposed to user mode; requires a kernel driver — out of scope by project rules |

## H. Implementation proposal (conditional on confirming §D.1 clears the wedge)

> **Status update 2026-10-04 (late):** Level 5 has been **implemented** as rungs 5A/5B of `CameraRecoveryFailsafe` (core primitive `RemoveAndReenumerateCameraHardware(targetId, deepScope, &report)`, stage codes 70–80; watchdog escalation extended 1→2→3→4→5A→5B→exhaust, bounded once per incident, same guards/parking; builds verified `Release|x64`/`Win32`, 0 errors, no new warnings; audit: no device operations in the watchdog, `src/core` diff insertions-only). **Efficacy is unverified** — the wedge captured at 19:18 predates this build. Re-run `tools\CameraStressRepro.ps1 -Mode MidStreamToggle -UntilFailure` with the new build installed to run the discriminating experiment; the `Rung=5A/5B` result lines (`Scope/Removed/Ancestor/GoneMs/ReturnMs/Before/After`) decide between the devnode-keyed branch (recovered → done) and the per-boot PEAuth branch (`MF_E_REBOOT_REQUIRED_persists` → reboot is the only cure).

- **Watchdog side** (`src/watchdog/CameraRecoveryFailsafe`): new rung 5 between the existing rung 4 and exhaustion; same guards, single bounded attempt per incident, longer settle before the post-rung health check (device re-install takes seconds), IR-sibling status re-verification when the parent scope is used, park on failure.
- **Core side** (`src/core/MyForm_Camera.cpp`, additive, single-authority rule preserved): one new primitive `RemoveAndReenumerateCameraHardware(targetId, scope)` implementing the §D.1 sequence (locate → `DIF_REMOVE`/`DI_REMOVEDEVICE_GLOBAL` → poll `CR_NO_SUCH_DEVNODE` (bounded) → `CM_Reenumerate_DevNode` on root/hub parent → re-locate by instance ID), with `g_last*` stage attribution (stages 70–79).
- **Verification:** post-rung health check (the existing MF probe) + PnP status check that `DN_NEED_RESTART` (0x100) is gone.
- **Manual pre-validation available today** (no code): `pnputil /remove-device "USB\VID_04F2&PID_B829\0001" /subtree` then `pnputil /scan-devices` from an elevated prompt on the currently-wedged machine reproduces the exact mechanism.

## I. Confidence summary

| Conclusion | Confidence |
|---|---|
| Rung 1 was deferred (`DI_NEEDREBOOT`), driver stack never rebuilt | **Confirmed** (diagnostic.log + `setupapi.h` flag values) |
| RGB devnode carries `DN_NEED_RESTART`; IR sibling does not; still set after all rungs | **Confirmed** (rung-4 sibling snapshot + live re-check) |
| Frame Server service restart does not clear the wedge; state not resident in the service | **Confirmed** (log + live service state) |
| `CM_Reenumerate_DevNode` on the parent cannot restart started children | **Confirmed** (docs) |
| `0xC00D7167` is raised by mfcore.dll translating a kernel `peauth.sys` verdict (`PEAUTH_REBOOT_REQUIRED` flag 0x20) | **Confirmed** (MS clearkey CDM sample mapping + local binary scan: mfcore is the only in-box producer; FrameServer.dll only consumes it for a troubleshooter toast) |
| The camera stack reaches PEAuth because its DMFTs are Windows Hello–signed trusted components | **Likely** (`SignatureAttributes.WindowsHello` on this machine's DMFTs + third-party IR-camera documentation) |
| The sticky state is the per-boot PEAuth trust session (cleared only by reboot) | **Likely** — the only candidate consistent with all measurements; final proof = §E.5 experiment |
| `DN_NEED_RESTART` and `0xC00D7167` are parallel symptoms, not cause→effect | **Possible** (no code path maps one to the other; untested) |
| `DIF_REMOVE` removal is not handle-vetoed; remove→re-enumerate recreates the instance (same ID, fresh stack, driver store install) | **Confirmed** (docs + devcon source + OSR) |
| Recreation clears `DN_NEED_RESTART` | **Confirmed by mechanism** (devnode destroyed; registry keys deleted) |
| Recreation clears `MF_E_REBOOT_REQUIRED` | **Unknown** — Likely if the MF state is devnode-keyed; *no* if it is the PEAuth per-boot state. This is the single most important open question, answerable with the §H manual experiment |
| Disable-with-open-handles is vetoable; a completed disable→enable is a deeper rebuild than propchange | **Confirmed** (semantics) / outcome on this machine **Unknown** (rung-3 telemetry gap) |
| `CM_Setup_DevNode` READY is a no-op for started devnodes; RESET only addresses `CM_REMOVE_NO_RESTART` | **Confirmed** (docs) |
| No documented user-mode USB port/device reset exists (kernel-only cycle-port; WinUSB pipe-level only) | **Confirmed** (usbioctl.h/winusb.h + libusb documentation) |

---

## Final answer

> **Can we make Windows genuinely forget/release the current camera device instance and then rediscover/reinitialize it in the same session, in a way that is materially closer to physically unplugging and reconnecting the camera?**

**Partially yes — the mechanism exists and is documented, but it may not reach the wedge's owner.**

- **Yes (Confirmed):** `DIF_REMOVE` device-instance removal (§D.1) is not vetoed by the open handles that defeated the propchange restart; it destroys the flagged devnode and its registry keys entirely; a following `CM_Reenumerate_DevNode` recreates the same-named device instance with a fresh driver stack installed from the driver store. This is the documented software equivalent of Uninstall device → Scan for hardware changes, and it is materially deeper than anything in the current ladder. A USB port cycle would be deeper still, but that capability is **kernel-mode only** — no documented user-mode mechanism exists (§D.5).
- **The open question (Unknown, experimentally answerable):** whether a fresh devnode clears `0xC00D7167`. This investigation found the HRESULT is **mfcore translating a kernel `peauth.sys` trust verdict** (`PEAUTH_REBOOT_REQUIRED`), reached because the camera's Windows Hello–signed DMFTs are Protected-Media-Path-trusted components. If the sticky state is the per-boot PEAuth trust session (Likely), then **no PnP-level operation — removal included — clears it, and reboot genuinely is the only cure**; if the state is instead re-derived from the devnode's pending-restart condition (Possible), removal/recreation or a completed deferred restart fixes it in-session.
- **What state remains uncleared if removal is insufficient:** the kernel PEAuth trust session (per-boot), which nothing in user mode resets — the deepest available in-session mechanism is exactly Level 5, and the failsafe's current terminal behavior (honest `RecoveryExhausted … Recommendation=RebootWindows`) is then provably correct, not a cop-out.
- **Next steps, cheapest first (no code needed for steps 1–2):**
  1. On the currently-wedged machine: `pnputil /remove-device "USB\VID_04F2&PID_B829\0001" /subtree` then `pnputil /scan-devices` (elevated) — wait ~30 s, then `tools\CameraStressRepro.ps1 -Mode ProbeOnly`. `Healthy` → devnode-keyed state, implement rung 5. Still `RebootRequired` → PEAuth state confirmed, keep the honest reboot recommendation.
  2. Optional discriminator: enumerate handle holders (`handle.exe -a | findstr VID_04F2`), release them, complete the deferred restart, re-test — separates "re-derived from PnP state" from "PEAuth state".
  3. If step 1 succeeds, implement rung 5 per §H (`RemoveAndReenumerateCameraHardware` in `src/core`, additive; watchdog orchestrates; bounded once per incident).
