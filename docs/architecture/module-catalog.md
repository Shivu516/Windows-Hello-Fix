# Module Catalog (v2.1)

Per-module specification for the proposed `src/` architecture, derived from the
function-level classification in `code-classification.md`. Each entry covers
purpose, responsibilities, current implementation location, future files,
public interface, private implementation, dependencies, consumers, safety
implications, and extraction difficulty.

Seven modules + a future `src/main.cpp`. This count (7) was chosen because the
v2.0 code naturally clusters into exactly these responsibility groups:
orchestration, camera hardware, storage, message decoding, OS primitives,
presentation, and dependency-free helpers. The alternative — adding more
top-level folders (e.g. a separate `safety/`, a separate `timing/`) — was
rejected because no standalone code exists for those concerns: every safety
mechanism is embedded in a domain function (see "Safety implications" below),
and timing is two functions plus constants.

---

## 1. `utilities/` — dependency-free helpers *(partially extracted)*

- **Purpose:** generic, reusable, subsystem-independent helpers.
- **Responsibilities:** unified structured logging; string sanitization;
  timing constants and tick helpers.
- **Current implementation location:**
  - `ProductionLogger` (dead code in `ProductionUtilities.h:25–83`) → adopted
    already into `src/utilities/Logging.h/.cpp` (Phase 2, done).
  - `TrimTrailingChars` (was `MyForm.h:226–233`) → already moved to
    `src/utilities/StringHelpers.h` (Phase 2, done).
  - Timing constants (1500/100/250/500/1000/350/900 ms) and
    `RecordHardwareToggleTime`/`TryEnterHardwareToggleCooldown` semantics
    (`MyForm.h:479–502`) → not yet extracted.
- **Future files:** `Logging.h/.cpp` ✔ (exists), `StringHelpers.h` ✔ (exists),
  `Timing.h` (constants + `GetTickCount64` helper; extracted with `camera/`).
- **Public interface (intended):**
  - `Logging`: `ProductionLogger` — `LogHardwareOperation(...)`,
    `LogError(context, message)`, `LogInfo(context, message)` (already live).
  - `StringHelpers`: `TrimTrailingChars(const std::wstring&)` (already live).
  - `Timing`: `k*` constants; `GetTickCount64()` wrapper if a native helper is
    ever needed outside `camera/`.
- **Private implementation:** timestamp formatting stays in `Logging.cpp`.
- **Dependencies:** none.
- **Consumers:** every other module.
- **Safety implications:** `TrimTrailingChars` is SF-18 (config corruption
  protection); timing constants are the behavioral contract.
- **Extraction difficulty:** Low. Remaining work is only the timing constants
  (trivial).

---

## 2. `system/` — OS primitives

- **Purpose:** Windows API operations that are not device-specific.
- **Responsibilities:** elevation/integrity checks; single-instance mutex and
  wake event; process utilities (`taskkill` ghost recovery).
- **Current implementation location:**
  - `IsCurrentProcessElevatedNative` (`MyForm.h:227–239`).
  - `GetCurrentProcessIntegrityRid` (`MyForm.h:241–269`).
  - Mutex + wake-event block (`MyForm_Load:984–1053`).
  - `system("taskkill ...")` (`MyForm_Load:1044`).
  - Dead `ElevationChecker`/`SingleInstanceManager` in `ProductionUtilities.h`
    are merged conceptually (live semantics win).
- **Future files:** `PrivilegeInfo.h/.cpp`, `SingleInstance.h/.cpp`,
  `ProcessUtils.h/.cpp`.
- **Public interface (intended):**
  - `PrivilegeInfo`: `IsElevated()`, `GetIntegrityRid()`.
  - `SingleInstance`: acquire/release mutex + wake event (RAII); ownership of
    `Global\WindowsHelloFix_AppMutex` and
    `Global\WindowsHelloFix_WakeupEvent`.
  - `ProcessUtils`: `KillProcessByName(name)` wrapping `taskkill /F /IM ... /T`.
- **Private implementation:** token handling, handle creation, `system()`
  invocation.
- **Dependencies:** `utilities/` (logging).
- **Consumers:** `application/` (startup, shutdown, ghost recovery),
  `ui/` (wake listener uses the event handle).
- **Safety implications:** single-instance protection (SF-14), wake signaling
  (SF-15), ghost-reset process kill (SF-16 — partially in `application/`).
- **Extraction difficulty:** Low–Medium. Handle ownership must move to RAII
  (risk 6 in the risk register).

---

