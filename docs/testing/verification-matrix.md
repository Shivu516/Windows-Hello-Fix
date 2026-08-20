# Verification Matrix

Regression requirements for the v2.1 refactoring. Every row describes a
behavior that must still hold after each migration phase. "Future" rows are
planned capabilities — not required today.

## Build

| # | Test | Expected | Now | Future |
|---|---|---|---|---|
| B1 | Build `Release \| x64` | Clean build, `Windows_Hello_Fix_v2_0.exe` produced | ✔ | — |
| B2 | Build `Debug \| x64` | Clean build | ✔ | — |
| B3 | Build `Release \| Win32` (x86) | Clean build | ✘ (config incomplete) | x86 support |
| B4 | Build `Release \| ARM64` | Clean build | ✘ (C++/CLI blocker) | ARM64 support |
| B5 | Clean checkout → build | Succeeds without local artifacts | ✘ (icon sourced from build output) | after phase 11 |
| B6 | Build produces no new warnings | /W3 clean | ✔ | ✔ |

## Lifecycle

| # | Test | Expected |
|---|---|---|
| L1 | Normal launch | Form shows, dropdown populated, status "Service Stopped" |
| L2 | Launch with `/background` | Form hidden; monitors if config `monitoring=1` |
| L3 | Start monitoring | Status "Service Running", dropdown disabled |
| L4 | Stop monitoring | Camera enabled, status "Service Stopped", `monitoring=0`, dropdown enabled |
| L5 | Close via X | Window hides; process keeps running; notice shown once |
| L6 | Kill process | Nothing left disabled |
| L7 | Normal exit | Camera re-enabled, `monitoring=1` saved |
| L8 | Logoff / shutdown | Camera disabled, `monitoring=1` saved, `isSystemEnding` path taken |

## Session

| # | Test | Expected |
|---|---|---|
| S1 | Lock (Win+L) | `SessionLock_Disable` logged, camera disabled, Verify=PASS |
| S2 | Unlock | `SessionUnlock_Enable` logged, camera enabled, Verify=PASS |
| S3 | Duplicate lock within 1.5 s | `SessionEvent_DedupIgnored`, no second toggle |
| S4 | Session event while monitoring off | `SessionEvent_Ignored_MonitoringOff`, no hardware change |
| S5 | WTS registration retry (early logon) | Registration succeeds eventually (6×500 ms retry) |

## Power

| # | Test | Expected |
|---|---|---|
| P1 | Sleep (suspend) while monitoring | `PowerEvent_Disable`, camera disabled, 500 ms window |
| P2 | Resume | `PowerEvent_Enable`, camera enabled after 1000 ms, lock released |
| P3 | Lid close | Camera disabled (GUID_LIDSWITCH_STATE_CHANGE) |
| P4 | Power button press | Camera disabled (GUID_POWER_BUTTON_TIMESTAMP) |
| P5 | Irrelevant power-setting GUID | `PowerSetting_IrrelevantGuid`, no action |
| P6 | Duplicate power event within 1.5 s | `PowerEvent_DedupIgnored`, no second toggle |

## Camera / Device

| # | Test | Expected |
|---|---|---|
| C1 | Device discovery | `Camera`/`Image` class devices enumerated (present only) |
| C2 | Target identification | Selection → config → `MI_00` → first; exact or case-insensitive match |
| C3 | Disable | Device disabled via SetupAPI (CFGMGR32 fallback if needed) |
| C4 | Enable | Device enabled via SetupAPI (fallback if needed) |
| C5 | Verification | State confirmed up to 3×100 ms after change |
| C6 | Retry | 3 attempts; reinit-on-mismatch when requested |
| C7 | Recovery cycle | RecoverCameraHardware cycle timings (350/900/500 ms) when `cycleDevice` |
| C8 | Already-disabled on disable | `..._AlreadyDisabled`, no churn |
| C9 | Already-enabled on enable | `..._AlreadyEnabled`, no churn |
| C10 | Missing / invalid target | `..._NoTarget` logged; no crash |
| C11 | Check-before-change | No hardware command if state already matches |

## Safety

| # | Test | Expected |
|---|---|---|
| SF1 | Wrong-device protection | Only matching instance ID / `MI_00` target selected |
| SF2 | Second instance | Signals wake event, exits quietly |
| SF3 | Frozen instance + interactive launch | Force-reset: recover camera, `taskkill`, restart |
| SF4 | Not elevated | Logged error; operations fail gracefully, no crash |
| SF5 | Config corruption (`\r\n`) | `TrimTrailingChars` keeps matching working |
| SF6 | Repeated start/stop cycles | No handle leaks, stable behavior |
| SF7 | Clean exit | Mutex, event, power notification handles released |

## Related Documents

- `baseline.md` — the known-good reference definition.
- `regression-checklist.md` — the practical per-phase checklist built from this
  matrix.