!include "MUI2.nsh"
!include "LogicLib.nsh"

; --- Project Info ---
Name "Windows Hello Fix v2.1"
OutFile "Windows_Hello_Fix_Setup.exe"
InstallDir "$PROGRAMFILES64\WindowsHelloFix"
RequestExecutionLevel admin

; --- Interface Settings ---
!define MUI_ICON "WindowsHelloFix.ico"
!define MUI_UNICON "WindowsHelloFix.ico"
!define MUI_ABORTWARNING

; --- Pages ---
!insertmacro MUI_PAGE_LICENSE "LICENCE.rtf"
!insertmacro MUI_PAGE_COMPONENTS
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
  !define MUI_FINISHPAGE_RUN "$INSTDIR\Windows_Hello_Fix_v2_0.exe"
!insertmacro MUI_PAGE_FINISH

!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES

!insertmacro MUI_LANGUAGE "English"

; --- Initialization ---
Function .onInit
  ; 1. THE BULLETPROOF SINGLE INSTANCE CHECK
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

  ; 2. REINSTALLATION CLEANUP PRE-CHECK
  SetRegView 64
  ReadRegStr $0 HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\WindowsHelloFix" "UninstallString"
  SetRegView default

  ${If} $0 != ""
    MessageBox MB_YESNO|MB_ICONQUESTION "Windows Hello Fix is already installed.$\n$\nDo you want to reinstall and overwrite the current version?" /SD IDYES IDYES keep_going
    Abort
    keep_going:
  ${EndIf}
FunctionEnd

; --- Core Installation ---
Section "Core Files (Required)" SEC01
  SectionIn RO

  ; Ensure installer operates in elevated context throughout install session
  SetShellVarContext all

  ; Clean up old loose desktop links before overwrite
  Delete "$DESKTOP\Windows Hello Fix.lnk"
  SetShellVarContext all
  Delete "$DESKTOP\Windows Hello Fix.lnk"
  SetShellVarContext current

  ; Terminate any running active instances of the engine before swapping binaries
  nsExec::Exec 'taskkill /F /IM Windows_Hello_Fix_v2_0.exe /T'
  Sleep 1500

  SetOutPath "$INSTDIR"

  ; Deploy Binaries and documentation assets
  File "Windows_Hello_Fix_v2_0.exe"
  File "Windows_Hello_Fix_v2_0.exe.metagen"
  File "WindowsHelloFix.ico"
  File "README.html"
  File "LICENCE.rtf"

  ; --- CRITICAL PRODUCTION FIX ---
  ; Explicitly unblock downloaded binaries to avoid SmartScreen / Zone.Identifier privilege weirdness
  nsExec::ExecToLog 'powershell -WindowStyle Hidden -Command "Unblock-File -Path \"$INSTDIR\Windows_Hello_Fix_v2_0.exe\" -ErrorAction SilentlyContinue"'

  ; Explicitly provision local AppData configuration tracking directories
  SetShellVarContext current
  CreateDirectory "$APPDATA\Windows Hello Fix"

  ; Pre-create diagnostic log to avoid first-launch permission edge cases
  FileOpen $0 "$APPDATA\Windows Hello Fix\diagnostic.log" w
  FileClose $0

  ; Clear any hardware ghost lock states prior to registration by resetting the driver stack
  nsExec::ExecToLog '"$INSTDIR\Windows_Hello_Fix_v2_0.exe" /restore-camera'
  Sleep 3000

  ; Create Start Menu shortcuts safely inside isolated directory layouts
  CreateDirectory "$SMPROGRAMS\Windows Hello Fix"
  CreateShortcut "$SMPROGRAMS\Windows Hello Fix\Windows Hello Fix.lnk" "$INSTDIR\Windows_Hello_Fix_v2_0.exe" "" "$INSTDIR\WindowsHelloFix.ico" 0
  CreateShortcut "$SMPROGRAMS\Windows Hello Fix\Uninstall Windows Hello Fix.lnk" "$INSTDIR\Uninstall.exe" "" "$INSTDIR\WindowsHelloFix.ico" 0

  ; Write registry configuration keys for Add/Remove Programs control interface
  SetRegView 64
  DeleteRegValue HKLM "Software\Microsoft\Windows\CurrentVersion\Run" "WindowsHelloFix"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\WindowsHelloFix" "DisplayName" "Windows Hello Fix v2.1"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\WindowsHelloFix" "UninstallString" '"$INSTDIR\Uninstall.exe"'
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\WindowsHelloFix" "DisplayIcon" "$INSTDIR\WindowsHelloFix.ico"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\WindowsHelloFix" "Publisher" "Shivu516"

  ; --- SILENT STARTUP ELEVATION FIX ---
  ; Do not set RUNASADMIN compatibility flags. They can force a visible UAC prompt and fight Task Scheduler elevation.
  DeleteRegValue HKLM "Software\Microsoft\Windows NT\CurrentVersion\AppCompatFlags\Layers" "$INSTDIR\Windows_Hello_Fix_v2_0.exe"
  DeleteRegValue HKCU "Software\Microsoft\Windows NT\CurrentVersion\AppCompatFlags\Layers" "$INSTDIR\Windows_Hello_Fix_v2_0.exe"

  SetRegView default

  ; --- AUTOMATED SYSTEM TASK REGISTRATION ENGINE ---

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
  FileWrite $1 "$$exe = '$INSTDIR\Windows_Hello_Fix_v2_0.exe'$\r$\n"
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
  ; Uses existing --enable-camera (IsRestoreCameraCommand early-exit at MyForm_Core.cpp:208, enable-only via Recover(false)) — no new flag.
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
  nsExec::ExecToLog '"$INSTDIR\Windows_Hello_Fix_v2_0.exe" /restore-camera'
  Sleep 2500

  WriteUninstaller "$INSTDIR\Uninstall.exe"