## 3. `events/` — message decoding, dedup, notification registration

- **Purpose:** turn raw Win32 messages into semantic events; manage
  notification registration.
- **Responsibilities:** decode `WM_POWERBROADCAST`/`WM_WTSSESSION_CHANGE`/
  end-session messages into `SystemEvent` values; GUID filter for
  `PBT_POWERSETTINGCHANGE`; 1500 ms dedup; power + WTS registration with the
  6×500 ms retry.
- **Current implementation location:**
  - Decode/dedup portions of `WndProc` (`MyForm.h:1246–1349`).
  - Registration block (`MyForm_Load:1060–1066`, `1148–1168`).
- **Future files:** `SystemEvent.h` (enum), `WinEventDecoder.h/.cpp`,
  `EventCooldown.h/.cpp`, `NotificationRegistrar.h/.cpp`.
- **Public interface (intended):**
  - `WinEventDecoder::Decode(UINT msg, WPARAM, LPARAM) → SystemEvent` with
    members `None, SystemEnding, QueryEnd, Suspend, Resume, PowerSettingLid,
    PowerSettingButton, PowerSettingOther, SessionLock, SessionUnlock`.
  - `EventCooldown::ShouldSuppress(code, now)` / `Record(code, now)`.
  - `NotificationRegistrar`: register/unregister given an `HWND`.
- **Private implementation:** raw message constants, `POWERBROADCAST_SETTING`
  parsing, `IsEqualGUID` filtering, retry loop.
- **Dependencies:** `utilities/` (logging).
- **Consumers:** `application/` (policy via decoded events),
  `ui/` (WndProc decode-only).
- **Safety implications:** dedup (SF-10), lid/button GUID discrimination
  (SF-11/12 rely on correct decode), registration lifetime (risk 6).
- **Extraction difficulty:** Medium — event misclassification is subtle
  (risk 3); keep the policy mapping table from `data-flow.md` open.

---

## 4. `camera/` — device discovery + hardware control

- **Purpose:** everything about camera/device hardware, in one coherent
  subsystem.
- **Responsibilities:**
  - *Discovery:* enumerate present `Camera`/`Image` class devices.
  - *Identification:* instance-ID matching + `MI_00` target heuristic.
  - *State operations:* enable/disable via SetupAPI with CFGMGR32 fallback.
  - *Verification:* confirm requested state (3 × 100 ms).
  - *Recovery:* retry/self-healing, reinit-on-mismatch, power-cycle recovery.
- **Current implementation location:** `MyForm.h:208–224` (types + decls),
  `271–573` (implementations): `CameraDeviceInfo`, `ScanSystemCameras`,
  `ToggleCameraHardware`, `LocateCameraDevInst`, `ToggleCameraHardwareCfgMgr`,
  `GetCameraHardwareDisabledState`, `VerifyCameraHardwareState`,
  `TryEnterHardwareToggleCooldown` (dormant), `RecordHardwareToggleTime`,
  `SetCameraHardwareStateVerified`, `RecoverCameraHardware`,
  `RestoreAllCameraHardware`; plus `g_last*` globals
  (`MyForm.h:36–39`) which become `DeviceError`.
- **Future files:** `CameraDevice.h/.cpp` (struct + scan + heuristic),
  `CameraHardware.h/.cpp` (toggle/locate/state/verify),
  `CameraRecovery.h/.cpp` (verified transitions, recovery cycle,
  restore-all, cooldown helpers), `DeviceError.h` (result struct).
- **Public interface (intended):**
  - `CameraDevice`: `CameraDeviceInfo`, `ScanSystemCameras()`,
    `PickInfraredConflict(...)` (the `MI_00` heuristic).
  - `CameraHardware`: `ToggleCameraHardware`, `LocateCameraDevInst`,
    `ToggleCameraHardwareCfgMgr`, `GetCameraHardwareDisabledState`,
    `VerifyCameraHardwareState`.
  - `CameraRecovery`: `SetCameraHardwareStateVerified`,
    `RecoverCameraHardware`, `RestoreAllCameraHardware`.
  - `DeviceError`: `{ setupErr, configRet, stage }` returned by the toggle
    functions (replaces the `g_last*` globals).
- **Private implementation:** `HDEVINFO`/`SP_DEVINFO_DATA`/`DEVINST` handling —
  created and destroyed inside each call (Rule 9), never leaked above the
  module.
- **Dependencies:** `utilities/` (logging, timing constants), optionally
  `system/PrivilegeInfo` for elevation-aware diagnostics; libs `setupapi`,
  `cfgmgr32`, `devguid` (via `#pragma comment(lib)` in the owning module).
