; install_x64.nsi — standalone x64 installer for Windows Hello Fix v2.1.
; Payload: Release|x64 (Windows_Hello_Fix_v2_1_x64.exe). Behavior: see installer/common.nsh
; (shared logic, ported from x64/Release/install_script.nsi without behavior change).

!include "MUI2.nsh"
!include "LogicLib.nsh"
!include "x64.nsh"

!define APP_ARCH_X64
!define APP_EXE_NAME "Windows_Hello_Fix_v2_1_x64.exe"
!define APP_METAGEN_NAME "Windows_Hello_Fix_v2_1_x64.exe.metagen"
!define APP_EXE_SRCDIR "..\x64\Release"
!include "common.nsh"

; --- Project Info ---
Name "Windows Hello Fix v2.1"
OutFile "..\Release\Windows_Hello_Fix_Setup_x64.exe"
InstallDir "$PROGRAMFILES64\WindowsHelloFix"
RequestExecutionLevel admin

; --- Interface Settings ---
!define MUI_ICON "..\x64\Release\WindowsHelloFix.ico"
!define MUI_UNICON "..\x64\Release\WindowsHelloFix.ico"
!define MUI_ABORTWARNING

; --- Pages ---
!insertmacro MUI_PAGE_LICENSE "..\x64\Release\LICENCE.rtf"
!insertmacro MUI_PAGE_COMPONENTS
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
  !define MUI_FINISHPAGE_RUN "$INSTDIR\$AppExeName"
!insertmacro MUI_PAGE_FINISH

!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES

!insertmacro MUI_LANGUAGE "English"

; --- Initialization ---
Function .onInit
  StrCpy $AppExeName "${APP_EXE_NAME}"
  StrCpy $AppMetagenName "${APP_METAGEN_NAME}"
  !insertmacro COMMON_ONINIT_BODY
FunctionEnd

; --- Core Installation ---
Section "Core Files (Required)" SEC01
  SectionIn RO

  SetShellVarContext all
  Delete "$DESKTOP\Windows Hello Fix.lnk"
  SetShellVarContext all
  Delete "$DESKTOP\Windows Hello Fix.lnk"
  SetShellVarContext current

  ; Terminate stale instances before swapping binaries (all known identities)
  !insertmacro KILL_ALL_KNOWN_EXES

  SetOutPath "$INSTDIR"
  !insertmacro CLEAN_STALE_PAYLOADS

  ; Deploy native x64 payload
  File "${APP_EXE_SRCDIR}\${APP_EXE_NAME}"
  File "${APP_EXE_SRCDIR}\${APP_METAGEN_NAME}"

  !insertmacro CORE_INSTALL_BODY
SectionEnd

; Adding '/o' unchecks this box by default!
Section /o "Create Desktop Shortcut" SEC02
  !insertmacro DESKTOP_SHORTCUT_BODY
SectionEnd

; --- Uninstaller Logic ---
Section "Uninstall"
  !insertmacro CORE_UNINSTALL_BODY
SectionEnd
