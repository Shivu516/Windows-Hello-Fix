# Architecture-to-Code Mapping (Migration Map)

This is the authoritative table that maps every meaningful piece of the current
v2.0 code to its future v2.1 module and migration phase. Migration phase numbers
refer to `../refactoring/migration-phases.md`.

## Source Files

| Current File | Content Summary | Future Module | Notes |
|---|---|---|---|
| `main.cpp` | Managed entry point, arg scan for hidden launch | `src/main.cpp` + `src/application/CommandLine` | Arg scan moves to CommandLine |
| `MyForm.h` | Everything (see breakdown below) | split across all modules | The monolith |
| `ProductionUtilities.h` | Dead scaffolding (`ProductionLogger`, `HardwareOperationQueue`, `SingleInstanceManager`, `ElevationChecker`, `EnhancedSetupAPI`, `ShutdownManager`) | `src/utilities/Logging` (adopt); rest retired | Never included in v2.0 build |
| `resource.h` / `resource1.h` | Resource IDs (inconsistent: `IDI_ICON1` 102 vs 114) | consolidate into one resource header | Fix in migration phase 11 |
| `Windows_Hello_Fix_v2_0.rc` | Orphan resource script (absolute paths) | decide: fold into live `.rc` or delete | Not compiled today |
| `Windows_Hello_Fix_v2_0_resources.rc` | Compiled resource script | keep + fix relative icon path | Icon sourced from build output today |
| `.sln` / `.vcxproj` / `.vcxproj.filters` | Project + build config | kept as primary build system | Add `src/` items during migration |
| `app.manifest` | requireAdministrator manifest | kept; version bumped in Phase 12 | Coordinated rename |

## MyForm.h — Function-Group Level Mapping