- **Consumers:** `application/` (policy execution), `ui/` (scan only, for the
  dropdown).
- **Safety implications:** the majority of the safety inventory lives here:
  wrong-device protection (SF-1, SF-2), `MI_00` heuristic (SF-3), check-before-
  change (SF-5), verification (SF-6), retry (SF-7), reinit (SF-8), recovery
  timings (SF-13), target-empty guards (SF-20). Extraction must be verbatim.
- **Extraction difficulty:** Medium (raw PnP), but the bodies are
  self-contained native code — safe to move early (risk 2, 4).

---

## 5. `config/` — storage

- **Purpose:** `%APPDATA%\Windows Hello Fix\config.txt` and `diagnostic.log` —
  paths, format, read/write.
- **Responsibilities:** config path resolution; diagnostic log append
  (thread-safe); `monitoring=`/`device=` save/load; default-file creation;
  `\r\n` sanitization via `TrimTrailingChars`.
- **Current implementation location:** `MyForm.h:682–785`:
  `GetConfigFilePath`, `GetDiagnosticLogFilePath`, `WriteDiagnosticLog`,
  `WriteDiagnosticLogWithDevice`, `SaveConfigState`, `LoadConfigState`,
  `EnsureConfigFileExists`; `diagnosticLogSync` member.
- **Future files:** `ConfigPaths.h/.cpp`, `ConfigStore.h/.cpp`.
- **Public interface (intended):**
  - `ConfigPaths`: `GetConfigFilePath()`, `GetDiagnosticLogFilePath()`.
  - `ConfigStore`: `WriteDiagnosticLog(...)`, `WriteDiagnosticLogWithDevice(...)`,
    `SaveConfigState(monitoring, device)`, `LoadConfigState(...)`,
    `EnsureConfigFileExists(device)`.
- **Private implementation:** `StreamReader`/`StreamWriter` (managed — keep for
  exact encoding parity first), internal lock.
- **Dependencies:** `utilities/` (`TrimTrailingChars`, logging format policy is
  its own).
- **Consumers:** `application/` (policy reads/writes), `ui/` (indirectly
  through `application/` — currently direct, to be fixed).
- **Safety implications:** config corruption protection (SF-18); shutdown
  state persistence (SF-17 depends on `SaveConfigState`).
- **Extraction difficulty:** Medium — the native port is a separate isolated
  phase with an encoding checkpoint (risk 15).

---

## 6. `application/` — orchestration

- **Purpose:** application lifecycle and the monitoring state machine.
- **Responsibilities:** event→operation policy (`isMonitoring` gate,
  `isAlreadyDisabled` lock, op selection); target resolution order;
  startup/shutdown orchestration; single-instance flow and ghost recovery;
  command-line parsing (single source of truth).
