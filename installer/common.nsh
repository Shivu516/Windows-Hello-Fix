; common.nsh — shared Windows Hello Fix v2.1 installer logic.
;
; Behavioral reference: x64/Release/install_script.nsi (authoritative v2.0/v2.1 installer).
; This file contains NO architecture-specific payload selection itself. The thin
; wrapper scripts (install_x86.nsi / install_x64.nsi / install_universal.nsi)
; define how the payload reaches $INSTDIR, then all three share the macros below:
;
;   SHARED INSTALLATION BEHAVIOR
;     -> ARCHITECTURE-SPECIFIC PAYLOAD (per-script DEPLOY macro)
;     -> ARCHITECTURE-SPECIFIC INSTALL LOCATION / REGISTRY VIEW ($INSTDIR + SetRegView)
;
; Parent script requirements (before !including this file):
;   !include "MUI2.nsh"     (parent includes MUI first so MUI_* defines below work)
;   !include "LogicLib.nsh"
;   !include "x64.nsh"      (for ${RunningX64} in the universal script)
;   !define APP_ARCH_X86 | APP_ARCH_X64 | UNIVERSAL   (exactly one)
;   Standalone: !define APP_EXE_NAME / APP_METAGEN_NAME (compile-time payload names)
;
; Runtime variables owned by this file: $AppExeName / $AppMetagenName.
; Standalone scripts StrCpy them from !defines in .onInit; the universal script
; selects them with ${RunningX64} so exactly ONE native binary is installed.
; Future ARM64: add a third payload + a RunningARM64 branch (additive, no redesign).

Var AppExeName
Var AppMetagenName

; --- Known executable identities (upgrade safety, PART 14) ---
; Old release line plus both v2.1 architecture payloads. Install/uninstall logic
; must handle all three so renaming v2_0 -> v2_1_x86/x64 never orphans files,
; processes, shortcuts, tasks or AppCompat entries.
!define OLD_EXE_NAME "Windows_Hello_Fix_v2_0.exe"
!define OLD_METAGEN_NAME "Windows_Hello_Fix_v2_0.exe.metagen"
!define X86_EXE_NAME "Windows_Hello_Fix_v2_1_x86.exe"
!define X86_METAGEN_NAME "Windows_Hello_Fix_v2_1_x86.exe.metagen"
!define X64_EXE_NAME "Windows_Hello_Fix_v2_1_x64.exe"
!define X64_METAGEN_NAME "Windows_Hello_Fix_v2_1_x64.exe.metagen"

; --- Registry view for this installation ---
; Standalone scripts resolve at compile time; universal resolves at runtime.
!macro SET_ARCH_REGVIEW
  !ifdef UNIVERSAL
    ${If} ${RunningX64}
      SetRegView 64
    ${Else}
      SetRegView 32
    ${EndIf}
  !else
    !ifdef APP_ARCH_X86
      SetRegView 32
    !else
      SetRegView 64
    !endif
  !endif
!macroend

; --- Shared .onInit: single-instance + reinstall detection (both registry views) ---
!macro COMMON_ONINIT_BODY
  ; 1. THE BULLETPROOF SINGLE INSTANCE CHECK (unchanged)
  System::Call 'kernel32::CreateMutex(p 0, i 0, t "WindowsHelloFixSetup_Mutex") p .r1 ?e'
  Pop $0

  ${If} $0 != 0
    FindWindow $1 "#32770" "Windows Hello Fix v2.1 Setup"
    ${If} $1 != 0
      ShowWindow $1 5
      BringToFront
    ${EndIf}
    MessageBox MB_OK|MB_ICONEXCLAMATION "Another instance of the installer is already running."
    Abort
  ${EndIf}

  ; 2. REINSTALLATION CLEANUP PRE-CHECK (both views: handles x86<->x64 upgrades)
  SetRegView 64
  ReadRegStr $0 HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\WindowsHelloFix" "UninstallString"
  SetRegView 32
  ReadRegStr $1 HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\WindowsHelloFix" "UninstallString"
  SetRegView default

  ${If} $0 != ""
  ${OrIf} $1 != ""
    MessageBox MB_YESNO|MB_ICONQUESTION "Windows Hello Fix is already installed.$\n$\nDo you want to reinstall and overwrite the current version?" /SD IDYES IDYES keep_going
    Abort
    keep_going:
  ${EndIf}