SectionEnd

; Adding '/o' unchecks this box by default!
Section /o "Create Desktop Shortcut" SEC02
  SetShellVarContext all
  CreateShortcut "$DESKTOP\Windows Hello Fix.lnk" "$INSTDIR\Windows_Hello_Fix_v2_0.exe" "" "$INSTDIR\WindowsHelloFix.ico"
  SetShellVarContext current
SectionEnd

; --- Uninstaller Logic ---
Section "Uninstall"
  ; Self-healing hardware check: verification safety loop to make absolutely sure the camera driver is working before removing app files
  IfFileExists "$INSTDIR\Windows_Hello_Fix_v2_0.exe" 0 skip_pre_kill_camera_restore
    nsExec::ExecToLog '"$INSTDIR\Windows_Hello_Fix_v2_0.exe" /restore-camera'
    Sleep 3000
  skip_pre_kill_camera_restore:

  ; Terminate any active process instance safely
  nsExec::Exec 'taskkill /F /IM Windows_Hello_Fix_v2_0.exe /T'
  Sleep 1500

  ; Final defensive redundancy sweep to maximize driver stability
  IfFileExists "$INSTDIR\Windows_Hello_Fix_v2_0.exe" 0 skip_post_kill_camera_restore
    nsExec::ExecToLog '"$INSTDIR\Windows_Hello_Fix_v2_0.exe" /restore-camera'
    Sleep 3000
  skip_post_kill_camera_restore:

  ; Purge registered task arrays
  nsExec::ExecToLog 'schtasks /Delete /TN "WindowsHelloFix" /F'
  nsExec::ExecToLog 'schtasks /Delete /TN "WindowsHelloFix_Wake" /F'
  nsExec::ExecToLog 'schtasks /Delete /TN "WindowsHelloFix_Lock" /F'
  nsExec::ExecToLog 'schtasks /Delete /TN "WindowsHelloFix_Unlock" /F'
  nsExec::ExecToLog 'schtasks /Delete /TN "WindowsHelloFix_LogCleanup" /F'

  ; Clean registry values
  SetRegView 64
  DeleteRegValue HKLM "Software\Microsoft\Windows\CurrentVersion\Run" "WindowsHelloFix"
  DeleteRegKey HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\WindowsHelloFix"
  DeleteRegValue HKLM "Software\Microsoft\Windows NT\CurrentVersion\AppCompatFlags\Layers" "$INSTDIR\Windows_Hello_Fix_v2_0.exe"
  DeleteRegValue HKCU "Software\Microsoft\Windows NT\CurrentVersion\AppCompatFlags\Layers" "$INSTDIR\Windows_Hello_Fix_v2_0.exe"
  SetRegView default

  ; Clean deployed installation binaries
  Delete "$INSTDIR\Windows_Hello_Fix_v2_0.exe"
  Delete "$INSTDIR\Windows_Hello_Fix_v2_0.exe.metagen"
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
SectionEnd
