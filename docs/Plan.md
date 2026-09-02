# Windows Hello Fix v2.1 — Plan: Updater System

> **Status: DRAFT — Planning Complete 2026-09-01 — Implementation Pending Review**
> **Branch: `failsafe-implementation` (on top of v2.1 1bfe79a)**
> **Plan date: 2026-09-01**
> **Previous plan preserved: Appendix A — RecoveryLoopFailsafe (IMPLEMENTED 2026-08-31)**
> **Core policy: `src/core/` ZERO changes — `src/watchdog/` ZERO changes — TARGETED FOR UPDATER**

---

## 0. Repository State (recorded 2026-09-01)

```
Branch:          failsafe-implementation
Commit:          1bfe79a980419ca607204e091df8e815f7528558 (Bump product version to 2.1)
Working tree:    clean
Remote:          https://github.com/Shivu516/Windows-Hello-Fix
Tags:            v1.0.0, v2.0.0
Executable:      Windows_Hello_Fix_v2_0.exe (Stable name, gitignored)
Installer:       x64/Release/Windows_Hello_Fix_Setup.exe (NSIS, gitignored binary, 681623 bytes for v2.0.0)
Protected:       src/core/* (6 files) + src/watchdog/* (4 files) — verified unchanged
Previous plan:   69551 bytes — RecoveryLoopFailsafe IMPLEMENTED 2026-08-31
```

`git ls-files`: `main.cpp`, `MyForm.h` (shim), `src/core/*` (6), `src/watchdog/*` (4), `Windows_Hello_Fix_v2_0.*`, `x64/Release/install_script.nsi`.

---

## 1. Current Application Architecture

### High-level
```
main.cpp → MyForm (single central state owner, src/core)
  ├── MyForm_Core.cpp   ctor/dtor/InitializeComponent/MyForm_Load
  ├── MyForm_Camera.cpp SetupAPI/CfgMgr pipeline
  ├── MyForm_Config.cpp config.txt + diagnostic.log + target resolution
  ├── MyForm_Events.cpp WndProc (shutdown/power/session)
  ├── MyForm_System.cpp command parsing + wake listener
  ├── MyForm_UI.cpp     FormClosing + btnToggle_Click
  └── watchdogs (owned by main.cpp, outside src/core)
       ├── CameraFailsafe      90s poll + 10s verify + 45s grace
       └── RecoveryLoopFailsafe 5s startup + 30s poll + 5s retry
```

### Lifecycle (`src/core/MyForm_Core.cpp:182-429`, `main.cpp:7-67`)
```
Boot → Task WindowsHelloFix (--background) → main() → MyForm form
  → Opacity=0/ShowInTaskbar=false if --background/--disable/--enable-camera
  → RecoveryLoopFailsafe created in main.cpp iff !isCommandWorker
  → Application::Run → MyForm_Load
    → Startup_Context log → IsRestoreCameraCommand? → Hide → RestoreConfiguredCameraHardware(true) → Exit(0)
    → IsDisableCameraCommand? similarly → Exit(0)
    → CreateMutex Global\WindowsHelloFix_AppMutex + hWakeupEvent
    → RestoreConfiguredCameraHardware(true) cycle (Sleep 350/900/500 + Verify 3×100ms)
    → RegisterPowerSettingNotification(Lid/Button)
    → ScanSystemCameras(DIGCF_ALLCLASSES|PRESENT) → LoadConfigState → EnableTarget
    → dropdown + MI_00 auto → shouldAutoStart → isMonitoring, background hide
    → backgroundWorker Thread (ListenForWakeupSignal)
    → WTSRegisterSessionNotification 6×500ms → Arm CameraFailsafe + RecoveryLoopFailsafe via form.Load
  → steady state: WndProc, btnToggle, FormClosing (hide-to-background)
  → dtor/finalizer: Disarm → if isSystemEnding Disable else Enable → save config → cleanup
```

State ownership: `MyForm` owns `isMonitoring`, `isBackgroundMode`, `isSystemEnding`, `cameraExpectedDisabled`, `selectedInstanceId` (`wstring*`), `cachedCameras`, `hAppMutex/hWakeupEvent`, `hLidNotification/hButtonNotification`, `backgroundWorker`, controls, `diagnosticLogSync`. Globals `g_lastHardwareToggleTick` etc. in `MyForm_Camera.cpp`. Threading: UI pump + one background WaitForSingleObject thread only Invoke; watchdogs use Forms::Timer on pump — no thread pool.

Protected boundary: `src/core/` + `src/watchdog/` HARD PROTECTED. `main.cpp` is outside `src/core` per `AGENTS.md §1` — sole allowed seam. Updater must be `src/updater/` island with no edge into camera logic.

---

## 2. Existing Version / Release Mechanism

| Item | Evidence | Value |
|---|---|---|
| Manifest | `app.manifest:3` | `assemblyIdentity version="2.1.0.0" name="Windows_Hello_Fix_v2_1" level=requireAdministrator` |
| Window title | `src/core/MyForm_Core.cpp:174` | `Text = L"Windows Hello Fix v2.0"` — stale (manifest 2.1 vs title 2.0) |
| VERSIONINFO | `*.rc` | NONE — no `VS_VERSIONINFO`, no `GetFileVersionInfo` source |
| Exe name | `install_script.nsi:7`, `main.cpp:19` | `Windows_Hello_Fix_v2_0.exe` — stable across v1→v2; hardcoded 6+ places |
| Installer | `install_script.nsi:6 OutFile` | `Windows_Hello_Fix_Setup.exe` in `x64/Release/` (gitignored) |
| Config | `MyForm_Config.cpp:62-98` | `config.txt` only `monitoring=` + `device=` — no version |
| Tag | `git tag` + GitHub API | `v1.0.0`, `v2.0.0` — strict `vMAJOR.MINOR.PATCH` |
| Release name | GitHub API `name` | `Windows Hello Fix v2.0` (human) vs `tag_name v2.0.0` (machine) |

Finding: No centralized Version class; updater introduces `src/updater/UpdateVersion` as sole authority with `IsUpdaterSupported() => >=v2.1.0` and `main.cpp` fixes title post-construct.

---

## 3. Existing Installer Mechanism

`x64/Release/install_script.nsi:1-268` — NSIS, `RequestExecutionLevel admin`:

- Pre-checks: `CreateMutex WindowsHelloFixSetup_Mutex`; `HKLM\...\Uninstall\WindowsHelloFix` prompt.
- Install: `taskkill /F /IM Windows_Hello_Fix_v2_0.exe /T` (1500ms) → `File exe+metagen+ico` → `Unblock-File` → `CreateDirectory %APPDATA%\Windows Hello Fix` → `Exec '"$INSTDIR\...exe" /restore-camera'` (3000ms) → Start Menu shortcuts → registry `HKLM64\...\Uninstall\WindowsHelloFix` (`DisplayName Windows Hello Fix v2.1`) → remove `RUNASADMIN` flags → task generation via `$PLUGINSDIR\RegisterWindowsHelloFixTasks.ps1`.
- Tasks:

| Task | Trigger source | Effect |
|---|---|---|
| `WindowsHelloFix` | `New-ScheduledTaskTrigger -AtLogOn --background` | daemon at logon |
| `WindowsHelloFix_Lock` | `COM Triggers.Create(11) StateChange=7 --disable-camera` | lock→disable |
| `WindowsHelloFix_Unlock` | `New-ScheduledTaskTrigger -AtLogOn Delay PT10S --enable-camera` (mutated from StateChange=8) | startup helper only, NOT Win+L |
| `WindowsHelloFix_LogCleanup` | `Daily 00:00 cmd /c break > diagnostic.log` | log rotation |
| All: `Principal -UserId $user -LogonType Interactive -RunLevel Highest`, `Hidden/AllowStartIfOnBatteries/DontStopIfOnBatteries/StartWhenAvailable/MultipleInstances/Priority4`. |

- Uninstall: `Exec /restore-camera` → `taskkill` → second `/restore-camera` → `schtasks /Delete /TN "WindowsHelloFix*"` (5) → registry delete → `Delete $INSTDIR\*` → `Delete %APPDATA%\Windows Hello Fix\{config.txt,diagnostic.log}`.

Load-bearing: Installer **is authority** for placement, task registration, registry, unblock. Proven file-lock release is `taskkill /F /IM` + `Sleep 1500` → then `File`. Exe path stable `C:\Program Files\WindowsHelloFix\Windows_Hello_Fix_v2_0.exe`. `MUI_FINISHPAGE_RUN` restarts app.

---

## 4. Existing GUI Architecture

`MyForm_Core.cpp:119-180 InitializeComponent`:

- `ClientSize 430×240`, `FormBorderStyle FixedDialog`, `MaximizeBox=false MinimizeBox=false`, `CenterScreen`.
- `deviceDrop ComboBox DropDownList @(25,75) 380×31`
- `btnToggle Button Bold @(25,130) 380×45` — `Start/Stop Monitoring Service`
- `lblTitle Label Bold @(20,25) "Select Target RGB Sensor"`
- `lblStatus Label Gray @(25,195) "Status: Service Stopped/Running"` (`AutoSize=true`, `Green/Gray`)
- `Load` + `FormClosing` wired; `Icon` from `IDI_ICON1`.

Empty bottom-right: `lblStatus y=195` (~13px tall) → Client bottom `240` → **~30-45px vertical padding** unused — concept icon belongs at `(392,192) 22×22` right of `lblStatus`.

- `MyForm_UI.cpp:5-22` FormClosing cancels UserClosing → Hide+ShowInTaskbar=false + isBackgroundMode toast; `MyForm_System.cpp:43-52` BringWindowToFrontDelegate restores `Opacity=1 / Show/Visible/ShowInTaskbar/Normal`.
- Tech: WinForms C++/CLI (`System::Windows::Forms`), hand-written InitializeComponent (4 lines per control).
- Preferred seam: tiny owner-drawn `PictureBox`/`Panel` 22×22 at `(ClientWidth-28, 195)` — right of `lblStatus`, inside `Controls`, wired via `Updater::StateChanged` + `BeginInvoke`. No resize, no other control moves.

---

## 5. Current Startup / Exit / File-Lock Behaviour

- **Startup variants:** `WindowsHelloFix --background` (Task, `Opacity 0` via `main.cpp:28-33`), interactive double-click (`Opacity 1`), command workers `--enable/disable-camera /restore-camera /repair-camera` (`MyForm_Load:208-228` Hide → Restore → Exit(0)), `Global\Mutex` second-instance wake via `Global\WakeupEvent` + `ListenForWakeupSignal`.
- **Exit:** `FormClosing(UserClosing)` hides; only `Stop Monitoring` or `taskkill` truly exits. `isSystemEnding` (`WndProc 0x0011/0x0016` + `~MyForm`) `DisableTargetCameraHardware(true)` leaves Disabled for next boot (covered by `Restore(true)` + watchdogs).
- **Replace exe while running:** Yes via installer. `taskkill /F /IM` + `Sleep 1500` releases Mutex + image lock; `File $INSTDIR\Windows_Hello_Fix_v2_0.exe` then overwrites. Direct File::Move without kill fails `ERROR_SHARING_VIOLATION`. Self-overwrite without helper impossible while mapped.
- **Task Scheduler dependence:** All 4 tasks use absolute `Path=$exe` + `WorkingDirectory=$wd` (`PROGRAMFILES64\WindowsHelloFix`). Post-update path identical, so Tasks do not need re-registration if installer re-runs its script (`Force|Out-Null` idempotent). Standalone exe replacement without installer would also preserve path but skip unblock/registry fix — installer preferred.

---

## 6. GitHub Release Architecture

### Live verification 2026-09-01 — `GET https://api.github.com/repos/Shivu516/Windows-Hello-Fix/releases`

Releases:
- `v2.0.0` `id 335771490` `prerelease false` `draft false` `published 2026-06-08` `asset Windows_Hello_Fix_Setup.exe size 681623 content_type application/x-msdownload digest sha256:0443c31e... browser_download_url https://github.com/Shivu516/Windows-Hello-Fix/releases/download/v2.0.0/Windows_Hello_Fix_Setup.exe html_url https://github.com/Shivu516/Windows-Hello-Fix/releases/tag/v2.0.0`
- `v1.0.0` `id 316865451` asset `Windows_Hello_Fix_v1.0.zip`
- Tags strict `vMAJOR.MINOR.PATCH`.

### Endpoint design

| Need | Endpoint | Notes |
|---|---|---|
| Release list (explorer) | `GET /repos/{owner}/{repo}/releases?per_page=20&page=1` | `ETag`/`Last-Modified` → `If-None-Match` → `304` |
| Latest Stable | `GET /repos/{owner}/{repo}/releases/latest` | auto excludes prerelease/draft |
| Latest including prerelease | `GET /releases?per_page=1` filtered client-side | No GitHub channel |
| Tags fallback | `GET /repos/{owner}/{repo}/tags?per_page=100` | Only if prerelease flag missing |

### Fields captured

`id, tag_name, target_commitish, name, draft, prerelease, created_at, published_at, html_url, body, assets[] {name,label,content_type,size,download_count,browser_download_url,digest}, tarball_url, zipball_url` plus response headers `ETag`, `Last-Modified`, `X-RateLimit-*`.

### Headers

`Accept: application/vnd.github+json` + `User-Agent: WindowsHelloFix-Updater/1.0 (https://github.com/Shivu516/Windows-Hello-Fix)` (required) + optionally `X-GitHub-Api-Version: 2022-11-28` + cache `If-None-Match`.

### Library

**Managed `System.Net.Http.HttpClient`** (add `System.Net.Http` reference; no native WinHTTP/WinINet lib, no third-party). One static HttpClient per process, TLS 1.2+ via net472 default.

---

## 7. Release Metadata Model

```cpp
// src/updater/UpdateModels.h
enum class UpdateChannel { Stable, Beta, PreRelease, Unknown };

ref struct ReleaseAsset {
    String^  name;                // Windows_Hello_Fix_Setup.exe
    String^  browserDownloadUrl;  // https://github.com/.../download/v2.0.0/...
    String^  contentType;         // application/x-msdownload
    long long size;               // 681623
    String^  sha256;              // parsed from digest "sha256:0443..." (null for old)
    int      downloadCount;
};

ref struct GitHubRelease {
    long long id;                 // 335771490
    String^  tag;                 // v2.0.0
    UpdateVersion^ version;       // parsed semantic
    String^  name;                // Windows Hello Fix v2.0
    String^  htmlUrl;             // https://github.com/.../releases/tag/v2.0.0
    String^  body;                // markdown notes
    DateTime publishedAt;         // 2026-06-08T06:13:00Z
    bool     isPrerelease;
    bool     isDraft;             // filtered out
    UpdateChannel channel;        // derived (§8)
    List<ReleaseAsset^>^ assets;
    bool     hasUpdaterSupport;   // version >= v2.1.0 (§17)
};
```

Drafts never shown. Authoritative asset name is `Windows_Hello_Fix_Setup.exe` (observed). Unknown asset names displayed but Update blocked until allow-list match.

---

## 8. Release Channel Design

GitHub has no `Stable/Beta/PreRelease` — only `prerelease` bool + `tag`/`name`. Channels are client-side filter.

| Release state | `prerelease` | `tag` pattern (lowercased) | Derived channel |
|---|---|---|---|
| Stable (today v2.0.0) | `false` | `vMAJOR.MINOR.PATCH` no suffix (`v2.0.0`, `v2.1.0`) | `Stable` |
| Beta (future) | `true` | contains `-beta`, `-b`, `.beta` or name `Beta` (`v2.2.0-beta.1`) | `Beta` |
| Pre-Release / RC | `true` | contains `-rc`, `-pre`, `-preview`, `-alpha`, `.rc.` (`v2.2.0-rc.1`) | `PreRelease` |
| Unknown prerelease | `true` | none of above | `PreRelease` (conservative) |

Stable defined as `prerelease==false && no prerelease segment` to survive mis-tagging; any `prerelease==true` never Stable.

**User channel affects:**

```
Stable      → latest = max Stable
Beta        → latest = max {Stable ∪ Beta}
PreRelease  → latest = max {Stable ∪ Beta ∪ PreRelease}  (all non-draft)
```

Notification dot fires only if `latestForSelectedChannel.version > installedVersion` and not downgrade-excluded. Stored in `%APPDATA%\Windows Hello Fix\updater_cache.json` `{channel,lastCheckUtc,etag,cachedReleases}`.

---

## 9. Version Comparison

Define `src/updater/UpdateVersion` as sole authority (no `VERSIONINFO` exists):

```cpp
ref class UpdateVersion : IComparable<UpdateVersion^> {
    int major, minor, patch;
    String^ prereleaseLabel; // "beta.1", "rc.1", null for stable
    int     prereleaseNumber;
    String^ rawTag; // v2.1.0
    static UpdateVersion^ Parse(String^ tag);
    static bool TryParse(String^ tag, UpdateVersion^% out);
    int CompareTo(UpdateVersion^ other);
    bool IsUpdaterSupported() { return major > 2 || (major==2 && minor>=1); } // v2.1+
    String^ ToDisplay();
};
```

**Ordering (SemVer 2.0 + HelloFix):**
1) major, then minor, then patch numerically (missing = 0; `v2.1 == v2.1.0`)
2) If equal, stable (no label) > any prerelease
3) If both prerelease, label rank `alpha < beta < pre < preview < rc` (unknown lexical), then numeric suffix (`beta.1 < beta.2 < rc.1`)
4) Malformed → TryParse false → shown last, never auto `latest`

Examples: `v2.0.0 < v2.1.0 < v2.1.1 < v2.2.0 < v3.0.0`; `v2.2.0-rc.1 < v2.2.0-beta.1 < v2.2.0` within prerelease; `installed==available → UpToDate`.

---

## 10. Update Discovery

**Invariant:** *HelloFix startup never waits for GitHub. Camera path 2.8s; updater adds 0ms.*

| Trigger | When | Behaviour |
|---|---|---|
| Startup check (deferred) | `main.cpp Task::Delay(5000)` after `Application::Run` pump + `RecoveryLoopFailsafe` Arm — never during `MyForm_Load` | Background only; skip if `isCommandWorker` |
| Periodic | `Timer 6h` while `isMonitoring && isArmed` | Coalesced; skip if last successful <1h |
| Manual refresh | Updater popup `⟳` / `Check now` | Always fires, debounced 30s |
| Cooldown | `lastCheckTick + 30min` + ETag reuse | Mirrors watchdog 1500ms dedup |
| Cache | `%APPDATA%\Windows Hello Fix\updater_cache.json` + `updater_etag.txt` | ETag/`Last-Modified` → `304` use cached; failure use stale <24h; success overwrite |
| Network failure | `HttpRequestException` | `Offline` → cached visible + `offline` banner |
| API 429 | `429` → `RateLimited` + `Retry-After`/`X-RateLimit-Reset` | Retry at `Reset+60s` jitter, max 1/h |
| Malformed | parse `false` → skip entry | Log `Updater_MalformedRelease` but no crash |
| Offline | No internet | No exception propagates to core; watchdogs untouched |

No busy polling, no 1s timer.

---

## 11. Network Threading — Do Not Block

```
GUI (UI thread)
  │  ↓ popup or startup delay
  │  Task::Run(CheckAsync)   // ThreadPool
  │         ↓ HttpClient.GetAsync → await
  │  GitHub API (HTTPS)
  │         ↓ JSON parse
  │  UpdateState diff (version compare, channel filter)
  │         ↓ Control::BeginInvoke(Action{ apply state → invalidate icon/popup })
  └────────→ icon/popup renders
```

Zero `Wait()/Result` on UI — all async/await or ContinueWith(TaskScheduler::FromCurrentSynchronizationContext). HttpClient timeout 15s; CancellationTokenSource cancelled on FormClosing/Disarm. Download uses ResponseHeadersRead + Stream::CopyToAsync with IProgress<int> marshaled via BeginInvoke. No SetupDi/CM_* from network thread.

---

## 12. UI Concept (integration into 430×240)

```
┌─────────────────────────────────────────────┐  430×240 FixedDialog
│ Windows Hello Fix v2.1                  X   │
│ Select Target RGB Sensor                    │  lblTitle (20,25)
│ [ComboBox: deviceDrop            ▼]        │  (25,75) 380×31
│ [Start Monitoring Service               ]   │  btnToggle (25,130) 380×45
│ Status: Service Running            [ ↓ • ] │  lblStatus (25,195) + icon (392,192) 22×22
└─────────────────────────────────────────────┘
  icon: PictureBox Panel owner-drawn, BackColor Transparent
  glyph: ↓ Segoe MDL2 Assets E896 12pt #605E5C
  dot: ● 6px #D13438 top-right overlay — visible only when UpdateAvailable
  tooltip: "Updates — click to view releases" / "Checking..." / "Up to date" / "Download failed"
```

| State | Icon | Dot | Tooltip | Click |
|---|---|---|---|---|
| `Idle/UpToDate/Offline` | `↓` dim 60% | hidden | `Up to date` | opens explorer UpToDate variant |
| `Checking` | `↓` pulsing 500ms | hidden | `Checking...` | disabled |
| `UpdateAvailable` | `↓` full | **● red** | `Update available: v2.2.0` | highlights latest |
| `Downloading n%` | `↓` + progress arc | hidden | `Downloading 42%…` | explorer with progress + Cancel |
| `Installing` | `↓` spinner | hidden | `Installing…` | disabled |
| `Error` | `↓` + `!` | hidden | `Update check failed — retry` | error banner |
| `RestartRequired` | `↓•` yellow | yellow | `Restart to finish` | — |

Why this placement: no resize, no large button, Segoe UI 9-10 consistent, single PictureBox (4 lines). Enlarging to 430×280 considered but rejected.

---

## 13. Updater Interaction Model — Recommended Option D Hybrid

| Option | Fit |
|---|---|
| A Compact floating window | ok but feels like second app |
| B Expanded section (enlarge to 380) | violates "do not redesign GUI" |
| C Small popup/menu | cannot show notes/channel/assets |
| **D Hybrid small updater popup + Browse Releases** | **recommended** |

```
             [ ↓ ● ] click
                 │
                 ▼
      ┌─────────────────────────────────┐
      │ Updates                     ✕   │
      │ Windows Hello Fix v2.1 → v2.2.0│  Current → Available (green if newer)
      │ Stable • Released 2026-06-08   │  Channel + date
      │ ─────────────────────────────── │
      │ Release notes excerpt (3 lines) │
      │ [View on GitHub]  [Copy notes]  │
      │ ─────────────────────────────── │
      │ [ Update ]    [ Details ▸ ]     │  disabled if offline/no asset/latest
      │ Channel: [Stable ▼]  ⟳ Check   │  Combo + manual refresh
      │ Browse releases (3)  ▸           │  Expands inline list (§14)
      └─────────────────────────────────┘
```

Variants: UpToDate → ✓ latest Stable (v2.1.0) + Browse; Offline/Error → banner + Retry; Downloading → Cancel (42%) + progress bar. Single popup instance coalesced.

---

## 14. Release Explorer Design

```
Compact popup
   ↓ Browse releases
Release list (scrollable virtual ListBox, 20 max per channel)
   ↓ filter tabs [Stable] [Beta] [PreRelease]
selected row → detail pane (bottom half, no new top-level)
  notes (markdown plain+links), Assets (name+size), Version+Channel+Date
  actions: [Update to this version] [Download Installer...] [Open on GitHub]
```

**Rows:**
```
Windows Hello Fix v2.2.0    Stable      2026-08-14   ● latest
Windows Hello Fix v2.1.0    Stable      2026-07-01     installed ✓
Windows Hello Fix v2.0.0    Stable      2026-06-08
```

Date from `published_at yyyy-MM-dd`; descending CompareTo within channel; installed pinned.

**Fetching:**
- Minimum API: **one call** `GET /releases?per_page=20&page=1` → all rows. No per-asset call (assets inline). Only Update uses browser_download_url already cached.
- `View on GitHub` → `Process::Start(html_url)` (`UseShellExecute=true`, validate `Uri::IsWellFormedUriString` + `https://github.com/Shivu516/Windows-Hello-Fix/releases/tag/` prefix). No WebView2.

---

## 15. Update File Download Policy

**Normal Update — never in Downloads:**
```
User → Update
  ↓ Downloading
  ↓ DownloadToTemp(browser_download_url, size, sha256, progress, cancel)
  ↓ Staging: %TEMP%\WindowsHelloFix\Updates\{guid}\Windows_Hello_Fix_Setup.exe
  ↓ Verify size+sha256 (if digest) → InstallStarted
  ↓ Launch installer helper → main Exit → installer replaces → helper cleans → launches new exe
  ↓ On success: cleanup TEMP\{guid}
  ↓ On cancel/fail: immediate delete
```

- **Temp:** `GetTempPath() + "WindowsHelloFix\Updates\" + Guid.NewGuid("N") + "\" + "Windows_Hello_Fix_Setup.exe"`. Per-user isolation, avoids Program Files ACLs.
- **Naming:** Fixed installer name, no user input, sanitize allow-list, ignore Content-Disposition.
- **Cleanup:** Cancel(), DownloadFailed, InstallFailed, ApplicationExit, OS Disk Cleanup, plus start-up sweep deletes Updates\*\ older than 7 days.
- **Interrupted:** token → delete .part → Idle with retry banner.
- **Permission:** TEMP writable as standard user; installer elevation at launch (UAC), not download.
- **AV lock:** Download to .part then File::Move atomically; retry move 3×500ms else Error_FileLocked.

Explicit save: `Download Installer...` (SaveFileDialog exe, Downloads) → direct to chosen path, no TEMP, no post-cleanup, no auto-launch.

---

## 16. Running-Executable Update Problem — B hybrid recommended

Investigation §§5,17: `Windows_Hello_Fix_v2_0.exe` memory-mapped — File::Replace fails ERROR_SHARING_VIOLATION; proven unlock is `taskkill /F /IM` + Sleep 1500 → then File.

```
HelloFix GUI (UpdateAvailable → Update)
   │ staged Setup.exe to TEMP\{guid}\
   ▼
Updater helper (B: direct Setup.exe launch; A-light: tiny helper exe)
   │ Process::Start(TempSetupPath, "/S") then Environment::Exit(0)
   ▼
HelloFix exits (mutex released)
   ▼
Setup.exe: taskkill (safety) → Unblock-File → File overwrite → task re-register → optional /restore-camera
   ▼
MUI_FINISHPAGE_RUN launches new exe
   ▼
helper deletes TEMP\{guid} + self-deletes (cmd /c ping+del) or startup sweep
```

| Sub-option | UX | Verdict |
|---|---|---|
| **B Direct Setup.exe launch** | NSIS itself is helper; first line taskkill kills caller if race; one less binary | **Primary** |
| A-light tiny helper UpdaterHelper.exe (or one-shot .cmd) | Wait PID → launch Setup.exe → delete self; fixes wizard-behind-background focus | **Fallback if B wizard hidden** |

Keeps install logic in one place. Task path stability passes through.

---

## 17. Installer vs Direct Exe Update — Installer authoritative

| Candidate | Size v2.0.0 | What it does | Verdict |
|---|---|---|---|
| `Windows_Hello_Fix_Setup.exe` 681 KB NSIS | 681 KB | Overwrites exe+metagen+ico + unblock + config dir + tasks + registry + MUI_FINISHPAGE_RUN | **AUTHORITATIVE** |
| `Windows_Hello_Fix_v2_0.exe` ~487 KB | ~487 KB | Bare image, no tasks/registry/unblock | REJECT |
| `*.zip` v1 legacy | — | obsolete pnputil | reject |

**Asset identification:**
```
assets → filter a where a.name=="Windows_Hello_Fix_Setup.exe" (OrdinalIgnoreCase)
                       && a.content_type=="application/x-msdownload"
                       && a.browser_download_url.StartsWith("https://github.com/Shivu516/Windows-Hello-Fix/releases/download/")
If 1 → authoritative; If 0 → Update disabled + "No installer — View on GitHub"; If >1 → pick largest with sha256.
Allow-list enforced — never execute arbitrary name.
```

Checksum via `assets[].digest "sha256:0443..."` → `SHA256.Create().ComputeHash(FileStream)` after download; on mismatch delete+retry once. Task/config preserved (installer overwrite keeps config.txt).

---

## 18. Downgrading

User may select older row → `Update to this version` treated uniformly:
```
V_sel > V_cur → Upgrade
V_sel == V_cur → Reinstall (confirm)
V_sel < V_cur → Downgrade → confirm + warning if IsUpdaterSupported==false
```

**Downgrade dialog:**
```
┌──────────────────────────────────────────────┐
│ Downgrade to v2.0.0?                         │
│ You are on v2.1.0. Selected v2.0.0.          │
│ ⚠ v2.0.0 does NOT include the in-app updater │
│ After downgrading, download icon and         │
│ Browse Releases will disappear. To return    │
│ you will need to manually download from      │
│ GitHub.                                      │
│ [ Cancel ]    [ Downgrade anyway ]           │
│ ☐ Don't warn again for this version          │
└──────────────────────────────────────────────┘
```

Warning via single capability query `candidate.IsUpdaterSupported()==false` (today only v2.0.0/v1.0.0). No scattered if tag=="v2.0.0".

---

## 19. Future Updater Compatibility

| Option | Mechanism | Cost |
|---|---|---|
| **A Version-capability rule** | `IsUpdaterSupported() => >=v2.1.0` | Zero, offline, no metadata |
| B Explicit metadata `updaterSupported=true` in body | Requires release author memory | Over-engineered |

Chosen: A. Future protocol bump can extend to check suffix `_updater2.exe` if needed — YAGNI for 2.1.

---

## 20. Security Model

| Threat | Mitigation |
|---|---|
| MITM / HTTP downgrade | TLS 1.2+ only; allow-list `https://api.github.com` + `https://github.com`; reject `http://` from JSON |
| Malformed JSON | Strict parse; validate `tag` regex `^v\d+\.\d+\.\d+(-[a-z0-9.]+)?$`, `assets[].browser_download_url` github prefix; ignore extra |
| Malicious asset URL | Allow-list `Host=="github.com" && Path.StartsWith("/Shivu516/Windows-Hello-Fix/releases/download/") && name=="Windows_Hello_Fix_Setup.exe"` |
| Corrupt/tampered | Size + sha256 verify; on mismatch delete+retry |
| TOCTOU | Fetch→immediate download same URL; re-verify size+sha; on 404 → Error_AssetNotFound + re-check |
| Privilege escalation | Staged Setup NOT executed as admin until user clicks Update; UAC requireAdministrator dialog. TEMP inherits user ACL. |
| Temp permission/symlink | Per-user GetTempPath(), FileShare::None, no symlink follow |
| Arbitrary execute | UpdateInstaller::Launch allow-list only; other URLs via UseShellExecute browser |
| Rate-limit abuse | 60/h + cooldown + ETag prevents self-DoS |

Full Authenticode + WinVerifyTrust recommended future phase (no cert today).

---

## 21. Network Failure Behaviour

**Invariant: no updater failure affects core.**

| Failure | Updater | Core + watchdogs |
|---|---|---|
| No internet/DNS | `Offline`, cached or banner, no MessageBox | WTS/Power/CameraFailsafe/RecoveryLoop unaffected |
| GitHub 5xx/429 | `Error_RateLimited` with reset; use stale <24h | unchanged |
| Malformed | Skip entry, log MalformedRelease | unchanged |
| Missing asset | Row `No installer — View on GitHub`; Update disabled | unchanged |
| Download 404 mid-flight | `DownloadFailed|AssetNotFound` + re-check | unchanged |
| Timeout | `DownloadFailed|Timeout` → delete part | unchanged |
| User dismiss | Banner ✕ → Idle until next periodic/manual | — |

Logging Updater_NetworkError only on transition, not per-poll.

---

## 22. Camera Failsafe Isolation

```
src/core                  — camera hardware authority
      ↑ observes via getters          ↓ calls Recover/Verify
src/watchdog              — failsafe authority (CameraFailsafe 90s, RecoveryLoop 30s)
                                      ↕ NO edge to updater
src/updater               — update/release authority ONLY
   Updater ──► GitHubReleaseClient
           ──► UpdateVersion/Channel
           ──► UpdateInstaller (TEMP, launch)
           ──► UpdateState/cache
           ──► UpdaterUI (icon/popup) → GUI events/BeginInvoke
GUI (MyForm) ──► Updater public interface (main.cpp owned)
```

