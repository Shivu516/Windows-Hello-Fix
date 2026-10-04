@echo off
rem CameraStressRepro.bat — convenience launcher for CameraStressRepro.ps1
rem Usage examples:
rem   CameraStressRepro.bat                                  (default: MidStreamToggle, 60 cycles, self-elevates)
rem   CameraStressRepro.bat -Mode ProbeOnly                  (read-only camera health probe, no admin needed)
rem   CameraStressRepro.bat -Mode MidStreamToggle -UntilFailure
rem   CameraStressRepro.bat -Mode IdleToggleChurn -Iterations 200 -ToggleTool HelloFix
rem All arguments are passed straight through to the PowerShell script.
setlocal
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0CameraStressRepro.ps1" %*
echo.
echo Exit code: %ERRORLEVEL%
pause