!macroend

; --- Remove a possibly-stale payload of the OTHER architecture / old release ---
; Called after SetOutPath "$INSTDIR", before the new payload lands, so a same-directory
; arch switch or v2_0 -> v2_1 upgrade never leaves a stale exe behind.
!macro CLEAN_STALE_PAYLOADS
  Delete "$INSTDIR\${OLD_EXE_NAME}"
  Delete "$INSTDIR\${OLD_METAGEN_NAME}"
  Delete "$INSTDIR\${X86_EXE_NAME}"
  Delete "$INSTDIR\${X86_METAGEN_NAME}"
  Delete "$INSTDIR\${X64_EXE_NAME}"
  Delete "$INSTDIR\${X64_METAGEN_NAME}"
!macroend

; --- Terminate every known app identity (old + both arches) before swapping binaries ---
!macro KILL_ALL_KNOWN_EXES
  nsExec::Exec 'taskkill /F /IM ${OLD_EXE_NAME} /T'
  nsExec::Exec 'taskkill /F /IM ${X86_EXE_NAME} /T'
  nsExec::Exec 'taskkill /F /IM ${X64_EXE_NAME} /T'
  Sleep 1500
!macroend

; --- Shared core-install body. Requires: $AppExeName/$AppMetagenName set,
; --- payload already deployed to $INSTDIR. Mirrors install_script.nsi SEC01.
!macro CORE_INSTALL_BODY
  ; Ensure installer operates in elevated context throughout install session
  SetShellVarContext all

  ; Clean up old loose desktop links before overwrite
  Delete "$DESKTOP\Windows Hello Fix.lnk"
  SetShellVarContext all
  Delete "$DESKTOP\Windows Hello Fix.lnk"
  SetShellVarContext current

  ; Terminate any running active instances of the engine before swapping binaries
  !insertmacro KILL_ALL_KNOWN_EXES

  SetOutPath "$INSTDIR"

  ; Deploy documentation assets (arch-neutral, shared source)
  File "..\x64\Release\WindowsHelloFix.ico"
  File "..\x64\Release\README.html"
  File "..\x64\Release\LICENCE.rtf"

  ; --- CRITICAL PRODUCTION FIX ---
  ; Explicitly unblock downloaded binaries to avoid SmartScreen / Zone.Identifier privilege weirdness
  nsExec::ExecToLog 'powershell -WindowStyle Hidden -Command "Unblock-File -Path \"$INSTDIR\$AppExeName\" -ErrorAction SilentlyContinue"'

  ; Explicitly provision local AppData configuration tracking directories
  SetShellVarContext current
  CreateDirectory "$APPDATA\Windows Hello Fix"

  ; Pre-create diagnostic log to avoid first-launch permission edge cases
  FileOpen $0 "$APPDATA\Windows Hello Fix\diagnostic.log" w
  FileClose $0

  ; Clear any hardware ghost lock states prior to registration by resetting the driver stack
  nsExec::ExecToLog '"$INSTDIR\$AppExeName" /restore-camera'
  Sleep 3000

  ; Create Start Menu shortcuts safely inside isolated directory layouts
  CreateDirectory "$SMPROGRAMS\Windows Hello Fix"
  CreateShortcut "$SMPROGRAMS\Windows Hello Fix\Windows Hello Fix.lnk" "$INSTDIR\$AppExeName" "" "$INSTDIR\WindowsHelloFix.ico" 0
  CreateShortcut "$SMPROGRAMS\Windows Hello Fix\Uninstall Windows Hello Fix.lnk" "$INSTDIR\Uninstall.exe" "" "$INSTDIR\WindowsHelloFix.ico" 0

  ; Write registry configuration keys for Add/Remove Programs control interface
  !insertmacro SET_ARCH_REGVIEW
  DeleteRegValue HKLM "Software\Microsoft\Windows\CurrentVersion\Run" "WindowsHelloFix"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\WindowsHelloFix" "DisplayName" "Windows Hello Fix v2.1"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\WindowsHelloFix" "UninstallString" '"$INSTDIR\Uninstall.exe"'
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\WindowsHelloFix" "DisplayIcon" "$INSTDIR\WindowsHelloFix.ico"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\WindowsHelloFix" "Publisher" "Shivu516"

  ; --- SILENT STARTUP ELEVATION FIX ---
  ; Do not set RUNASADMIN compatibility flags. They can force a visible UAC prompt and fight Task Scheduler elevation.
  ; Clean stale flags for every known exe identity (upgrade safety).
  DeleteRegValue HKLM "Software\Microsoft\Windows NT\CurrentVersion\AppCompatFlags\Layers" "$INSTDIR\${OLD_EXE_NAME}"
  DeleteRegValue HKCU "Software\Microsoft\Windows NT\CurrentVersion\AppCompatFlags\Layers" "$INSTDIR\${OLD_EXE_NAME}"
  DeleteRegValue HKLM "Software\Microsoft\Windows NT\CurrentVersion\AppCompatFlags\Layers" "$INSTDIR\${X86_EXE_NAME}"
  DeleteRegValue HKCU "Software\Microsoft\Windows NT\CurrentVersion\AppCompatFlags\Layers" "$INSTDIR\${X86_EXE_NAME}"
  DeleteRegValue HKLM "Software\Microsoft\Windows NT\CurrentVersion\AppCompatFlags\Layers" "$INSTDIR\${X64_EXE_NAME}"
  DeleteRegValue HKCU "Software\Microsoft\Windows NT\CurrentVersion\AppCompatFlags\Layers" "$INSTDIR\${X64_EXE_NAME}"

  SetRegView default

  ; --- AUTOMATED SYSTEM TASK REGISTRATION ENGINE (unchanged semantics) ---

  ; Wipe away duplicate stale task configurations
  nsExec::ExecToLog 'schtasks /Delete /TN "WindowsHelloFix" /F'
  nsExec::ExecToLog 'schtasks /Delete /TN "WindowsHelloFix_Lock" /F'
  nsExec::ExecToLog 'schtasks /Delete /TN "WindowsHelloFix_Unlock" /F'
  nsExec::ExecToLog 'schtasks /Delete /TN "WindowsHelloFix_LogCleanup" /F'
  Sleep 1000

  ; --- CRITICAL TASK SCHEDULER FIXES ---
  ; Generate a PowerShell registration script instead of using fragile one-line shell quoting.
  ; This creates the background logon task plus lock/unlock failsafe tasks with highest privileges.
  InitPluginsDir
  FileOpen $1 "$PLUGINSDIR\RegisterWindowsHelloFixTasks.ps1" w
  FileWrite $1 "$$exe = '$INSTDIR\$AppExeName'$\r$\n"
  FileWrite $1 "$$wd = '$INSTDIR'$\r$\n"
  FileWrite $1 "$$user = [System.Security.Principal.WindowsIdentity]::GetCurrent().Name$\r$\n"
  FileWrite $1 "$$action = New-ScheduledTaskAction -Execute $$exe -Argument '--background' -WorkingDirectory $$wd$\r$\n"
  FileWrite $1 "$$trigger = New-ScheduledTaskTrigger -AtLogOn$\r$\n"
  FileWrite $1 "$$principal = New-ScheduledTaskPrincipal -UserId $$user -LogonType Interactive -RunLevel Highest$\r$\n"
  FileWrite $1 "$$settings = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries -StartWhenAvailable -MultipleInstances IgnoreNew -ExecutionTimeLimit (New-TimeSpan -Seconds 0) -Priority 4$\r$\n"
  FileWrite $1 "Register-ScheduledTask -TaskName 'WindowsHelloFix' -Action $$action -Trigger $$trigger -Principal $$principal -Settings $$settings -Force | Out-Null$\r$\n"
  FileWrite $1 "function Register-WhfSessionTask([string]$$name, [int]$$stateChange, [string]$$arguments) {$\r$\n"
  FileWrite $1 "  $$service = New-Object -ComObject 'Schedule.Service'$\r$\n"
  FileWrite $1 "  $$service.Connect()$\r$\n"
  FileWrite $1 "  $$root = $$service.GetFolder('\')$\r$\n"
  FileWrite $1 "  $$task = $$service.NewTask(0)$\r$\n"
  FileWrite $1 "  $$task.RegistrationInfo.Author = $$user$\r$\n"
  FileWrite $1 "  $$task.Principal.UserId = $$user$\r$\n"
  FileWrite $1 "  $$task.Principal.LogonType = 3$\r$\n"
  FileWrite $1 "  $$task.Principal.RunLevel = 1$\r$\n"
  FileWrite $1 "  $$trigger = $$task.Triggers.Create(11)$\r$\n"
  FileWrite $1 "  $$trigger.StateChange = $$stateChange$\r$\n"
  FileWrite $1 "  $$trigger.UserId = $$user$\r$\n"
  FileWrite $1 "  $$trigger.Enabled = $$true$\r$\n"
  FileWrite $1 "  $$action = $$task.Actions.Create(0)$\r$\n"
  FileWrite $1 "  $$action.Path = $$exe$\r$\n"
  FileWrite $1 "  $$action.Arguments = $$arguments$\r$\n"
  FileWrite $1 "  $$action.WorkingDirectory = $$wd$\r$\n"
  FileWrite $1 "  $$task.Settings.Enabled = $$true$\r$\n"
  FileWrite $1 "  $$task.Settings.Hidden = $$true$\r$\n"
  FileWrite $1 "  $$task.Settings.DisallowStartIfOnBatteries = $$false$\r$\n"
  FileWrite $1 "  $$task.Settings.StopIfGoingOnBatteries = $$false$\r$\n"
  FileWrite $1 "  $$task.Settings.StartWhenAvailable = $$true$\r$\n"
  FileWrite $1 "  $$task.Settings.MultipleInstances = 2$\r$\n"
  FileWrite $1 "  $$task.Settings.ExecutionTimeLimit = 'PT5M'$\r$\n"
  FileWrite $1 "  $$task.Settings.Priority = 4$\r$\n"
  FileWrite $1 "  $$root.RegisterTaskDefinition($$name, $$task, 6, $$null, $$null, 3, $$null) | Out-Null$\r$\n"
  FileWrite $1 "}$\r$\n"
  FileWrite $1 "Register-WhfSessionTask 'WindowsHelloFix_Lock' 7 '--disable-camera'$\r$\n"
  ; WindowsHelloFix_Unlock — STARTUP/SIGN-IN recovery helper (NO LONGER ordinary unlock).
  ; Fires once at logon (delay PT10S to survive S0/FastStartup zero-delay drop) and NOT on Win+L.
  ; Uses existing --enable-camera (IsRestoreCameraCommand early-exit at MyForm_Core.cpp, enable-only via Recover) — no new flag.
  FileWrite $1 "$$unlockAction = New-ScheduledTaskAction -Execute $$exe -Argument '--enable-camera' -WorkingDirectory $$wd$\r$\n"
  FileWrite $1 "$$unlockTrigger = New-ScheduledTaskTrigger -AtLogOn$\r$\n"
  FileWrite $1 "$$unlockTrigger.Delay = 'PT10S'$\r$\n"
  FileWrite $1 "$$unlockPrincipal = New-ScheduledTaskPrincipal -UserId $$user -LogonType Interactive -RunLevel Highest$\r$\n"
  FileWrite $1 "$$unlockSettings = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries -StartWhenAvailable -MultipleInstances IgnoreNew -ExecutionTimeLimit (New-TimeSpan -Minutes 1) -Priority 4$\r$\n"
  FileWrite $1 "$$unlockTask = New-ScheduledTask -Action $$unlockAction -Trigger $$unlockTrigger -Principal $$unlockPrincipal -Settings $$unlockSettings$\r$\n"
  FileWrite $1 "$$unlockTask.Description = 'Windows Hello Fix startup/sign-in recovery helper: verifies the IR camera is enabled after sign-in and recovers it if disabled. Not for ordinary Win+L unlock (handled by WndProc).'$\r$\n"
  FileWrite $1 "$$unlockTask.Settings.Hidden = $$true$\r$\n"
  FileWrite $1 "Register-ScheduledTask -TaskName 'WindowsHelloFix_Unlock' -InputObject $$unlockTask -Force | Out-Null$\r$\n"
  FileWrite $1 "$$cleanupAction = New-ScheduledTaskAction -Execute 'cmd.exe' -Argument '/c break > $\"$APPDATA\Windows Hello Fix\diagnostic.log$\"'$\r$\n"
  FileWrite $1 "$$cleanupTrigger = New-ScheduledTaskTrigger -Daily -At 00:00$\r$\n"
  FileWrite $1 "Register-ScheduledTask -TaskName 'WindowsHelloFix_LogCleanup' -Action $$cleanupAction -Trigger $$cleanupTrigger -Principal $$principal -Settings $$settings -Force | Out-Null$\r$\n"
  FileClose $1
  nsExec::ExecToLog 'powershell -NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File "$PLUGINSDIR\RegisterWindowsHelloFixTasks.ps1"'

  ; --- STARTUP RELIABILITY DELAY FIX ---
  ; Give Windows device stack time to stabilize after install before first launch
  Sleep 2000

  ; Warm-up recovery pass before handing control to user
  nsExec::ExecToLog '"$INSTDIR\$AppExeName" /restore-camera'
  Sleep 2500

  WriteUninstaller "$INSTDIR\Uninstall.exe"