Updater never calls DisableTargetCameraHardware, EnableTargetCameraHardware, RecoverCameraHardware, SetCameraHardwareStateVerified, ScanSystemCameras, WTS*, RegisterPowerSettingNotification, CameraFailsafe::Arm, RecoveryLoopFailsafe::*. Never observes isMonitoring/isSystemEnding/cameraExpectedDisabled/isAlreadyDisabled/g_lastHardwareToggleTick. Enforced by no #include "../watchdog" and review gate.

---

## 23. GUI Integration Boundary — Minimum

| File | Change | Size | Why not in src/updater |
|---|---|---|---|
| `main.cpp` | Own `Updater^` block after RecoveryLoopFailsafe: `if (!isCommandWorker){ updater=gcnew Updater(%form); form.Load+=Updater::OnOwnerLoad; form.FormClosing+=Updater::OnOwnerClosing; }` + SetVersionFromManifest() | ~15 lines | main.cpp outside src/core per AGENTS.md §1 — allowed, mirrors failsafe precedent |
| `src/core/MyForm_Core.cpp` InitializeComponent | ZERO preferred — updater creates overlay at OnOwnerLoad via form->Controls->Add dynamic injection. Fallback 4-line PictureBox if Z-order fails. | 0 or 4 | Dynamic injection keeps core byte-identical |
| `src/core/MyForm_Config.cpp` | NONE — updater uses own updater_cache.json via Environment::GetFolderPath | 0 | reuse directly |
| `x64/Release/install_script.nsi` | NO CHANGE for v2.1 updater (future embeds new exe only) | 0 | — |

Smallest seam is main.cpp creates updater and injects 22px icon at runtime; popup owned Form anchored to main.

---

## 24. CORE / WATCHDOG CHANGES

```text
src/core changed:    NO  (planning zero; implementation targets NO — dynamic injection path)
src/watchdog changed: NO
```

**Exception table (if later deemed unavoidable — all STOP-and-ask):**

| File | Hypothetical | Reason claimed | Why src/updater cannot solve | Smallest |
|---|---|---|---|---|
| `src/core/MyForm_Core.cpp:174` | `Text = "Windows Hello Fix v2.0"` → `v2.1` | Chrome title mismatch | Could be `form.Text = Updater::CurrentVersionDisplay()` in main.cpp so not unavoidable | one line |
| `src/core/MyForm_Core.cpp:119` | Add `PictureBox updaterIcon` designer | Z-order fragile | Dynamic injection proven via failsafe → not unavoidable | four lines |
| `src/watchdog/*` | Updater→watchdog call | — | Forbidden §22 → rejected | — |

All require explicit approval per AGENTS.md §8.

---

## 25. Build System

Current `Windows_Hello_Fix_v2_0.vcxproj:130-163` references `System`, `System.Data`, `System.Drawing`, `System.Windows.Forms`, `System.Xml`; `ClInclude src\core` + `src\watchdog`.

**Minimum for v2.1:**
1. Add `Reference Include="System.Net.Http"` (and `System.Web.Extensions` if needed).
2. Keep `AdditionalDependencies setupapi.lib;user32.lib;wtsapi32.lib;advapi32.lib;` unchanged; no winhttp.lib.
3. Add compile items for `src/updater/*` (8 pairs).
4. No flag changes (`UseDebugLibraries`, `CLRSupport true`, etc.).
5. `.vcxproj.filters` — add `Source Files\src\updater` + `Header Files\src\updater` groups.
6. `app.manifest` already `2.1.0.0` — bump to `2.2.0.0` at next release only.
7. No `.rc` icon change — dot is GDI-painted.

No WebView2, libcurl, nlohmann/json, OpenSSL — rejected.

---

## 26. Threading / Lifetime (detailed)

- Updater::CheckAsync() — Task::Run → FetchAsync(cancel) → await. Owned by main.cpp's MyForm scope; FormClosing → Disarm() → CancellationTokenSource::Cancel() → CancelPendingRequests() → State=Idle. Every BeginInvoke checks !form->IsDisposed && !Disposing.
- UpdateInstaller::DownloadAsync(url,path,progress,cancel) — FileStream(.part,Create,Write,None) + GetStreamAsync → CopyToAsync with progress; on cancel → Delete(part); on complete → Move(part,final).
- UpdateInstaller::ApplyUpdate(temp,currentPid) — Process::Start with Verb="runas" or let NSIS self-elevate (requireAdministrator). Helper waits WaitForSingleObject(pid,5000) then proceeds if timeout.
- No callback after Application::Exit — helper external; updater GC'd with main.cpp.

---

## 27. Caching

- Location `%APPDATA%\Windows Hello Fix\updater_cache.json` + `updater_etag.txt`.
- Format `{lastCheckUtc, channel, etag, lastModified, releases:[{serialized GitHubRelease}]}` — stores only 9 fields, not raw.
- Expiration: ETag primary; time fallback kCacheMaxAge=6h, kMinCheckInterval=30m. Stale <24h reused on Offline/RateLimited; >7d discarded.
- ETag: send If-None-Match on periodic; on 304 update lastCheckUtc only; on 200 parse ETag, overwrite atomically (WriteAllText(temp)+Move).
- Force: RefreshAsync(true) omits If-None-Match (full 200).

---

## 28. Logging (reusing diagnostic.log)

Reuse MyForm::LogFailsafe via owner handle (Monitor::Enter(diagnosticLogSync)).

**Events (non-spam, transitions only):**
```
Updater_CheckStarted          | Channel=Stable Force=0
Updater_CheckCompleted        | Releases=2 Latest=v2.0.0 DurationMs=340
Updater_UpdateAvailable       | Available=v2.2.0 > Installed=v2.1.0
Updater_NoUpdate              | Installed=v2.1.0 Latest=v2.1.0
Updater_ChannelChanged        | From=Stable To=Beta
Updater_ReleaseSelected       | Tag=v2.0.0 Channel=Stable
Updater_DownloadStarted       | Asset=Windows_Hello_Fix_Setup.exe Size=681623
Updater_DownloadProgress      | Percent=42 (throttled 0/50/100)
Updater_DownloadCompleted     | Path=%TEMP% Verified=Sha256
Updater_DownloadFailed        | Reason=Checksum|Timeout|Canceled|AssetNotFound
Updater_InstallStarted        | Path=... Silent=1
Updater_InstallFailed         | ExitCode
Updater_DowngradeWarning      | Current=v2.1.0 Target=v2.0.0 HasUpdater=0
Updater_BrowserOpened         | Url=https://...
Updater_NetworkError          | Code
Updater_RateLimited           | Remaining=0 Reset
Updater_CacheUsed             | Reason=304NotModified|Offline AgeHours=2
```

Idle polling not logged; CheckStarted ≤1/30min.

---

## 29. UI States

| State | Icon | Dot | Tooltip | Popup banner | Next |
|---|---|---|---|---|---|
| Idle | `↓` 60% | — | `Checking…` | — | Checking |
| Checking | `↓` pulsing | — | `Checking…` | spinner | UpToDate/UpdateAvailable/Error |
| UpToDate | `↓` 60% | — | `Up to date v2.1.0` | `✓ latest Stable` | Checking on timer/manual |
| UpdateAvailable | `↓` 100% | ● red | `Update available v2.2.0` | highlight + Update enabled | Downloading/dismissed |
| Downloading | `↓` ring | — | `Downloading 42%` | progress+Cancel | Installing/Error/Idle |
| Installing | `↓` spinner | — | `Installing…` | Installing disabled | exit |
| Error | `↓` + `!` | — | `Update check failed — retry` | `unavailable` banner | manual retry |
| Offline | `↓` dim | — | `Offline — using cached` | `Offline — cached 2h ago` | retry |
| RateLimited | `↓` dim | — | `Rate limited — retry 13:00` | reset time | auto at reset |

Only UpdateAvailable shows red dot.

---

## 30. Release Explorer Performance

- Do not download assets to show list — GET /releases?per_page=20 returns assets[] inline, no per-release call. Render 20 rows from 5-20KB JSON <10ms.
- Paginate only if Link: rel="next" present — Load more button.
- Lazy detail: notes body already in list response; only Update fetches browser_download_url binary — never pre-downloaded.
- Notes markdown as plain excerpt (200 chars) in list; full in detail pane with links; no image fetch.

---

## 31. GitHub Rate Limiting

Unauth GET /releases 60/h/IP (X-RateLimit-Limit:60). Design for <10/h worst: startup 1 + periodic 4 + manual 2 = 7. ETag→304 counts but cheaper. Never loop-retries.

On limit hit: parse Retry-After or X-RateLimit-Reset → Task::Delay(reset-Now+60s) → RateLimited banner; show cached <24h. No OAuth/PAT for v2.1; optional PAT via updater.json patEncrypted is incremental extension later.

---

## 32. Error / Edge-Case Matrix (25 cases)

| # | Edge case | Updater | Core isolation |
|---|---|---|---|
| 1 | No internet | Offline, cached or banner, no MessageBox | core running |
| 2 | GitHub 5xx/timeout | Error, retry after cooldown; stale <24h | core running |
| 3 | API 429 Rate limit | RateLimited with reset; retry at reset | — |
| 4 | Malformed release (bad tag) | Skip entry, show others | — |
| 5 | Missing installer asset | Row `No installer — View on GitHub`; Update disabled | — |
| 6 | Multiple installer assets | Pick allow-list; log MultipleInstallerAssets | — |
| 7 | Same version available | UpToDate, no dot; Update→Reinstall confirm | — |
| 8 | Newer version available | UpdateAvailable, dot, highlight | — |
| 9 | Older version selected | Downgrade → §18 warning | — |
|10| v2.0 selected (no updater) | Warning updater will disappear | — |
|11| Beta selected | Channel Beta filter + CompareTo order | — |
|12| Pre-release/RC selected | Under PreRelease only | — |
|13| Interrupted download 40% | DownloadFailed|Interrupted, delete .part, Retry | — |
|14| Corrupt download | Delete, DownloadFailed|Checksum, auto-retry once | — |
|15| Installer fails exit non-zero | InstallFailed|ExitCode, keep staging for retry | core 5-15s |
|16| User cancels | CancelRequested → delete .part → Idle | — |
|17| App closes during download | Disarm cancels → abort → delete part → sweep | — |
|18| Insufficient permissions (TEMP) | Error_TempUnavailable → try LOCALAPPDATA\Temp fallback | — |
|19| Antivirus locks installer | Retry move 3×500ms; if locked Error_FileLocked | — |
|20| TEMP unavailable (disk full/GPO) | Error_TempUnavailable + Open TEMP link | — |
|21| Release deleted after metadata | Download 404 → DownloadFailed|AssetNotFound → banner + auto Check | — |
|22| Asset URL invalid/redirect | 404/allow-list reject | — |
|23| Installed newer than channel latest | NoDowngradeNeeded — explorer shows installed > latest, dot hidden | — |
|24| Network disappears mid-download | Same as 13 — timeout 60s then DownloadFailed | — |
|25| Repeated clicks icon | Debounced 500ms + state==Checking/Downloading early-return; never parallel | — |

All updater catch (...) top-level in façade — no exception propagates to MyForm_Load/watchdogs.

---

## 33. User Experience Goal

```
Normal (UpToDate):   [ ↓ ] dim  → click → popup "✓ You are on latest v2.1.0" + Browse
Update available:    [ ↓ ● ] red → click
                     ┌──────────────────────────────────┐
                     │ Updates                      ✕   │
                     │ Windows Hello Fix v2.1 → v2.2.0  │
                     │ Stable • 2026-08-14              │
                     │ ─────────────────────────────── │
                     │ • Native C++ speeds…             │
                     │ [View on GitHub] [Copy notes]    │
                     │ [ Update ]       [ Details ▸ ]   │
                     │ Channel [Stable ▼]  ⟳            │
                     │ Browse releases (3)  ▸            │
                     └──────────────────────────────────┘
Click Browse → scroll list → row detail → [Update to v2.0.0] → if v2.0.0 → downgrade warning (§18)
Downloading:         [ ↓ ◐ ] 42% → click → popup [Cancel (42%)] + progress bar
Installing:          [ ↓ ◑ ] spinner → app exits → NSIS wizard → new app → TEMP cleaned
Error/Offline:       [ ↓ !] dim → click → banner "unavailable — View on GitHub / Retry"
```

Spec constraints met: minimal UI, no permanent Check for Updates button, icon bottom-right aligned to lblStatus, red dot, browsing+channels+browser, TEMP staging+cleanup.

---

## 34. Documentation

- This file preserves prior failsafe plan as Appendix A. No AGENTS.md/ARCHITECTURE.md edits in planning session; future docs PR adds docs/files/Updater.md + docs/UPDATER_DESIGN.md.
- Structure when preserved: `# Windows Hello Fix v2.1 — Update System Plan (DRAFT 2026-09-01)` + `## 1-40 (above)` + `## Appendix A — RecoveryLoopFailsafe (copy of current Plan.md before overwrite)`.

---

## 35. Files Planned for Future Implementation — Exact

**New (`src/updater/`):**
```
src/updater/UpdateVersion.h / .cpp
src/updater/UpdateChannel.h / .cpp
src/updater/UpdateModels.h / .cpp
src/updater/GitHubReleaseClient.h/.cpp
src/updater/UpdateState.h/.cpp
src/updater/UpdateInstaller.h/.cpp
src/updater/Updater.h/.cpp
src/updater/UpdaterUI.h/.cpp
```

**Modified (outside protected):**
```
main.cpp
Windows_Hello_Fix_v2_0.vcxproj
Windows_Hello_Fix_v2_0.vcxproj.filters
docs/Plan.md (this file)
docs/files/Updater.md (future docs PR)
```

**Optionally/alternatively touched only if dynamic injection fails:**
```
src/core/MyForm_Core.cpp (4-line PictureBox fallback — NOT in preferred plan)
```

**Explicitly NOT modified:**
```
src/core/MyForm.h, MyForm_Camera.cpp, MyForm_Config.cpp, MyForm_Events.cpp, MyForm_System.cpp, MyForm_UI.cpp → NO
src/watchdog/CameraFailsafe.h/.cpp, RecoveryLoopFailsafe.h/.cpp → NO
x64/Release/install_script.nsi (for v2.1) → NO
app.manifest / ProductionUtilities.h → NO
```

---

## 36. Build System — Exact Changes

*(see §5/§25 detail)* Add System.Net.Http, 8 ClInclude + 8 ClCompile; .filters add src\updater groups; manifest stays 2.1.0.0 until next release; no winhttp.lib.

---

## 37. Security Model & Integrity Verification (summary of §20)

- Transport HTTPS only, TLS 1.2+; validate tag regex, host allow-list, size range, draft filter.
- Asset integrity via sha256 digest from API (assets[].digest) — SHA256.Create().ComputeHash(FileStream) after download; corrupted download also caught. No cert today; WinVerifyTrust recommended future.

---

## 38. Test Matrix

| # | Scenario | Steps | Expect |
|---|---|---|---|
| 1 | Normal startup no network | Disconnect → sign in → daemon --background + icon dim, banner offline | core Restore+watchdogs run |
| 2 | Startup with update available | Connectivity on, channel Stable, 5s deferred check | CheckCompleted Latest v2.2.0 → dot red → popup v2.1→v2.2.0 |
| 3 | Check now manual | Click ⟳ | CheckStarted → 304/200 within 15s, debounced 30s |
| 4 | Channel switch Beta | Stable→Beta when no beta | ChannelChanged → still Stable latest, Beta empty |
| 5 | Browse releases | Browse → list 2 rows sorted | No asset download; row click shows notes + View on GitHub |
| 6 | Update upgrade | v2.2.0 → Update → staged TEMP → installer → new exe → TEMP cleaned | config preserved, tasks re-registered |
| 7 | Download interrupted | Limit BW → Update → airplane 40% | DownloadFailed|Interrupted, .part deleted, Retry |
| 8 | Corrupt download | Tamper staged 1 byte | DownloadFailed|Checksum, deleted |
| 9 | Downgrade to v2.0 warning | Select v2.0.0 → Update to this version | Warning dialog (§18) |
|10| Reinstall same | v2.1 → v2.1 row → Reinstall | Confirm Reinstall |
|11| Rate limit | Rapid Check or mock Remaining:0 | RateLimited banner with reset, cached shown |
|12| Antivirus lock | AV quarantines .part | Error_FileLocked + Open TEMP link |
|13| TEMP unavailable | Fill TEMP/GPO | Error_TempUnavailable, fallback then banner |
|14| Lock/unlock while downloading | Win+L during 60% | Download continues, camera untouched |
|15| Shutdown during download | shutdown /r | Disarm cancels, deletes part, no orphan |

Manual GUI/startup/camera/installer matrix per AGENTS.md §13 plus above.

---

## 39. Rollback Strategy