- **Current implementation location:**
  - `DisableTargetCameraHardware`/`EnableTargetCameraHardware`
    (`MyForm.h:107–177`).
  - `TryGetTargetCameraInstanceId` (`MyForm.h:787–823`).
  - `IsRestoreCameraCommand`/`IsDisableCameraCommand` (`MyForm.h:827–849`).
  - `RestoreConfiguredCameraHardware` (`MyForm.h:851–870`).
  - Startup sequence (`MyForm_Load:935–1169` — orchestration portions).
  - Shutdown policy (`~MyForm:600–648`, `!MyForm:650–680` policy portions).
  - WndProc policy blocks (`MyForm.h:1255–1346`).
  - Arg parsing in `main.cpp:16–25` (with the form's parser).
- **Future files:** `CommandLine.h/.cpp`, `ApplicationController.h/.cpp`
  (with a narrow `IUiSink` callback interface for UI updates).
- **Public interface (intended):**
  - `CommandLine`: `ParseArgs(...) → { background, restoreCamera,
    disableCamera }`.
  - `ApplicationController`: `Start(...)`, `Stop()`, `HandleEvent(SystemEvent)`,
    `HandleSystemEnd()`, `Shutdown(...)`.
- **Private implementation:** the state machine, target resolution order,
  ghost-recovery orchestration, `taskkill` invocation via `system/ProcessUtils`.
- **Dependencies:** all modules — `camera/`, `config/`, `system/`, `events/`,
  `utilities/`.
- **Consumers:** `ui/` (the form delegates to it), `main.cpp`.
- **Safety implications:** `isAlreadyDisabled` lock (SF-9), 500/1000 ms
  windows (SF-11/12), selection-order contract (SF-4), ghost recovery (SF-16),
  shutdown dual-path (SF-17), monitoring gate.
- **Extraction difficulty:** **High** — startup ordering, ghost recovery,
  destructor semantics, and event-policy mapping are intertwined
  (risk 4, 10, 11, 13). Migrate last, after modules 1–5.

---

## 7. `ui/` — presentation

- **Purpose:** the WinForms form, its controls, and a WndProc that only
  decodes messages and forwards them.
- **Responsibilities:** `InitializeComponent`; control event handlers; form
  hide-on-close behavior; bring-to-front; wake-listener thread + `Invoke`;
  dropdown population (via `camera/` scan); presentation-only state
  (`isBackgroundMode`).
- **Current implementation location:**
  - `MyForm::MyForm` (`MyForm.h:579–597`).
  - `InitializeComponent` (`MyForm.h:872–933`).
  - `MyForm_FormClosing` (`MyForm.h:1195–1212`).
  - `btnToggle_Click` (`MyForm.h:1214–1244` — policy/config parts move out).
  - `ListenForWakeupSignal` / `BringWindowToFrontDelegate`
    (`MyForm.h:1171–1193`).
  - WndProc shell (`MyForm.h:1246–1349` — decode/policy parts move out).
  - Dropdown population (`MyForm_Load:1068–1111`).
- **Future files:** `MyForm.h/.cpp`, `UiConstants.h` (strings, layout numbers).
- **Public interface (intended):** the managed `MyForm` class; consumes
  `ApplicationController` and reports UI events through it.
- **Private implementation:** control wiring, icon load, `Invoke` marshaling.
- **Dependencies:** `application/` (policy), `camera/` (scan only),
  `events/` (decode only). **Never the reverse.**
- **Safety implications:** user-facing notices only (hide-on-close message,
  force-reset prompt). No safety mechanism lives here.
- **Extraction difficulty:** Medium — the final phase where the 1,354-line
  header disappears (risk 7: UI/business coupling must not return).

---

## 8. `src/main.cpp` (future)

- **Purpose:** process bootstrap.
- **Responsibilities:** create the form, run the message loop; hidden-window
  setup for command/background launches.
- **Current implementation location:** `main.cpp` (all 37 lines).
- **Future files:** `src/main.cpp`.
- **Public interface:** none (entry point).
- **Dependencies:** `ui/`, `application/CommandLine`.
- **Extraction difficulty:** Low — move after `CommandLine` exists.

---

## Safety as a First-Class Concern (why there is no `safety/` folder)

Every safety mechanism in the application (SF-1…SF-20 in
`code-classification.md`) is **implemented inside a domain function** — there is
no standalone safety code. Creating a `safety/` module would therefore be an
empty directory or would require *moving* safety logic away from the hardware
state it protects (e.g. separating verification from the toggle that it
verifies — a behavior hazard).

Instead, safety is treated as a **cross-cutting contract**:

- `camera/CameraRecovery` owns the *hardware-state* protections
  (verification, retry, check-before-change, recovery timings).
- `events/EventCooldown` owns *event* protections (dedup).
- `application/ApplicationController` owns *policy* protections
  (state lock, event windows, shutdown semantics, ghost recovery).
- `system/SingleInstance` owns *process* protections (single instance).
- `utilities/` owns *data-integrity* helpers (`TrimTrailingChars`).

The migration phases must preserve each mechanism verbatim in place; the
`code-classification.md` §5 table is the checklist proving none was lost.

## Module Count Rationale

Seven modules, each with a single clear sentence:

1. `utilities` — helpers that belong to nobody.
2. `system` — OS calls that belong to no subsystem.
3. `events` — everything about *receiving* Windows events.
4. `camera` — everything about *acting on* camera hardware.
5. `config` — everything about *remembering* state.
6. `application` — everything about *deciding* what to do.
7. `ui` — everything about *showing* it.

This matches the existing documented architecture
(`target-architecture.md`) and stays within the 5–10 module guideline. Finer
splitting (per-function folders, separate verification/recovery modules,
separate `safety/` and `timing/`) was explicitly rejected as
over-engineering: the code does not justify it.

## Related Documents

- `code-classification.md` — the function-level table this catalog summarizes.
- `source-tree.md` — the concrete file tree.
- `myform-decomposition.md` — how the monolith is dismantled into these
  modules.
- `dependency-map.md` — allowed dependency edges.
- `../refactoring/modular-extraction-order.md` — the safe extraction sequence.