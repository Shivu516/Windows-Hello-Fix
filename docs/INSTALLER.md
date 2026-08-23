# NSIS Installer (as-built)

Baseline: branch `test`, commit `acc37d8`. Source:
`x64/Release/install_script.nsi` (NSIS, MUI2 + LogicLib). Documented only —
not modified.

## 1. Package identity

| Item | Value |
|---|---|
| Output | `Windows_Hello_Fix_Setup.exe` |
| Name | "Windows Hello Fix v2.0" |
| Install directory | `$PROGRAMFILES64\WindowsHelloFix` |
| Elevation | `RequestExecutionLevel admin` (whole installer runs elevated) |
| UI | MUI2: License (`LICENCE.rtf`) → Components → Directory → InstFiles → Finish; separate uninstall confirm/instfiles pages |
| Icon | `WindowsHelloFix.ico` for installer and uninstaller |

`.onInit`:
1. Creates mutex `WindowsHelloFixSetup_Mutex` (**no `Global\` prefix** →
   per-session scope). If it already exists, tries to surface an existing
   setup window (`FindWindow "#32770" "Windows Hello Fix v2.0 Setup"`), shows
   a message box, and aborts.
2. Reads `HKLM\Software\Microsoft\Windows\CurrentVersion\Uninstall\
   WindowsHelloFix\UninstallString` (64-bit view); if present asks
   reinstall-yes/no (default Yes via `/SD IDYES`).

## 2. Files installed

Section `SEC01` "Core Files (Required)" (`SectionIn RO`), with
`SetShellVarContext all`:

```
$INSTDIR\Windows_Hello_Fix_v2_0.exe
$INSTDIR\Windows_Hello_Fix_v2_0.exe.metagen
$INSTDIR\WindowsHelloFix.ico
$INSTDIR\README.html
$INSTDIR\LICENCE.rtf
$INSTDIR\Uninstall.exe            (WriteUninstaller at end)
```

Pre-file steps: delete legacy per-user desktop shortcut, then
`nsExec::Exec 'taskkill /F /IM Windows_Hello_Fix_v2_0.exe /T'` +
`Sleep 1500` to stop any running app before swapping binaries.

Post-file steps:

- `powershell Unblock-File` on the exe (SmartScreen/Zone.Identifier cleanup).
- `SetShellVarContext current`; `CreateDirectory $APPDATA\Windows Hello Fix`;
  create empty `$APPDATA\Windows Hello Fix\diagnostic.log`.
- Run `"$INSTDIR\Windows_Hello_Fix_v2_0.exe" /restore-camera`
  (`Sleep 3000` after) — camera ghost-state cleanup pass.

## 3. Shortcuts

| Shortcut | Target | Notes |
|---|---|---|
| `$SMPROGRAMS\Windows Hello Fix\Windows Hello Fix.lnk` | exe, **no arguments**, icon from installed .ico | Always created |
| `$SMPROGRAMS\Windows Hello Fix\Uninstall Windows Hello Fix.lnk` | `$INSTDIR\Uninstall.exe` | Always created |
| `$DESKTOP\Windows Hello Fix.lnk` | exe | Section `SEC02`, created **unchecked by default** (`Section /o`) |

No arguments on the Start Menu link ⇒ manual launches show the interactive GUI
(see STARTUP.md #1).

## 4. Registry entries

Written (64-bit view):

- `HKLM\Software\Microsoft\Windows\CurrentVersion\Uninstall\WindowsHelloFix`:
  `DisplayName`, `UninstallString = "$INSTDIR\Uninstall.exe"`,
  `DisplayIcon`, `Publisher = Shivu516`.

Deleted by design:

- `HKLM\…\CurrentVersion\Run\WindowsHelloFix` — the installer deliberately
  does **not** use Run keys; it removes any legacy value instead.
- `AppCompatFlags\Layers` value for the exe in both HKLM and HKCU — explicitly
  avoids `RUNASADMIN` compatibility flags ("they can force a visible UAC
  prompt and fight Task Scheduler elevation").

No Startup-folder shortcuts are created.

## 5. Scheduled task registration

Stale tasks are wiped first (`schtasks /Delete /TN … /F` for `WindowsHelloFix`,
`WindowsHelloFix_Lock`, `WindowsHelloFix_Unlock`, `WindowsHelloFix_LogCleanup`;
`Sleep 1000`). Then a PowerShell script is generated into `$PLUGINSDIR`
(`RegisterWindowsHelloFixTasks.ps1`) and executed with
`powershell -NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden`.

The script creates four tasks (full parameter analysis in
`TASK_SCHEDULER.md`):

| Task | Mechanism | Trigger | Arguments |
|---|---|---|---|
| `WindowsHelloFix` | `Register-ScheduledTask` | At logon | `--background` |
| `WindowsHelloFix_Lock` | Schedule.Service COM (`RegisterTaskDefinition`, create-or-update=6) | Session state change **7** (lock) | `--disable-camera` |
| `WindowsHelloFix_Unlock` | same COM helper | Session state change **8** (unlock) | `--enable-camera` |
| `WindowsHelloFix_LogCleanup` | `Register-ScheduledTask` | Daily 00:00 | `cmd.exe /c break > "$APPDATA\Windows Hello Fix\diagnostic.log"` |

All four run as the installing user with highest privileges; see
TASK_SCHEDULER.md for the exact settings and one suspected defect in the
log-cleanup command.

After task creation: `Sleep 2000`, second warm-up
`"$INSTDIR\Windows_Hello_Fix_v2_0.exe" /restore-camera` (`Sleep 2500`),
then `WriteUninstaller`.

## 6. MUI_FINISHPAGE_RUN behavior

```
!define MUI_FINISHPAGE_RUN "$INSTDIR\Windows_Hello_Fix_v2_0.exe"
!insertmacro MUI_PAGE_FINISH
```

- `MUI_FINISHPAGE_RUN_PARAMETERS` is **not defined** → finish-page launch runs
  the exe **with no arguments**.
- Consequence: launched process inherits the elevated installer token, shows
  the normal visible GUI, follows STARTUP.md path #2. It will coexist with
  nothing else (installer killed prior instances), and its Load handler may
  auto-start monitoring if config says so (fresh installs write no config).

## 7. Uninstall behavior (`Section "Uninstall"`)

Order of operations, exactly as scripted:

1. If the exe exists: run `/restore-camera`, `Sleep 3000` ("pre-kill"
   hardware safety restore).
2. `taskkill /F /IM Windows_Hello_Fix_v2_0.exe /T`, `Sleep 1500`.
3. If the exe still exists: run `/restore-camera` again, `Sleep 3000`
   ("post-kill redundancy sweep").
4. Delete scheduled tasks: `WindowsHelloFix`, `WindowsHelloFix_Wake`
   *(legacy name never created by this script)*, `_Lock`, `_Unlock`,
   `_LogCleanup`.
5. Registry cleanup: Run value `WindowsHelloFix`, uninstall key,
   AppCompatFlags Layers values (HKLM+HKCU).
6. Delete files: exe, `.metagen`, `.ico`, `README.rtf` *(note: installer
   deployed `README.html`, not `README.rtf`)*, `LICENCE.rtf`, `config.txt`,
   `Uninstall.exe`.
7. Delete desktop shortcuts (current + all context) and Start Menu shortcuts;
   remove Start Menu folder.
8. AppData cleanup (per-user context): delete
   `%APPDATA%\Windows Hello Fix\config.txt` and `diagnostic.log`;
   `RMDir /r` of `$APPDATA\Windows_Hello_Fix` (underscore variant),
   `$APPDATA\Windows Hello Fix`, `$APPDATA\WindowsHelloFix` — two of the three
   names never exist; harmless belt-and-braces.
9. `RMDir $INSTDIR` (fails silently if non-empty).

## 8. Uncertainties / observed oddities (documented, NOT fixed)

1. Uninstall deletes `README.rtf` but install ships `README.html` → README
   leftover in `$INSTDIR` after uninstall.
2. The LogCleanup action string uses `$APPDATA` (PowerShell-style variable)
   inside a `cmd.exe` argument; `cmd.exe` does not expand `$VAR` syntax, so
   the daily truncation may target a literal `$APPDATA\...` path under the
   task's working directory rather than the real log. Needs runtime
   verification — flagged here only.
3. Installer mutex is session-local, so two users in different sessions could
   theoretically run setups simultaneously.
4. `MUI_FINISHPAGE_RUN` has no parameters definition; if someone later adds
   `MUI_FINISHPAGE_RUN_PARAMETERS`, note the finish launch intentionally stays
   interactive today.