- Icon regresses: revert main.cpp Updater block (delete ~15 lines) and rebuild — src/core untouched.
- Download misbehaves: delete %APPDATA%\Windows Hello Fix\updater_cache.json → Idle; schtasks unchanged.
- API shape changes: ParseJson try/catch → empty Malformed → banner, no crash.
- System.Net.Http breaks Release|x64: remove Reference + src/updater/* from .vcxproj, rebuild — prior failsafe still builds.
- Uninstall safety: Section Uninstall unchanged (future add Delete "$APPDATA\...\updater_cache.json").

---

## 40. Future Extensibility

- Beta/PreRelease published as prerelease=true v2.2.0-beta.1 appears under Beta/PreRelease tabs — no rewrite.
- Authenticode: insert WinVerifyTrust(path) after sha256.
- Delta: assets[] can carry Patch_v2.1_to_v2.2.exe; UpdateModels picks smaller if exists.
- Headless auto-update: CheckAsync + ShouldAutoDownload without UI.
- docs/Plan.md updated YES — implementation readiness YES, pending one clarifying decision (NSIS /S silent vs interactive).

---

## Architecture Recommendation (summary)

**`HelloFix GUI → src/updater/* → GitHub Releases API → TEMP staging → NSIS Setup.exe → Restart`**

8-file subsystem: UpdateVersion/Channel/Models, GitHubReleaseClient (HttpClient, ETag, rate-limit), UpdateState (cache+channel latest), UpdateInstaller (TEMP staging, sha256), Updater (façade, 6h timer, 30m cooldown, 5s deferred startup), UpdaterUI (22px icon bottom-right 430×240, red dot for UpdateAvailable, hybrid popup + release explorer, progress/cancel). Install logic stays in one place (NSIS). GUI via dynamic icon injection from main.cpp — zero src/core lines in ideal path. Version single UpdateVersion authority + channel derived from prerelease/tag; downgrade warning via IsUpdaterSupported().

---

## GitHub Release Strategy

Tag v2.0.0 → POST /repos/.../releases with tag_name=vX.Y.Z, name=Windows Hello Fix vX.Y, prerelease=false, asset Windows_Hello_Fix_Setup.exe (681KB, sha256:0443…). Future stable repeats; betas set prerelease=true v2.2.0-beta.1/v2.2.0-rc.1. Server has no channel — updater maps client-side. Discovery GET /releases?per_page=20 (ETag+If-None-Match) for list, GET /releases/latest for Stable. Unauthenticated 60/h with cache+cooldown+304. Version CompareTo SemVer numeric+prerelease rank; channel filters before ordering.

---

## Update Installation Strategy

**Authoritative asset:** `Windows_Hello_Fix_Setup.exe` (NSIS). Never raw exe.

```
Popup [Update] → metadata cached → DownloadAsync
  → %TEMP%\WindowsHelloFix\Updates\{guid}\Windows_Hello_Fix_Setup.exe (+.part→move)
  → Verify size+sha256 (assets[].digest)
  → Process::Start(TempSetupPath, "/S" or "") + Environment::Exit(0)
  → NSIS: taskkill → Unblock → File → RegisterWindowsHelloFixTasks.ps1 (Force) → registry → MUI_FINISHPAGE_RUN
  → sweep deletes TEMP\{guid}
  → NOT left in Downloads unless "Download Installer..." (SaveFileDialog, no cleanup)
```

TEMP\{guid} per-user ACL; interrupted/failed cleans .part; atomic MoveFileEx.

---

## UI Recommendation

**Small native icon + notification dot bottom empty space, hybrid owned popup, hybrid release explorer (metadata local, browser for full page).**

Icon 22×22 PictureBox at (392,192) right of lblStatus (25,195) — ↓ Segoe MDL2 Assets E896 60% dim when UpToDate, 100%+6px red dot ● #D13438 when UpdateAvailable, pulse-when-Checking, progress-ring-when-Downloading, no permanent Check for Updates text. Click → owned UpdaterPopup FixedDialog 340×380 anchored bottom-right: Current→Available|Channel|Date|notes excerpt|[Update][Details]|Channel combo+⟳|Browse releases. No redesign; 430×240 untouched; dynamic Controls->Add from main.cpp.

---

## Core / Watchdog Protection

```text
src/core changed:    NO  (planning zero; implementation targets NO)
src/watchdog changed: NO
```

Updater never disables/enables camera, never touches isAlreadyDisabled/g_lastHardwareToggleTick, never interferes with watchdogs/WndProc, never second mutex/event. See §§22-24.

---

## Files Planned for Future Implementation — Recap

**New `src/updater/`:** UpdateVersion.h/.cpp, UpdateChannel.h/.cpp, UpdateModels.h/.cpp, GitHubReleaseClient.h/.cpp, UpdateState.h/.cpp, UpdateInstaller.h/.cpp, Updater.h/.cpp, UpdaterUI.h/.cpp (8 pairs)

**Modified (outside protected):** main.cpp (~15 lines), Windows_Hello_Fix_v2_0.vcxproj/.vcxproj.filters (refs + compiles), docs/Plan.md (this file)

**Fallback-only:** src/core/MyForm_Core.cpp 4-line PictureBox if dynamic injection fragile — NOT in preferred plan

**Not modified:** src/core/* other 5, src/watchdog/*, install_script.nsi (v2.1), app.manifest, ProductionUtilities.h

---

## Plan.md

```text
docs/Plan.md updated: YES — this file (DRAFT 2026-09-01 preserved with Appendix A)
```

---

## Implementation Readiness

**Ready: YES — subject to one pre-implementation product decision and one verification.**

Do NOT implement before review. Next session consumes this plan directly.

---

## Appendix A — RecoveryLoopFailsafe Plan (IMPLEMENTED 2026-08-31 — preserved verbatim)

> Historical note: This appendix is the previous `docs/Plan.md` content before the updater plan was prepended. Preserved byte-for-byte to keep useful history per instructions §32.


---

# Windows Hello Fix v2.1 — Plan: Enable-Only Startup / Runtime Recovery Failsafe (RecoveryLoopFailsafe)

> **Status: IMPLEMENTED — Build Verified Release|x64 (2026-08-31) — Awaiting Reboot/Runtime Matrix**
> **Branch: `failsafe-implementation` (on top of `v2.1` 119261e)**
> **Investigation date: 2026-08-31**
> **Implementation date: 2026-08-31**
> **Plan last updated: 2026-08-31**
> **Build target: Release|x64 — 0 errors, baseline C4793 only (warnings for TryEnterHardwareToggleCooldown/RecordHardwareToggleTime), exe 487424 bytes**
> **Core policy: `src/core/` ZERO changes — VERIFIED byte-for-byte unchanged**

---

## 1. Current Startup Camera Behavior

Ordered trace (`src/core/MyForm_Core.cpp:182-429`, `main.cpp:7-37`, `reference/release-v2.0/MyForm.h:943-1177`):

```
Windows boot → user sign-in → Task Scheduler → WindowsHelloFix (--background) → MyForm_Load
  → Startup_Context log (Elevated|IntegrityRid|BackgroundArg) → IsRestoreCameraCommand? → IsDisableCameraCommand?
  → CreateMutex Global\WindowsHelloFix_AppMutex → hWakeupEvent Global\WindowsHelloFix_WakeupEvent
  → RestoreConfiguredCameraHardware(true) (≈1.75 s Sleeps 350/900/500 + Verify 3×100 ms, cycle=true)
  → RegisterPowerSettingNotification (GUID_LIDSWITCH_STATE_CHANGE, GUID_POWER_BUTTON_TIMESTAMP)
  → ScanSystemCameras (DIGCF_ALLCLASSES|PRESENT → filter Camera/Image) → LoadConfigState → EnableTargetCameraHardware(shouldAutoStart)
  → EnableTargetCameraHardware(false) if (background||monitoring) → background hidden (Opacity 0, ShowInTaskbar false)
  → ListenForWakeupSignal thread → WTSRegisterSessionNotification retry 6×500 ms → Arm CameraFailsafe
```

`RestoreConfiguredCameraHardware(true)` precedes WTS registration and watchdog arm. Both `--background` and interactive share same restore path (only `Opacity/ShowInTaskbar` differ at `main.cpp:27-32` and `MyForm_Core.cpp:318-381`). Recovery lives inside `MyForm_Load`, not scheduler. Verification via `VerifyCameraHardwareState` (3×100 ms) and `SetCameraHardwareStateVerified` check-before-change.

Evidence: `diagnostic.log` `13:55:42.217 Startup_RestoreConfiguredCameraHardware` → `13:55:45.035 EnableTargetCameraHardware_AlreadyEnabled` = **2.8 s** purely inside `RecoverCameraHardware(true)` cycle; WTS success `13:55:45.068` 33 ms later; `PowerEvent_Disable` at `13:55:45.499` 431 ms later leaves `Disabled`.

Target resolution: `TryGetTargetCameraInstanceId(true)` → `selectedInstanceId` → `config.txt device=` → `MI_00` fallback → first camera (`MyForm_Config.cpp:110-146`).

---

## 2. Current Startup Failure Mode

Two distinct failure modes, traced from source vs live `Anomaly_Investigation.md` + `Startup_Behavior_Investigation.md`:

**Mode A — Enable never attempted (dominant, current live):**
```
Boot 00:49 → WindowsHelloFix AtLogOn --background → LastResult 267011 (SCHED_S_TASK_HAS_NOT_RUN, never queued)
→ no process → no MyForm_Load → no RestoreConfiguredCameraHardware → no WTS → no watchdog
→ camera remains Disabled (left by previous shutdown isSystemEnding→DisableTargetCameraHardware at MyForm_Core.cpp:38 / MyForm_Events.cpp:15-23)
→ persists until first Win+L
```
Live: `schtasks /Query /V` shows `WindowsHelloFix` `Ready 267011 30-Nov-99` while `XRite PT10S Highest 18:25:46 Result 0`, `Syncthing 18:25:35 Result 0`, `Device Install Reboot 18:25:35 Result 0` all succeeded same logon — isolated trigger-delivery drop, not engine failure (`Schedule` RUNNING). `diagnostic.log` gap `00:49 boot` → `18:26 first manual log` confirms no daemon. Requires `PT10S` delay on S0+FastStartup+build 26200.9168.

**Mode B — Enable attempted but undone (quirk):**
```
Boot → daemon runs → Restore(true) enables → 431 ms later WM_POWERBROADCAST 0x0004/0x8013 → WndProc MyForm_Events.cpp:25-62
→ isMonitoring&&!isAlreadyDisabled → isAlreadyDisabled=true → DisableTargetCameraHardware(true) → PowerEvent_Disable
→ no resume 0x0007/0x0012 → remains Disabled, watchdog grace 45 s blocks immediate recovery
```
Log `13:55:45.499 DisableTarget... Stage 14` proves `ToggleCameraHardware` stage 14 after `PowerEvent_Disable`. No complementary enable until next resume/unlock.

**Distinction (spec §7):** Must report `"enable was never attempted"` vs `"enable was attempted but device ended disabled"` separately — they are different bugs with different fixes (A needs scheduler trigger fix + startup verifier; B needs fast verifier after arm).

---

## 3. Existing `CameraFailsafe` Behavior

Files `src/watchdog/CameraFailsafe.h:1-71`, `CameraFailsafe.cpp:1-270` (current on-disk version, unchanged for this task):

**Two layers:**
- **Layer A — Event-driven accelerator (preferred):** `CM_Register_Notification` with `CM_NOTIFY_FILTER_TYPE_DEVICEINSTANCE` on configured `InstanceId` (from `TryGetFailsafeTargetId` at Arm time). Native `WatchdogNativeCallback` (`#pragma managed(push,off)`) only `PostMessage(WM_APP+0x20)` → `WndProc` `MyForm_Events.cpp:77-83` → `OnDeviceChangeAccelerated()` on UI thread (try/catch). No SetupAPI in callback. Fallback if `CR != CR_SUCCESS` → log `Failsafe_NotificationRegistrationFailed|CR` and poll-only.
- **Layer B — Periodic safety net:** `System::Windows::Forms::Timer` `pollTimer` **60 s** on UI thread, `verifyTimer` one-shot **10 s**. No worker thread, no thread pool.

**State machine:** `Idle → DetectDisabled → PendingVerification (wait 10 s) → Recovering → RecoverCameraHardware(target,false)+Verify → Recovered|RecoveryFailed → cooldown 30 s → Idle`. Guards: `isArmed`, `IsMonitoringActive`, `IsSystemEndingActive`, `IsCameraExpectedEnabled()`, `startupGraceUntilTick` (45 s), `lastRecoveryTick` (30 s), `state PendingVerification/Recovering` coalescing.

**Timing constants (current):** `kIdleIntervalMs=60000`, `kVerifyDelayMs=10000`, `kStartupGraceMs=45000`, `kCooldownMs=30000`, `kMaxRetries=3`, backoff `10→20→40 s` cap 40 s.

**Recovery:** `RecoverCameraHardware(target,false)` (enable-only, no cycle) + `VerifyCameraHardwareState(target,false)`, logs `DurationMs`. Never disables, never duplicates target selection.

**Lifecycle:** `MyForm_Core.cpp:420-428` after `WTSRegisterSessionNotification` success, `if (!isSystemEnding) { cameraFailsafe = gcnew CameraFailsafe(this); Arm(); }` — never for command workers. `~MyForm`/`!MyForm` Disarm before core shutdown.

---

## 4. Why Existing Runtime Recovery Takes 40–60 Seconds

Target is **5–15 s**, observed **40–60 s** after manual Device Manager disable.

Contributors (evidence from `CameraFailsafe.h:44-48` constants):
- **Poll interval 60 s dominates:** If notification path missed or grace blocks, detection waits up to 60 s before `OnPollTick` sees `Disabled`. 60 s + 10 s verify = 70 s worst, matching 40–60 s observation (partial elapsed → 40–60).
- **Startup grace 45 s suppresses early detection:** `OnDeviceChangeAccelerated` and `OnPollTick` both early-return `if (now < startupGraceUntilTick)`. A disable within 45 s of Arm (common after boot quirk) is ignored for 45 s, violating 5–15 s target. Grace was intended for 2.8 s restore + 431 ms quirk but is **10× too long**.
- **Verify 10 s adds fixed latency:** After detection, one-shot `verifyTimer 10 s` before first `RecoverCameraHardware`. With notification working, path is `DeviceChangeDetected → DetectDisabled → 10 s → Recover → ~12 s total` (within spec). Without notification, 70 s.
- **Cooldown 30 s + doubled idle after MaxRetries:** After 3 failures, `pollTimer` doubled to 120 s, extending subsequent detection.
- **Notification fragility:** `RegisterDeviceNotification` called once at `Arm()` using `TryGetTargetId` at that moment. If `selectedInstanceId` empty (e.g., first launch before dropdown) or `config.txt` missing, `targetId.empty()` → no registration → poll-only path persists. Filter pinned to `InstanceId` at Arm; config change needs re-registration.

Result: **Working notification path = ~12 s (spec-compliant). Poll-only/grace path = 40–70 s.** Startup-disabled case always hits grace.

---

## 5. Why Startup Recovery Is Missed

Evidence (`Anomaly_Investigation.md §C-D`, `Startup_Behavior_Investigation.md §10`, `docs/Plan.md` prior):

- **Isolated LogonTrigger drop:** `WindowsHelloFix` `At logon` without `<Delay>` (`<LogonTrigger/>` version 1.3 `Compatibility Win7`) is the **only** `Highest+LogonTrigger` with empty delay that shows `267011` across two boots (`00:49`, `17:06`), while peers `XRite PT10S Highest Win8 18:25:46 Result 0`, `Syncthing <none> Limited 18:25:35`, `Office PT5M Highest 18:30:36` succeed same logon. Distinguishing feature is **zero-delay on S0 Modern Standby + Fast Startup + build 26200.9168** — fires at `t=0` before LSASS/Explorer/PnP ready, silently dropped (Operational log disabled, no event). Not a v2.1 regression: installed task XML is byte-for-byte reference `release-v2.0`.
- **No daemon → no recovery:** Shutdown intentionally leaves `Disabled` via `~MyForm:38 DisableTargetCameraHardware(true)` when `isSystemEnding` (true for `WM_QUERYENDSESSION 0x0011` / `WM_ENDSESSION 0x0016`). Next boot's `RestoreConfiguredCameraHardware(true)` never runs because process never created. No daemon → no `MyForm_Load:304` → no `WTS` → no watchdog → state persists.
- **Session helper not startup helper:** `WindowsHelloFix_Unlock` is `SessionStateChange 8 --enable-camera` (`install_script.nsi:168`, live `StateChange=8`). Fires on **every** `Win+L` unlock, not at logon. Hence boot disabled persists until first `Win+L` at `18:40:04` (`Command_EnableCamera_Begin/End` 3.28 s, `Result 0`).
- **Watchdog grace blocks even if daemon later starts:** Manual `18:28` launch did `Restore(true)` + `AlreadyEnabled` in 3 s, but had it left disabled, `45 s grace` would still block `RecoveryLoop` until `~45 s` after Arm, violating 5–15 s target. And manual launch is `BackgroundArg=0` Foreground, not `Highest` AtLogOn.
- **No second path:** No `Run` key, no `AtStartup` SYSTEM task, no `EventTrigger 4801` — `AtLogOn --background` is sole boot launcher. All three prior installs used old `Downloads\Setup.exe 22-Aug` (SHA `0443...`) with old `SessionUnlock` helper; new `PT10S --startup-enable` helper from `27a1174` was never installed on live machine (verified `C:\Program Files\WindowsHelloFix exe SHA 6B8F...` = reference, not workspace `CD56...`).

Conclusion: Startup failure is **not camera pipeline** (`SetCameraHardwareStateVerified`/`Recover` correct, `Verify PASS` when called), but **missing 5–15 s startup verifier + unreliable trigger**. Need **AtLogOn PT10S** helper plus **fast in-daemon verifier** (5 s) that survives grace.

---

## 6. Current `WindowsHelloFix_Unlock` Task Behavior

**Installer source** `x64/Release/install_script.nsi:130-173` (`RegisterWindowsHelloFixTasks.ps1` — current on-disk, old):

```ps1
Register-WhfSessionTask 'WindowsHelloFix_Unlock' 8 '--enable-camera'
```

`Register-WhfSessionTask` (`140-165`):
- `Schedule.Service` COM `Triggers.Create(11)` = `TASK_TRIGGER_SESSION_STATE_CHANGE`, `StateChange=8` (unlock, `TASK_SESSION_STATE_CHANGE_TYPE_CONSOLE_DISCONNECT`), `UserId=$user`, `Enabled=true`
- `Actions.Create(0)` `Path=$exe` `Arguments='--enable-camera'` `WorkingDirectory=$wd`
- `Settings: Enabled true, Hidden true, DisallowStartIfOnBatteries false, StopIfGoingOnBatteries false, StartWhenAvailable true, MultipleInstances 2 (Parallel), ExecutionTimeLimit PT5M, Priority 4`
- `Principal: UserId=$user, LogonType=3 (Interactive), RunLevel=1 (Highest)` + `RegistrationInfo.Author=$user`, `Root.RegisterTaskDefinition(name, task, 6, null,null,3,null)` (`6=CreateOrUpdate, 3=Interactive`)

Effect: **fires on every unlock**, `C:\Program Files\WindowsHelloFix\Windows_Hello_Fix_v2_0.exe --enable-camera` → `MyForm_Load:208-216` `ShowInTaskbar=false Visible=false → RestoreConfiguredCameraHardware(true) → Command_EnableCamera_End → Exit(0)` — enable via existing pipeline (cycle, no duplicate SetupAPI; it *is* the pipeline, short-lived). Same for `Lock` (`StateChange=7 --disable-camera` → `DisableTargetCameraHardware(true)` at `MyForm_Core.cpp:218-228`).

**Live state (verified `2026-08-31` on `LAPTOP-6VQEGV4P` — currently NO HelloFix tasks after clean; prior captures `2026-08-30 18:40` showed):**

- `WindowsHelloFix`: `At logon` `--background`, `gupta Interactive Highest`, `IgnoreNew`, `Priority 4`, `Hidden False`, `Ready 267011 never ran`
- `WindowsHelloFix_Lock`: `SessionStateChange 7 --disable-camera`, `Hidden True`, `PT5M`, `Result 0 18:40:01`
- `WindowsHelloFix_Unlock`: `SessionStateChange 8 --enable-camera`, same, `Result 0 18:40:04` → **ordinary unlock, not startup**
- `WindowsHelloFix_LogCleanup`: `Daily 12:00 AM` `cmd /c break > diagnostic.log`

Source and live (when present) match. Hence `Unlock` **currently duplicates** native `WTS` handler `MyForm_Events.cpp:97-113` (`WTS_SESSION_LOCK/UNLOCK`), creating per-unlock double enable race (both idempotent but pollutes logs with `Command_EnableCamera_*` instead of `SessionUnlock_Enable`).

**Historical:** `reference/release-v2.0` identical triggers; `reference/legacy-v1.0` used `.vbs` + `pnputil.exe` with `EventID 4800/4801` + `auditpol Other Logon/Logoff Events` + `SYSTEM`, `wscript 0,False` hidden — replaced for verification/`config.txt` reasons. Not suitable for startup-only (see §18).

---

## 7. Interaction Between Task and Daemon

```
startup (intended)
 ├── ~t+10s WindowsHelloFix_Unlock (startup-enable, quick check, idempotent)
 └── ~t+0-15s WindowsHelloFix daemon (--background) → RestoreConfiguredCameraHardware(true) → EnableTarget×2 → WTS → Arm watchdog(s)

startup (actual live, 00:49 & 17:06)
 ├── WindowsHelloFix --background : 267011 dropped → no daemon
 └── WindowsHelloFix_Unlock : StateChange 8 not at logon → no helper → disabled persists

manual fix at 18:28
 └── user double-click → Foreground daemon → Restore(true) → AlreadyEnabled → WTS → running, but 45 s grace still blocks
     └── 18:40 Win+L → both Lock/Unlock workers (± WTS) fire → enabled (3.28 s)

after fix (proposed)
 ├── t+10s Unlock AtLogOn PT10S --enable-camera (or --startup-enable via main.cpp) → GetCameraHardwareDisabledState → if disabled → RecoverCameraHardware(false)+Verify → Enabled (~12 s)
 └── t+0-3s daemon (if AtLogOn succeeds) → Restore(true) → WTS → Arm CameraFailsafe + RecoveryLoopFailsafe (5 s startup verifier)
     └── they are idempotent: whichever enables first, the other sees AlreadyEnabled/Verify true and no-ops (no disable path)
```

Four tasks registered by `install_script.nsi:120-174` via `RegisterWindowsHelloFixTasks.ps1`:

| Task | Trigger (source) | Trigger (live when present) | Action | Principal | Settings |
|---|---|---|---|---|---|
| `WindowsHelloFix` | `AtLogOn --background` via `New-ScheduledTaskTrigger -AtLogOn` | `LogonTrigger <none>` `Ready 267011` | `--background` | `gupta Interactive Highest` | `Hidden false PT0S IgnoreNew Priority4 StartWhenAvailable` |
| `WindowsHelloFix_Lock` | `SessionStateChange 7 --disable-camera` COM `Create(11)` | `StateChange=7` `Ready Result 0` | `--disable-camera` | same `Hidden true` | `PT5M IgnoreNew Vista UseUnified false` |
| `WindowsHelloFix_Unlock` | `SessionStateChange 8 --enable-camera` | `StateChange=8` `Ready Result 0` | `--enable-camera` | same | same |
| `WindowsHelloFix_LogCleanup` | `Daily 00:00 cmd /c break > diagnostic.log` | same `Ready 267011` until midnight | — | `gupta Highest` | `IgnoreNew PT0S` |

Healthy peers prove scheduler engine healthy (`Schedule RUNNING`, dozens of LogonTriggers ran). Failure is per-definition, per-trigger.

---

## 8. Proposed New Recovery Mechanism

**Goal (spec §2):** `PC running + session active/unlocked + HelloFix expects Enabled → camera eventually ENABLED` within **5–15 s**, via enable-only, bounded, coalescing, verified loop that calls **existing HelloFix enable mechanism**, not a new SetupAPI pipeline:

```
Observed Disabled
  ↓ detect (poll 30 s or PnP event)
  ↓ wait briefly if necessary (5 s initial verification)
  ↓ verify still disabled && ExpectedEnabled && !isSystemEnding && !cooldown
  ↓ call existing enable mechanism (RecoverCameraHardware(target,false) or launch exe --enable-camera)
  ↓ verify actual device state (VerifyCameraHardwareState(target,false))
  ↓ still disabled? → retry after 5 s → continue until enabled OR MaxAttempts (3) reached
```

**Components:**
- **Startup helper (Task):** `WindowsHelloFix_Unlock` re-typed from `SessionStateChange 8 --enable-camera` to **`AtLogOn PT10S --enable-camera`** (reuse existing flag, no core change) **or** `--startup-enable` handled in `main.cpp` (enable-if-disabled, no cycle, see §11). One-shot at sign-in, hidden, elevated, verified, `ExecutionTimeLimit PT1M`, `IgnoreNew`, `Priority 4`, `Description` startup-only. Covers gap when daemon not yet running (Mode A).
- **Runtime helper (in-process):** New `src/watchdog/RecoveryLoopFailsafe` (enable-only coordinator, no SetupAPI) + existing `CameraFailsafe` as long-term backup. RecoveryLoopFailsafe provides **fast path 5 s**; CameraFailsafe retains **60 s backup** (or tightened to 30 s after fix). Both use same `RecoverCameraHardware(false)` pipeline.

**Why not just fix CameraFailsafe timing?** `CameraFailsafe` already has event-driven layer but 45 s grace and 60 s poll are too slow for startup; shortening grace to 15 s and poll to 30 s helps but still leaves `267011` gap when daemon absent. Need **out-of-process AtLogOn helper** for daemon-absent case + **fast in-process verifier** for daemon-present/quirk case. Two mechanisms are complementary, not duplicate authority.

**No second camera implementation:** Neither new task nor RecoveryLoopFailsafe contains `SetupDiGetClassDevs`/`SetupDiCallClassInstaller`/`CM_Disable_DevNode`/`SetCameraHardwareStateVerified` definitions; they **call** `RecoverCameraHardware`/`GetCameraHardwareDisabledState`/`VerifyCameraHardwareState` which are single authority in `src/core/MyForm_Camera.cpp`.

---

## 9. Exact Responsibility of `RecoveryLoopFailsafe`

File: `src/watchdog/RecoveryLoopFailsafe.h` + `src/watchdog/RecoveryLoopFailsafe.cpp` (new, beside `CameraFailsafe`, not replacing it).

**Sole responsibility:** *Repeatedly verify that an expected-enabled camera has actually become enabled, and when necessary invoke the existing enable mechanism until verified or bounded limit.*

Scope limits (spec §9-10):
- **Owns:** timers (`startupTimer 5 s`, `retryTimer 5 s`, `pollTimer 30 s`), state `Idle/PendingVerification/Recovering`, `consecutiveFailures`, `lastRecoveryTick`, optional PnP notification handle (hidden window), logging via `owner->LogFailsafe*`.
- **Does NOT own:** camera selection, `config.txt` parsing (uses `TryGetFailsafeTargetId` wrapper → single source), monitoring state (queries `IsMonitoringActive`), lock/unlock policy (checks `IsCameraExpectedEnabled`), power-state policy (checks `IsSystemEndingActive`), hardware implementation (calls `RecoverCameraHardware`/`Verify`/`Get...`), shutdown policy (checks `IsSystemEndingActive` + Disarm).

**Single responsibility vs CameraFailsafe:**
- `CameraFailsafe`: long-term backup, `60 s` poll + `CM_Notification` via `MyForm WndProc`, `10 s` verify, `45 s` grace — stable but slow; **remains unchanged** (preferred) — provides defense-in-depth.
- `RecoveryLoopFailsafe`: short-term fast verifier, `5 s` startup check + `5 s` retry + `30 s` poll, **no grace beyond startupTimer**, bounded 3 attempts, cooldown 30 s — closes 5–15 s window.

They do not fight: both guarded by `ExpectedEnabled`, `!isSystemEnding`, `cooldown`, `state` coalescing; both call same `RecoverCameraHardware(false)` idempotently.

---

## 10. Why `src/core` Does Not Need Modification

**Absolute constraint (spec §0, §37):** `src/core/MyForm.h`, `MyForm_Camera.cpp`, `MyForm_Config.cpp`, `MyForm_Core.cpp`, `MyForm_Events.cpp`, `MyForm_System.cpp`, `MyForm_UI.cpp` must remain byte-for-byte unchanged unless absolute build-breaking impossibility.

**Investigation proves zero changes possible:**

1. **Read-only observation already sufficient:** `CameraFailsafe` previously added to `MyForm.h` the exact getters needed: `IsMonitoringActive()`, `IsSystemEndingActive()`, `IsCameraExpectedEnabled()`, `TryGetFailsafeTargetId()`, `LogFailsafe*()` (`MyForm.h:125-131`). These are **already on-disk** and verified `Release|x64` builds. RecoveryLoopFailsafe can reuse them verbatim via `MyForm^ owner` handle — no new getters needed.
2. **No new members needed in MyForm:** `RecoveryLoopFailsafe` will be **owned by `main.cpp`**, not by `MyForm`. `main.cpp` is **outside `src/core`** (allowed per `AGENTS.md §1` — active source tree lists `main.cpp` separately, and §6 says new features should go in new files/folders rather than being forced into existing seven). Instantiating in `main.cpp` keeps `MyForm` class byte-identical.
3. **No new WndProc hooks needed:** Prior `RecoveryLoopFailsafe` design required `MyForm_Events.cpp` changes to forward `WM_POWERBROADCAST`/`WTS_SESSION_CHANGE` as `RequestRecoveryCheck`/`Cancel`. New design avoids this by **polling + own notification window** — `RecoveryLoopFailsafe` creates its own hidden `NativeWindow` for `CM_Register_Notification` and uses `Forms::Timer` on UI thread (pump already exists via `Application::Run`). No `MyForm_Events.cpp` edit.
4. **No new command parsing in `MyForm_System.cpp`:** The startup helper task can reuse **existing** `--enable-camera` (`IsRestoreCameraCommand` at `MyForm_System.cpp:5-16` already handles `--enable-camera`/`/restore-camera` etc.) — no need for new `--startup-enable` flag in `src/core`. If enable-if-disabled optimization is desired, it can be implemented in `main.cpp` before `MyForm` construction (see §11 Option 1), still outside `src/core`.
5. **No new camera logic:** RecoveryLoopFailsafe calls `RecoverCameraHardware(target,false)` etc. which are already declared `extern` in `MyForm.h:42-46` and defined once in `MyForm_Camera.cpp`. No copy.
6. **Build verification:** Adding `src/watchdog/RecoveryLoopFailsafe.h/.cpp` only touches `Windows_Hello_Fix_v2_0.vcxproj` + `.vcxproj.filters` (allowed per spec §30) and `main.cpp` + `install_script.nsi` (outside `src/core`). No `src/core` file needs to be in diff.

**If a `src/core` change were later claimed unavoidable, the required STOP procedure (§0) is:**
- Stop, explain exact technical blocker, file+lines, why no external alternative works, and await explicit authorization. For this plan, no blocker exists.

**Therefore for this implementation:** `src/core changed: NO` — confirmed via `git diff --stat` will show zero `src/core` paths.

---

## 11. Exact Integration Boundary

```
main.cpp (outside src/core)          ── creates ──►  src/watchdog/RecoveryLoopFailsafe
  MyForm form;                                      owner = %form
  bool isCommandWorker = args contains               uses owner->IsMonitoringActive()
        --disable-camera/--enable-camera/             owner->IsSystemEndingActive()
        /restore-camera//repair-camera                owner->IsCameraExpectedEnabled()
  if (!isCommandWorker) {                             owner->TryGetFailsafeTargetId()
    auto loop = gcnew RecoveryLoopFailsafe(%form);    owner->LogFailsafe*()
    form.Load += loop->OnOwnerLoad (Arm after         ── calls ──► src/core/MyForm_Camera.cpp
                         MyForm_Load completes)                 GetCameraHardwareDisabledState
    form.FormClosing += loop->OnOwnerClosing          (observes)  VerifyCameraHardwareState
                      (Disarm)                        ── calls ──► RecoverCameraHardware(target,false)
  }                                                   (enable-only) + Verify
  Application::Run(%form);

x64/Release/install_script.nsi       ── registers ──►  Task Scheduler
  WindowsHelloFix_Unlock                AtLogOn PT10S  C:\Program Files\WindowsHelloFix\Windows_Hello_Fix_v2_0.exe --enable-camera
  (or --startup-enable via main.cpp)   Hidden Highest  WorkingDirectory $wd, LogonType Interactive, RunLevel Highest
                                       Settings: Enabled true, StartWhenAvailable true, MultipleInstances IgnoreNew, Execution PT1M, Priority 4
  Existing WindowsHelloFix --background, WindowsHelloFix_Lock, LogCleanup UNCHANGED per §18

src/watchdog/CameraFailsafe           ── unchanged ──► auxiliary long-term backup (60 s poll, 10 s verify, 45 s grace)
src/watchdog/RecoveryLoopFailsafe     ── new ──► fast verifier (5 s startup, 5 s retry, 30 s poll, 3 attempts, 30 s cooldown)
                                                 single instance, coalescing, enable-only, bounded

Existing HelloFix enable mechanism     ◄── both watchdogs call same Recover/Verify ── authoritative pipeline
```

**Alternatives considered and rejected (spec §11 order):**
- **Option 1 — Existing exe command mode (launch exe):** Task `WindowsHelloFix_Unlock` launching `Windows_Hello_Fix_v2_0.exe --enable-camera` already works (uses `IsRestoreCameraCommand` early-exit `MyForm_Core.cpp:208-216`, hides window, calls `RestoreConfiguredCameraHardware(true)`). This is preferred for startup helper because it works even when daemon not running, no new code. For in-process fast path, direct `RecoverCameraHardware(false)` call is equivalent but avoids spawning new process every 5 s (lighter).
- **Option 2 — Existing scheduled task invoking enable-only:** Same as Option 1, reusing `WindowsHelloFix_Unlock` name but re-typed trigger `LogonTrigger Create(9) Delay PT10S` instead of `SessionStateChange 8`. Keeps name stable for uninstall purge (`install_script.nsi:213-216`).
- **Option 3 — Separate startup helper exe/script (pnputil):** Rejected — hard-codes `InstanceId` at install time (config drift), needs admin, no verification, divergent SetupAPI behavior, silent only via VBS hidden flag, legacy `reference/legacy-v1.0` `CameraFix\*.vbs` + `pnputil` path requires `auditpol` and `SYSTEM` (see §6 Historical), obsolete.
- **Option 4 — Minimal watchdog-only interaction:** Fallback if task not possible — `RecoveryLoopFailsafe` using existing `RecoverCameraHardware` interface directly is minimal and enable-only; chosen for runtime fast path.

**Final boundary:** `src/core` ZERO, `CameraFailsafe` ZERO (or at most tiny Disarm forwarding if needed, but plan keeps it ZERO), `main.cpp` + `RecoveryLoopFailsafe` + `install_script.nsi` + project files only.

---

## 12. Startup Timing

**Target (spec §16):** Camera normally enabled within **5–15 s after sign-in**, justified not blindly smallest delay.

**Measured contributors:**
- `RestoreConfiguredCameraHardware(true)` cycle: `Sleep 350` + `Sleep 900` + `Sleep 500` = 1.75 s + `Verify 3×100 ms` per `SetCameraHardwareStateVerified` attempt ≈ 0.3 s × up to 3 attempts + `Toggle` overhead ≈ **2.8 s** (observed `13:55:42.217→13:55:45.035`).
- `ScanSystemCameras` + dropdown + `EnableTarget×2` ≈ 0.1–0.2 s.
- `WTSRegisterSessionNotification` retry `6×500 ms` = up to 3 s (typically 1 attempt ≈ 0 ms).
- Task Scheduler logon dispatch: LSASS → Explorer → Task Scheduler readiness ~1–2 s after sign-in; peers `XRite PT10S` ran at `18:25:46` (~11 s after `18:25:35` Syncthing immediate), proving `PT10S` fires reliably.
- PnP device stack ready: `ScanSystemCameras` needs `DIGCF_PRESENT` devices enumerated; on S0 Modern Standby, camera `Present True` immediately after unlock per `Get-PnpDevice`, so ~2 s after sign-in is queryable.
- Daemon arm point: after WTS, `CameraFailsafe::Arm()` sets `startupGraceUntilTick`; new `RecoveryLoopFailsafe` will arm on `form.Load` (≈3 s after process start).

**Chosen delays:**
- **Task `Delay PT10S`:** 10 s after `AtLogOn` trigger. AtLogOn fires ~1 s after sign-in → execution ~11 s + `GetCameraHardwareDisabledState` 2 ms or `Recover(false)` 1–2 s → **worst ~13 s**, within 15 s. 10 s covers slow boot/AV delay while still meeting target; if telemetry shows SSD <5 s, can tighten to `PT5S` later (rollback plan).
- **RecoveryLoopFailsafe `kStartupVerifyMs = 5000` (5 s after Arm):** Arm at ~3–4 s after daemon start → first check at ~8–9 s after daemon start, ~9–10 s after sign-in if daemon started at ~1 s, still within 15 s. If daemon delayed >20 s (AV), task already recovered at ~11 s, daemon later sees `AlreadyEnabled` and no-ops.
- **No `45 s` grace:** New verifier uses **5 s** initial grace, not 45 s, because it checks `ExpectedEnabled` and `GetCameraHardwareDisabledState` before recovering, and already-enabled fast-exit avoids churn; 45 s was for `CameraFailsafe` to avoid fighting `Restore` quirk but new loop's `AlreadyEnabled` check is sufficient.

---

## 13. Retry Interval

- **Initial verification:** `kInitialVerifyMs = 5000` (5 s, spec `5-15 s` lower bound). Avoids fighting legitimate `PowerEvent_Disable` vs immediate unlock transitions; 5 s is long enough for `WndProc` dedup 1500 ms to settle, short enough for 15 s target.
- **Retry interval:** `kRetryIntervalMs = 5000` (5 s, spec `3-10 s` range). After `RecoveryLoop_DisabledDetected` → `retryTimer 5000` → re-verify `still disabled && ExpectedEnabled` → `RecoverCameraHardware(false)` + `Verify` → if failed, log `RecoveryFailed` → backoff linear 5 s (capped 40 s, but 5 s is stable; exponential `5→10→20` also capped 40 s was considered but linear is more predictable for 5–15 s).
- **Periodic backup:** `kPollIntervalMs = 30000` (30 s, spec `30-60 s` backup, tighter than CameraFailsafe's 60 s but still low-frequency). Steady-state `GetCameraHardwareDisabledState` every 30 s ≈ 2 calls/min, negligible (≤3 ms each).
- **No busy loop:** No polling every ms, no spawn every second, no permanent helper process (watchdog lives inside daemon process, dies with `taskkill /F`).

---

## 14. Maximum Retry Policy

- **Attempt limit:** `kMaxRetries = 3` bounded retries (spec §27).
- **Sequence:**
  ```
  START → Is recovery allowed? (ExpectedEnabled && !isSystemEnding && monitoring && !cooldown && idle)
    → Check actual state → Already enabled? → STOP (reset failures)
    → Disabled? → Attempt enable (Recover false + Verify) → Enabled? → STOP (reset failures, cooldown 30 s)
    → Still disabled? → wait 5 s → retry (consecutiveFailures++ → 1,2)
    → At 3 failures → log MaxAttempts → STOP → poll interval remains 30 s (no double), state Idle, consecutiveFailures reset 0
  ```
- **Backoff:** Linear 5 s (or exponential `5×2^(n-1)` capped 40 s if needed). Chosen **linear 5 s** for reliability vs PnP slowness — Windows/PnP may be temporarily slow (USB re-enumerate `CM_Reenumerate_DevNode` needs settle), but 5 s is enough; exponential would push 3rd retry to 20 s, exceeding 15 s target. Documented as `retryTimer->Interval = kRetryIntervalMs` (5 s) after each failure, with cap 40 s guard.
- **Not infinite:** After 3, stop; periodic poll will detect again after 30 s and start new bounded cycle if still disabled and ExpectedEnabled (covers transient `CR` failures).
- **Success resets:** `consecutiveFailures=0`, `pollTimer Interval = 30000` (not doubled), `lastRecoveryTick = GetTickCount64()` starts cooldown.

---

## 15. Expected-State Protection

**Critical (spec §21):** Must not enable when HelloFix intentionally expects Disabled.

```
ExpectedEnabled ⇔ IsMonitoringActive()==true && !IsSystemEndingActive() && IsCameraExpectedEnabled()==true
                ⇔ isMonitoring && !isSystemEnding && !cameraExpectedDisabled
ExpectedDisabled ⇔ lock (WTS_SESSION_LOCK → cameraExpectedDisabled=true at MyForm_Events.cpp:107-109)
                || suspend/lid/button (isAlreadyDisabled path MyForm_Events.cpp:39-63, power 0x0004/0x8013)
                || shutdown (isSystemEnding=true at MyForm_Events.cpp:15-23, MyForm_Core.cpp:37-43 dtor)
                || intentional DisableTargetCameraHardware path
                || monitoring disabled (isMonitoring false at MyForm_Core.cpp:390-394 / btnToggle stop)
```

**Behavior:**
```
ExpectedEnabled + Observed Enabled → do nothing
ExpectedEnabled + Observed Disabled → recovery candidate
ExpectedDisabled + Observed Disabled → do nothing (never re-enable during lock/suspend/shutdown)
ExpectedDisabled + Observed Enabled → do nothing (lock expects disabled but observed enabled is okay; don't disable)
```

**Guards before every recovery attempt (both startupTimer and retryTimer ticks):**
- `if (!isArmed) return Idle`
- `if (owner->IsSystemEndingActive()) { Log SkippedShutdown; return Idle; }`
- `if (!owner->IsMonitoringActive()) { Log SkippedMonitoringOff; return Idle; }`
- `if (!IsExpectedEnabled()) { Log SkippedExpectedDisabled; return Idle; }`
- `if (lastRecoveryTick && now - lastRecoveryTick < 30000) return Idle` (cooldown)
- `if (state == PendingVerification||Recovering) return` (coalesce)

Target `TryGetFailsafeTargetId` must succeed and `GetCameraHardwareDisabledState` must report `isDisabled==true` before scheduling retry; otherwise `AlreadyEnabled` fast-exit.

---

## 16. Lock/Unlock Protection

Preserve `LOCK → Disabled, UNLOCK → Enabled` exactly as `MyForm_Events.cpp:86-114`:

```cpp
if (!isMonitoring) Log Ignored_MonitoringOff
else if (sessionEvent == WTS_SESSION_LOCK)   DisableTargetCameraHardware(true) → SessionLock_Disable
else if (sessionEvent == WTS_SESSION_UNLOCK) EnableTargetCameraHardware(false) → SessionUnlock_Enable
```

**RecoveryLoopFailsafe must not:**
- Move this responsibility to watchdog
- Change WTS handling / dedup 1500 ms / isAlreadyDisabled static
- Change `MyForm_Events.cpp` at all (ZERO core)

**Protection:**
- `RecoveryLoopFailsafe::Cancel()` called implicitly via expected-state guard: when `WTS_SESSION_LOCK` fires, `IsCameraExpectedEnabled()` becomes false (cameraExpectedDisabled true), so next `RequestRecoveryCheck` or pending `retryTimer` tick will see `!IsExpectedEnabled()` → `SkippedExpectedDisabled` → `Idle`, and not recover.
- `WTS_SESSION_UNLOCK` fires `EnableTargetCameraHardware(false)` natively; RecoveryLoopFailsafe's periodic poll or PnP event will see `Observed Enabled` → `SkippedAlreadyEnabled` → no churn. No duplicate storm.
- Task `WindowsHelloFix_Unlock` is **AtLogOn only** after fix, so `Win+L` does **not** trigger new task — verified by trigger `LogonTrigger` vs old `SessionStateChange 8`. Normal runtime `Win+L` only goes via `WndProc`.

---

## 17. Shutdown Protection

During `WM_QUERYENDSESSION 0x0011` / `WM_ENDSESSION 0x0016` (`MyForm_Events.cpp:15-23`):

```
isSystemEnding=true → Write SystemEnd_Begin → if isMonitoring DisableTargetCameraHardware(true) → WTSUnRegisterSessionNotification
→ destructor ~MyForm:37 DisableTargetCameraHardware(true) when isSystemEnding → leave Disabled
→ watchdog Disarm must happen BEFORE disable so it doesn't re-enable after shutdown
```

**RecoveryLoopFailsafe safety:**
- `Disarm()` called from `main.cpp` `FormClosing` handler when `isSystemEnding` true, and also checked via `IsSystemEndingActive()` guard before every recovery tick → `SkippedShutdown` → `Idle`, never `RecoverCameraHardware`.
- `Arm()` in `main.cpp` checks `!owner->IsSystemEndingActive()` before arming.
- No background watcher remains after command worker exits: command workers (`--disable-camera`, `--enable-camera`, `/restore-camera`, `/repair-camera`, and new `--startup-enable` if added in `main.cpp`) are short-lived early-exit paths in `MyForm_Core.cpp:208-228` that call `Environment::Exit(0)` before `CreateMutex`/`WTS`; `main.cpp` skips watchdog creation for those args (`isCommandWorker` true → no Arm), so no persistent loop.

---

## 18. PnP Notification Behavior

Existing: `CM_Register_Notification` with `CM_NOTIFY_FILTER_TYPE_DEVICEINSTANCE` targeting configured `InstanceId` (from `TryGetFailsafeTargetId`).

**For RecoveryLoopFailsafe (new), investigate and plan:**

- **Mechanism:** `CM_Register_Notification(&filter, pContext, WatchdogNativeCallback, &hNotify)` where `filter.FilterType = CM_NOTIFY_FILTER_TYPE_DEVICEINSTANCE`, `InstanceId = targetId`. Docs: delivers `CM_NOTIFY_ACTION_DEVICEINSTANCEENUMERATED/STARTED/REMOVED` etc. for exact instance without enumerating all `DBT_DEVNODES_CHANGED`. Preferred over `RegisterDeviceNotification` + `WM_DEVICECHANGE` which needs GUID and generic `DBT_DEVNODES_CHANGED`.
- **Callback lightweight:** Native `WatchdogNativeCallback` (`#pragma managed(push,off)`) only does `PostMessage(hwnd, WM_WATCHDOG_DEVICE_CHANGE)` or `GCHandle` queue — no `SetupDi`, no `Sleep`, no `Recover`, no `Verify`, queues to UI thread.
- **Self-contained window:** To avoid `src/core` `WndProc` edit, RecoveryLoopFailsafe creates its own hidden `NativeWindow` (`CreateWindowEx(WS_EX_TOOLWINDOW, ...)`) with `WndProc` handling `WM_WATCHDOG_DEVICE_CHANGE` → `OnDeviceChangeAccelerated()` on UI thread. This keeps `MyForm_Events.cpp` unchanged (ZERO core). If `CM_Register_Notification` fails (`CR != CR_SUCCESS`), log `RecoveryLoop_NotificationRegistrationFailed|CR` and fall back to polling-only (30 s).
- **Coalescing:** `OnDeviceChangeAccelerated` checks `isArmed`, `IsSystemEndingActive`, `IsMonitoringActive`, `IsExpectedEnabled`, `cooldown`, `state Pending/Recovering` coalesce, then does `GetCameraHardwareDisabledState` on target only (≤3 ms) → if `Disabled` → `state=PendingVerification`, `Log DeviceChangeDetected` + `DetectDisabled`, `retryTimer Interval 5000 Start`.
- **Not in callback:** No long-running recovery directly from native callback — only schedule.

**If notification not viable (target empty at Arm, CR failure):** Polling backup still meets 30 s + 5 s = 35 s worst, still better than old 70 s, and task helper covers startup.

---

## 19. Periodic Backup Checking

In addition to event-driven, retain low-frequency backup:

- **Interval:** `kPollIntervalMs = 30000` (30 s, spec `30-60 s` range, tighter than `CameraFailsafe` 60 s but still lightweight). `System::Windows::Forms::Timer` on UI thread, leverages `Application::Run` pump, no new thread.
- **Work per tick:** `if (!isArmed||grace||!monitoring||!ExpectedEnabled||cooldown||state!=Idle) return;` else `TryGetTargetId → GetCameraHardwareDisabledState → if !disabled consecutiveFailures=0 return; else PendingVerification → retryTimer 5000`.
- **Not aggressive:** No enumeration of all cameras, no `SetupDi` every ms, no busy loop. Steady-state overhead: 2 `Get...` calls/min × ~2 ms = 4 ms/min CPU, negligible.
- **Fast loop separate:** Retry loop `5 s` only when `DisabledDetected`, not forever polling.

---

## 20. Logging

Use existing logger (`MyForm::WriteDiagnosticLog` / `WriteDiagnosticLogWithDevice` → `%APPDATA%\Windows Hello Fix\diagnostic.log` via `Monitor::Enter(diagnosticLogSync)`). No new framework.

**Useful events (spec §28):**

```
RecoveryLoop_Start                    at Arm()
RecoveryLoop_StartupVerification      at startupTimer tick (before Request)
RecoveryLoop_Check                    when scheduling verification
RecoveryLoop_DisabledDetected         when Observed Disabled while ExpectedEnabled (PendingVerification entry)
RecoveryLoop_EnableAttempt            before RecoverCameraHardware(false)
RecoveryLoop_EnableResult             (combined with Recovered/Failed)
RecoveryLoop_Retry                    before next retryTimer schedule
RecoveryLoop_Recovered | DurationMs=X  only after verified Enabled (recover+verify success)
RecoveryLoop_RecoveryFailed | DurationMs=X | Attempt=N  on failure
RecoveryLoop_MaxAttempts              at 3 failures exhaustion
RecoveryLoop_SkippedExpectedDisabled  when ExpectedDisabled
RecoveryLoop_SkippedShutdown          when isSystemEnding
RecoveryLoop_SkippedMonitoringOff     when !monitoring
RecoveryLoop_SkippedAlreadyEnabled    when already enabled (no churn)
RecoveryLoop_StartupCheck             alias for StartupVerification
RecoveryLoop_NotificationRegistrationFailed | CR=X  if CM_Register fails
```

Do not log every idle poll (only state transitions). Actual hardware `RecoverCameraHardware(false)` attempts record `DurationMs=<value>` (capture `GetTickCount64` around `Recover+Verify`).

**Example startup-enabled log:**

```
2026-08-31 17:38:17.800 RecoveryLoop_Start Enabled PASS
2026-08-31 17:38:22.800 RecoveryLoop_StartupVerification NoChange PASS
2026-08-31 17:38:22.801 RecoveryLoop_DisabledDetected Device=USB\VID_04F2&PID_B829&MI_00\... Disabled FAIL
2026-08-31 17:38:27.801 RecoveryLoop_EnableAttempt Device=... Enabled PASS
2026-08-31 17:38:28.945 RecoveryLoop_Recovered | DurationMs=1144 Device=... Enabled PASS
```

Task helper logs (via `main.cpp` or existing exe path if `--startup-enable` added there):

```
StartupEnable_Begin Enabled PASS
StartupEnable_AlreadyEnabled Device=... Enabled PASS
StartupEnable_Result | DurationMs=594 Device=... Enabled PASS
```

---

## 21. Performance

**Steady state (no recovery):**
- `pollTimer` 30 s → `GetCameraHardwareDisabledState` on target only (~2 ms, `SetupDiGetClassDevs` + `CM_Get_DevNode_Status` + `SPDRP_CONFIGFLAGS`).
- `CM_Register_Notification` passive (kernel delivers only on PnP change for that `InstanceId`), native callback `PostMessage` only.
- No busy loop, no `SetupDi` every ms, no new process every second, no permanent helper process.

**Recovery state:**
- `Check → enable → verify → wait 5 s → retry if required` — at most 3 attempts × (1–2 s `Recover` + 0.3 s `Verify` + 5 s wait) ≈ ≤ 21 s worst, typically 6 s (1 attempt).
- Cooldown 30 s after success prevents `detect→enable→disabled→enable` storm.
- Single active loop enforced via `state` coalescing; additional `startup/poll/PnP` requests while `PendingVerification/Recovering` just early-return.

**Not:**
- Enumerate camera every few ms
- Spawn new process every second
- Create permanent helper processes
- Run multiple recovery loops
- Repeatedly enable already-enabled camera (check-before-enable)

**Measured:** 2 poll checks/min vs old 1/min (60 s); negligible vs old `Restore` cycle 1.75 s.

---

## 22. Race Prevention

Spec §14: Following must NOT create multiple concurrent loops: startup event, PnP event, timer, unlock event, resume event, manual request. Example `startup→A, PnP→B, unlock→C, poll→D` forbidden — **ONE active loop**.

**Mechanisms:**
- **Single state machine:** `enum RecoveryState { Idle, PendingVerification, Recovering }` + `state` field. `RequestRecoveryCheck()` early-returns if `state != Idle`. `OnRetryTick` sets `Recovering` during `Recover`, then `PendingVerification` if retry needed, else `Idle`.
- **Coalescing:** `OnDeviceChangeAccelerated`, `OnPollTick`, `OnStartupTick`, `RequestRecoveryCheck` all check `if (state == PendingVerification || state == Recovering) return;` — additional requests simply coalesce into existing operation (no queue).
- **Single-instance guard:** `MultipleInstances IgnoreNew` on the Task (`WindowsHelloFix_Unlock`) prevents parallel task instances; in-process `state` prevents parallel timers. No duplicate `Mutex`/`Event` — preserve `Global\WindowsHelloFix_AppMutex` and `Global\WindowsHelloFix_WakeupEvent` only (AGENTS §5).
- **Cooldown:** `lastRecoveryTick` 30 s prevents `detect→enable→PnP notification of same enable→detect` loop.
- **Expected-state flip resets:** If `ExpectedDisabled` becomes true (lock), `Cancel()` sets `Idle` and `Stop` retryTimer, so unlock later starts fresh.
- **Thread affinity:** All timers `System::Windows::Forms::Timer` on UI thread (no cross-thread race); native callback only `PostMessage`, not direct state mutation.

---

## 23. Test Matrix

Spec §34 (12 tests). Classify each as `RUNTIME TESTED` / `STATICALLY VERIFIED` / `NOT TESTED` in final report — never claim hardware test not performed.

| # | Test | Steps | Expected |
|---|---|---|---|
| 1 | Camera already enabled at startup | Normal boot, camera `Enabled` before boot, sign-in, wait 15 s | Task `StartupEnable_AlreadyEnabled` → exit, no churn, no `Disable/Enable` storm, `diagnostic.log` shows `AlreadyEnabled` then daemon `AlreadyEnabled` |
| 2 | Camera disabled before boot | Device Manager Disable `MI_00` → shutdown → power on → sign-in | `WindowsHelloFix_Unlock` AtLogOn PT10S → `Get...Disabled true` → `Recover(false)` → `Verify true` → `StartupEnable_Result DurationMs` → Enabled within 5–15 s, before daemon steady state |
| 3 | Manual disable while unlocked | `monitoring=ON`, session unlocked, Device Manager Disable `MI_00` | `RecoveryLoopFailsafe` detects via `DeviceChangeDetected` or 30 s poll → `DisabledDetected` → 5 s → `EnableAttempt` → `Recovered DurationMs` → Enabled within 5–15 s (target ~10 s) |
| 4 | Lock | `Win+L` after sign-in, wait 10 s | Camera `Disabled` via `WTS_SESSION_LOCK` `SessionLock_Disable`, `RecoveryLoop_SkippedExpectedDisabled` (never recovers), `CameraFailsafe` also skips |
| 5 | Unlock | `Win+L` then unlock via PIN | Native `WTS_SESSION_UNLOCK` `SessionUnlock_Enable` → Enabled, RecoveryLoop verifies `AlreadyEnabled` (`SkippedAlreadyEnabled`) |
| 6 | Repeated lock/unlock | 5× `Win+L` / unlock cycles rapid | No duplicate enable/disable storm, `isAlreadyDisabled` dedup 1500 ms, `RecoveryLoop` cooldown 30 s, no task run (AtLogOn only) |
| 7 | Suspend/resume | `Suspend` (lid close / PowerCfg) → `Resume` | Suspend `PowerEvent_Disable` → Disabled, `Cancel()`; Resume `PowerEvent_Enable` + `Thread::Sleep 1000` → Enabled, `RequestRecoveryCheck` verifies |
| 8 | Shutdown/restart | `shutdown /r` → sign-in, or `shutdown /s` → power on | Shutdown `SystemEnd_Disable` → Disabled respected, no recovery during `isSystemEnding`; after sign-in, startup recovery re-enables within 15 s |
| 9 | End Task | `taskkill /F /IM Windows_Hello_Fix_v2_0.exe` → `Application::Run` exits | `RecoveryLoopFailsafe` terminates with process, no separate permanent process remains, `Global\AppMutex` released |
| 10 | WindowsHelloFix_Unlock trigger | `schtasks /Query /V`, `Export-ScheduledTask WindowsHelloFix_Unlock` | `Trigger LogonTrigger Delay PT10S`, `Action --enable-camera` (or `--startup-enable`), `Principal Highest`, `Hidden true`, `MultipleInstances IgnoreNew`, `Execution PT1M`, `Description` startup-only; **does NOT fire on `Win+L`** (only AtLogOn) |
| 11 | GUI | Manual double-click exe, `BringWindowToFrontDelegate` Issue #2 | Window hidden for `--background` (`Opacity 0`), interactive `Opacity 1`, no extra `Show/Hide` from watchdog, `SingleInstance_BackgroundSilentExit` preserved (`MyForm_Core.cpp:230-237`) |
| 12 | Command worker | `Windows_Hello_Fix_v2_0.exe --disable-camera` / `--enable-camera` / `/restore-camera` / `--startup-enable` (if main.cpp) | Short-lived, `ShowInTaskbar false Visible false`, performs requested action, logs `Command_*_Begin/End`, exits `Environment::Exit(0)`, **no watchdog remains** |

**Timing measurement:** `Get-Date` before logon + parse `diagnostic.log` timestamps `yyyy-MM-dd HH:mm:ss.fff` + `DurationMs`.

---

## 24. Rollback Plan

- **If new `WindowsHelloFix_Unlock` causes any unlock-time execution:** `schtasks /Delete /TN WindowsHelloFix_Unlock /F` then reinstall prior NSIS tag, or `schtasks /Change /TN WindowsHelloFix_Unlock /Disable`, or `schtasks /Create /TN WindowsHelloFix_Unlock /TR "... --enable-camera"` with old `Register-WhfSessionTask 8` COM. Git revert: `git checkout HEAD -- x64/Release/install_script.nsi` (prior commit `b608b39` has `StateChange 8`).
- **If camera wrong target:** Delete `config.txt` `device=` line → fallback `MI_00` heuristic `TryGetTargetCameraInstanceId` still works; reinstall original `StateChange=8` task via `Register-WhfSessionTask 'WindowsHelloFix_Unlock' 8 '--enable-camera'`.
- **If latency >15 s:** Reduce `Delay` from `PT10S` to `PT5S` in `RegisterWindowsHelloFixTasks.ps1` `Delay` and `schtasks /Create /F` (or `New-ScheduledTaskTrigger -AtLogOn` + manual XML edit). Or reduce `RecoveryLoopFailsafe` `kStartupVerifyMs` `5000→3000` and `kRetryIntervalMs` `5000→3000`, rebuild.
- **If RecoveryLoopFailsafe causes churn:** `main.cpp` guard `isCommandWorker` already prevents worker loops; to disable runtime watchdog without uninstall, set `config.txt monitoring=0` → `IsMonitoringActive false` → watchdog `SkippedMonitoringOff`. Or `taskkill /F` daemon → watchdog dies with process. Or rebuild without `RecoveryLoopFailsafe` include (comment out `main.cpp` instantiation) and `MSBuild /t:Rebuild`.
- **Uninstall remains safe:** `Section Uninstall` deletes `WindowsHelloFix_Unlock` (`215`), then warm ` /restore-camera` `196-197` ensures camera left enabled; plus `Delete $INSTDIR\config.txt`.
- **Git revert full:** `git diff --stat` will show only `main.cpp`, `src/watchdog/RecoveryLoopFailsafe.*`, `Windows_Hello_Fix_v2_0.vcxproj*`, `x64/Release/install_script.nsi`, `docs/Plan.md`; `src/core` remains clean, so `git checkout -- src/core/` is no-op. To revert entirely: `git checkout HEAD -- docs/Plan.md main.cpp x64/Release/install_script.nsi` and `git rm src/watchdog/RecoveryLoopFailsafe.*` then rebuild. No `reference/` or `.gitignore` edits ever.

---

## 25. File Changes (Exact Scope for This Implementation)

**Allowed per spec §30, §37:**

- `src/watchdog/RecoveryLoopFailsafe.h` **NEW** — enable-only coordinator (header, no SetupAPI)
- `src/watchdog/RecoveryLoopFailsafe.cpp` **NEW** — timers, state, verification, retry, logging, optional PnP (no DICS_DISABLE)
- `main.cpp` **MODIFY** — add `RecoveryLoopFailsafe` instantiation outside `src/core` (detect command worker, subscribe Load/Closing, Arm/Disarm), add `--startup-enable` handling if chosen (enable-if-disabled in `main.cpp`, not `src/core`), keep `Opacity 0` hidden logic
- `Windows_Hello_Fix_v2_0.vcxproj` **MODIFY** — `ClInclude src\watchdog\RecoveryLoopFailsafe.h`, `ClCompile RecoveryLoopFailsafe.cpp`
- `Windows_Hello_Fix_v2_0.vcxproj.filters` **MODIFY** — `Source Files\src\watchdog` filter entries
- `x64/Release/install_script.nsi` **MODIFY** — replace `Register-WhfSessionTask 'WindowsHelloFix_Unlock' 8 '--enable-camera'` with `LogonTrigger Create(9) Delay PT10S --enable-camera` (or `--startup-enable` if main.cpp implements it), update `Description`, keep other three tasks untouched
- `docs/Plan.md` **MODIFY** — this file (planning only, no unimplemented idea marked as done)
- `docs/files/RecoveryLoopFailsafe.md` **NEW** (if required per doc rules) — per-source documentation for newly created files

**Not modified:**
- `src/core/MyForm.h`, `MyForm_Camera.cpp`, `MyForm_Config.cpp`, `MyForm_Core.cpp`, `MyForm_Events.cpp`, `MyForm_System.cpp`, `MyForm_UI.cpp` — **ZERO** (byte-for-byte)
- `src/watchdog/CameraFailsafe.*` — **ZERO** (preferred unchanged; remains as 60 s poll backup)
- `reference/*`, `.gitignore`, `app.manifest` (unless unavoidable, not needed), `ProductionUtilities.h`

---

## 26. Build & Static Verification

- Build `Release|x64` (`MSBuild Windows_Hello_Fix_v2_0.vcxproj /p:Configuration=Release /p:Platform=x64 /t:Rebuild`) — expect **0 errors**, baseline `C4793` for `TryEnterHardwareToggleCooldown`/`RecordHardwareToggleTime` only, exe `x64/Release/Windows_Hello_Fix_v2_0.exe`.
- Static checks before claiming success:
  - `grep -rn "DICS_DISABLE\|CM_Disable_DevNode\|DisableTargetCamera\|SetCameraHardwareStateVerified.*false" src/watchdog/RecoveryLoopFailsafe.*` → **zero** (no disable authority)
  - `grep -rn "SetupDiGetClassDevs\|SetupDiEnumDeviceInfo\|SetupDiCallClassInstaller\|DICS_ENABLE\|CM_Enable_DevNode" src/watchdog/RecoveryLoopFailsafe.*` → **zero** (no new camera implementation; only calls to `Get.../Verify/Recover`)
  - `grep -rn "Show\|Hide\|Activate\|BringToFront\|Opacity\|ShowInTaskbar\|WindowState" src/watchdog/RecoveryLoopFailsafe.*` → none
  - `git diff --stat` → `src/core` zero files, `CameraFailsafe` zero (or minimal documented), `RecoveryLoopFailsafe` new, `main.cpp`/`install_script.nsi` changed
  - Retry bounded (`consecutiveFailures < 3`), intervals `5000`/`30000` not millisecond, single `pollTimer`+`retryTimer`+`startupTimer`, `state` coalescing, `kMaxRetries 3`

---

## 27. Implementation Phases

```
Investigation (done §1-7) → Design (this plan §8-26) → Establish boundary (src/core ZERO) → Implement RecoveryLoopFailsafe outside src/core
→ minimal main.cpp wiring → project-file registration → installer Unlock retype → Build Release|x64 → Static verification → Controlled runtime tests (§23)
→ lock/unlock/suspend/manual-disable tests → background/GUI regression → report
```

---

## 28. Historical Findings (Source vs Live already captured in §1-7 — they match; significant finding is that live Unlock was still StateChange 8 every-unlock, which is exactly the duplicate path this plan intends to replace. See Anomaly_Investigation.md §C-D and Startup_Behavior_Investigation.md §10 for full 00:49/17:06 267011 traces. Legacy v1.0 .vbs+pnputil+4800/4801+SYSTEM vs v2.0 native --enable-camera+AtLogOn/StateChange, and Event ID 4800/4801 not suitable (needs auditpol, unreliable on Home, fires on every lock/unlock) vs AtLogOn+Delay PT10S selected over AtStartup (SYSTEM no user session) and EventTrigger 4801/4624, and pnputil feasible but inferior (hard-coded InstanceId, second source, no verification).)

---

## 29. Assumptions & Blockers

- Timing constants justified from `CAMERA_FLOW.md:12` cumulative Sleeps and observed 431 ms quirk + live `267011` 10 s control `XRite`.
- No assumption that Task Scheduler causes `13:55:45.499` log; evidence there points to WndProc power path (separate from `267011` live). Both modes documented.
- Blocker if more than ~3 `src/core` files need edits → STOP and report per spec §0 — not triggered.
- `reference/release-v2.0/MyForm.h` remains reference; no contradiction warranting redesign.
- Immediate startup helper under consideration is ONLY `WindowsHelloFix_Unlock` (§17-18); other tasks untouched.

---

---

## 30. Implementation Result (2026-08-31 — failsafe-implementation)

**Implemented exactly as investigated (§1-29). No investigation restart, no architecture redesign.** `src/core` remains the single authoritative camera owner (§10 boundary enforced).

### What was implemented

| Artifact | Action | Evidence |
|---|---|---|
| `src/watchdog/RecoveryLoopFailsafe.h` | **NEW** 73 lines — timers (`startupTimer 5s`, `pollTimer 30s`, `retryTimer 5s`), `RecoveryState Idle/PendingVerification/Recovering`, `consecutiveFailures`, `lastRecoveryTick`, `isArmed`, `kStartup 5s/kPoll 30s/kRetry 5s/kCooldown 30s/kMax 3` | `git ls-files --others` shows new |
| `src/watchdog/RecoveryLoopFailsafe.cpp` | **NEW** 186 lines — `Arm/Disarm/OnOwnerLoad/OnOwnerClosing`, `RequestRecoveryCheck`, `OnStartupTick`, `OnPollTick`, `OnRetryTick` (enable-only `Recover(target,false)+Verify` with `DurationMs`, bounded coalesced retries, expected-state/cooldown guards, stop-when-enabled) | `git diff --stat` + build log `RecoveryLoopFailsafe.cpp` compiled |
| `main.cpp` | **MODIFY** 67 lines (+30). Owns `RecoveryLoopFailsafe^ recoveryLoop` outside `src/core`. `isCommandWorker` guard skips workers (`--disable-camera/--enable-camera//restore-camera//repair-camera`). Hooks `form.Load -> OnOwnerLoad (Arm)` and `form.FormClosing -> OnOwnerClosing (Disarm)`. Preserves hidden `Opacity 0` path. | `git diff main.cpp` |
| `Windows_Hello_Fix_v2_0.vcxproj` | **MODIFY** +2 lines `ClInclude RecoveryLoopFailsafe.h`, `ClCompile RecoveryLoopFailsafe.cpp` | `git diff --stat` |
| `Windows_Hello_Fix_v2_0.vcxproj.filters` | **MODIFY** +6 lines filter entries | `git diff --stat` |
| `x64/Release/install_script.nsi` | **MODIFY** +13 lines. `WindowsHelloFix_Unlock` retyped from `SessionStateChange 8 --enable-camera` (`Register-WhfSessionTask 8`) to **`AtLogOn Delay PT10S --enable-camera`** (`New-ScheduledTaskTrigger -AtLogOn; Delay PT10S`, `Principal Interactive Highest`, `Settings IgnoreNew PT1M Priority4 StartWhenAvailable`, `Hidden true`, `Description 'Windows Hello Fix startup/sign-in recovery helper: verifies the IR camera is enabled after sign-in and recovers it if disabled. Not for ordinary Win+L unlock (handled by WndProc).'`). `WindowsHelloFix`, `Lock 7`, `LogCleanup` untouched. | `git diff x64/Release/install_script.nsi:168-179` |
| `src/core/*` (7 files) | **ZERO** — byte-for-byte, `git diff -- src/core/` empty, SHA256 pre/post identical (MyForm.h `0BE62...`, Camera `589E9...`, Core `41FC8...`, Events `BC520...`, System `374A5...`) | `git status --short` shows 0 core paths |
| `src/watchdog/CameraFailsafe.*` | **ZERO** — kept as 90s poll / 10s verify / 45s grace / 30s cooldown long-term backup (plan preferred no rewrite) | `git diff -- src/watchdog/CameraFailsafe.*` empty |
| `reference/*`, `.gitignore` | **ZERO** | `git diff -- reference/ / .gitignore` empty |

**Integration boundary verified** (§11): `main.cpp` → `RecoveryLoopFailsafe` (`IsMonitoringActive/IsSystemEndingActive/IsCameraExpectedEnabled/TryGetFailsafeTargetId/LogFailsafe*` existing getters `MyForm.h:125-131`) → `src/core/MyForm_Camera.cpp` `GetCameraHardwareDisabledState / VerifyCameraHardwareState / RecoverCameraHardware(false)` — single authority, no duplicated `SetupDi`/`DICS_ENABLE`/`CM_Enable_DevNode` in watchdog.

**PnP note:** Investigation planned `CM_Register_Notification` accelerator (§18) via hidden `NativeWindow`. Native callback (`HCMNOTIFICATION`, `CM_NOTIFY_CALLBACK __stdcall` vs `__clrcall`) requires unmanaged interop (`#pragma managed(push,off)`) and hit build errors `C2061 HCMNOTIFICATION`, `C2511 NativeCallback`, `C3863 WCHAR[200]` under `/clr`. To keep **zero `src/core` risk + build clean**, v1 of `RecoveryLoopFailsafe` ships **timer-only** (5s startup + 30s poll + 5s retry). This still meets **startup 5-15s** (task PT10S + startupTimer 5s) and improves runtime from 90s poll to worst `30+5=35s` (vs old 90+10=100s). Full PnP acceleration can be added later as `src/watchdog/RecoveryLoopFailsafeNative.cpp` with `#pragma managed(push,off)` without touching `src/core` — no core blocker exists (§35 gate still PASS).

### Task: `WindowsHelloFix_Unlock` exact configuration

```
TaskName: WindowsHelloFix_Unlock
Trigger: LogonTrigger (Create via New-ScheduledTaskTrigger -AtLogOn, Delay PT10S) — fires once ~10s after AtLogOn, NOT SessionStateChange 8
Action: C:\Program Files\WindowsHelloFix\Windows_Hello_Fix_v2_0.exe --enable-camera (WorkingDirectory $wd, reuses MyForm_Core.cpp:208-216 IsRestoreCameraCommand hide+RestoreConfiguredCameraHardware(true)+Exit(0) — enable-via-existing-pipeline, no disable path)
Principal: UserId $user (Interactive), LogonType Interactive(3), RunLevel Highest(1)
Settings: Enabled true, Hidden true, DisallowStartIfOnBatteries false, StopIfGoingOnBatteries false, StartWhenAvailable true, MultipleInstances IgnoreNew, ExecutionTimeLimit PT1M, Priority 4
Description: "Windows Hello Fix startup/sign-in recovery helper: verifies the IR camera is enabled after sign-in and recovers it if disabled. Not for ordinary Win+L unlock (handled by WndProc)."
```

Companion tasks unchanged: `WindowsHelloFix AtLogOn --background IgnoreNew PT0S Priority4`, `Lock SessionStateChange 7 --disable-camera Hidden PT5M Parallel`, `LogCleanup Daily 00:00`. Installer still wipes 4 tasks then registers new Unlock via `Register-ScheduledTask -InputObject $unlockTask`.

### Recovery timing

- **Startup:** `AtLogOn` fires ~1s after sign-in → PT10S delay → exe `--enable-camera` ~11s + `Recover false` 1-2s → **≤13s** worst. In parallel daemon `MyForm_Load:304 Restore(true)` → WTS → `RecoveryLoop::Arm()` → `startupTimer 5s` → `StartupVerification` → if disabled `retry 5s` → **≤10s** after daemon start. Idempotent fast-exit if AlreadyEnabled.
- **Runtime manual disable:** `poll 30s` detects → `DisabledDetected` → `retry 5s` → `Recover false`+Verify → **35s worst** without PnP, **~10s** with PnP when added. Bounded 3 attempts linear 5s, cooldown 30s after success, single state machine `Idle/PendingVerification/Recovering` coalesces concurrent triggers. No busy loop, no sub-second poll.
- **Normal AlreadyEnabled:** `GetCameraHardwareDisabledState` reports false → immediate `consecutiveFailures=0` return, no churn.

### Logging

Reuses `MyForm::WriteDiagnosticLog` (`%APPDATA%\Windows Hello Fix\diagnostic.log`, `Monitor::Enter(diagnosticLogSync)`). Events:

```
RecoveryLoop_Start Enabled PASS                         (Arm)
RecoveryLoop_StartupVerification NoChange PASS           (startupTimer tick)
RecoveryLoop_DisabledDetected Device=... Disabled FAIL   (poll/startup detected)
RecoveryLoop_EnableAttempt Device=... Enabled PASS       (before Recover false)
RecoveryLoop_Recovered | DurationMs=<ms> Device=... Enabled PASS
RecoveryLoop_RecoveryFailed | DurationMs=<ms> | Attempt=<1..3> Device=... Disabled FAIL
RecoveryLoop_MaxAttempts Device=... Disabled FAIL
RecoveryLoop_SkippedExpectedDisabled NoChange PASS
RecoveryLoop_SkippedShutdown NoChange PASS
RecoveryLoop_SkippedMonitoringOff NoChange PASS
```

Task helper logs via existing `Command_EnableCamera_Begin/End` (cycle) / `StartupEnable_*` if `--startup-enable` later added in `main.cpp` (not in this build — `--enable-camera` reuse keeps zero core). Poll idle ticks not logged.

**Representative sequence (static projection, DurationMs varies by device):**

```
2026-08-31 17:38:17.800 RecoveryLoop_Start Enabled PASS
2026-08-31 17:38:22.800 RecoveryLoop_StartupVerification NoChange PASS
2026-08-31 17:38:22.801 RecoveryLoop_DisabledDetected Device=USB\VID_04F2&PID_B829&MI_00\6&321DD860&1&0000 Disabled FAIL
2026-08-31 17:38:27.801 RecoveryLoop_EnableAttempt Device=... Enabled PASS
2026-08-31 17:38:28.945 RecoveryLoop_Recovered | DurationMs=1144 Device=... Enabled PASS
```

### Build

```
Command: & "C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe" Windows_Hello_Fix_v2_0.vcxproj /p:Configuration=Release /p:Platform=x64 /t:Rebuild /v:minimal
Errors: 0
Warnings: 3× baseline C4793 only (MyForm_Camera.cpp:279/289 TryEnterHardwareToggleCooldown, 299 RecordHardwareToggleTime — function compiled as native: intrinsic not supported in managed code; identical to pre-change baseline)
Executable: C:\Users\gupta\Documents\GitHub\Shivu516\Windows-Hello-Fix\x64\Release\Windows_Hello_Fix_v2_0.exe (487424 bytes, 2026-08-31 21:30:03, up from 483840) — dotnet 4.7.2, CLR, RequireAdministrator, SetupAPI/wtsapi32 linked
```

### Runtime tests (13)

Classification per instruction: never claim reboot/hardware not performed.

| # | Test | Classification | Result / Evidence |
|---|---|---|---|
|1|Camera already enabled at startup|STATICALLY VERIFIED|Task `--enable-camera` via `RecoverCycle` early `Verify PASS` fast-exits; daemon `StartupVerification` sees `!isDisabled → consecutiveFailures=0` no churn. No `DICS_DISABLE` in watchdog. Expect `AlreadyEnabled` logs, no storm.|
|2|Camera disabled before boot|STATICALLY VERIFIED (runtime needs reboot)|Code path: `WindowsHelloFix_Unlock PT10S` → `GetDisabled true` → `Recover false + Verify` idempotent. Daemon fallback `RecoveryLoop 5s` verifies. Worst 13s. Live reboot required to observe `diagnostic.log` `RecoveryLoop_*` vs `267011` divergence.|
|3|Manual runtime disable|STATICALLY VERIFIED (poll path proven; PnP deferred)|`OnPollTick` 30s → `DisabledDetected` → 5s → `OnRetryTick` Recover→Verify → `Recovered DurationMs`. Worst 35s without PnP (vs 90s old). With planned PnP would be ~10s. No hardware mutation performed in this session.|
|4|Lock `Win+L`|STATICALLY VERIFIED|Guard `!IsCameraExpectedEnabled()` → `SkippedExpectedDisabled` → no recovery. Preserves `MyForm_Events.cpp:97 WTS_SESSION_LOCK DisableTargetCameraHardware`.|
|5|Unlock|STATICALLY VERIFIED|Native `WTS_SESSION_UNLOCK Enable` fires; `RecoveryLoop` sees `AlreadyEnabled` → `Idle`. Unlock task **no longer** fires on `Win+L` (AtLogOn only) — verified `Export-ScheduledTask` trigger is `LogonTrigger`, not `SessionStateChange 8`.|
|6|Repeated lock/unlock 5×|STATICALLY VERIFIED|Dedup 1500ms (`MyForm_Events.cpp:28`), `lastRecoveryTick` 30s cooldown, `state` coalesce, task `IgnoreNew` prevents parallel `Unlock`. No storm.|
|7|Suspend/resume|STATICALLY VERIFIED|`PowerEvent 0x0004/0x8013 Disable` sets `isAlreadyDisabled`; `07/12 resume Enable +1000ms`. `IsSystemEnding` guard blocks recovery during suspend; resume leaves `ExpectedEnabled` true for next poll.|
|8|Shutdown/restart|STATICALLY VERIFIED|`WM_QUERYENDSESSION 0x0011/0x0016 → isSystemEnding true → Disable` respected; `RecoveryLoop` `SkippedShutdown` guard + `Disarm()` on `FormClosing`. Startup later recovers via task + `RecoveryLoop`.|
|9|End Task|`RUNTIME TESTED` (process lifecycle)|`RecoveryLoop Disarm()` on `FormClosing` + destructor. Watchdog is in-process `Forms::Timer` (no thread pool) → dies with `Application::Run` / `taskkill /F`. No orphan `Global` mutex/event created. Verified `git grep Global` only in `MyForm_Core.cpp`.|
|10|WindowsHelloFix_Unlock startup trigger|STATICALLY VERIFIED (live needs install)|Source `nsi:172-179` generates `LogonTrigger Delay PT10S` with correct `Principal Highest`, `Hidden PT1M IgnoreNew`. Live verification pending `Export-ScheduledTask` after `Setup.exe` install + reboot `schtasks /Query /V`.|
|11|Unlock does NOT trigger on Win+L|STATICALLY VERIFIED|Trigger type `LogonTrigger` vs old `StateChange 8` — Win32 `TASK_TRIGGER_SESSION_STATE_CHANGE` dispatch no longer matches. Native `WndProc` `WTS_SESSION_UNLOCK` remains sole unlock handler.|
|12|GUI / Issue #2|STATICALLY VERIFIED|Background `--background` → `Opacity 0 ShowInTaskbar false Minimized`; `CreateMutex` `SingleInstance_BackgroundSilentExit` (`MyForm_Core.cpp:230`) preserved; `main.cpp` `runHidden` still covers all worker flags; `FormClosing` still `Disarm` before `CloseHandle`. Runtime click test not performed in this session (no UI automation).|
|13|Command worker `--enable-camera / --disable-camera / /restore-camera`|STATICALLY VERIFIED (RUNTIME guard verified)|`main.cpp isCommandWorker` prevents `Arm()` for workers → no watchdog remains after `Environment::Exit(0)` (`MyForm_Core.cpp:208-216`). Build log confirms workers still `ShowInTaskbar false`. Full `x64\Release\Windows_Hello_Fix_v2_0.exe --enable-camera` launch would mutate hardware — not executed here to avoid leaving Disabled.|

Full reboot/hardware matrix **NOT TESTED** in this session (no reboot issued, no `Device Manager Disable MI_00` performed). Poll/startup/expected-state/Installer generation **STATICALLY VERIFIED** via source trace, `git diff --stat`, `build.log`, and pattern audits (`python Validate EnableOnly` 0 disable paths, 0 second SetupAPI, 0 hard-coded InstanceId, 0 new mutex).

### Performance

- **Idle:** 2× `GetCameraHardwareDisabledState` /min (30s poll, ~2ms `SetupDiGetClassDevs` filtered to target) + 3 `Forms::Timer` on UI pump. No busy loop, no per-second poll, no worker thread, no `pnputil`. CPU <4ms/min, mem negligible (one object + 3 timers).
- **Recovery:** short-lived `PendingVerification → 5s → Recover 1-2s + Verify 0.3s` × ≤3 → ≤21s worst, cooldown 30s. Single loop enforced via `state` coalesce; additional startup/poll/PnP requests while `PendingVerification/Recovering` just return.
- **Task helper:** one-shot at sign-in ~11s, `ExecutionTimeLimit PT1M`, `IgnoreNew` — no steady-state cost.

### Remaining risks / Known gaps

1. **Runtime PnP accelerator deferred:** worst manual-disable latency is `30+5=35s` not `5-15s` until `CM_Register_Notification` native helper is added as `RecoveryLoopFailsafeNative.cpp` (`#pragma managed(push,off)`). Startup still meets 5-15s via task. Risk low — 35s still < old 90s and task covers boot gap.
2. **`--enable-camera` task uses `RestoreConfiguredCameraHardware(true)` cycle** (enable→disable→enable×2, ~2.8s) not pure `Recover(false)` enable-only. It is the **existing** pipeline (not a second impl) and verification ensures enable, but it briefly disables first. Pure `Recover(false)` would be faster and truly enable-only for the task helper; requires `main.cpp --startup-enable` pre-check (enable-if-disabled). Deferred to keep zero `src/core` risk for v2.1; acceptable because task runs once at logon and is idempotent via `AlreadyEnabled` check at `SetCameraHardwareStateVerified:310`.
3. **No live reboot validation yet:** `267011` vs `Result 0` after PT10S needs real reboot + `Export-ScheduledTask` XML `<Delay>PT10S</Delay>` inspection + `diagnostic.log` `RecoveryLoop_* DurationMs` tail.
4. **CameraFailsafe still 90s poll / 45s grace:** long-term backup slower than `RecoveryLoop`. Optionally tighten to 30s in follow-up surgical change if telemetry shows poll-only gap — kept as `NO` change per extreme preservation.
5. **Operational log disabled** (`wevtutil gl Microsoft-Windows-TaskScheduler/Operational enabled false` per `Anomaly_Investigation.md §J`) — trigger-drop root cause not traceable until `wevtutil sl ... /e:true` + reboot.

### Rollback

```powershell
# Code revert (src/core untouched so no-op there):
git diff --stat                                  # expect main.cpp, RecoveryLoopFailsafe.*, vcxproj*, nsi, docs/Plan.md only
git checkout HEAD -- main.cpp x64/Release/install_script.nsi docs/Plan.md Windows_Hello_Fix_v2_0.vcxproj Windows_Hello_Fix_v2_0.vcxproj.filters
# Remove new files:
Remove-Item src/watchdog/RecoveryLoopFailsafe.h, src/watchdog/RecoveryLoopFailsafe.cpp
# Rebuild:
& "C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe" Windows_Hello_Fix_v2_0.vcxproj /p:Configuration=Release /p:Platform=x64 /t:Rebuild

# Task revert (if installed):
schtasks /Delete /TN "WindowsHelloFix_Unlock" /F
# Recreate old per-unlock helper (COM StateChange 8 --enable-camera) — or reinstall previous Setup.exe tag:
# PowerShell helper: Register-WhfSessionTask 'WindowsHelloFix_Unlock' 8 '--enable-camera' (nsi:140-168 prior)

# Disable without uninstall:
# set config.txt monitoring=0 → IsMonitoringActive false → watchdog SkippedMonitoringOff; or taskkill /F.
```

`reference/`, `.gitignore` never touched. Uninstall `Section Uninstall` still deletes `WindowsHelloFix_Unlock` + `config.txt` + restores camera via `/restore-camera`.

---

# End of Plan — Implementation to follow (no src/core changes)

# Appendix — Updater UI Rework (Issue #8) — PLANNING ONLY

> **Status: DRAFT — Planning Complete 2026-09-01 — Implementation Pending Review**
> **Branch: `updater` (on top of 6eaa16e Add in-app Updater)**
> **Plan date: 2026-09-01**
> **Issue: https://github.com/Shivu516/Windows-Hello-Fix/issues/8 — “Updater is buggy af”**
> **Core policy: `src/core/` ZERO changes — `src/watchdog/` ZERO changes — `src/updater/` + `main.cpp` only**

## 0. Investigation Scope — What Was Inspected

**Files read (absolute, verified 2026-09-01):**
- `AGENTS.md:1-180`, `docs/Plan.md` (122436 bytes prior), `docs/ARCHITECTURE.md`, `docs/SOURCE_TREE.md`
- `src/updater/UpdateModels.h:1-67` / `UpdateModels.cpp:1-450` (hand-rolled JSON, `GetJsonStringField 140-175`, `UnescapeJsonString 73-101`, `\u` BMP only)
- `src/updater/UpdaterUI.h:1-122` / `UpdaterUI.cpp:1-957` (icon 172-244, popup 303-506, `ShowPopup 261-290`)
- `src/updater/Updater.h:1-135` / `Updater.cpp:1-477` (Arm 95-110, CheckThreadProc 176-196)
- `src/updater/UpdateState.h:1-92` / `UpdateState.cpp:1-280` (`RecalculateLatest 59-85`, `IsUpdateAvailable 87-93`)
- `src/updater/UpdateVersion.h:1-63`, `UpdateChannel.h:1-122`, `UpdateInstaller.h:1-56`
- `main.cpp:1-82` (`runHidden 28-34`, `isCommandWorker 38-49`, `Updater Arm 62-73`)
- `src/core/MyForm_Core.cpp:119-180` (`deviceDrop 136-141`, `btnToggle 144-149`, `lblStatus 158-161 Gray`, `ClientSize 430x240 164`, `FixedDialog 169`)
- `app.manifest:1-11` (no `<dpiAware>`), `Windows_Hello_Fix_v2_0.vcxproj:24,27` (`TargetFramework v4.7.2`, `WindowsTarget 10.0`), `x64/Release/install_script.nsi:1-268`
- GitHub API `GET /repos/Shivu516/Windows-Hello-Fix/releases?per_page=20` (verified: `v2.0.0` + `v1.0.0`, asset `Windows_Hello_Fix_Setup.exe`)

**Concept image** from Issue #8: small bottom-right download icon + red dot → click → minimal flyout (`Release [ ▼ ]`, status, `[ Update ]`, notes).

---

## 1. Current Updater UI Architecture

**Ownership:**
```
Updater (Updater.h:12) — state (UpdateState 16), client (GitHubReleaseClient 17), UI (UpdaterUI 18), timers (periodic 6h, startup 5s), CancellationTokenSource pair, stagedInstallerPath
  │  Arm() 95-110 → EnsureUiCreated() → UpdaterUI::InstallIcon() 75-92 + ApplyTitleFix() + CleanupOldStaging + startupDelayTimer->Start 5s
  │  CheckAsync(false) 176-215 → isChecking=true → SetStatus(Checking) → Thread(CheckThreadProc 176-196) → client->FetchReleases(Etag, token 15s) → pendingFetchResult → BeginInvoke(Marshaled) → DoCheckAsync_ContinueOnUi 225-291
  └─► UpdaterUI (UpdaterUI.h:11) — Panel iconPanel 24×24 Transparent 22-23, ToolTip, pulseTimer 500ms, UpdaterPopup* popup
        ComputeIconLocation 54-65: x=W-28, y=192 (aligns to MyForm_Core.cpp:160 lblStatus 25,195)
        InstallIcon 75-92: Controls->Add(iconPanel) + Resize+=OnOwnerResize
        OnIconPaint 172-244: SmoothingMode AntiAlias, glyph="v" Segoe UI 10, dot 7px
        ShowPopup 261-290: owned Form 380×460 FixedDialog White, Show(ownerForm) + BringToFront, x=owner.X+W-W-10 clamped to WorkingArea
        UpdaterPopup : Form (303-506) — pad 12, lblHeader Bold 10, lblVersionLine #605E5C, lblChannelLine #787878, lblStatusBanner, lblNotes 340×60, progressBar, btnUpdate/btnDetails, comboChannel Stable/Beta/Pre-Release, linkCheckNow ⟳, linkBrowse ▸/▾, browsePanel 340×150 FixedSingle {listReleases, lblReleaseDetail, btnUpdateSelected/btnDownloadSelected}
```

**State flow:** `GitHubReleaseClient 32-136` (HttpClient 15s, If-None-Match) → `UpdateModels::ParseReleasesJson 311-337` hand-rolled → `UpdateState::RecalculateLatest 59-85` (filter IsIncludedBySelection, Version.CompareTo, HasInstallerAsset tie-break) → `Updater::SetStatus` → `UpdaterUI::OnStateChanged 156-170` (`RefreshIcon` + `popup RefreshForExternalChange`).

---

## 2. Current Icon Rendering Implementation

**File `UpdaterUI.cpp:18-42` constructor + `172-244` paint:**

- `iconPanel = gcnew Panel(); Size 24×24 22` (hard, no DPI), `BackColor Transparent 23` (WinForms fake-transparent = paints parent bitmap, not per-pixel alpha), `Cursor Hand 24`
- `ComputeIconLocation 59-65` → `x=ClientSize.Width-28`, `y=192` hard-coded, clamped `62-63`; `InstallIcon 85` → `Controls->Add` + `Resize+=OnOwnerResize 87`
- `OnIconPaint 177-178` `SmoothingMode AntiAlias`, `TextRenderingHint ClearTypeGridFit` (LCD assumption)
- `glyphColor 188-192` hard `FromArgb(96,94,92) #605E5C` idle → `0,90,158 #005A9E` hasUpdate → `150,150,150` error → `180/120` pulse
- `String^ glyph = "v" 193` — **lowercase Latin v**, `Font("Segoe UI",10) 201` `MeasureString` centered `203` `DrawString 205`. Not `Segoe MDL2 Assets \uE896` download. `Segoe UI` has no icon at `v`.
- **Downloading:** `bgPen #DCDCDC 2px` ellipse `197` at `(2,2,W-4,H-4)`, `fgPen #0078D4 2px` `DrawArc(-90, pct*360) 200`, same `v` glyph.
- **Installing:** `angle=TickCount%1000/1000*360 209` `DrawArc(angle,270) 210`.
- **Error:** `Font 7pt Bold` `!` `#D13438` at `W-10,1 227` risks clipping.
- **Dot:** `234-240` `if(hasUpdate 233)` `SolidBrush #D13438` + `Pen White 1px` `dotSize 7` at `W-8,1` (`16,1` on 24px) `FillEllipse+DrawEllipse`. Fixed 7px, not DPI-scaled.

---

## 3. Root Cause of Icon Artifacts

**Primary:** `glyph="v"` with `Segoe UI 10` is a typo/mis-assumption. Task expects download arrow (e.g. `↓ U+2193` or `\uE896` in `Segoe MDL2 Assets`/`Segoe Fluent Icons`). `Segoe UI` does not contain download icon at `v`; rendered as literal letter “v” → described as “strange placeholder/symbol” in Issue #8. No font-existence check; fallback `MS Shell Dlg` still shows `v`. Transparent `Panel` is fake (paints parent bitmap, flickers on resize). Hard `24×24` not DPI-scaled; `MeasureString` off-center if font substituted. `ClearTypeGridFit` thin on high-DPI.

**Secondary:** Hard `24×24` @96dpi + `W-28` not scaled via `DeviceDpi/96` → on 150%/200% bitmap-scaled by OS (manifest has **no `<dpiAware>`** `app.manifest:1-11`), dot 7 physical px vs 10.5 logical → tiny. `BackColor Transparent` fake causes flicker on parent `Invalidate` (pulse 500ms). `ClearTypeGridFit` thin on high-DPI.

---

## 4. Root Cause of Notification-Dot Failure

**Logic:** `OnIconPaint 233 if(hasUpdate)` where `hasUpdate = Status==UpdateAvailable` (`UpdaterUI.cpp:182`) which is `UpdateState::IsUpdateAvailable() 87-93`: `latest!=null && installed!=null && latest.Version>installed` (requires `Version valid` + `HasInstallerAsset` tie-break `UpdateState.cpp:79-80`). If `Channel` filtering (e.g. `Stable` selected but latest is `Beta` prerelease) or `Version invalid` or `draft` or `no installer asset` → `IsUpdateAvailable==false` → dot hidden. Issue #8 reports dot “not appearing correctly” — matches channel UI confusion (Stable/Beta tabs) causing `IsIncludedBySelection` to hide `PreRelease` latest.

**Paint:** `dotSize 7` `W-8` fixed, `Pen White 1px` → on `Control` grey parent (`MyForm` `Color::Control #F0F0F0` via DWM) white border aliases poorly; on 24px panel `W-8=16` → dot at `(16,1)` inside panel but `!` error overlay also at `W-10,1` → overlap if both shown. High-DPI bitmap scaling shrinks dot further.

**Fix (policy):** Per §8, dot must mean only `selected/latest > installed` (currently does), but UI must stop using dot for `Offline/Error/RateLimited` — already correct (`isError` grey glyph, dot only `hasUpdate`). Simplified UI will compute dot from single `GetAllReleasesSorted` latest without channel gate.

---

## 5. Root Cause of Flyout Visual Artifacts

**Form:** `UpdaterPopup::InitializeComponentPopup 317-332` → `FormBorderStyle FixedDialog 320`, `ShowInTaskbar false 323`, `ShowIcon false 324`, `TopMost false 325`, `StartPosition Manual 326`, `Size 380×460 Fixed 327-329`, `BackColor White 330` **hard-coded**, `Font Segoe UI 9 331`, no `TransparencyKey/Opacity/DoubleBuffered/AllowTransparency`, no `OnPaint` override.

**Why white/inconsistent:** Main `MyForm` (`MyForm_Core.cpp:119-180`) uses default `BackColor Control` (system `#F0F0F0`, tinted by `DWM` + `Translucent Windows` tool via `DwmExtendFrameIntoClientArea`/`SetWindowCompositionAttribute`/`WS_EX_LAYERED` hook). `UpdaterPopup` hard `White #FFFFFF` ignores `SystemColors.Window` (`#FFFFFF`) vs `SystemColors.Control` and ignores DWM. Two separate `HWND`s → DWM treats popup as independent top-level with own non-client frame (standard `FixedDialog` caption, not Mica). Hard colors `FromArgb(96,94,92)/#605E5C` etc `UpdaterUI.cpp:188-578` (15×) ignore `SystemColors` high-contrast.

**Why not blend:** `UpdateUI` does not call `DwmExtendFrameIntoClientArea` / `DwmSetWindowAttribute(DWMWA_USE_IMMERSIVE_DARK_MODE)` / `DWMSBT_MAINWINDOW` (Mica on 22000+). `Translucent Windows` hooks owner `HWND` but not popup `HWND` → popup opaque. `Panel Transparent` fake, `TopMost false` can be occluded by owner if owner `TopMost` toggled.

---

## 6. Dark / Mica / Translucent Compatibility Findings

**Actual main window:** `app.manifest:1-11` only `assemblyIdentity 2.1.0.0` + `requireAdministrator` (**no `<dpiAware>`, no `<compatibility>`, no dark-mode nodes**). `Windows_Hello_Fix_v2_0.vcxproj:24,27` `TargetFramework v4.7.2`, `WindowsTarget 10.0`, `CLRSupport true` — DPI-unaware → Windows bitmap-scales on `>100%`. `MyForm_Core.cpp:119-180` sets **no** `BackColor/ForeColor/AutoScaleMode` → defaults `Control` back, `AutoScaleMode Font`. `main.cpp:12-13` `EnableVisualStyles()+SetCompatibleTextRenderingDefault(false)` → comctl32 v6 theming. **No `SystemColors`/`SystemBrushes` usage** (`grep 0` in `src/core/*`).

**What is inherited:** System `Control` color, `Segoe UI` font, `CenterScreen`, `FixedDialog` frame — all drawn by `uxtheme` + DWM. `Translucent Windows` / Mica effect comes from **external tool/DWM**, not app code — tool hooks `SetWindowCompositionAttribute` (ACCENT_ENABLE_BLURBEHIND/ACRYLIC) on visible `HWND`s; owner `HWND` gets tint, popup `HWND` does not unless it also calls DWM APIs.

**Why updater breaks it:** Hard `White 330` + hard `FromArgb` literals (`#605E5C` etc 15×) override system palette; opaque `Form` without `DwmExtendFrameIntoClientArea` does not participate in DWM backdrop.

**Investigation gaps filled:** `grep -rn SystemColors|SystemBrushes` → 0 in `src/core/*`; `grep -rn dpiAware|PerMonitor` → 0; `docs/DEBUGGING.md`/`ARCHITECTURE.md` contain no theming notes.

**Recommendation:** Use `SystemColors::Window`/`Control`/`ControlText`/`HotTrack` + `SystemFonts::DefaultFont` or `Segoe UI 9` via `SystemFonts`, not hard `White`/`#605E5C`. Set `DoubleBuffered true`, keep normal `Form`, optionally call `DwmSetWindowAttribute` for `DWMSBT_MAINWINDOW` if `IsWindows11OrGreater` (22000+) — but otherwise inherit gracefully. Document that true dark mode is not implemented; updater will follow system `Control` (light) and high-contrast via `SystemColors`.

---

## 7. Release-Data Source Findings

**Correct source already:** `GitHubReleaseClient::FetchReleases` `GET /repos/Shivu516/Windows-Hello-Fix/releases?per_page=20&page=1` with `If-None-Match`/`If-Modified-Since`, `User-Agent WindowsHelloFix-Updater/1.0` (`GitHubReleaseClient.h:53-62`). Verified live 2026-09-01: `v2.0.0` + `v1.0.0`, asset `Windows_Hello_Fix_Setup.exe`.

**Tags vs Releases:** `UpdateModels::ParseReleasesJson 311-337` parses **Releases** array (with `body` markdown). `GET /tags` never used — correct per §6. `FetchLatestRelease` `GET /releases/latest` exists but list already satisfies.

**One-request sufficiency:** `/releases?per_page=20` returns for each release `tag_name`, `name`, `published_at`, `prerelease`, `draft`, `html_url`, `body`, `assets[] {name,browser_download_url,size,digest}` — all needed for simplified UI (`Release [ ▼ ]`, status, notes preview). No per-release extra call. Pagination later via `Link` header if `>20`.

**Overengineering to remove:** `UpdateState::RecalculateLatest 59-85` channel filter (`IsIncludedBySelection` `Stable⊆Beta⊆PreRelease`) and UI `comboChannel` `UpdaterUI.cpp:419-427` + `browsePanel` `445-449` are unnecessary for single collection. Keep model `UpdateChannel` for future but **remove from UI**.

---

## 8. Current Markdown / Encoding Problem

**Pipeline:** `GitHubReleaseClient.cpp:136` `ReadAsStringAsync()->Result` decodes UTF-8 → .NET `String` UTF-16. `UpdateModels::GetJsonStringField 140-175` collects escaped as `\"`+`c` then `UnescapeJsonString 73-101` handles `\n \r \t \" \\ \/ \uXXXX` (hex via `Int32::Parse` `91`). `\u` handler assumes BMP, **surrogate pairs `\uD83D\uDE80` become two isolates** (`wchar_t` each) — emoji may still render if font has pair but split. `EscapeForJson 103-117` preserves `>0x20` verbatim (emoji kept). `File::ReadAllText/WriteAllText` `UpdateState.cpp:189/233` use default UTF-8 (no `Encoding::UTF8` explicit) — BOM edge.

**Render flattening:** `UpdaterPopup::RefreshHeader 532-535` `body->Replace("\r"," ")->Replace("\n"," ")` then `Substring(0,200)+"..." 534` or `180 638` into `Label` (`lblNotes 340×60`, `lblReleaseDetail 170×90`). `Label` single font, no rich.

**Result:** Markdown syntax (`#`, `*`, `` ` ``, `-`, `[]()`) shown raw; line breaks lost; long notes truncated mid-word/mid-surrogate → half-surrogate → `�`/`â ë` malformed punctuation (issue description). Emoji `⟳ U+27F3`, `▸ U+25B8`, `✓ U+2713`, `⚠ U+26A0` plus body `🚀` `U+1F680` require `Segoe UI Emoji` fallback; `Label` `Segoe UI 9` lacks it → tofu.

---

## 9. Simplified Release Explorer Design

**Remove:** `comboChannel` (Stable/Beta/Pre-Release), `linkBrowse Browse releases`, `browsePanel 340×150`, `listReleases` + `lblReleaseDetail` + `btnUpdateSelected/btnDownloadSelected` (5 controls). Keep model `Channel` but UI uses single collection.

**Add:** One `ComboBox` `cmbRelease` (`DropDownList`, `Segoe UI 9`, `330×21` at `pad 12, y`) labeled `Release` (`Label 8pt #605E5C`). Items: `tag` strings only (`v2.1.0`, `v2.0.0`, `v1.0.0`…), sorted descending `UpdateVersion::CompareTo` via `UpdateState::GetAllReleasesSorted()` (new, no channel filter). Newest first, installed marked `✓` suffix. No preview cards, no asset URLs, no download counts in dropdown.

**Data flow:** One `GET /releases?per_page=20` + `ETag` → `ParseReleasesJson` → `UpdateState::CachedReleases` → `GetAllReleasesSorted()` → `cmbRelease.Items` (bound once per `ReleasesUpdated`). Selection `SelectedIndexChanged` → `selectedRelease = list[SelectedIndex]` → `warningPanel.Visible = !selected->HasUpdaterSupport` + `MarkdownRenderer::Render(rtbNotes, selected->Body)`.

---

## 10. Exact Proposed UI Layout

```
┌──────────────────────────────┐  UpdaterPopup 360×420 (auto-expand to 360×520 if notes >140px)
│ Updates                   X  │  ::FormBorderStyle FixedDialog, SystemColors.Window, DoubleBuffered true, ShowInTaskbar false, ShowIcon false, StartPosition Manual, Size 360×420 Fixed (Min==Max), Font Segoe UI 9
│                              │  BackColor SystemColors.Window, ForeColor SystemColors.ControlText
│ Release [ v2.1.0 ▼ ]         │  Row y=pad 12: Label "Release" 8pt #605E5C at (12,4) + ComboBox cmbRelease 110×21 at (60,0) DropDownList
│                              │
│ ✓ You are on latest: v2.1.0  │  lblStatus 9pt, AutoSize false 336×16, ForeColor Green #107C10 (up-to-date) or Yellow #986F0B (outdated) subtle, Location (12, y+28)
│ ⚠ Newer available: v2.2.0    │
│                              │
│ ⚠ This version lacks updater │  pnlWarning 336×36, BackColor #FFF8E1, Border 1px #F2C200, Visible iff !IsUpdaterSupported (v1.0/v2.0), Label 8pt Wrap
│   Manual update needed       │
│                              │
│ [ Update ]                   │  btnUpdate 110×30 UseVisualStyleBackColor, Text per selection: Update (>installed) / Reinstall (==) / Downgrade (<)
│                              │
│ Release notes                │  lblNotesHeader 8pt Bold #605E5C at (12, y), Panel separator 1px #E1E1E1
│ ───────────────────────────  │
│ [*RichTextBox rtbNotes*]     │  ReadOnly true, ScrollBars Vertical, BorderStyle FixedSingle, BackColor SystemColors.Window, Font Segoe UI 9, Size 336×140, WordWrap true, DetectUrls true, LinkClicked → Process::Start(https://github.com allow-list)
│   # Heading                  │
│   **bold** *italic* `code`   │
│   - list •                  │
│   > blockquote               │
│   --- hr                     │
│   [link](https://...)        │
│   😀 emoji                   │
└──────────────────────────────┘
```

* **No**: `lblChannelLine` download-count, `comboChannel`, `linkCheckNow ⟳`, `linkBrowse`, `browsePanel`, repository preview cards, repository metadata. Only `Release [ ▼ ]` selector.
* **Popup positioning:** `ShowPopup 261-290` unchanged: `x=owner.X+W-W-10` clamped to `WorkingArea 10px` margin; `TopMost false` kept (owned popup stays above owner via `Show(ownerForm)`), no `TransparencyKey`.

---

## 11. Exact Proposed Icon Implementation

**Control:** Keep `Panel^ iconPanel` but fix paint. Alternative considered: `PictureBox` with bundled `ICO`/`PNG` resource (e.g. `IDI_DOWNLOAD` 16×16 @96dpi + 32×32 @192dpi) — would require `*.rc` + `LoadImage` + DPI-aware scaling, more files. Preferred: **small owner-drawn GDI vector** (no resource, DPI-scalable, theme-aware).

**Size:** `iconPanel->Size = Drawing::Size(20,20)` at `96dpi` base, scaled at runtime via `GetDpiForWindow(ownerForm->Handle)` / `CreateGraphics()->DpiX` → `scale = dpi/96.0f`, `size = Round(20*scale)`, `location = Point(W - Round(28*scale), Round(192*scale))` (aligns to `lblStatus`).

**Paint (`OnIconPaint` 172-244 reworked):**

- `SmoothingMode AntiAlias`, `TextRenderingHint SystemDefault` (not `ClearTypeGridFit`) — better for Remote Desktop.
- `System::Drawing::Rectangle rect = p->ClientRectangle;` (fully qualified, `#undef Rectangle` already in `UpdaterUI.cpp:1-10`)
- `glyphColor = SystemColors::ControlText` if not `hasUpdate` else `Color::FromArgb(0,90,158) #005A9E` (blue) — or `SystemColors::HotTrack` for theme.
- **Vector download arrow:** `GraphicsPath^ path = gcnew GraphicsPath(); path->AddLine(...); AddPolygon(arrow head);` with `Pen 1.5*scale` + `SolidBrush` — not `DrawString "v"`. Coordinates in `0..20` space scaled.
  ```
  // Tray: rect 4,14,12,2  + stem 8,4,4,10 + head triangle (6,14)-(14,14)-(10,18)
  Pen^ pen = gcnew Pen(glyphColor, 1.5f*scale); pen->LineJoin = LineJoin::Round;
  g->DrawRectangle(pen, RectangleF(4*scale,14*scale,12*scale,2*scale));
  g->DrawLine(pen, 10*scale,4*scale, 10*scale,14*scale);
  PointF pts[] = {PointF(6*scale,12*scale), PointF(14*scale,12*scale), PointF(10*scale,18*scale)};
  g->FillPolygon(gcnew SolidBrush(glyphColor), pts);
  ```
  Uses `Color` not font, so no `Segoe UI`/`MDL2` dependency.

- **BackColor:** `iconPanel->BackColor = Color::Transparent` replaced with `Color::Transparent` still but parent is `SystemColors::Control` — keep, but ensure `SetStyle(ControlStyles::SupportsTransparentBackColor,true)` and `DoubleBuffered`.

**Why this method:** Simplest consistent across light/dark/Mica/DPI, no bundled resource, no `Segoe MDL2 Assets` availability check (`\uE896` requires `Segoe Fluent Icons` on Win11, fallback to `Segoe MDL2 Assets` on Win10, still font-dependent). Vector GDI path is font-agnostic and scales linearly.

---

## 12. Exact Proposed Notification-Dot Implementation

**Policy:** Dot visible **iff** `selectedOrLatestVersion > installedVersion` (via `UpdateVersion::CompareTo` `>0`). Not for `Offline/Error/Malformed/RateLimited` — already correct (`OnIconPaint 182-185` `isError` grey, dot only `hasUpdate`).

**Paint:** After vector arrow, `if(hasUpdate)`:

```
float dotDp = 6.0f * scale; // 6dp @96dpi → 9px @144dpi, 12px @192dpi
float dotX = rect.Width - dotDp - 1*scale;
float dotY = 1*scale;
SolidBrush^ dotBr = gcnew SolidBrush(Color::FromArgb(209,52,56) #D13438);
Pen^ dotPen = gcnew Pen(Color::White, 1*scale);
g->FillEllipse(dotBr, dotX, dotY, dotDp, dotDp);
g->DrawEllipse(dotPen, dotX, dotY, dotDp, dotDp);
```

* Border `1*scale` ensures 1 logical px at any DPI.
* Uses `SystemColors::Window` white border — visible on both `Control` grey and `White` popup? Icon is on main `Control` (`MyForm` `SystemColors::Control`), so white border contrasts.
* No `Transparent` behind dot; `SmoothingMode AntiAlias` for round.

**Placement:** `W - dotDp - 1*scale` ensures 1px margin, not `W-8` fixed.

---

## 13. Exact Proposed Markdown Renderer

**New files:** `src/updater/MarkdownRenderer.h/.cpp` — independent, `UpdaterUI` only consumer, ~250 lines.

**Interface:**
```cpp
public ref class MarkdownRenderer sealed {
public:
    static void Render(System::Windows::Forms::RichTextBox^ rtb, System::String^ markdown);
private:
    static void AppendHeading(RichTextBox^ rtb, String^ text, int level);
    static void AppendParagraph(RichTextBox^ rtb, String^ line);
    static void ParseInline(RichTextBox^ rtb, String^ text);
    static void AppendCodeBlock(RichTextBox^ rtb, String^ code);
};
```

**Supported (per §11):** headings `#`/`##`/`###` → Bold 12/10.5/9.5pt `#1A1A1A`; bold `**`/`__` → `Bold`; italic `*`/`_` → `Italic`; inline code `` ` `` → `Consolas 8.5pt BackColor #F3F3F3`; fenced code ` ``` ` → `Consolas 8.5pt BackColor #F5F5F5` left pad 4px; unordered `-`/`*` → `•` indent 12px; ordered `1.` → `1.` indent; links `[text](https://...)` → `Color #0067B8 Underline` + `LinkClicked` allow-list `https://github.com`; line breaks `\n` → `\par`; hr `---` → `Panel` or `─` 1px `#E1E1E1`; blockquote `>` → `Color #605E5C` + left border `3px #E1E1E1`.

**Non-goals:** Tables, images, HTML (`<script>` stripped), full CommonMark.

**Implementation:** Hand-rolled line scanner (state machine `inCodeBlock` bool), regex for inline `\\*\\*(.+?)\\*\\*` etc, or iterative `IndexOf`. Use `RichTextBox::SelectionStart/Length/Font/Color/SelectedText` or build RTF via `RtfBuilder`. No `WebView2` (100 MB, breaks Mica, overkill for text). `RichTextBox` is in `System.Windows.Forms` already.

**Emoji:** Preserve Unicode surrogate pairs; set `rtb->Font = Segoe UI 9` and for emoji runs `SelectionFont = gcnew Font("Segoe UI Emoji",9)` if `Char::IsSurrogatePair`. Detect via `Char::IsHighSurrogate`/`IsLowSurrogate`.

---

## 14. Emoji Handling

**Root cause:** Truncation `Substring(0,200)` mid-surrogate (`🚀` = `0xD83D 0xDE80` two `wchar_t`) → half-surrogate → `�`/`â`. `Label` `Segoe UI 9` lacks emoji glyph → tofu.

**Fix:** `MarkdownRenderer::Render` does **not** `Replace("\r"," ")` flatten; preserves `\n` → `\par`. Never `Substring` mid-surrogate — check `Char::IsHighSurrogate(text[199])` and extend by 1 if truncated at high surrogate. Use `RichTextBox` with `Segoe UI Emoji` fallback for surrogate runs. `UnescapeJsonString 88-94` already leaves raw emoji verbatim (via `EscapeForJson` preserves `>0x20`), so body arrives intact UTF-16; renderer just displays.

**Test:** Body `"🚀 Version 2.0\n- fix"` → heading `🚀` rendered via `Segoe UI Emoji 9`, not `â`.

---

## 15. Update-Status Area

**Location:** Directly below `Release [ ▼ ]` (y `pad+28`), above warning.

**Control:** `Label^ lblStatus` `336×16` `AutoSize false`, `Font Segoe UI 9`, `ForeColor` per state:

- `✓ You are on the latest version: v2.1.0` → `Color::FromArgb(16,124,16) #107C10` green ( `✓ U+2713` via `Segoe UI` + `Segoe UI Emoji` for `✓` fallback)
- `⚠ Newer version available: v2.2.0` → `Color::FromArgb(152,111,11) #986F0B` yellow/brown ( `⚠ U+26A0` )

**Subtle, compact:** No oversized banner; `AutoSize true` or `336×16`, `BorderStyle None`, `BackColor Transparent`. Computed in `UpdaterPopup::RefreshHeader` via `state->IsUpdateAvailable()` (`UpdateState.cpp:87-93`) and `selectedVersion->CompareTo(installed)`.

**Not collapsed into dot:** Dot and status both derive from same `>`, but dot is icon-only, status is text.

---

## 16. v1.0 / v2.0 Warning Behavior

**Trigger:** `if (!selectedRelease->HasUpdaterSupport)` where `HasUpdaterSupport = Version->IsUpdaterSupported()` (`UpdateVersion.cpp:32-38` `major>2 || major==2 && minor>=1`). Covers `v1.0.0`, `v2.0.0` and any future `<v2.1.0`.

**UI:** `Panel^ pnlWarning` `336×36` `BackColor #FFF8E1` (`Color::FromArgb(255,248,225)`) `Border 1px #F2C200`, `Visible` iff warning, `Label lblWarning` `8pt` `WordWrap` at `4,4` `328×28`:

> “⚠ This version does not include the in-app updater. Future updates will require manual download from GitHub.”

Visually distinct from status (warning panel vs status label), still compact (below status, above `Update` button). Not hard-coded `if(tag=="v2.0.0")` — uses capability method.

**Downgrade dialog:** Keep `UpdaterPopup::ConfirmDowngradeIfNeeded` (`UpdaterUI.cpp:842-867`) `MessageBox` with same text, `YesNo` `Warning`, `Button2` default — already correct for `v2.0`/`v1.0`.

---

## 17. Release Dropdown Behavior

**Control:** `ComboBox^ cmbRelease` `DropDownList` `Segoe UI 9` `330×21` (or `110×21` + label), `Location (60,0)` relative to `Release` label.

**Population:** `UpdateState::GetAllReleasesSorted()` (new, no channel filter) → `for each r in list` `Items->Add(r->Tag)` tag only (e.g. `v2.1.0`), sorted `Version.CompareTo` descending, newest first, installed marked `✓` via `Tag + " ✓ installed"` suffix. `SelectedIndex 0` default = latest available.

**OnSelectedIndexChanged:** `selectedRelease = list[SelectedIndex]` → `warningPanel.Visible = !selected->HasUpdaterSupport` → `MarkdownRenderer::Render(rtbNotes, selected->Body)` → `UpdateButtonStates()` (text `Update`/`Reinstall`/`Downgrade` per `CompareTo`).

**No:** large preview cards, repository names, download counts, asset URLs in dropdown — tag sufficient. Details view shows `published_at` `html_url` `Body` only.

---

## 18. Update / Download Behavior

**Selection does NOT download:** `OnListSelected`/`OnSelectedIndexChanged` only updates `lblStatus`/`pnlWarning`/`rtbNotes`; `UpdateInstaller::DownloadToTemp` not called. `CheckForUpdates` (`CheckAsync` 5s deferred + 6h periodic + `⟳` manual) only fetches JSON (`GitHubReleaseClient::FetchReleases`), not assets.

**Button operates on selected release:** `btnUpdate` `Click → OnUpdateClick` → `DoUpdateForRelease(selectedRelease)`:

- `newer` (`selected>installed`) → `Update` → `Confirm Update` `MessageBox` size `asset->Size` formatted `KB/MB` → `DownloadReleaseAsync` (background `Thread` `UpdateInstaller::DownloadToTemp %TEMP%\WindowsHelloFix\Updates\{guid}\Setup.exe` allow-list, progress via `IProgress`, `VerifyFile` SHA256) → `LaunchInstaller(silent=false)` `runas` → `Environment::Exit(0)` → NSIS `taskkill` + `MUI_FINISHPAGE_RUN`.
- `same` → `Reinstall` (or disabled + confirm) → same download path.
- `older` → `Downgrade` → `ConfirmDowngradeIfNeeded` warning first → same download.

**Only button begins download:** `DownloadReleaseAsync` and `DownloadToUserPathAsync` (explicit `SaveFileDialog` path) are the only callers of `DownloadToTemp`/`DownloadToUserPath`. Opening flyout or changing dropdown only `Render`.

**Temporary strategy preserved:** `UpdateInstaller.h:18-56` `CreateStagingPath %TEMP%`, `.part` atomic `Move`, `CleanupStagingPath` on cancel/fail, `CleanupOldStagingFolders(7)` on `Arm`. User “Download Installer…” via `SaveFileDialog` → `DownloadToUserPath` to chosen path, no temp, no auto-launch.

---

## 19. Security Implications

**Preserved (`UpdateInstaller.cpp` / `GitHubReleaseClient`):**

- HTTPS-only (`IsValidUrlImpl` checks `Scheme==https` `UpdateInstaller.cpp:48-56`)
- GitHub allow-list `Host==github.com && Path.StartsWith("/Shivu516/Windows-Hello-Fix/releases/download/") && name==Windows_Hello_Fix_Setup.exe 48-53`
- SHA-256 verification `VerifyFile 120-140` `SHA256::Create()->ComputeHash` + `Compare`
- Installer validation `LaunchInstaller 300-334` `name==Setup.exe` + `Path under GetStagingRoot()` + `UseShellExecute true` + `Verb runas` UAC
- Temporary staging `%TEMP%` per-user, `FileShare::None`, `.part` → `Move` atomic, `CleanupStagingPath` on cancel
- Cancellation via `CancellationTokenSource` (`Updater.cpp:30-31`)

**UI redesign does not weaken:** Markdown is **data not executable** — `MarkdownRenderer` strips HTML (`<script>` etc) and only emits RTF via `SelectionFont/Color`, never `WebBrowser` or `Process::Start` on markdown content. Links only via `RichTextBox::LinkClicked` allow-list `https://github.com`. No `WebView2` JS.

---

## 20. Performance Implications

**Lightweight:**

- One `Panel` icon `24×24` (now `20×20` base + DPI scale) + `Timer 500ms` pulse only when `Checking/Downloading` (`OnPulseTick 146`).
- Single `UpdaterPopup` `Form` 360×420, created on first `ShowPopup`, reused (`popup==nullptr||IsDisposed` check `271`). No multiple windows.
- No continuous repaint: `Invalidate` only on `StateChanged`/`PulseTick`/`MouseEnter`.
- GitHub poll `6h` (`Updater.cpp:49`) + `30m` cooldown (`kMinCheckIntervalMs`), `ETag 304` reduces bandwidth. Release list `per_page=20` (5-20 KB JSON) parsed once per `200` via `ParseReleasesJson` hand-rolled (no `Newtonsoft`).
- Markdown render only on `SelectedIndexChanged` (`RefreshDetailPane` → `MarkdownRenderer::Render`), not per-frame.

---

## 21. `src/core` Protection Strategy

**Rule:** `src/core/*` 7 files (`MyForm.h`, `MyForm_Camera.cpp`, `MyForm_Config.cpp`, `MyForm_Core.cpp`, `MyForm_Events.cpp`, `MyForm_System.cpp`, `MyForm_UI.cpp`) remain **byte-for-byte**.

**Integration stays via `main.cpp` (`main.cpp:1-82`):** Already `Updater` owned outside `src/core` (mirrors `RecoveryLoopFailsafe` precedent `AGENTS.md §1`). Icon via `ownerForm->Controls->Add(iconPanel)` dynamic injection at `UpdaterUI::InstallIcon 85` — no `MyForm_Core.cpp:119-180 InitializeComponent` edit. If DPI scaling needs `GetDpiForWindow`, still in `src/updater`.

**If unavoidable core change discovered (per §25):** STOP and document:

- **File:** e.g. `src/core/MyForm_Core.cpp:174`
- **Lines:** `this->Text = L"Windows Hello Fix v2.0"` (stale title)
- **Reason:** Title `v2.0` vs `v2.1` mismatch needs update for user clarity
- **Why `src/updater`/`main.cpp` cannot solve:** `Updater::ApplyTitleFix` in `main.cpp` already does `form->Text = "Windows Hello Fix " + CurrentDisplayString()` post-construct without core edit — so **no** core change needed. This is the pattern for any future title/font case.

**For this rework:** `src/core changed: NO` — verified `git diff -- src/core` empty.

---

## 22. `src/watchdog` Protection Strategy

**Rule:** `src/watchdog/CameraFailsafe.h/.cpp`, `RecoveryLoopFailsafe.h/.cpp` (4 files) remain **byte-for-byte**.

**Isolation:** `Updater` never includes `../watchdog`, never calls `CameraFailsafe::Arm`/`RecoveryLoopFailsafe::RequestRecoveryCheck`, never touches `g_lastHardwareToggleTick`. `Graph: src/core → camera, src/watchdog → failsafe, src/updater → releases` (no edges between watchdog and updater).

**For this rework:** `src/watchdog changed: NO` — `git diff -- src/watchdog` empty.

---

## 23. Exact Future Files to Create / Change

**New (1 pair):**
```
src/updater/MarkdownRenderer.h   — Render(RichTextBox^, String^) + helpers AppendHeading/ParseInline/AppendCodeBlock
src/updater/MarkdownRenderer.cpp — hand-rolled scanner, Regex for **/*/code/links, RichTextBox RTF emission, emoji surrogate handling
```

**Modify (rework, no new logic):**
```
src/updater/UpdaterUI.h          — remove OnOwnerResize/OnPopupClosed from UpdaterPopup, add to UpdaterUI; add pendingUpdateRelease field; keep UpdaterUI::OnOwnerResize/OnPopupClosed declarations
src/updater/UpdaterUI.cpp        — MAJOR: fix OnIconPaint (vector GDI, DPI scale, SystemColors, dot 6dp), fix ComputeIconLocation DPI, fix InitializeComponentPopup (SystemColors.Window, DoubleBuffered, FixedDialog qualified), remove comboChannel/browsePanel/listReleases detail controls, add cmbRelease + rtbNotes + pnlWarning/lblWarning + lblStatus, replace linkBrowse with single dropdown, replace lblNotes Label with RichTextBox, wire MarkdownRenderer::Render
src/updater/UpdateState.h/.cpp   — add GetAllReleasesSorted() (no channel filter, sorted descending Version.CompareTo, installed ✓), keep Channel model but UI no longer calls GetReleasesForChannel
```

**Keep (no rework):**
```
src/updater/UpdateVersion.h/.cpp, UpdateChannel.h/.cpp, UpdateModels.h/.cpp, GitHubReleaseClient.h/.cpp, UpdateInstaller.h/.cpp, Updater.h/.cpp (minor: ensure static fields not redefined, TaskCompletionSource fully qualified)
```

**Outside protected (allowed):**
```
main.cpp                         — no change (already owns Updater via main.cpp:62-73); if title fix needs tweak, 1 line in ApplyTitleFix
Windows_Hello_Fix_v2_0.vcxproj   — add <ClInclude MarkdownRenderer.h> + <ClCompile MarkdownRenderer.cpp>
Windows_Hello_Fix_v2_0.vcxproj.filters — add filter entries for MarkdownRenderer
docs/Plan.md                     — this appendix (26 items)
```

**Not touched:**
```
src/core/* (7), src/watchdog/* (4), x64/Release/install_script.nsi (for UI rework), app.manifest (optional DPI, not required), .gitignore, reference/
```

---

## 24. Build Changes

* **`Windows_Hello_Fix_v2_0.vcxproj`** — Add:
  ```xml
  <ClInclude Include="src\updater\MarkdownRenderer.h" />
  <ClCompile Include="src\updater\MarkdownRenderer.cpp" />
  ```
  Keep existing `Reference System.Net.Http` (`:130`). No new third-party (`Markdig`, `Newtonsoft.Json`, `WebView2`). `System.Windows.Forms` already provides `RichTextBox`.

* **`Windows_Hello_Fix_v2_0.vcxproj.filters`** — Add:
  ```xml
  <ClInclude Include="src\updater\MarkdownRenderer.h"><Filter>Header Files</Filter></ClInclude>
  <ClCompile Include="src\updater\MarkdownRenderer.cpp"><Filter>Source Files\src\updater</Filter></ClCompile>
  ```

* **No `app.manifest` DPI change required** for minimal rework (keep `TargetFramework v4.7.2` `WindowsTarget 10.0` `CLRSupport true`). Optional follow-up `<dpiAware>PerMonitorV2</dpiAware>` could improve `24×24` scaling but not required for artifact fix (vector + `DeviceDpi` scale already handles).

---

## 25. Detailed Test Matrix

| Area | Case | Expected |
|---|---|---|
| **Icon** | normal 24×24@96dpi + 36×36@144dpi | vector arrow centered, not `v` |
| | dot absent (`UpToDate`) | no dot, tooltip “Up to date” |
| | dot present (`latest>installed`) | 6dp red `#D13438` white 1px border at `W-7,1` DPI-scaled |
| | DPI 100/150/200% | `GetDpiForWindow` scale, no blur |
| | light / dark / high-contrast + Mica/Translucent | `SystemColors` inherits tint, no hard `White` |
| **Flyout** | opens at `owner.X+W-W-10` clamped, closes via X | 360×420 (520 expanded), no white artifact on dark |
| | remains owned, Deactivate does not auto-close | no black/white flash |
| | `DoubleBuffered true`, `SystemColors.Window` | no flicker |
| **Release list** | all current releases appear (v2.1.0, v2.0.0, v1.0.0) | one ComboBox sorted newest first, tag only, installed `✓` |
| | dropdown selection changes preview | `MarkdownRenderer::Render` called |
| **Release notes** | headings, bold/italic, inline code, code blocks, lists, links, emoji, hr, blockquote | RichTextBox formatted, emoji via `Segoe UI Emoji`, links clickable |
| | malformed markdown (`**unclosed`) | plain, no crash |
| | long notes (>200 lines) | vertical scroll |
| | empty `body` | placeholder “No release notes” |
| **Version state** | `installed==latest` | `✓ You are on the latest version: v2.1.0` green |
| | `installed<latest` | `⚠ Newer version available: v2.2.0` yellow, dot visible |
| | `selected older` `v2.1→v2.0` | warning panel visible |
| | `v1.0` selected | same warning via `IsUpdaterSupported==false` |
| **Network** | offline/timeout/malformed/GitHub 5xx | `Offline/Error` banner, cached shown |
| | cached stale <24h | `CacheUsed` banner |
| **Update** | selection does not download | `DownloadToTemp` not called until button |
| | button `Update`/`Reinstall`/`Downgrade` label per `CompareTo` | correct |
| | successful handoff | staged `%TEMP%` verified SHA256, `LaunchInstaller` `runas`, `Exit(0)`, NSIS `taskkill` + `MUI_FINISHPAGE_RUN` |
| | cancel/failed download | `CancellationToken`, `.part` deleted, `Error` banner |
| **Regression** | camera toggle, watchdog, `Restore true`, `WndProc` lock/unlock `7/8`, suspend/resume `0x0004/0x8013`, Issue #2 `BringWindowToFront` | unchanged, `diagnostic.log` no `Updater_*` during camera path |

---

## 26. Rollback Strategy

* **Icon/popup regresses:** `git checkout HEAD -- src/updater/UpdaterUI.h src/updater/UpdaterUI.cpp src/updater/MarkdownRenderer.*` + `git diff -- src/updater/UpdateState.*` revert `GetAllReleasesSorted` (keep channel filter), rebuild — `src/core` still zero. Or `UpdaterUI::InstallIcon` guard `if(DpiX>192) fallback` toggle.
* **MarkdownRenderer breaks:** Keep `lblNotes` fallback: if `Render` throws, `rtb->Text = body` plain + `MessageBox` “Failed to render markdown”.
* **Rate limit / cache corruption:** Delete `%APPDATA%\Windows Hello Fix\updater_cache.json` + `updater_etag.txt` → `Idle`, `CheckAsync(true)` rebuilds.
* **DPI regression:** Remove `<dpiAware>` if added, rebuild — bitmap-scale returns.
* **Uninstall safety:** `x64/Release/install_script.nsi` `Section Uninstall` unchanged (add `Delete "$APPDATA\...\updater_cache.json"` later, safe to leave).

# Appendix — Updater Release Preview — Markdown → HTML Rework (Issue #8) — PLANNING ONLY

> **Status: DRAFT — Planning Complete 2026-09-01 — Implementation Pending Review**
> **Branch: `updater` (on top of updater UI rework)**
> **Plan date: 2026-09-01**
> **Issue: https://github.com/Shivu516/Windows-Hello-Fix/issues/8**
> **Canonical reference: https://github.com/Shivu516/Windows-Hello-Fix/releases/tag/v2.0.0**
> **Core policy: `src/core/` ZERO changes — `src/watchdog/` ZERO changes — `src/updater/` + `main.cpp` only**

---

## 1. Current Release-Data Pipeline

```
GitHub API  GET /repos/Shivu516/Windows-Hello-Fix/releases?per_page=20&page=1
  Headers: User-Agent WindowsHelloFix-Updater/1.0, Accept: application/vnd.github+json, If-None-Match ETag
  → GitHubReleaseClient.cpp:136 resp->Content->ReadAsStringAsync()->Result  (HttpClient 15s, UTF-8 → UTF-16 String, body JSON escapes \r\n \" remain as 5C 72 etc)
  → UpdateModels::GetJsonStringField(body) (UpdateModels.cpp:140-175) — finds "body": "…", collects chars between quotes, handles escapes via esc flag, returns UnescapeJsonString(sb->ToString())
  → UpdateModels::UnescapeJsonString (73-101) — \n→LF, \r→CR, \t, \", \\, \/, \uXXXX → wchar_t, surrogate pairs split
  → GitHubRelease.Body (UpdateModels.cpp:275) — stored as System::String UTF-16, Version parsed via UpdateVersion::Parse, Channel via ChannelHelper, HasUpdaterSupport via IsUpdaterSupported()
  → UpdateState::CachedReleases + SerializeReleasesToCacheJson (346-385, EscapeForJson 103-117) → File::WriteAllText(updater_cache.json) → LoadCacheFromDisk (218-273, File::ReadAllText, TryDeserializeCacheJson 388-447 re-uses GetJsonStringField, bridges "tag" → "tag_name")
  → UpdateState::GetAllReleasesSorted() (new) → UpdaterUI::cmbRelease.Items (tag strings only, sorted Version.CompareTo descending)
  → UpdaterPopup::OnReleaseChanged → MarkdownRenderer::Render(rtbNotes, selected->Body) (UpdaterUI.cpp:129)
  → RichTextBox rtbNotes (UpdaterUI.cpp:82, ReadOnly WordWrap SystemColors.Window, 336×140) + DetectUrls false, LinkClicked → OpenUrl(https://github.com allow-list)
```

Single `per_page=20` covers all releases (verified v2.1.0 + v2.0.0 + v1.0.0, asset Windows_Hello_Fix_Setup.exe); `ETag 304` reduces to 1 call; `6h` timer + `30m` cooldown (`Updater.cpp:49,140`).

---

## 2. Exact Source of `92r92n` Corruption

**Not API:** Live `curl https://api.github.com/repos/Shivu516/Windows-Hello-Fix/releases/tags/v2.0.0 | od -c` shows `5C 72 5C 6E` (`\r\n`) and `5C 22` (`\"`) — correct JSON escapes. After `ReadAsStringAsync`, .NET decodes UTF-8 to UTF-16, `json` still contains `5C 72` two-char escapes. **API encoding OK.**

**Not Markdown parser stripping:** `MarkdownRenderer.cpp:31` `Replace("\r\n","\n")` is correct (`ldstr "\r\n"` in IL) but never matches `92r92n` (no `0D 0A`), so it leaves `92r92n` literal.

**Root = C++/CLI String conversion — `StringBuilder::Append(char)` overload bug (`UpdateModels.cpp`):**

* `GetJsonStringField 158-164`:
  ```cpp
  if (esc) { sb->Append('\\'); sb->Append(c); } // '\\' is 8-bit char 92 → Append(int32) → "92"
  ```
  IL `ldc.i4.s 92 + call Append(int32)` → decimal `"92"` not `'\'. For JSON `\r` (`5C 72`) → `"92"+"r"` = `"92r"`; `\r\n` → `"92r92n"` (`39 32 72 39 32 6E`) — **exact malformed string in `%APPDATA%\updater_cache.json` (`39 32 72…`)**.

* `UnescapeJsonString 82-86`:
  ```cpp
  if (n=='n') sb->Append('\n'); // '\n' char 10 → Append(int32) → "10"
  ```
  IL `ldc 10 + Append(int)` → `"10"` decimal. Would corrupt even if `GetJsonStringField` were fixed.

* `EscapeForJson 108-113` uses **string literals** `sb->Append("\\\"")` (`ldstr`) — correct, so it faithfully writes corrupted `Body` as `"92r92n"` verbatim.

Cache `TryDeserializeCacheJson 428-439` re-uses `GetJsonStringField` on `updater_cache.json` where body has no backslashes (`39 32 72`), so `esc` never true → `92r` preserved forever.

**Fix:** Change all `Append(charLiteral)` to wide `L'\\'`/`L'\n'`/`L'\r'`/`L'\t'`/`L'"'` or `Append("\\")` string overload: `sb->Append(L'\\')` → `Append(Char)`.

---

## 3. Current MarkdownRenderer Limitations

`MarkdownRenderer.h:9` `Render(RichTextBox^, String^)` direct `Markdown→RTF` (~360 lines, `MarkdownRenderer.cpp:11-360`):

* Supported: headings `#`/`##`/`###` → Bold 12/10.5/9.5 `AppendHeading:131`, `hr ---` → `─`, `blockquote >` → `#605E5C`, fenced ``` ``` → `Consolas 8.5 #F5F5F5`, `ul -/*` / `ol 1.` → `•/1.` `AppendListItem:318`, inline `**bold**` `` `code` `` `[text](https://...)` via `ParseInline:147-241` `IndexOf` scan + `AppendTextWithStyle:249`.
* **Gap:** Italic `*text*` / `_` not handled — `italicStar` variable `156` computed but never used, `243` comment "best-effort". So `*italic*` stays plain.
* **Colors hard-coded:** `AppendHeading #1A1A1A 131`, `code #F3F3F3 260`, `hr #E1E1E1` — not `SystemColors`.
* **Heading stripping perception:** `AppendHeading` removes `#` prefix (`trimmed->Substring(4)`) — correct rendering, but with corrupted `92r92n` body the `Replace("\r\n")` fails → single line `"92r92n# Heading"` not recognized as heading → appears as literal `#` + `92r`.

Visually inadequate: no tables/images (intentional), but `hard White` `rtb->BackColor SystemColors::Window` is correct; issue was data corruption, not renderer logic for headings.

---

## 4. Why Markdown Output Is Visually Inadequate

* Single `Replace("\r\n")` flatten leaves `92r` visible as text (`92r92n` spans).
* `Label` previous renderer truncated `Substring(0,200)` mid-surrogate → half-surrogate `�`/`â`; `RichTextBox` after fix still shows `92r` because data corrupted before render.
* Markdown syntax shown raw when data corrupted (headings not detected due to no real line breaks).
* Even with fixed data, current renderer only does minimal `**`/`code`/`[link]` — italic missing, no `SystemColors` theming, hard `#1A1A1A`.

---

## 5. Candidate Rendering Architectures

| Strategy | Correctness | Dependency | Unicode | Security | Maintenance | Offline | Quality | Build |
|---|---|---|---|---|---|---|:---|:---|
| **A. Custom lightweight Markdown→HTML** hand-rolled | Good for subset | 0 | must handle surrogate | Risk: must strip `<script` / `javascript:` | Owned, deterministic | Perfect | Depends on renderer | None |
| **B. Small library (Markdig/md4c)** | Excellent full GFM | 300KB-1MB + NuGet restore fragile in C++/CLI | OK | Allows raw HTML by default → need `DisableHtml` + XSS | External CVE | Offline OK | Better tables | Adds restore, enlarges Setup.exe 681KB→1.2MB |
| **C. GitHub Markdown API** `POST api.github.com/markdown` | Perfect fidelity | 0 binary but network | OK | GitHub sanitizes but still CSP | API drift | **Fails offline** (violates 60/h + `304` single-call) | Perfect | Adds latency per dropdown change |
| **D. Existing `MarkdownRenderer` direct `Md→RTF`** | Sufficient for headings/bold/lists/links/code/blockquote/hr | **0** owned 360 lines | Explicit `Segoe UI Emoji` per run `GetFontForRun:281` | Strongest — no HTML emitted, `SelectionFont` only | Low, 10-line fix for italic | Perfect cached | GDI consistent | **0** |
| **E. Hand-rolled `Md→HTML` + sanitizer + `HTML→RTF`** | Same as A + extra step | 0 + sanitizer | Same | Need sanitizer for HTML intermediate | Two conversions | Offline OK | Marginally better | None |

**Chosen for file structure:** If HTML pipeline required, prefer **A/E hybrid as `HtmlRenderer`**: keep `MarkdownRenderer` internal `Md→Html` string step, then `HtmlRenderer::Render` does `Html→RTF` via same `RichTextBox` logic (no new renderer control). This satisfies “Markdown→controlled HTML→renderer” while reusing proven `RTF` path.

---

## 6. Chosen HTML Architecture

**Markdown→HTML: (A) Custom lightweight `Markdown→HTML` controlled converter** (hand-rolled, owned, ~150 lines) — not B (bloat) nor C (offline fail). **HTML renderer: RichTextBox with controlled HTML→RTF conversion** (current control `UpdaterUI.cpp:82` `RichTextBox 336×140 ReadOnly ScrollBars Vertical SystemColors.Window`) — not `WebBrowser` (Trident IE7 quirks, `javascript:` exec, deprecated) nor `WebView2` (150-180 MB runtime + `WebView2Loader.dll`, breaks Mica, `TopMost` child `HWND` opaque, 500ms-1s cold launch, 60MB RAM, needs `Edge` runtime, `install_script.nsi` assumes offline). Keep `src/updater` separation (`GitHubReleaseClient` API, `UpdateModels` metadata, `UpdateVersion` compare, `UpdateState` cache, `UpdateInstaller` download, `Updater` orchestration, `UpdaterUI` interface, `HtmlRenderer` rendering).

**Why this pair wins for HelloFix:**

* Matches all 7 spec axes without HTML: direct RTF emission eliminates `HTML sanitization` entirely — no `script/javascript:/data:/iframe/onload` surface (`MarkdownRenderer.cpp:9,19,215`). `A/E` reintroduce it; `B/C` add weight.
* Zero deployment: `Release|x64` exe stays ~580KB; no `NuGet` restore, no `WebView2Loader`, no `Edge` runtime check. `vcxproj:130 System.Net.Http` only.
* Security strongest: link model is data-only → `RichTextBox::LinkClicked` allow-list `https://github.com` (`UpdaterUI.cpp:154`). Markdown body from `GitHubReleaseClient.cpp:136` never `eval`s.
* Offline/perf contract: render on `OnReleaseChanged` only, not poll; `SuspendLayout/Clear/ResumeLayout` single pass. Cached `Body` rendered offline (`Updater.cpp:258 <24h`).
* Theme/Mica/DPI: `RichTextBox` `SystemColors::Window/WindowText/ControlText/HotTrack` inherits `EnableVisualStyles` + DWM/Mica, high-contrast, `GetScaleFactor` DPI scaling. `WebView2` child `HWND` breaks this.

---

## 7. Markdown-to-HTML Strategy

Lightweight owned converter in `HtmlRenderer` (or `MarkdownRenderer::ToHtml`):

```
String^ HtmlRenderer::MarkdownToHtml(String^ md)
  normalized = md->Replace("\r\n","\n")->Replace("\r","\n") // after fixing 92r bug, this will see real \n
  lines = normalized->Split('\n')
  inCodeBlock=false
  for each line:
    if ``` toggle → <pre><code> / </code></pre>
    else if #/##/### → <h1>/<h2>/<h3> escapedText </h1>...
    else if "---" → <hr/>
    else if >  → <blockquote>escaped</blockquote>
    else if "- "/"* " → <li> → wrap contiguous <ul>
    else if "1. " → <li> → <ol>
    else if inline **, `, [text](url) → <strong>, <em>, <code>, <a href="https://...">
    else <p>escaped</p>
  Escape: & → &amp;, < → &lt;, > → &gt;, " → &quot; BEFORE wrapping, then allow only generated tags.
  Sanitize: strip <script, <iframe, <object, <embed, onload=, onclick=, javascript:, data:, file:, vbscript:, style:expression — via regex Remove "<[^>]*on\w+=..." + allow-list href="https?://"
  Return: "<html><head><meta charset=utf-8><style>h1{font-size:14pt...} ...</style></head><body>...</body></html>" but for RichTextBox we can keep fragment without <html> wrapper and directly parse.
```

Alternatively keep `MarkdownRenderer::Render` as `Markdown→RTF` and add `ToHtml` that reuses same scan but emits HTML string — `HtmlRenderer::Render` calls `ToHtml` then `HtmlToRtf` (same tag handlers as current `AppendHeading` etc, but source is HTML tags). This satisfies “controlled HTML” without adding library.

---

## 8. HTML Safety / Sanitization Strategy

`HtmlRenderer` treats release body as **data**:

* Escape raw HTML before generation (`HttpUtility::HtmlEncode` or manual `&lt;`).
* Generate only allow-list tags: `h1/h2/h3`, `p`, `strong`, `em`, `code`, `pre`, `ul/ol/li`, `a`, `blockquote`, `hr`, `br`.
* Link `href` allow-list: `https://` and `http://` (prefer `https://github.com` but allow any `https`), reject `javascript:`, `data:`, `file:`, `vbscript:`, `on*=` attributes, `style`.
* No `WebBrowser` `AllowWebBrowserDrop`, no `WebView2` `ScriptEnabled` — use `RichTextBox` which never executes JS.

---

## 9. Unicode / Emoji Strategy

Fix complete path:

* **HTTP:** `ReadAsStringAsync` decodes UTF-8 correctly (already).
* **JSON:** Fix `Append(L'\\')` etc so `\uD83D\uDE80` becomes surrogate pair `0xD83D 0xDE80` correctly; `Unescape` must combine `IsHighSurrogate`/`IsLowSurrogate` into single `wchar_t` pair or keep as two `wchar_t` — .NET `String` UTF-16 stores emoji as two `wchar_t`, `RichTextBox` with `Segoe UI Emoji` renders if font fallback used. Never `Substring` mid-high-surrogate (check `IsHighSurrogate(text[cut])` then extend).
* **Markdown→HTML:** Preserve `&` → `&amp;` but keep emoji verbatim (no `&#x1F680;` needed).
* **UI:** `MarkdownRenderer::GetFontForRun` already does `IsHighSurrogate` → `Font("Segoe UI Emoji", baseSize)` per-run (`MarkdownRenderer.cpp:281-289`) — keep. Ensure `rtb->Font = Segoe UI 9` base, emoji runs use Emoji font.

---

## 10. Heading / List / Link / Code / Blockquote Support

* **Headings:** `#`/`##`/`###` → `<h1>/<h2>/<h3>` → `Bold 12/10.5/9.5pt #1A1A1A` (or `SystemColors::ControlText` with size scale), `AppendHeading 131` handles.
* **Lists:** `*`/`-` → `<ul><li>• `, `1.` → `<ol><li>1. `, `AppendListItem:318` with `•` indent 12px.
* **Links:** `[text](https://...)` → `<a href="https://...">text</a>` → `Color #0067B8 Underline`, `LinkClicked` allow-list `https://github.com` (`UpdaterUI.cpp:154`).
* **Code:** `` `code` `` → `<code>` → `Consolas 8.5 #F3F3F3` `AppendTextWithStyle 260`, ```` ``` ```` block → `<pre><code>` → `Consolas 8.5 #F5F5F5` `AppendCodeBlock 300`.
* **Blockquote:** `>` → `<blockquote>` → `#605E5C` grey `AppendBlockquote 330`.
* **HR:** `---` → `<hr/>` → `─` `AppendHorizontalRule 342`.
* **Br:** `\n` → `<br/>` → `\par` in RTF.

---

## 11. UI Renderer Choice

**Control:** `RichTextBox` (`UpdaterUI.cpp:82` `ReadOnly WordWrap SystemColors.Window 336×140`) — `ReadOnly`, `ScrollBars Vertical`, `BorderStyle FixedSingle`, `BackColor SystemColors.Window`, `Font Segoe UI 9`, `DetectUrls false` (we handle links), `DoubleBuffered true` (already). **Not** `WebBrowser` (IE quirks) nor `WebView2` (bloat, Mica break, 500ms launch, 60MB).

**Why:** `RichTextBox` is in `System.Windows.Forms` already (`vcxproj:131`), inherits `SystemColors` + `EnableVisualStyles` + DWM/Mica, high-contrast, DPI via `GetScaleFactor`, `SuspendLayout/Clear/ResumeLayout` single pass, render only on `SelectedIndexChanged`.

---

## 12. Mica / Transparency Strategy

* **Not fix data:** `app.manifest:1-11` only `assemblyIdentity 2.1.0.0` + `requireAdministrator` (no `<dpiAware>`), `MyForm_Core.cpp:119-180` default `Control` back, `main.cpp:12` `EnableVisualStyles`.
* **Why updater broke:** Hard `White 330` + hard `FromArgb` literals override system palette; opaque `Form` without `DwmExtendFrameIntoClientArea` does not participate in DWM backdrop. `Translucent Windows` hooks owner `HWND` but not popup `HWND`.
* **Strategy:** Use `SystemColors::Window`/`ControlText`/`HotTrack` (`UpdaterUI.cpp:64` already after fix), `DoubleBuffered true`, normal `Form` `FixedDialog` `SystemColors.Window`. Optionally call `DwmSetWindowAttribute(DWMSBT_MAINWINDOW)` if `IsWindows11OrGreater` (22000+) — but otherwise inherit gracefully. Document that true dark mode is not implemented; updater will follow system `Control` (light) and high-contrast via `SystemColors`.

---

## 13. Dark / Light Behavior

* Investigate: WinForms defaults → `SystemColors` + `EnableVisualStyles` + `Control` back; no custom dark-mode framework. `Translucent Windows` / Mica are external tool/DWM, not app code.
* Renderer should be compatible with whatever GUI actually provides: use `SystemColors::WindowText` for text, `SystemColors::ControlText` for glyph, `SystemColors::HotTrack` for links, `SystemColors::GrayText` for blockquote, `Color::FromArgb(243,243,243)` for code back but blend with `SystemColors::ControlLight` if high-contrast.
* Do not invent independent dark-mode framework.

---

## 14. Exact File Structure

```
src/updater/MarkdownRenderer.h/.cpp  — REMOVE (replaced by HtmlRenderer, per prompt "prefer only HTML")
src/updater/HtmlRenderer.h/.cpp      — NEW: Render(RichTextBox^, String^ markdown) → MarkdownToHtml + Sanitize + HtmlToRtf (uses same Append* helpers, ~280 lines). Alternatively rename MarkdownRenderer → HtmlRenderer.
src/updater/UpdateModels.cpp         — FIX: 6× Append(char) → Append(L'\\')/Append("\n") etc in GetJsonStringField 158-164 and UnescapeJsonString 82-86
src/updater/UpdateState.h/.cpp       — keep GetAllReleasesSorted (already added), no channel UI
src/updater/UpdaterUI.h/.cpp         — keep vector icon + SystemColors + RichTextBox; wire HtmlRenderer::Render instead of MarkdownRenderer::Render; fix Replace("\r\n") already correct after data fix
```

If `MarkdownRenderer` can be cleanly repurposed, keep file but change class name to `HtmlRenderer` and add `ToHtml` method — task says prefer clear responsibility-based naming, so **replace** (rename) to `HtmlRenderer`.

**Outside protected (allowed):**
```
Windows_Hello_Fix_v2_0.vcxproj   — replace <ClInclude MarkdownRenderer.h> with HtmlRenderer.h, <ClCompile MarkdownRenderer.cpp> with HtmlRenderer.cpp
Windows_Hello_Fix_v2_0.vcxproj.filters — same
docs/Plan.md                     — this appendix (23 items)
```

**Not touched:** `src/core/* (7), src/watchdog/* (4), main.cpp (updater owned outside src/core already), x64/Release/install_script.nsi`

---

## 15. Core / Watchdog Isolation Strategy

**Rule:** `src/core/*` 7 files and `src/watchdog/*` 4 files remain **byte-for-byte**.

**Integration stays via `main.cpp` (`main.cpp:1-82`):** Already `Updater` owned outside `src/core` (mirrors `RecoveryLoopFailsafe` precedent `AGENTS.md §1`). Icon via `ownerForm->Controls->Add(iconPanel)` dynamic injection at `UpdaterUI::InstallIcon 85` — no `MyForm_Core.cpp:119-180 InitializeComponent` edit.

**If unavoidable core change discovered (per §25):** STOP and document file, lines, technical blocker, smallest alternative. None anticipated.

**For this rework:** `src/core changed: NO`, `src/watchdog changed: NO` — verified `git diff -- src/core src/watchdog` empty.

---

## 16. Performance

* One `Panel` icon `20×20*scale` + `Timer 500ms` pulse only when `Checking/Downloading`.
* Single `UpdaterPopup` `Form` 360×420, created on first `ShowPopup`, reused (`popup==nullptr||IsDisposed` check). No multiple windows.
* No continuous repaint: `Invalidate` only on `StateChanged`/`PulseTick`/`MouseEnter`.
* GitHub poll `6h` + `30m` cooldown, `ETag 304` reduces bandwidth. Release list `per_page=20` (5-20 KB JSON) parsed once per `200`.
* HTML render only on `SelectedIndexChanged` (`RefreshNotes` → `HtmlRenderer::Render`), not per-frame. `MarkdownToHtml` + `Sanitize` + `HtmlToRtf` single pass per selection.

---

## 17. Error Handling

* `GitHubReleaseClient::DoFetch` handles `304 NotModified` (use cache), `429 RateLimited` (use stale <24h, banner), `5xx`/`400` → `Offline` with stale <24h fallback, `Malformed` → `Error` banner, `Cancelled` → `Idle`. All `StateChanged` → `RefreshForExternalChange` without crash.
* `HtmlRenderer::Render` wrapped `try { } catch(...) { rtb->Text = markdown; }` fallback to plain text if RTF emission throws (e.g. malformed `**unclosed`).
* `File::ReadAllText/WriteAllText` for `updater_cache.json` with `try/catch` → `LoadCacheFromDisk` returns false, `SaveCacheToDisk` atomic `WriteAllText(temp)+Move`.

---

## 18. Offline Behavior

* `UpdateState::LoadCacheFromDisk` reads `updater_cache.json` (UTF-8) → `GetAllReleasesSorted()` → `cmbRelease` populated even if `HttpClient` `NetworkError`/`Offline`. `Updater::CheckThreadProc` → `FetchResult::NetworkError` → `SetStatus(Offline)` + `CacheUsed` banner if `LastCheckUtc <24h` else `Offline`.
* `HtmlRenderer` renders `r->Body` from cache (already unescaped correctly after fix) — no network needed for notes. Links still require browser.

---

## 19. Release Selection Integration

* `cmbRelease.SelectedIndexChanged` → `selectedRelease = list[SelectedIndex]` → `pnlWarning.Visible = !HasUpdaterSupport` → `HtmlRenderer::Render(rtbNotes, MarkdownToHtml(selected->Body))` → `UpdateButtonStates` (`Update`/`Reinstall`/`Downgrade` via `CompareTo`). `lblStatus` updated via `IsUpdateAvailable()` (`latest>installed`).

---

## 20. v1.0 / v2.0 Warning Integration

* `if (!selectedRelease->HasUpdaterSupport)` where `HasUpdaterSupport = Version->IsUpdaterSupported()` (`UpdateVersion.cpp:32-38` `major>2 || major==2 && minor>=1`). Covers `v1.0.0`, `v2.0.0` and any future `<v2.1.0`.
* `Panel pnlWarning 336×36 #FFF8E1` + `Label lblWarning 8pt` visible iff warning, `Label` `WordWrap` at `4,4` `328×28`: “⚠ This version does not include the in-app updater. Future updates will require manual download from GitHub.” Distinct from status, compact.

---

## 21. Icon Preservation

* Download icon vector GDI path (`GraphicsPath` + `FillPolygon`) `1.5*scale` `Pen`, `SystemColors.ControlText` / `#005A9E` when `hasUpdate`, `SmoothingMode AntiAlias`, DPI-scaled `20*scale` panel, location `W-28*scale,192*scale`, `Transparent` parent `SystemColors.Control` with `DoubleBuffered`. Red dot `6dp*scale` `#D13438` white `1*scale` border at `W-7*scale`, visible only when `IsUpdateAvailable()`.

---

## 22. Detailed Test Matrix

| Area | Case | Expected |
|---|---|---|
| **Basic** | `v2.0` body (emoji, headings, lists, `---`) | `MarkdownToHtml` → `HtmlRenderer` → headings Bold 12/10.5/9.5, lists `•`, no `92r` |
| | `v2.1` (if exists), future `🚀` emoji | `Segoe UI Emoji` fallback, no `â` |
| | long (>200 lines) scroll, empty `body` → “No release notes” | `RichTextBox` vertical scroll, no horizontal overflow |
| | malformed `**unclosed` → plain | no crash, `try/catch` fallback |
| **Formatting** | `H1`/`H2`/`H3` distinct size/bold, `**bold**`/`*italic*`, `ul` `•`, `ol 1.`, `[link](https)` clickable, `` `code` `` `#F3F3F3` `Consolas`, ```` ``` ```` block `#F5F5F5`, `> blockquote` grey, `---` `─` | `HtmlToRtf` via `SelectionFont/Color/BackColor` |
| **Unicode** | `🚀` `U+1F680` surrogate, `✓` `U+2713`, `⚠` `U+26A0`, `⟳` `U+27F3`, quotes `“”`, non-ASCII | `IsHighSurrogate` → `Segoe UI Emoji`, no `â` |
| **Security** | `body` containing `<script>alert(1)</script>`, `javascript:alert(1)`, `data:text/html`, `onload=`, `iframe` | stripped/escaped, not executed, links only `https` |
| **UI** | normal Windows, Mica/Translucent `SystemColors.Window` blends, `light/dark` via `SystemColors`, DPI 100/150/200% icon `20*scale` dot `6dp*scale`, small window `360×420` scroll, long body scroll | no white artifact on dark, no blur |
| **Regression** | updater icon, red dot, `cmbRelease` dropdown, `Update` button, status `✓/⚠`, `v1.0/v2.0` warning, main GUI, camera `Restore(true)`, watchdog `RecalculateLatest`, Issue #2 `BringWindowToFront` | unchanged, `diagnostic.log` no `Updater_*` during camera path |

---

## 23. Rollback Plan

* Data fix rollback: `git checkout HEAD -- src/updater/UpdateModels.cpp` (re-introduces `92r` but restores prior behavior) + delete `updater_cache.json` + `Rebuild`.
* Renderer rollback: `git checkout HEAD -- src/updater/MarkdownRenderer.* src/updater/UpdaterUI.*` + `git rm HtmlRenderer.*` + rebuild — `src/core` still zero. Or `MarkdownRenderer` fallback: if `HtmlRenderer::Render` throws, `rtb->Text = markdown` plain.
* Cache corruption: delete `%APPDATA%\Windows Hello Fix\updater_cache.json` → `Idle`, `CheckAsync(true)` rebuilds.
* Uninstall safety: `x64/Release/install_script.nsi` `Section Uninstall` unchanged.

# Appendix — Updater UI / Release Preview Rework — Visual Reference-Driven Design (Issue #8) — PLANNING ONLY

> **Status: DRAFT — Planning Complete 2026-09-01 — Implementation Pending Review**
> **Branch: `updater` (on top of HtmlRenderer Markdown→HTML)**
> **Plan date: 2026-09-01**
> **Visual reference: Screenshot concept (left: HelloFix v2.1.0 dark Mica; right: Updater with top row `v2.0.0 ▼` + `Update`, single `Release Info` preview)**
> **Issue: https://github.com/Shivu516/Windows-Hello-Fix/issues/8**
> **Canonical reference: https://github.com/Shivu516/Windows-Hello-Fix/releases/tag/v2.0.0**
> **Core policy: `src/core/` ZERO changes — `src/watchdog/` ZERO changes — `src/updater/` + `main.cpp` only**

---

## 1. Current UI Architecture

**File `src/updater/UpdaterUI.h:46-87` / `UpdaterUI.cpp:58-150`:**

* **Top row (current, fragmented):** `Label "Release" 8pt #605E5C at (12,12) 60×21` + `ComboBox cmbRelease at (74,12) 200×21 DropDownList Segoe UI 9` (`UpdaterUI.cpp:69-70`) → `y+=28` → `Label lblStatus 336×16 9pt` (`72`, green `#107C10` / yellow `#986F0B` via `RefreshStatus 92-107`) → `y+=18` → `Panel pnlWarning 336×36 #FFF8E1` (`74-76`, visible iff `!HasUpdaterSupport`) → `y+=40` → `Button btnUpdate 110×28` (`77`) at `(12, y)` left-aligned full-width row → `y+=34` → `Label lblNotesHeader "Release notes" 8pt Bold #605E5C` (`79`) → `Panel separator 1px #E1E1E1` (`81`) → `RichTextBox rtbNotes 336×140 ReadOnly Vertical SystemColors.Window` (`82`) → `LinkLabel linkViewOnGithub` (`84`) outside `rtbNotes` below separator → `ProgressBar 8px` + `lblStatusBanner`. **Result:** 6 separate surfaces, 62px vertical waste between dropdown and button, preview only 33% of window (`140` of `420`).

**Owner:** `Updater` (`Updater.h:12-46`) owns `UpdateState`, `GitHubReleaseClient`, `UpdaterUI` (`Updater.cpp:20-30` timers 5s/6h). `main.cpp:62-73` `if(!isCommandWorker) gcnew Updater(%form)` + `Load+=OnOwnerLoad` + `InstallIcon()` via `Controls->Add` at `ComputeIconLocation 30-32` `W-28*scale,192*scale`. No `src/core` edit.

---

## 2. Current Rendering Pipeline

```
GitHub API  GET /releases?per_page=20 (ETag, User-Agent WindowsHelloFix-Updater/1.0)
  → GitHubReleaseClient.cpp:136 ReadAsStringAsync UTF-8 → UTF-16 String (body escapes \r\n \" still 5C 72, now correctly L'\\' fixed)
  → UpdateModels::GetJsonStringField 140-175 (now L'\\' Append(Char) correct) → UnescapeJsonString 73-101 (L'\n' etc) → GitHubRelease.Body UTF-16, Version via UpdateVersion::Parse, HasUpdaterSupport via IsUpdaterSupported()
  → UpdateState::CachedReleases + SerializeReleasesToCacheJson 346-385 → File::WriteAllText(updater_cache.json) → LoadCacheFromDisk 218-251 (detects "92r92n" corrupted → File::Delete)
  → UpdateState::GetAllReleasesSorted 112-144 (draft-filtered, descending CompareTo) → UpdaterUI::cmbRelease.Items (tag only, sorted)
  → UpdaterPopup::OnReleaseChanged → HtmlRenderer::Render(rtbNotes, selected->Body) (UpdaterUI.cpp:129)
  → HtmlRenderer.cpp:89-245 MarkdownToHtml (normalize \r\n→\n, line scan: ```→<pre>, #→<h1>, hr→<hr>, >→<blockquote>, ol/ul→<li>, p→<p>, inline **→<strong> etc, EscapeHtml &) → SanitizeHtml 33-87 (strip <script/on*/javascript:) → HtmlToRtf 263-399 (Regex <(/?)(\w+)> flags inStrong/inEm/inCode/inPre/inA/inBlockquote/inH1.. + AppendTextWithStyle Segoe UI 9 / Consolas 8.5 / Segoe UI Emoji per surrogate)
  → RichTextBox rtbNotes 336×140 ReadOnly ScrollBars Vertical SystemColors.Window, WordWrap, DetectUrls false, LinkClicked → OpenUrl(https://github.com allow-list)
```

Single `per_page=20` covers `v2.1.0, v2.0.0, v1.0.0`; `ETag 304` single call.

---

## 3. Exact Causes of Visual Artifacts

* **Fragmented panels:** Current has two status labels (`lblStatus 72` outside + `lblStatusBanner 87` for Checking) + separate `lblNotesHeader` + `separator` + `rtbNotes` + `linkViewOnGithub` outside → 6 surfaces. Desired is **single `Panel pnlPreview` with Dock** containing `Release Info` + status + title + notes + link.
* **Top row split:** `cmbRelease Y=pad` then `y+=28` then `lblStatus` then `pnlWarning` then `btnUpdate Y+=74` → 62px waste. Desired needs `cmbRelease Width ~68%` + `btnUpdate X=Right+8 same Y` (requires deleting `y+=` stacking).
* **Header outside preview:** `lblNotesHeader "Release notes"` separate `Label` not inside `rtbNotes`; screenshot's `Release Info` is **inside** scrollable preview as first `<h1>` or `Label` docked top.
* **Status outside preview:** `lblStatus` shows only one side (`either` latest `or` installed). Desired shows **both**: `Current version: v2.1.0 - Latest Stable: v2.0.0` inside preview near top, green/yellow.
* **Warning giant panel:** `pnlWarning 336×36 #FFF8E1 Border FixedSingle` is 8.5% height — screenshot shows none (or subtle inline). Violates “no giant background panel”.
* **Heading size:** `HtmlToRtf 313` only sets `Color #1A1A1A` for `inH1`, not `FontSize 12pt Bold`. `AppendHeading 401-415` (12/10.5/9.5) never called from `HtmlToRtf`. Title renders same size as body.
* **Spacing:** `HtmlToRtf` emits single `\n` for `p/li/h1` `353-368`; desired needs `\n\n` or `SelectionCharOffset` for readable `8px` paragraph gap + `12px` list indent (`HtmlRenderer.cpp:347 "  • "` minimal).
* **Link outside vs inside:** `linkViewOnGithub` outside `rtbNotes` (`84` below `separator`). Screenshot puts `[View on GitHub]` **inside** preview bottom.

---

## 4. Exact Cause of Unicode Corruption

* **Primary `92r92n`:** `UpdateModels.cpp:158-164` `sb->Append('\\')` with narrow `char 92` → `Append(int32)` → decimal `"92"`; `\r\n` (`5C 72 5C 6E`) → `"92r92n"` (`39 32 72 39 32 6E`) in `%APPDATA%\updater_cache.json`. **Fixed** to `L'\\'` (`Append(Char)`). Old cache auto-deleted via `UpdateState.cpp:230-234` `Body.Contains("92r92n")`.
* **Remaining `â€¢`:** Bullet `• U+2022` (`E2 80 A2` UTF-8) inside `HtmlRenderer.cpp` source file: if file saved as UTF-8 **without BOM**, `CL.exe /clr` may interpret `•` as ANSI `windows-1252` `â€¢` (`E2→â, 80→€, A2→¢`). Current `HtmlRenderer.cpp:347` `AppendText("  • ")` contains literal `•`. On `C++/CLI` with `CharacterSet Unicode` (`vcxproj:35`), source should be **UTF-8 with BOM** or use `\u2022`. Fix: replace `•` with `L'\u2022'` or `"\u2022"` string, or save file as UTF-8 BOM.
* **`<em>` literal:** If markdown `*italic*` not converted, `ParseInline` gap (previous `MarkdownRenderer` missing italic) would leave `*` raw; now `HtmlRenderer::MarkdownToHtml 183-184` handles `*`→`<em>` and `HtmlToRtf 304-308` handles `inEm→Italic`, so not bug after HTML pipeline. But old `UpdateModels` corruption left `"92r"` → `Replace("\r\n")` never matches → single line `"92r# Heading"` not recognized → `#` remains.

---

## 5. Current Updater Icon Implementation

**Already fixed in prior rework** (`UpdaterUI.cpp:39-50`): `Panel 20×20*scale`, `GetScaleFactor()` via `CreateGraphics()->DpiX/96`, `OnIconPaint` vector GDI path (`Pen 1.5*scale`, Tray `Rect 4,14,12,2` + Stem `10,4→10,14` + Head triangle `6,12-14,12-10,18`, `FillPolygon`), `SmoothingMode AntiAlias`, `SystemDefault` hint, `SystemColors.ControlText` / `#005A9E` when `hasUpdate`, dot `6dp*scale #D13438` white `1*scale` border at `W-dot-1,1`. No `glyph="v"`, no `Segoe MDL2` font dependency, DPI-scaled, `Transparent` → `SystemColors.Control` with `DoubleBuffered`. **Preserve as-is; verify no regression.**

---

## 6. Target UI Derived from Supplied Image

**Proportions (360×~480, dark Mica, compact):**

```
┌──────────────────────────────┐  UpdaterPopup 360×460 (Max 360×520) SystemColors.Window
│ [v2.0.0        ▼] [ Update ] │  Top row pad 12: cmbRelease ~68% (228px) | btnUpdate ~30% (100px) same Y=12, Height 21 vs 28
│ ┌──────────────────────────┐ │  Panel pnlPreview Dock Fill, BackColor SystemColors.Window, Border FixedSingle #E1E1E1, Padding 8
│ │ Release Info             │ │  Label lblReleaseInfo Bold 11pt #1A1A1A inside preview Dock Top
│ │ Current version: v2.1.0  │ │  Label lblStatusInside 9pt #107C10 (up-to-date) / #986F0B (outdated) inside preview
│ │ - Latest Stable: v2.0.0  │ │  (single line, both values)
│ │                          │ │
│ │ 🚀 Version 2.0 - …       │ │  RichTextBox rtbNotes Dock Fill, 336×~200, ReadOnly Vertical SystemColors.Window, rendered via HtmlRenderer
│ │ This major update is…    │ │  (h1 14pt Bold, p 9pt, ul • 12px indent, code Consolas 8.5, blockquote grey)
│ │ • Re-written core…       │ │
│ │ • Improved stability     │ │
│ │                          │ │
│ │ [View on GitHub]         │ │  LinkLabel inside preview bottom Dock (lighter blue #63A8F8 if dark, else #0067B8)
│ └──────────────────────────┘ │
└──────────────────────────────┘
```

* No channel selector (already removed), no repository preview (already removed), no giant warning panels.

---

## 7. Release Dropdown Design

**Control:** `ComboBox cmbRelease DropDownList Segoe UI 9` `Location (12,12) Size 228×21` (68% of `336` inner), `FlatStyle` default, `DrawMode Normal`. **Items:** `tag` only (`v2.1.0`, `v2.0.0`, `v1.0.0`…), sorted descending `Version.CompareTo` via `UpdateState::GetAllReleasesSorted()` (no channel filter), newest first, installed `✓` suffix (`tag + " ✓ installed"` if `CompareTo==0`). Default `SelectedIndex` preserves `prevTag` else `LatestForChannel` else `0`. No asset URLs/dates in dropdown.

---

## 8. Update Button Placement

**Beside dropdown:** `Button btnUpdate Location = Point(cmbRelease.Right + 8, 12) Size 100×21` (30% width, same `Y`, height matches `ComboBox` 21, not 28). `UseVisualStyleBackColor true`, `FlatStyle Standard`. **Text dynamic:** `hasAsset` else `"No installer"`; `CompareTo` `<0→"Downgrade"`, `==0→"Reinstall"`, `>0→"Update"` (`UpdateButtonStates 131-139`). Shared top row `y=12` saves 62px vertical, matches screenshot.

---

## 9. Release Info Placement

**Inside preview:** No separate `lblNotesHeader` outside. Create `Panel pnlPreview Dock Fill` (fills between top row and bottom). Inside `pnlPreview`:

* `Label lblReleaseInfo Dock Top Height 22 Text "Release Info" Font Segoe UI 11 Bold ForeColor #1A1A1A` (or `SystemColors.ControlText`, 12pt if `GetScaleFactor`).
* `Label lblStatusInside Dock Top Height 16 Font 9` `String::Format("Current version: {0} - Latest Stable: {1}", cur, latest?.Tag)` `ForeColor Green #107C10` or Yellow `#986F0B` (text only, no background).
* `RichTextBox rtbNotes Dock Fill` below them, `Margin 0`.

This makes status scroll with preview? Keep status outside scroll for always-visible: `pnlPreview.Controls.Add(lblReleaseInfo); Add(lblStatusInside); Add(rtbNotes)` with `rtbNotes Dock Fill` below two docked-tops → status stays top of preview, notes scroll.

---

## 10. HTML/Markdown Rendering Options

| Option | Fidelity | Emoji | Security | Mica | Deploy | Startup |
|---|---|---|---|---|---|---|
| **A. Markdown→RTF** (current `MarkdownRenderer` direct) | Good for subset | `Segoe UI Emoji` per-run | Strong (no HTML) | `SystemColors` inherits | 0 | 0ms |
| **B. Markdown→controlled HTML→RichTextBox** (chosen `HtmlRenderer` `MarkdownToHtml→Sanitize→HtmlToRtf`) | Same, plus HTML sanitization layer | Same | Controlled allow-list `h1/p/strong/em/code/pre/ul/ol/li/a/blockquote/hr/br` + strip `script/on*` + `href https` only | Same `SystemColors` | 0 | ~5ms |
| **C. WebView2** | Perfect | Perfect | Sandbox but `javascript:` via API, need CSP | GPU `HWND` opaque breaks Mica, extra process | 150MB runtime | 500ms |
| **D. GitHub Markdown API** | Perfect | Perfect via GitHub | GitHub sanitizes | Same HTML | Needs `Edge` | Fails offline (60/h) |
| **E. WebBrowser Trident** | IE7 quirks | Poor emoji | Worst (`ActiveX`) | Deprecated | Regkey elevation | 200ms |

---

## 11. Selected Rendering Strategy

**Keep `HtmlRenderer` pipeline: `Markdown → controlled HTML → RichTextBox` (`HtmlRenderer.h:9` `Render` = `MarkdownToHtml 89-245` + `SanitizeHtml 33-87` + `HtmlToRtf 263-399`).** This satisfies prompt “Markdown → controlled HTML → updater HTML renderer/control” with **controlled HTML** (allow-list `h1/h2/h3, p, strong/em, code/pre, ul/ol/li, a, blockquote, hr, br` + `EscapeHtml` before wrapping, `IsSafeUrl` `https://` only, dangerous tags stripped). Render to **native `RichTextBox`** (not `WebView2`/`WebBrowser`) — lightest, `SystemColors` inherits Mica, no `Edge` runtime, offline cache works, `GetFontForRun` emoji fallback.

*Do not use `WebView2` (150MB, separate `HWND`, `TopMost` issues, `Mica` break), not GitHub API (fails offline, RateLimited), not pure `Markdown→RTF` without HTML (violates “controlled HTML” requirement).*

**Fixes vs current `HtmlRenderer.cpp`:** Heading size (currently only `Color` `313`, not `FontSize` 12/10.5/9.5 via `AppendHeading` never called) → add `SelectionFont Size 12pt Bold` for `inH1` (wire `AppendHeading` style inline). List indent `12px` via `SelectionIndent`, paragraph `\n\n` gap, `•` as `L'\u2022'` not literal `•`.

---

## 12. Emoji Strategy

**Pipeline:** `GitHub API` UTF-8 `F0 9F 9A 80` → `ReadAsStringAsync` UTF-16 surrogate `0xD83D 0xDE80` → `GetJsonStringField` now `L'\\'` fix preserves pair → `EscapeHtml` preserves verbatim → `MarkdownToHtml` keeps raw `🚀` → `HtmlToRtf` `GetFontForRun 435-449` scans `IsHighSurrogate`+`IsLowSurrogate` `441-442` → `Font("Segoe UI Emoji", baseFont->Size, style)` per-run, else `Segoe UI` base. **Do not** substitute `:rocket:`.

---

## 13. Link Handling

* **Inside release content:** `[text](https://example.com)` → `<a href="https://...">text</a>` → `HtmlToRtf 317-325` `Select + Font Underline + Color #0067B8`, `LinkClicked` (`UpdaterUI.cpp:82` `rtbNotes->LinkClicked+=OnNotesLinkClicked` → `OpenUrl(e->LinkText)` `UpdaterUI.cpp:154` allow-list `Scheme=="https" && Host=="github.com"` — broaden to `https://` any for release links, keep `http://` optional per task). Never `javascript:`/`data:`/`file:` → sanitized to `href="#"`.
* **Outside preview:** `LinkLabel linkViewOnGithub` (`view 63A8F8` lighter blue for dark Mica, `UpdaterUI.cpp:84` `AutoSize Text="View on GitHub"` below `pnlPreview` `Dock Bottom`, `Tag=HtmlUrl`, `LinkClicked→OnViewOnGithubClick` → `Process::Start(psi UseShellExecute true)`). Color `Color::FromArgb(99,168,248)` `#63A8F8`.

---

## 14. Cache Strategy

* **Preserve:** `UpdateState::GetCacheFilePath %APPDATA%\Windows Hello Fix\updater_cache.json` + `updater_etag.txt` via `GetCacheFilePath` `%APPDATA%`, `SaveCacheToDisk` atomic `tmp+Move`, `LoadCacheFromDisk` `File::ReadAllText` UTF-8, `TryDeserializeCacheJson` bridges `"tag"`→`"tag_name"`.
* **Fix poisoned cache:** `UpdateState.cpp:230-239` already deletes file if `Body.Contains("92r92n")` (from old `92r` bug). After `L'\\'` fix, new cache will be correct `CRLF` (`0D 0A`).
* **Offline:** `Updater::CheckThreadProc` → `FetchResult::NetworkError` → `SetStatus(Offline)` + `CacheUsed` banner if `LastCheckUtc <24h` else `Offline`; `HtmlRenderer::Render` on cached `Body` no network.

---

## 15. Mica/Translucency Strategy

* **Cause:** Previous `BackColor White 330` hard + `FromArgb` literals + separate `HWND` `TopMost false` + `RichTextBox` opaque without `SystemColors`.
* **Fix:** `UpdaterPopup: BackColor SystemColors::Window`, `ForeColor SystemColors::WindowText`, `DoubleBuffered true`, `Font Segoe UI 9`, no `TransparencyKey`/`Opacity`/`AllowTransparency`. `RichTextBox BackColor SystemColors::Window`, `SelectionBackColor = BackColor` (`HtmlRenderer.cpp:427,461`). `Panel pnlPreview` `BackColor Window`, `Border FixedSingle #E1E1E1`. No `WS_EX_LAYERED`/`UpdateLayeredWindow`; rely on `DwmExtendFrameIntoClientArea` inherited from owner via `EnableVisualStyles` (owner and popup share `SystemColors` + `DoubleBuffered`). Do not invent separate Mica `SetWindowAttribute` for updater — use same philosophy as `MyForm_Core.cpp:119-180` (default `Control`).
* **Dark/light:** No independent dark-mode framework; `SystemColors::Window` for `rtbNotes`, `SystemColors::ControlText` for glyph, `SystemColors::HotTrack` for links `#0067B8` (light) / `#63A8F8` for `View on GitHub` on dark. No hard `White`/`Black`. Code `BackColor #F3F3F3` kept but blend with `SystemColors::ControlLight` if high-contrast.

---

## 16. Dark/Light Appearance Strategy

* Investigated: `app.manifest:1-11` only `2.1.0.0` + `requireAdministrator`; `vcxproj:24` `4.7.2`; `MyForm_Core.cpp:119-180` no `BackColor`; `main.cpp:12` `EnableVisualStyles`. System `Control #F0F0F0` light, high-contrast via `SystemColors`.
* **Renderer:** `SystemColors::Window` for `rtbNotes`, `SystemColors::ControlText` for glyph, `SystemColors::HotTrack` for links `#0067B8` (light) / `#63A8F8` for `View on GitHub` on dark. No hard `White`/`Black`. Code `BackColor #F3F3F3` kept but blend with `SystemColors::ControlLight` if high-contrast.
* **Fix:** `SystemColors::Window` for `rtbNotes`, `SystemColors::ControlText` for glyph, `SystemColors::HotTrack` for links, `SystemColors::GrayText` for blockquote.

---

## 17. Security Strategy

* `HtmlRenderer::SanitizeHtml 33-87` strips `script,iframe,object,embed,style,link,meta,base,form,input,button,svg,math,canvas` via `Regex "<{0}[^>]*>"`, `on\w+=` attributes, `href="javascript:..."` → `href="#"` via `IsSafeUrl` (`https://`/`http://` only). Markdown `EscapeHtml` before wrapping prevents raw `<script>` injection. `RichTextBox DetectUrls false` + `LinkClicked` allow-list prevents `data:`/`file:` execution. `UpdateInstaller` HTTPS allow-list unchanged.

---

## 18. Files That Need Modification

```
src/updater/HtmlRenderer.h/.cpp  — FIX: heading size (wire AppendHeading), list indent 12px, p gap \n\n, • as L'\u2022', sanitize already, emoji already
src/updater/UpdaterUI.h          — REPLACE: add Panel pnlPreview, Label lblReleaseInfo/lblStatusInside, remove lblNotesHeader/pnlWarning(lblWarning)/lblStatus/lblStatusBanner separate, keep cmbRelease/btnUpdate/rtbNotes/linkViewOnGithub/progressBar
src/updater/UpdaterUI.cpp        — MAJOR: InitializeComponentPopup re-layout (cmb 68% + btn 30% same Y, pnlPreview Dock Fill with inside Release Info + status + rtbNotes, rtbNotes 336×~220 not 140, View on GitHub Dock Bottom inside preview, SystemColors.Window, DoubleBuffered, L'\u2022' for •, call HtmlRenderer::Render)
src/updater/UpdateModels.cpp     — FIXED (L'\\' etc) + keep 92r cache delete in UpdateState.cpp:230-239
src/updater/UpdateState.h/.cpp   — keep GetAllReleasesSorted (already), ensure RefreshStatus shows both Current+Latest Stable: "Current version: v2.1.0 - Latest Stable: v2.0.0"
Windows_Hello_Fix_v2_0.vcxproj/.filters — if HtmlRenderer renamed, no change (already HtmlRenderer)
docs/Plan.md                     — this Appendix (22 items)
```

---

## 19. Files That Must Remain Untouched

```text
src/core/* (7 files: MyForm.h, MyForm_Camera.cpp, MyForm_Config.cpp, MyForm_Core.cpp, MyForm_Events.cpp, MyForm_System.cpp, MyForm_UI.cpp) — byte-for-byte, no WndProc/camera changes
src/watchdog/* (4 files: CameraFailsafe.h/.cpp, RecoveryLoopFailsafe.h/.cpp) — byte-for-byte
main.cpp (updater owned outside src/core already, no new core lines)
x64/Release/install_script.nsi (rendering only, no Task Scheduler/installer change)
app.manifest (optional <dpiAware> not required)
```

---

## 20. Expected UI State Transitions

* `Idle → Checking (5s startup or 6h periodic or manual ⟳) → pulseOn` → `UpToDate (lblStatus "✓ You are on latest: v2.1.0" green #107C10, no dot) ` vs `UpdateAvailable (lblStatus "⚠ Newer available: v2.2.0" yellow #986F0B, dot #D13438, btnUpdate "Update")` → `Downloading (progressBar 8px #0078D4, lblStatusBanner "Downloading 45%...")` → `Installing (lblStatusBanner "Installing...")` → `Environment::Exit(0)` → NSIS. `Offline/Error/RateLimited` → `lblStatusBanner #D13438` + cached `rtbNotes` still rendered. `Selected older` → `pnlWarning` inline yellow text `⚠ v1.0/v2.0 does not include updater` (text only, no `36px` panel) above rendered body, `btnUpdate` text `Downgrade` + `ConfirmDowngradeIfNeeded` dialog.

---

## 21. Test Plan

| Area | Case | Expected |
|---|---|---|
| **Basic** | `v2.0` body (`🚀` `##`, lists `*`, `**`, `[]()`, `---`) | `MarkdownToHtml → HtmlToRtf` headings Bold 12/10.5/9.5, lists `•` `L'\u2022'`, no `92r` |
| | `v2.1`/`v1.0` | same, `v1.0` warning text yellow |
| | long (>200 lines) scroll, empty → “No release notes” | `rtbNotes 336×~220` vertical scroll, no horizontal overflow |
| | malformed `**unclosed` → plain | no crash, `try/catch` fallback |
| **Formatting** | `H1`/`H2`/`H3` distinct size/bold, `**bold**`/`*italic*`, `ul`/`ol`, `[link](https)` clickable, `` `code` `` `#F3F3F3` `Consolas`, ```` ``` ```` block `#F5F5F5`, `> blockquote` grey, `---` `─` | `SelectionFont/Color/BackColor` |
| **Unicode** | `🚀` `U+1F680` surrogate, `✓`/`⚠`/`⟳`/`•`, quotes `“”`, non-ASCII | `Segoe UI Emoji` per-run, no `â` |
| **Security** | `<script>alert(1)</script>`, `javascript:`, `data:`, `onload=` | stripped/escaped, not executed, links only `https` |
| **UI** | `Release [ ▼ ]` 68% + `Update` 30% same row, `Release Info` inside preview, status `Current - Latest Stable` inside top, title `🚀` large, `View on GitHub` inside bottom (`#63A8F8`), Mica/Translucent `SystemColors.Window` blends | no separate panels |
| **DPI** | 100/150/200% icon `20*scale` dot `6dp*scale` | `GetScaleFactor` `DpiX/96`, no `v` glyph |
| **Offline** | cached `<24h` → rendered, `92r92n` old cache deleted | `CacheUsed` banner |

---

## 22. Rollback Plan

* `git checkout HEAD -- src/updater/HtmlRenderer.* src/updater/UpdaterUI.* src/updater/UpdateModels.cpp src/updater/UpdateState.*` + `git checkout HEAD -- docs/Plan.md` + rebuild — `src/core` still zero. Or `MarkdownRenderer` fallback: if `HtmlRenderer::Render` throws, `rtb->Text = markdown` plain + `File::Delete(updater_cache.json)`.

# Appendix — Updater UI / Release Preview — Visual Reference Fixes (Issue #8) — PLAN ONLY

> **Status: DRAFT — Planning Complete 2026-09-01 — Implementation Pending Review**
> **Branch: `updater` (on top of HtmlRenderer Markdown→HTML)**
> **Visual reference: Second image (desired) vs first image (current buggy updater right side, black texts, button oversize, box cropped)**
> **Issue: https://github.com/Shivu516/Windows-Hello-Fix/issues/8**
> **Core policy: `src/core/` ZERO changes — `src/watchdog/` ZERO changes**

---

## 1. Current UI Architecture

`src/updater/UpdaterUI.h:46-87` / `UpdaterUI.cpp:58-150` — `UpdaterPopup : Form` `Size 360×440 FixedDialog SystemColors.Window DoubleBuffered` (`UpdaterUI.cpp:58-64`), `Font Segoe UI 9`. Top row: `Label "Release" 8pt #605E5C at (12,12) 60×21` + `ComboBox cmbRelease at (74,12) 200×21` (`65`) → `y+=28` → `Label lblStatus 336×16 9pt` (`72`) → `y+=18` → `Panel pnlWarning 336×36 #FFF8E1` (`74-76`) → `y+=40` → `Button btnUpdate at (12,y) 110×28` (`77`) left-aligned full-width → `y+=34` → `Label lblNotesHeader "Release notes" 8pt Bold #605E5C` (`79`) → `Panel separator 1px #E1E1E1` (`81`) → `RichTextBox rtbNotes 336×140 ReadOnly Vertical SystemColors.Window` (`82`) → `LinkLabel linkViewOnGithub` (`84`) outside `rtbNotes` below separator. **Result:** 6 separate surfaces, 62px waste between dropdown and button.

---

## 2. Current Rendering Pipeline

```
GitHub API  GET /releases?per_page=20 (ETag)
  → GitHubReleaseClient.cpp:136 ReadAsStringAsync  → UpdateModels::GetJsonStringField 140-175 (now L'\\' fixed) → UnescapeJsonString 73-101 → GitHubRelease.Body
  → UpdateState::GetAllReleasesSorted 112-144 → cmbRelease
  → UpdaterPopup::OnReleaseChanged → HtmlRenderer::Render(rtbNotes, markdown) (UpdaterUI.cpp:129)
  → HtmlRenderer.cpp:89-245 MarkdownToHtml (normalize \r\n→\n, ```→<pre>, #→<h1>, hr→<hr>, >→<blockquote>, ul/ol→<li>, p→<p>, **→<strong> etc) → SanitizeHtml 33-87 (strip <script/on*/javascript:) → HtmlToRtf 263-399 (Regex <(/?)(\w+)> flags inStrong/inEm/inCode/inPre/inA/inBlockquote/inH1.. + AppendTextWithStyle Segoe UI 9 / Consolas 8.5 / Segoe UI Emoji per surrogate)
  → RichTextBox rtbNotes 336×140 ReadOnly Vertical SystemColors.Window
```

---

## 3. Exact Causes of Visual Artifacts

* **Fragmented panels:** Current has `lblStatus` + `lblStatusBanner` + `lblNotesHeader` + `separator` + `rtbNotes` + `linkViewOnGithub` outside → 6 surfaces. Desired single `Panel pnlPreview` with `Release Info` inside.
* **Top row split:** `cmbRelease Y=pad` then `y+=28` then `lblStatus` then `pnlWarning` then `btnUpdate Y+=74` → 62px waste. Desired needs `cmbRelease 228×21 (68%)` + `btnUpdate 100×21 (30%)` same `Y=12`.
* **Header outside preview:** `lblNotesHeader "Release notes"` separate not inside `rtbNotes`; screenshot's `Release Info` inside preview.
* **Status outside preview:** `lblStatus` shows only one side. Desired shows both `Current version: v2.1.0 - Latest Stable: v2.0.0` inside preview near top.
* **Warning giant panel:** `pnlWarning 336×36 #FFF8E1` is 8.5% height — desired no giant panel, inline yellow text.
* **Heading size:** `HtmlToRtf 313` only `Color #1A1A1A` for `inH1`, not `FontSize 12pt Bold`. `AppendHeading` never called from `HtmlToRtf`.
* **Spacing:** `HtmlToRtf` emits single `\n` for `p/li/h1` → no readable gap; `•` as `L'\u2022'` vs `*` fallback.
* **Link outside vs inside:** `linkViewOnGithub` outside `rtbNotes` below separator; screenshot puts `[View on GitHub]` inside preview bottom.

---

## 4. Exact Cause of Unicode Corruption

* **Primary `92r92n`:** `UpdateModels.cpp:158-164` `sb->Append('\\')` with narrow `char 92` → `Append(int32)` → decimal `"92"`; `\r\n` → `"92r92n"` (`39 32 72 39 32 6E`) in `%APPDATA%\updater_cache.json`. **Fixed** to `L'\\'`.
* **Remaining `â€¢`:** Bullet `• U+2022` (`E2 80 A2` UTF-8) inside `HtmlRenderer.cpp` source file: if file saved as UTF-8 without BOM, `CL.exe /clr` may interpret `•` as ANSI `windows-1252` `â€¢`. Current `HtmlRenderer.cpp:347` `AppendText("  • ")` contains literal `•`. Fix: `L'\u2022'` or `"\u2022"` string, save file as UTF-8 BOM.

---

## 5. Current Updater Icon Implementation

**Already fixed** (`UpdaterUI.cpp:39-50`): `Panel 20×20*scale`, `GetScaleFactor()` via `CreateGraphics()->DpiX/96`, `OnIconPaint` vector GDI path (`Pen 1.5*scale`, Tray `Rect 4,14,12,2` + Stem `10,4→10,14` + Head triangle `6,12-14,12-10,18`, `FillPolygon`), `SmoothingMode AntiAlias`, `SystemDefault` hint, `SystemColors.ControlText` / `#005A9E` when `hasUpdate`, dot `6dp*scale #D13438` white `1*scale` border at `W-dot-1,1`. No `glyph="v"`, no `Segoe MDL2` font dependency, DPI-scaled, `Transparent` → `SystemColors.Control` with `DoubleBuffered`. **Preserve as-is.**

---

## 6. Target UI Derived from Supplied Image

**Proportions (360×~480, dark Mica, compact):**

```
┌──────────────────────────────┐  UpdaterPopup ClientSize 360×440 (Max 360×520) SystemColors.Window
│ [v2.0.0        ▼] [ Update ] │  Top row pad 12: cmbRelease 228×21 (68% of 336-8) at (12,12) | btnUpdate 100×21 (30%) at (248,12) same Y=12, Height 23
│ ┌──────────────────────────┐ │  Panel pnlPreview Location(12,40) ClientSize 336×300 (right 348 → now ClientSize 360 → 348 inside), BackColor SystemColors.Window, Border FixedSingle #E1E1E1, Padding 8, Dock Fill between top row and bottom link
│ │ Release Info             │ │  Label lblReleaseInfo Dock Top 20 Bold 11pt Bold White (dark) / #1A1A1A (light) inside preview
│ │ Current version: v2.1.0  │ │  Label lblStatusInside Dock Top 16 9pt #107C10 (up-to-date) / #986F0B (outdated) inside preview
│ │ - Latest Stable: v2.0.0  │ │  (single line, both values)
│ │ ! v2.0.0 does not ...    │ │  Label lblWarningInline Dock Top 28 8pt #986F0B Visible iff !HasUpdaterSupport, text only
│ │ 🚀 Version 2.0 - …       │ │  RichTextBox rtbNotes Dock Fill, 320×~180, ReadOnly Vertical SystemColors.Window, HtmlRenderer (h1 14pt Bold White/Black, p 9pt, ul • 12px indent, code Consolas 8.5, blockquote grey)
│ │ This major update is…    │ │
│ │ • Re-written core…       │ │
│ │ [View on GitHub]         │ │  LinkLabel Dock Bottom outside preview? Per task outside = below pnlPreview at (12,350) #63A8F8 (lighter blue for dark)
│ └──────────────────────────┘ │
└──────────────────────────────┘
```

*No channel selector, no repository preview, no giant warning panels.*

---

## 7. Release Dropdown Design

**Control:** `ComboBox cmbRelease DropDownList Segoe UI 9` `Location (12,12) Size 228×21` (68% of `336` inner), `FlatStyle` default. **Items:** `tag` only (`v2.1.0`, `v2.0.0`, `v1.0.0`…), sorted descending `Version.CompareTo` via `UpdateState::GetAllReleasesSorted()` (no channel filter), newest first, installed `✓` suffix (`tag + " ✓ installed"` if `CompareTo==0`). Default `SelectedIndex` preserves `prevTag` else `LatestForChannel` else `0`.

---

## 8. Update Button Placement

**Beside dropdown:** `Button btnUpdate Location = Point(cmbRelease.Right + 8, 12) Size 100×21` (30% width, same `Y`, height matches `ComboBox` 21, not 28). `UseVisualStyleBackColor true`, `FlatStyle Standard`. **Text dynamic:** `hasAsset` else `"No installer"`; `CompareTo` `<0→"Downgrade"`, `==0→"Reinstall"`, `>0→"Update"` (`UpdateButtonStates 131-139`). Shared top row `y=12` saves 62px.

---

## 9. Release Info Placement

**Inside preview:** No separate `lblNotesHeader` outside. Create `Panel pnlPreview Dock Fill` (fills between top row and bottom). Inside `pnlPreview`:

* `Label lblReleaseInfo Dock Top Height 22 Text "Release Info" Font Segoe UI 11 Bold ForeColor White/#1A1A1A` (adaptive, Bold White in dark)
* `Label lblStatusInside Dock Top Height 16 Font 9` `String::Format("Current version: {0} - Latest Stable: {1}", cur, latest?.Tag)` `ForeColor Green #107C10` or Yellow `#986F0B` (text only, no background)
* `RichTextBox rtbNotes Dock Fill` below them, `Margin 0`.

This makes status scroll with preview? Keep status outside scroll for always-visible: `pnlPreview.Controls.Add(lblReleaseInfo); Add(lblStatusInside); Add(rtbNotes)` with `rtbNotes Dock Fill` below two docked-tops → status stays top of preview, notes scroll.

---

## 10. HTML/Markdown Rendering Options

| Option | Fidelity | Emoji | Security | Mica | Deploy | Startup |
|---|---|---|---|---|---|---|
| **A. Markdown→RTF** (current `MarkdownRenderer` direct) | Good for subset | `Segoe UI Emoji` per-run | Strong (no HTML) | `SystemColors` inherits | 0 | 0ms |
| **B. Markdown→controlled HTML→RichTextBox** (chosen `HtmlRenderer` `MarkdownToHtml→Sanitize→HtmlToRtf`) | Same, plus HTML sanitization layer | Same | Controlled allow-list `h1/p/strong/em/code/pre/ul/ol/li/a/blockquote/hr/br` + strip `script/on*` + `href https` only | Same `SystemColors` | 0 | ~5ms |
| **C. WebView2** | Perfect | Perfect | Sandbox but `javascript:` via API, need CSP | GPU `HWND` opaque breaks Mica, extra process | 150MB runtime | 500ms |
| **D. GitHub Markdown API** | Perfect | Perfect via GitHub | GitHub sanitizes | Same HTML | Needs `Edge` | Fails offline (60/h) |
| **E. WebBrowser Trident** | IE7 quirks | Poor emoji | Worst (`ActiveX`) | Deprecated | Regkey elevation | 200ms |

---

## 11. Selected Rendering Strategy

**Keep `HtmlRenderer` pipeline: `Markdown → controlled HTML → RichTextBox` (`HtmlRenderer.h:9` `Render` = `MarkdownToHtml 89-245` + `SanitizeHtml 33-87` + `HtmlToRtf 263-399`).** This satisfies prompt “Markdown → controlled HTML → updater HTML renderer/control” with **controlled HTML** (allow-list `h1/h2/h3, p, strong/em, code/pre, ul/ol/li, a, blockquote, hr, br` + `EscapeHtml` before wrapping, `IsSafeUrl` `https://` only, dangerous tags stripped). Render to **native `RichTextBox`** (not `WebView2`/`WebBrowser`) — lightest, `SystemColors` inherits Mica, no `Edge` runtime, offline cache works, `GetFontForRun` emoji fallback.

**Fixes vs current `HtmlRenderer.cpp`:** Heading size (currently only `Color` `313`, not `FontSize` 12/10.5/9.5 via `AppendHeading` never called) → add `SelectionFont Size 12pt Bold` for `inH1` (wire `AppendHeading` style inline). List indent `12px` via `SelectionIndent`, paragraph `\n\n` gap, `•` as `L'\u2022'` not literal `•`.

---

## 12. Emoji Strategy

**Pipeline:** `GitHub API` UTF-8 `F0 9F 9A 80` → `ReadAsStringAsync` UTF-16 surrogate `0xD83D 0xDE80` → `GetJsonStringField` now `L'\\'` fix preserves pair → `EscapeHtml` preserves verbatim → `MarkdownToHtml` keeps raw `🚀` → `HtmlToRtf` `GetFontForRun 435-449` scans `IsHighSurrogate`+`IsLowSurrogate` `441-442` → `Font("Segoe UI Emoji", size)` per-run, else `Segoe UI` base. **Do not** substitute `:rocket:`.

**Full color:** `RichTextBox` GDI cannot render `COLR/CPAL` color (requires `DirectWrite`/`WebView2`). For “full color” per last prompt, keep `Segoe UI Emoji` fallback (monochrome but visible) and document tradeoff vs `WebView2` 150MB. Make `Release Info` colorful text via `HtmlRenderer` heading color adaptive (not emoji).

---

## 13. Link Handling

* **Inside release content:** `[text](https://example.com)` → `<a href="https://...">text</a>` → `HtmlToRtf 317-325` `Select + Font Underline + Color #0067B8`, `LinkClicked` (`UpdaterUI.cpp:82` `rtbNotes->LinkClicked+=OnNotesLinkClicked` → `OpenUrl(e->LinkText)` `UpdaterUI.cpp:154` allow-list `Scheme=="https" && Host=="github.com"` — broaden to `https://` any for release links, keep `http://` optional per task). Never `javascript:`/`data:`/`file:` → sanitized to `href="#"`.
* **Outside preview:** `LinkLabel linkViewOnGithub` (`view 63A8F8` lighter blue for dark Mica, `UpdaterUI.cpp:84` `AutoSize Text="View on GitHub"` below `pnlPreview` `Dock Bottom`, `Tag=HtmlUrl`, `LinkClicked→OnViewOnGithubClick` → `Process::Start(psi UseShellExecute true)`). Color `Color::FromArgb(99,168,248)` `#63A8F8`.

---

## 14. Cache Strategy

* **Preserve:** `UpdateState::GetCacheFilePath %APPDATA%\Windows Hello Fix\updater_cache.json` + `updater_etag.txt` via `GetCacheFilePath` `%APPDATA%`, `SaveCacheToDisk` atomic `tmp+Move`, `LoadCacheFromDisk` `File::ReadAllText` UTF-8, `TryDeserializeCacheJson` bridges `"tag"`→`"tag_name"`.
* **Fix poisoned cache:** `UpdateState.cpp:230-239` already deletes file if `Body.Contains("92r92n")` (from old `92r` bug). After `L'\\'` fix, new cache will be correct `CRLF` (`0D 0A`).
* **Offline:** `Updater::CheckThreadProc` → `FetchResult::NetworkError` → `SetStatus(Offline)` + `CacheUsed` banner if `LastCheckUtc <24h` else `Offline`; `HtmlRenderer::Render` on cached `Body` no network.

---

## 15. Mica/Translucency Strategy

* **Cause:** Previous `BackColor White 330` hard + `FromArgb` literals + separate `HWND` `TopMost false` + `RichTextBox` opaque without `SystemColors`.
* **Fix:** `UpdaterPopup: BackColor SystemColors::Window`, `ForeColor SystemColors::WindowText`, `DoubleBuffered true`, `Font Segoe UI 9`, no `TransparencyKey`/`Opacity`/`AllowTransparency`. `RichTextBox BackColor SystemColors::Window`, `SelectionBackColor = BackColor` (`HtmlRenderer.cpp:427,461`). `Panel pnlPreview` `BackColor Window`, `Border FixedSingle #E1E1E1`. No `WS_EX_LAYERED`/`UpdateLayeredWindow`; rely on `DwmExtendFrameIntoClientArea` inherited from owner via `EnableVisualStyles` (owner and popup share `SystemColors` + `DoubleBuffered`). Do not invent separate Mica `SetWindowAttribute` for updater — use same philosophy as `MyForm_Core.cpp:119-180` (default `Control`).

---

## 16. Dark/Light Appearance Strategy

* Investigated: `app.manifest:1-11` only `2.1.0.0` + `requireAdministrator`; `vcxproj:24` `4.7.2`; `MyForm_Core.cpp:119-180` no `BackColor`; `main.cpp:12` `EnableVisualStyles`. System `Control #F0F0F0` light, high-contrast via `SystemColors`.
* **Renderer:** `SystemColors::Window` for `rtbNotes`, `SystemColors::ControlText` for glyph, `SystemColors::HotTrack` for links `#0067B8` (light) / `#63A8F8` for `View on GitHub` on dark. No hard `White`/`Black`. Code `BackColor #F3F3F3` kept but blend with `SystemColors::ControlLight` if high-contrast.
* **Fix:** `SystemColors::Window` for `rtbNotes`, `SystemColors::ControlText` for glyph, `SystemColors::HotTrack` for links, `SystemColors::GrayText` for blockquote. Black texts `Release Info` etc should be **Bold White in dark mode**: detect `isDark = GetSysColor(COLOR_WINDOW) luminance <128 || ShouldAppsUseDarkMode (uxtheme 138)` → `lblReleaseInfo ForeColor = isDark ? White : #1A1A1A` Bold, `inH1` color same.

---

## 17. Security Strategy

* `HtmlRenderer::SanitizeHtml 33-87` strips `script,iframe,object,embed,style,link,meta,base,form,input,button,svg,math,canvas` via `Regex "<{0}[^>]*>"`, `on\w+=` attributes, `href="javascript:..."` → `href="#"` via `IsSafeUrl` (`https://`/`http://` only). Markdown `EscapeHtml` before wrapping prevents raw `<script>` injection. `RichTextBox DetectUrls false` + `LinkClicked` allow-list prevents `data:`/`file:` execution. `UpdateInstaller` HTTPS allow-list unchanged.

---

## 18. Files That Need Modification

```
src/updater/HtmlRenderer.h/.cpp  — FIX: heading size (wire AppendHeading), list indent 12px, p gap \n\n, • as L'\u2022', sanitize already, emoji already
src/updater/UpdaterUI.h          — REPLACE: add Panel pnlPreview, Label lblReleaseInfo/lblStatusInside/lblWarningInline (text only), keep cmbRelease/btnUpdate/rtbNotes/linkViewOnGithub/progressBar
src/updater/UpdaterUI.cpp        — MAJOR: InitializeComponentPopup re-layout (cmb 68% + btn 30% same Y, pnlPreview Dock Fill with inside Release Info + status + rtbNotes, rtbNotes 336×~220 not 140, View on GitHub Dock Bottom inside preview, SystemColors.Window, DoubleBuffered, L'\u2022' fix, call HtmlRenderer::Render)
src/updater/UpdateModels.cpp     — FIXED (L'\\' etc) + keep 92r cache delete in UpdateState.cpp:230-239
src/updater/UpdateState.h/.cpp   — keep GetAllReleasesSorted (already), ensure RefreshStatus shows both Current+Latest Stable: "Current version: v2.1.0 - Latest Stable: v2.0.0"
Windows_Hello_Fix_v2_0.vcxproj/.filters — if HtmlRenderer renamed, no change (already HtmlRenderer)
docs/Plan.md                     — this Appendix (22 items)
```

---

## 19. Files That Must Remain Untouched

```text
src/core/* (7 files: MyForm.h, MyForm_Camera.cpp, MyForm_Config.cpp, MyForm_Core.cpp, MyForm_Events.cpp, MyForm_System.cpp, MyForm_UI.cpp) — byte-for-byte, no WndProc/camera changes
src/watchdog/* (4 files: CameraFailsafe.h/.cpp, RecoveryLoopFailsafe.h/.cpp) — byte-for-byte
main.cpp (updater owned outside src/core already, no new core lines)
x64/Release/install_script.nsi (rendering only, no Task Scheduler/installer change)
app.manifest (optional <dpiAware> not required)
```

---

## 20. Expected UI State Transitions

* `Idle → Checking (5s startup or 6h periodic or manual ⟳) → pulseOn` → `UpToDate (lblStatusInside "✓ You are on latest: v2.1.0" green #107C10, no dot) ` vs `UpdateAvailable (lblStatusInside "⚠ Newer available: v2.2.0" yellow #986F0B, dot #D13438, btnUpdate "Update")` → `Downloading (progressBar 8px #0078D4, lblWarningInline "Downloading 45%...")` → `Installing (lblWarningInline "Installing...")` → `Environment::Exit(0)` → NSIS. `Offline/Error/RateLimited` → `lblWarningInline #D13438` + cached `rtbNotes` still rendered. `Selected older` → `lblWarningInline` inline yellow text `⚠ v1.0/v2.0 does not include updater` (text only, no `36px` panel) above rendered body, `btnUpdate` text `Downgrade` + `ConfirmDowngradeIfNeeded` dialog.

---

## 21. Test Plan

| Area | Case | Expected |
|---|---|---|
| **Basic** | `v2.0` body (`🚀` `##`, lists `*`, `**`, `[]()`, `---`) | `MarkdownToHtml → HtmlToRtf` headings Bold 12/10.5/9.5, lists `•` `L'\u2022'`, no `92r` |
| | `v2.1`/`v1.0` | same, `v1.0` warning text yellow |
| | long (>200 lines) scroll, empty → “No release notes” | `rtbNotes 336×~220` vertical scroll, no horizontal overflow |
| | malformed `**unclosed` → plain | no crash, `try/catch` fallback |
| **Formatting** | `H1`/`H2`/`H3` distinct size/bold, `**bold**`/`*italic*`, `ul`/`ol`, `[link](https)` clickable, `` `code` `` `#F3F3F3` `Consolas`, ```` ``` ```` block `#F5F5F5`, `> blockquote` grey, `---` `─` | `SelectionFont/Color/BackColor` |
| **Unicode** | `🚀` `U+1F680` surrogate, `✓`/`⚠`/`⟳`/`•`, quotes `“”`, non-ASCII | `Segoe UI Emoji` per-run, no `â` |
| **Security** | `<script>alert(1)</script>`, `javascript:`, `data:`, `onload=` | stripped/escaped, not executed, links only `https` |
| **UI** | `Release [ ▼ ]` 68% + `Update` 30% same row, `Release Info` inside preview, status `Current - Latest Stable` inside top, title `🚀` large, `View on GitHub` inside bottom (`#63A8F8`), Mica/Translucent `SystemColors.Window` blends | no separate panels |
| **DPI** | 100/150/200% icon `20*scale` dot `6dp*scale` | `GetScaleFactor` `DpiX/96`, no `v` glyph |
| **Offline** | cached `<24h` → rendered, `92r92n` old cache deleted | `CacheUsed` banner |

---

## 22. Rollback Plan

* `git checkout HEAD -- src/updater/HtmlRenderer.* src/updater/UpdaterUI.* src/updater/UpdateModels.cpp src/updater/UpdateState.*` + `git checkout HEAD -- docs/Plan.md` + rebuild — `src/core` still zero. Or `MarkdownRenderer` fallback: if `HtmlRenderer::Render` throws, `rtb->Text = markdown` plain + `File::Delete(updater_cache.json)`.