| Current Code | Future Module | Migration Phase | Notes |
|---|---|---|---|
| `CameraDeviceInfo` struct | `camera/CameraDevice` | Phase 4 | verbatim move |
| `ScanSystemCameras` | `camera/CameraDevice` | Phase 4 | verbatim move |
| `MI_00` target heuristic (in `TryGetTargetCameraInstanceId`) | `camera/CameraDevice::TargetResolver` | Phase 4 | pure heuristic |
| `ToggleCameraHardware` | `camera/CameraHardware` | Phase 4 | verbatim move |
| `LocateCameraDevInst` | `camera/CameraHardware` | Phase 4 | verbatim move |
| `ToggleCameraHardwareCfgMgr` | `camera/CameraHardware` | Phase 4 | verbatim move |
| `GetCameraHardwareDisabledState` | `camera/CameraHardware` | Phase 4 | verbatim move |
| `VerifyCameraHardwareState` | `camera/CameraHardware` | Phase 4 | verbatim move |
| `SetCameraHardwareStateVerified` | `camera/CameraRecovery` | Phase 4 | verbatim move |
| `RecoverCameraHardware` | `camera/CameraRecovery` | Phase 4 | verbatim move |
| `RestoreAllCameraHardware` | `camera/CameraRecovery` | Phase 4 | verbatim move |
| `TryEnterHardwareToggleCooldown` | `camera/CameraRecovery` (or retire) | Phase 4 | dormant; decide on adoption |
| `RecordHardwareToggleTime` | `camera/CameraRecovery` | Phase 4 | |
| `g_lastHardwareToggleTick` / `g_lastSetupApiError` / `g_lastConfigManagerResult` / `g_lastHardwareToggleStage` | `camera/DeviceError` | Phase 4 | replace globals with result struct |
| `TrimTrailingChars` | `utilities/` (or `config/ConfigStore`) | Phase 2 | verbatim move |
| `IsCurrentProcessElevatedNative` | `system/PrivilegeInfo` | Phase 3 | verbatim move |
| `GetCurrentProcessIntegrityRid` | `system/PrivilegeInfo` | Phase 3 | verbatim move |
| `GetConfigFilePath` | `config/ConfigPaths` | Phase 5 | |
| `GetDiagnosticLogFilePath` | `config/ConfigPaths` | Phase 5 | |
| `WriteDiagnosticLog` | `config/ConfigStore` | Phase 5 | format preserved |
| `WriteDiagnosticLogWithDevice` | `config/ConfigStore` | Phase 5 | |
| `SaveConfigState` | `config/ConfigStore` | Phase 5 | |
| `LoadConfigState` | `config/ConfigStore` | Phase 5 | |
| `EnsureConfigFileExists` | `config/ConfigStore` | Phase 5 | |
| `IsRestoreCameraCommand` | `application/CommandLine` | Phase 6 | |
| `IsDisableCameraCommand` | `application/CommandLine` | Phase 6 | |
| `TryGetTargetCameraInstanceId` | `application/ApplicationController` | Phase 9 | ordering logic |
| `DisableTargetCameraHardware` | `application/ApplicationController` | Phase 9 | policy |
| `EnableTargetCameraHardware` | `application/ApplicationController` | Phase 9 | policy |
| `RestoreConfiguredCameraHardware` | `application/ApplicationController` | Phase 9 | orchestration |
| `InitializeComponent` | `ui/MyForm` | Phase 10 | |
| `MyForm_Load` startup sequence | `application/ApplicationController` + `ui/MyForm` | Phase 9–10 | |
| Single-instance mutex/event block in `MyForm_Load` | `system/SingleInstance` | Phase 8/9 | |
| Power/WTS registration in `MyForm_Load` | `events/NotificationRegistrar` | Phase 8 | |
| `WndProc` raw message decode + dedup | `events/WinEventDecoder` + `events/EventCooldown` | Phase 7 | |
| `WndProc` policy (isMonitoring gate, isAlreadyDisabled, op selection) | `application/ApplicationController` | Phase 9 | |
| `WndProc` remaining shell | `ui/MyForm` | Phase 10 | thin dispatcher |
| `MyForm_FormClosing` | `ui/MyForm` | Phase 10 | hide-on-close |
| `btnToggle_Click` | `ui/MyForm` (delegates to controller) | Phase 10 | |
| `ListenForWakeupSignal` | `ui/MyForm` | Phase 9–10 | needs Invoke |
| `BringWindowToFrontDelegate` | `ui/MyForm` | Phase 9–10 | |
| Destructor / Finalizer | `ui/MyForm` delegating to `application/ApplicationController::Shutdown` | Phase 9–10 | collapse duplication |
| Unused members (`cameraStateInitialized`, `restartQueuedByMismatch`, `lastCameraToggleTick`, `lastToggleTime`, `COOLDOWN_MILLISECONDS`) | retire | Phase 11 | never read |
| `taskkill` ghost recovery call | `system/ProcessUtils` | Phase 9 | |
| Icon load (`LoadImage(IDI_ICON1)`) | `ui/MyForm` + resource fix | Phase 11 | ID mismatch (102 vs 114) |

## ProductionUtilities.h — Dead Code Disposition

| Class | Disposition | Destination |
|---|---|---|
| `ProductionLogger` | **Adopt** as the unified logger | `utilities/Logging` |
| `HardwareOperationQueue` | **Retire** (behavior-changing async design) | — (documented in `data-flow.md`/`risk-register.md`) |
| `SingleInstanceManager` | **Retire** (superseded by live `Global\` mutex code) | `system/SingleInstance` reimplemented from live code |
| `ElevationChecker` | **Merge** into live privilege code | `system/PrivilegeInfo` |
| `EnhancedSetupAPI` | **Retire** (the live code already implements the same fallback strategy) | — |
| `ShutdownManager` | **Retire** | — |

## Migration Phase Reference

| Phase | Scope |
|---|---|
| 0 | Baseline verification |
| 1 | Docs + `src/` scaffolding (this phase) |
| 2 | `utilities/` |
| 3 | `system/PrivilegeInfo` |
| 4 | `camera/` core |
| 5 | `config/` |
| 6 | `application/CommandLine` |
| 7 | `events/` decode + cooldown |
| 8 | `events/NotificationRegistrar` + `system/SingleInstance` |
| 9 | `application/ApplicationController` (highest risk) |
| 10 | `ui/MyForm` thin shell |
| 11 | Cleanup: dead code, resources, vcxproj hygiene |
| 12 | v2.1 coordinated rename |

See `../refactoring/migration-phases.md` for full detail.