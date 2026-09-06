; install_universal.nsi — universal x86+x64 installer for Windows Hello Fix v2.1.
; Detects Windows architecture at install time (${RunningX64}), installs EXACTLY ONE
; native payload (never both) into the matching Program Files directory with the
; matching registry view. All other behavior: see installer/common.nsh (shared logic,
; ported from x64/Release/install_script.nsi without behavior change).
; Future ARM64: stage a third payload + add a branch here (additive, no redesign).

!include "MUI2.nsh"
!include "LogicLib.nsh"
!include "x64.nsh"

!define UNIVERSAL
!include "common.nsh"

; --- Project Info ---
Name "Windows Hello Fix v2.1"
OutFile "..\Release\Windows_Hello_Fix_Setup.exe"
; InstallDir is set dynamically in .onInit (PROGRAMFILES64 on x64, PROGRAMFILES32 on x86).
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
  ; Select native payload + matching install location BEFORE the directory page.
  ${If} ${RunningX64}
    StrCpy $AppExeName "${X64_EXE_NAME}"
    StrCpy $AppMetagenName "${X64_METAGEN_NAME}"
    StrCpy $INSTDIR "$PROGRAMFILES64\WindowsHelloFix"
  ${Else}
    StrCpy $AppExeName "${X86_EXE_NAME}"
    StrCpy $AppMetagenName "${X86_METAGEN_NAME}"
    StrCpy $INSTDIR "$PROGRAMFILES32\WindowsHelloFix"
  ${EndIf}
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

  ; Stage both payloads, deploy exactly the native one. The foreign payload never
  ; touches $INSTDIR, so no wrong-architecture binary is ever left behind.
  InitPluginsDir
  File /oname=$PLUGINSDIR\payload_x86.exe "..\Release\${X86_EXE_NAME}"
  File /oname=$PLUGINSDIR\payload_x86.metagen "..\Release\${X86_METAGEN_NAME}"
  File /oname=$PLUGINSDIR\payload_x64.exe "..\x64\Release\${X64_EXE_NAME}"
  File /oname=$PLUGINSDIR\payload_x64.metagen "..\x64\Release\${X64_METAGEN_NAME}"
  ${If} ${RunningX64}
    CopyFiles /SILENT "$PLUGINSDIR\payload_x64.exe" "$INSTDIR\${X64_EXE_NAME}"
    CopyFiles /SILENT "$PLUGINSDIR\payload_x64.metagen" "$INSTDIR\${X64_METAGEN_NAME}"
  ${Else}
    CopyFiles /SILENT "$PLUGINSDIR\payload_x86.exe" "$INSTDIR\${X86_EXE_NAME}"
    CopyFiles /SILENT "$PLUGINSDIR\payload_x86.metagen" "$INSTDIR\${X86_METAGEN_NAME}"
  ${EndIf}

  !insertmacro CORE_INSTALL_BODY
SectionEnd

; Adding '/o' unchecks this box by default!
Section /o "Create Desktop Shortcut" SEC02
  !insertmacro DESKTOP_SHORTCUT_BODY
SectionEnd

; --- Uninstaller Logic ---
Section "Uninstall"
  ; $AppExeName is empty in the uninstaller context until set: recover the
  ; actually-installed payload so /restore-camera + shortcuts target the right binary.
  IfFileExists "$INSTDIR\${X64_EXE_NAME}" has_x64_payload has_x86_check
  has_x86_check:
  IfFileExists "$INSTDIR\${X86_EXE_NAME}" has_x86_payload has_legacy_check
  has_legacy_check:
  IfFileExists "$INSTDIR\${OLD_EXE_NAME}" has_legacy_payload no_payload_found
  has_x64_payload:
    StrCpy $AppExeName "${X64_EXE_NAME}"
    StrCpy $AppMetagenName "${X64_METAGEN_NAME}"
    Goto payload_resolved
  has_x86_payload:
    StrCpy $AppExeName "${X86_EXE_NAME}"
    StrCpy $AppMetagenName "${X86_METAGEN_NAME}"
    Goto payload_resolved
  has_legacy_payload:
    StrCpy $AppExeName "${OLD_EXE_NAME}"
    StrCpy $AppMetagenName "${OLD_METAGEN_NAME}"
    Goto payload_resolved
  no_payload_found:
    ${If} ${RunningX64}
      StrCpy $AppExeName "${X64_EXE_NAME}"
      StrCpy $AppMetagenName "${X64_METAGEN_NAME}"
    ${Else}
      StrCpy $AppExeName "${X86_EXE_NAME}"
      StrCpy $AppMetagenName "${X86_METAGEN_NAME}"
    ${EndIf}
  payload_resolved:
  !insertmacro CORE_UNINSTALL_BODY
SectionEnd
