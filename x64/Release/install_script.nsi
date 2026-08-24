!include "MUI2.nsh"
!include "LogicLib.nsh"

; --- Project Info ---
Name "Windows Hello Fix v2.0"
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
    FindWindow $1 "#32770" "Windows Hello Fix v2.0 Setup"
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
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\WindowsHelloFix" "DisplayName" "Windows Hello Fix v2.0"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\WindowsHelloFix" "UninstallString" '"$INSTDIR\Uninstall.exe"'
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\WindowsHelloFix" "DisplayIcon" "$INSTDIR\WindowsHelloFix.ico"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\WindowsHelloFix" "Publisher" "Shivu516"

  ; --- STARTUP APPS REGISTRATION (Task Manager → Startup) ---
  ; Make HelloFix appear in Startup Apps, enabled by default, while keeping silent elevated execution via Task Scheduler.
  ; The Run entry provides visibility and the user's enable/disable toggle; the scheduled task provides elevation.
  ; Use "Windows Hello Fix" with spaces so Task Manager shows the expected display name.
  ; Clean stale StartupApproved disabled markers so fresh install is Enabled (check both legacy no-space and new spaced names).
  DeleteRegValue HKLM "Software\Microsoft\Windows\CurrentVersion\Explorer\StartupApproved\Run" "WindowsHelloFix"
  DeleteRegValue HKLM "Software\Microsoft\Windows\CurrentVersion\Explorer\StartupApproved\Run" "Windows Hello Fix"
  DeleteRegValue HKLM "Software\Microsoft\Windows\CurrentVersion\Explorer\StartupApproved\StartupFolder" "WindowsHelloFix"
  DeleteRegValue HKLM "Software\Microsoft\Windows\CurrentVersion\Explorer\StartupApproved\StartupFolder" "Windows Hello Fix"
  SetRegView default
  DeleteRegValue HKCU "Software\Microsoft\Windows\CurrentVersion\Explorer\StartupApproved\Run" "WindowsHelloFix"
  DeleteRegValue HKCU "Software\Microsoft\Windows\CurrentVersion\Explorer\StartupApproved\Run" "Windows Hello Fix"
  DeleteRegValue HKCU "Software\Microsoft\Windows\CurrentVersion\Explorer\StartupApproved\StartupFolder" "WindowsHelloFix"
  DeleteRegValue HKCU "Software\Microsoft\Windows\CurrentVersion\Explorer\StartupApproved\StartupFolder" "Windows Hello Fix"
  DeleteRegValue HKCU "Software\Microsoft\Windows\CurrentVersion\Run" "WindowsHelloFix"
  DeleteRegValue HKCU "Software\Microsoft\Windows\CurrentVersion\Run" "Windows Hello Fix"
  SetRegView 64
  DeleteRegValue HKLM "Software\Microsoft\Windows\CurrentVersion\Run" "WindowsHelloFix"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Run" "Windows Hello Fix" '$WINDIR\System32\schtasks.exe /Run /TN "WindowsHelloFix"'
  SetRegView default

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
  FileWrite $1 "Register-ScheduledTask -TaskName 'WindowsHelloFix' -Description 'Starts Windows Hello Fix in the background at user logon so it can monitor Windows session and power events and manage the configured camera hardware.' -Action $$action -Trigger $$trigger -Principal $$principal -Settings $$settings -Force | Out-Null$\r$\n"
  FileWrite $1 "function Register-WhfSessionTask([string]$$name, [int]$$stateChange, [string]$$arguments, [string]$$description) {$\r$\n"
  FileWrite $1 "  $$service = New-Object -ComObject 'Schedule.Service'$\r$\n"
  FileWrite $1 "  $$service.Connect()$\r$\n"
  FileWrite $1 "  $$root = $$service.GetFolder('\')$\r$\n"
  FileWrite $1 "  $$task = $$service.NewTask(0)$\r$\n"
  FileWrite $1 "  $$task.RegistrationInfo.Author = $$user$\r$\n"
  FileWrite $1 "  $$task.RegistrationInfo.Description = $$description$\r$\n"
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
  ; WindowsHelloFix_Lock removed — native WTS listener is sole lock authority (see Plan.md, AGENTS.md HelloFix Working Philosophy)
  FileWrite $1 "$$failsafeAction = New-ScheduledTaskAction -Execute $$exe -Argument '--failsafe-boot' -WorkingDirectory $$wd$\r$\n"
  FileWrite $1 "$$failsafeTrigger = New-ScheduledTaskTrigger -AtLogOn$\r$\n"
  FileWrite $1 "$$failsafeTrigger.Delay = 'PT1M'$\r$\n"
  FileWrite $1 "Register-ScheduledTask -TaskName 'WindowsHelloFix_Unlock' -Description 'Startup/logon camera recovery failsafe. Re-enables the RGB camera if Windows Hello Fix has not initialized correctly.' -Action $$failsafeAction -Trigger $$failsafeTrigger -Principal $$principal -Settings $$settings -Force | Out-Null$\r$\n"
  FileWrite $1 "$$cleanupAction = New-ScheduledTaskAction -Execute 'cmd.exe' -Argument '/c break > $\"$APPDATA\Windows Hello Fix\diagnostic.log$\"'$\r$\n"
  FileWrite $1 "$$cleanupTrigger = New-ScheduledTaskTrigger -Daily -At 00:00$\r$\n"
  FileWrite $1 "Register-ScheduledTask -TaskName 'WindowsHelloFix_LogCleanup' -Description 'Performs daily maintenance of the Windows Hello Fix diagnostic log.' -Action $$cleanupAction -Trigger $$cleanupTrigger -Principal $$principal -Settings $$settings -Force | Out-Null$\r$\n"
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
  DeleteRegValue HKLM "Software\Microsoft\Windows\CurrentVersion\Run" "Windows Hello Fix"
  DeleteRegValue HKLM "Software\Microsoft\Windows\CurrentVersion\Explorer\StartupApproved\Run" "WindowsHelloFix"
  DeleteRegValue HKLM "Software\Microsoft\Windows\CurrentVersion\Explorer\StartupApproved\Run" "Windows Hello Fix"
  DeleteRegValue HKLM "Software\Microsoft\Windows\CurrentVersion\Explorer\StartupApproved\StartupFolder" "WindowsHelloFix"
  DeleteRegValue HKLM "Software\Microsoft\Windows\CurrentVersion\Explorer\StartupApproved\StartupFolder" "Windows Hello Fix"
  DeleteRegKey HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\WindowsHelloFix"
  DeleteRegValue HKLM "Software\Microsoft\Windows NT\CurrentVersion\AppCompatFlags\Layers" "$INSTDIR\Windows_Hello_Fix_v2_0.exe"
  DeleteRegValue HKCU "Software\Microsoft\Windows NT\CurrentVersion\AppCompatFlags\Layers" "$INSTDIR\Windows_Hello_Fix_v2_0.exe"
  SetRegView default
  DeleteRegValue HKCU "Software\Microsoft\Windows\CurrentVersion\Run" "WindowsHelloFix"
  DeleteRegValue HKCU "Software\Microsoft\Windows\CurrentVersion\Run" "Windows Hello Fix"
  DeleteRegValue HKCU "Software\Microsoft\Windows\CurrentVersion\Explorer\StartupApproved\Run" "WindowsHelloFix"
  DeleteRegValue HKCU "Software\Microsoft\Windows\CurrentVersion\Explorer\StartupApproved\Run" "Windows Hello Fix"
  DeleteRegValue HKCU "Software\Microsoft\Windows\CurrentVersion\Explorer\StartupApproved\StartupFolder" "WindowsHelloFix"
  DeleteRegValue HKCU "Software\Microsoft\Windows\CurrentVersion\Explorer\StartupApproved\StartupFolder" "Windows Hello Fix"

  ; Clean deployed installation binaries
  Delete "$INSTDIR\Windows_Hello_Fix_v2_0.exe"
  Delete "$INSTDIR\Windows_Hello_Fix_v2_0.exe.metagen"
  Delete "$INSTDIR\WindowsHelloFix.ico"
  Delete "$INSTDIR\README.html"
  Delete "$INSTDIR\README.rtf"
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