!macroend

; --- Shared desktop-shortcut body (optional section, unchecked by default) ---
!macro DESKTOP_SHORTCUT_BODY
  SetShellVarContext all
  CreateShortcut "$DESKTOP\Windows Hello Fix.lnk" "$INSTDIR\$AppExeName" "" "$INSTDIR\WindowsHelloFix.ico"
  SetShellVarContext current
!macroend

; --- Shared uninstall body. Removes every known payload identity + both reg views. ---
!macro CORE_UNINSTALL_BODY
  ; Self-healing hardware check: verification safety loop to make absolutely sure the camera driver is working before removing app files
  ; Try the current payload first, then fall back to any stale identity still on disk.
  IfFileExists "$INSTDIR\$AppExeName" 0 skip_pre_kill_camera_restore
    nsExec::ExecToLog '"$INSTDIR\$AppExeName" /restore-camera'
    Sleep 3000
  skip_pre_kill_camera_restore:
  IfFileExists "$INSTDIR\${OLD_EXE_NAME}" 0 skip_pre_kill_legacy_restore
    nsExec::ExecToLog '"$INSTDIR\${OLD_EXE_NAME}" /restore-camera'
    Sleep 3000
  skip_pre_kill_legacy_restore:

  ; Terminate any active process instance safely (all known identities)
  !insertmacro KILL_ALL_KNOWN_EXES

  ; Final defensive redundancy sweep to maximize driver stability
  IfFileExists "$INSTDIR\$AppExeName" 0 skip_post_kill_camera_restore
    nsExec::ExecToLog '"$INSTDIR\$AppExeName" /restore-camera'
    Sleep 3000
  skip_post_kill_camera_restore:

  ; Purge registered task arrays
  nsExec::ExecToLog 'schtasks /Delete /TN "WindowsHelloFix" /F'
  nsExec::ExecToLog 'schtasks /Delete /TN "WindowsHelloFix_Wake" /F'
  nsExec::ExecToLog 'schtasks /Delete /TN "WindowsHelloFix_Lock" /F'
  nsExec::ExecToLog 'schtasks /Delete /TN "WindowsHelloFix_Unlock" /F'
  nsExec::ExecToLog 'schtasks /Delete /TN "WindowsHelloFix_LogCleanup" /F'

  ; Clean registry values (both views: handles cross-arch install residue)
  SetRegView 64
  DeleteRegValue HKLM "Software\Microsoft\Windows\CurrentVersion\Run" "WindowsHelloFix"
  DeleteRegKey HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\WindowsHelloFix"
  DeleteRegValue HKLM "Software\Microsoft\Windows NT\CurrentVersion\AppCompatFlags\Layers" "$INSTDIR\${OLD_EXE_NAME}"
  DeleteRegValue HKCU "Software\Microsoft\Windows NT\CurrentVersion\AppCompatFlags\Layers" "$INSTDIR\${OLD_EXE_NAME}"
  DeleteRegValue HKLM "Software\Microsoft\Windows NT\CurrentVersion\AppCompatFlags\Layers" "$INSTDIR\${X86_EXE_NAME}"
  DeleteRegValue HKCU "Software\Microsoft\Windows NT\CurrentVersion\AppCompatFlags\Layers" "$INSTDIR\${X86_EXE_NAME}"
  DeleteRegValue HKLM "Software\Microsoft\Windows NT\CurrentVersion\AppCompatFlags\Layers" "$INSTDIR\${X64_EXE_NAME}"
  DeleteRegValue HKCU "Software\Microsoft\Windows NT\CurrentVersion\AppCompatFlags\Layers" "$INSTDIR\${X64_EXE_NAME}"
  SetRegView 32
  DeleteRegValue HKLM "Software\Microsoft\Windows\CurrentVersion\Run" "WindowsHelloFix"
  DeleteRegKey HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\WindowsHelloFix"
  SetRegView default

  ; Clean deployed installation binaries (every known identity)
  Delete "$INSTDIR\${OLD_EXE_NAME}"
  Delete "$INSTDIR\${OLD_METAGEN_NAME}"
  Delete "$INSTDIR\${X86_EXE_NAME}"
  Delete "$INSTDIR\${X86_METAGEN_NAME}"
  Delete "$INSTDIR\${X64_EXE_NAME}"
  Delete "$INSTDIR\${X64_METAGEN_NAME}"
  Delete "$INSTDIR\WindowsHelloFix.ico"
  Delete "$INSTDIR\README.html"
  Delete "$INSTDIR\LICENCE.rtf"
  Delete "$INSTDIR\config.txt"
  Delete "$INSTDIR\Uninstall.exe"

  ; Clean Desktop shortcuts across multiple installation user profiles
  Delete "$DESKTOP\Windows Hello Fix.lnk"
  SetShellVarContext all
  Delete "$DESKTOP\Windows Hello Fix.lnk"
  SetShellVarContext current

  ; Clean Start Menu references
  Delete "$SMPROGRAMS\Windows Hello Fix\Windows Hello Fix.lnk"
  Delete "$SMPROGRAMS\Windows Hello Fix\Uninstall Windows Hello Fix.lnk"
  RMDir "$SMPROGRAMS\Windows Hello Fix"

  ; --- CLEAN UNINSTALL DATA EXTRACTION ENGINE ---
  ; Completely remove all variants of the AppData tracking configurations to guarantee a full, non-residual sweep
  SetShellVarContext current
  Delete "$APPDATA\Windows Hello Fix\config.txt"
  Delete "$APPDATA\Windows Hello Fix\diagnostic.log"
  RMDir /r "$APPDATA\Windows_Hello_Fix"
  RMDir /r "$APPDATA\Windows Hello Fix"
  RMDir /r "$APPDATA\WindowsHelloFix"

  ; Safely eliminate structural directories
  RMDir "$INSTDIR"
!macroend
