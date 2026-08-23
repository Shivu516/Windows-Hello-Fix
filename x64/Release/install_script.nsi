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

  ; --- SILENT STARTUP ELEVATION FIX ---
  ; Do not set RUNASADMIN compatibility flags. They can force a visible UAC prompt and fight Task Scheduler elevation.
  DeleteRegValue HKLM "Software\Microsoft\Windows NT\CurrentVersion\AppCompatFlags\Layers" "$INSTDIR\Windows_Hello_Fix_v2_0.exe"
  DeleteRegValue HKCU "Software\Microsoft\Windows NT\CurrentVersion\AppCompatFlags\Layers" "$INSTDIR\Windows_Hello_Fix_v2_0.exe"

  SetRegView default

  ; --- AUTOMATED SYSTEM TASK REGISTRATION ENGINE ---

   ; Wipe away duplicate stale task configurations and obsolete legacy camera-control tasks
   ; Idempotent cleanup - tolerant of missing tasks (/F) and case-insensitive names
   ; Category A (retain) are deleted then recreated; Category B (obsolete) are removed and never recreated
  nsExec::ExecToLog 'schtasks /Delete /TN "WindowsHelloFix" /F'
  nsExec::ExecToLog 'schtasks /Delete /TN "WindowsHelloFix_LogCleanup" /F'
  nsExec::ExecToLog 'schtasks /Delete /TN "WindowsHelloFix_Failsafe" /F'
  nsExec::ExecToLog 'schtasks /Delete /TN "WindowsHelloFix_Lock" /F'
  nsExec::ExecToLog 'schtasks /Delete /TN "WindowsHelloFix_Unlock" /F'
  nsExec::ExecToLog 'schtasks /Delete /TN "Disable Camera On Lock" /F'
  nsExec::ExecToLog 'schtasks /Delete /TN "Enable Camera On Unlock" /F'
  Sleep 1000

    ; --- CRITICAL TASK SCHEDULER FIXES ---
   ; Generate a PowerShell registration script instead of using fragile one-line shell quoting.
   ; Native v2.x is authoritative: register only AtLogOn daemon, daily log-cleanup, and boot failsafe (enable-only).
   ; Lock/Unlock camera tasks are obsolete - removed above and never recreated to prevent duplicate/race control.
  InitPluginsDir
  FileOpen $1 "$PLUGINSDIR\RegisterWindowsHelloFixTasks.ps1" w
  FileWrite $1 "$$exe = '$INSTDIR\Windows_Hello_Fix_v2_0.exe'$\r$\n"
  FileWrite $1 "$$wd = '$INSTDIR'$\r$\n"
  FileWrite $1 "$$user = [System.Security.Principal.WindowsIdentity]::GetCurrent().Name$\r$\n"
  FileWrite $1 "$$action = New-ScheduledTaskAction -Execute $$exe -Argument '--background' -WorkingDirectory $$wd$\r$\n"
  FileWrite $1 "$$trigger = New-ScheduledTaskTrigger -AtLogOn$\r$\n"
  FileWrite $1 "$$principal = New-ScheduledTaskPrincipal -UserId $$user -LogonType Interactive -RunLevel Highest$\r$\n"
  FileWrite $1 "$$settings = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries -StartWhenAvailable -MultipleInstances IgnoreNew -ExecutionTimeLimit (New-TimeSpan -Seconds 0) -Priority 4$\r$\n"
  FileWrite $1 "Register-ScheduledTask -TaskName 'WindowsHelloFix' -Description 'Starts Windows Hello Fix in the background at system startup so it can monitor Windows session and power events and manage the configured camera hardware.' -Action $$action -Trigger $$trigger -Principal $$principal -Settings $$settings -Force | Out-Null\r\n"
  FileWrite $1 "$$cleanupAction = New-ScheduledTaskAction -Execute 'cmd.exe' -Argument '/c break > $\"$APPDATA\\Windows Hello Fix\\diagnostic.log$\"'\r\n"
  FileWrite $1 "$$cleanupTrigger = New-ScheduledTaskTrigger -Daily -At 00:00\r\n"
  FileWrite $1 "Register-ScheduledTask -TaskName 'WindowsHelloFix_LogCleanup' -Description 'Windows Hello Fix maintenance task that cleans up old diagnostic log data.' -Action $$cleanupAction -Trigger $$cleanupTrigger -Principal $$principal -Settings $$settings -Force | Out-Null\r\n"
  FileWrite $1 "$$fsAction = New-ScheduledTaskAction -Execute $$exe -Argument '--failsafe-boot' -WorkingDirectory $$wd\r\n"
  FileWrite $1 "$$fsTrigger = New-ScheduledTaskTrigger -AtLogOn\r\n"
  FileWrite $1 "$$fsTrigger.Delay = 'PT1M30S'\r\n"
  FileWrite $1 "$$fsSettings = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries -StartWhenAvailable -MultipleInstances IgnoreNew -ExecutionTimeLimit (New-TimeSpan -Seconds 120) -Priority 4\r\n"
  FileWrite $1 "Register-ScheduledTask -TaskName 'WindowsHelloFix_Failsafe' -Description 'Windows Hello Fix boot safety check. Re-enables the configured camera if Hello Fix was configured for monitoring but failed to start. This task never disables the camera.' -Action $$fsAction -Trigger $$fsTrigger -Principal $$principal -Settings $$fsSettings -Force | Out-Null\r\n"
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

    ; Purge all HelloFix-created scheduled tasks - tolerant of missing (/F)
   ; Category A (current: WindowsHelloFix, LogCleanup, Failsafe) + Category B (legacy redundant, no longer created)
  nsExec::ExecToLog 'schtasks /Delete /TN "WindowsHelloFix" /F'
  nsExec::ExecToLog 'schtasks /Delete /TN "WindowsHelloFix_LogCleanup" /F'
  nsExec::ExecToLog 'schtasks /Delete /TN "WindowsHelloFix_Failsafe" /F'
  nsExec::ExecToLog 'schtasks /Delete /TN "WindowsHelloFix_Lock" /F'
  nsExec::ExecToLog 'schtasks /Delete /TN "WindowsHelloFix_Unlock" /F'
  nsExec::ExecToLog 'schtasks /Delete /TN "Disable Camera On Lock" /F'
  nsExec::ExecToLog 'schtasks /Delete /TN "Enable Camera On Unlock" /F'
  nsExec::ExecToLog 'schtasks /Delete /TN "WindowsHelloFix_Wake" /F'

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
