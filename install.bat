@echo off
setlocal enabledelayedexpansion
color 0B

echo =====================================================================
echo.
echo      WINDOWS HELLO CAMERA FIX - AUTOMATED INSTALLATION WIZARD
echo.
echo =====================================================================
echo.
echo [*] Initializing setup environment...
timeout /t 1 >nul

:: Privilege Check
net session >nul 2>&1
if %errorLevel% neq 0 (
    echo [!] ERROR: Administrative privileges are required.
    echo [*] Requesting elevation from Windows...
    powershell -Command "Start-Process '%0' -Verb RunAs"
    exit /b
)

echo [+] Administrator privileges verified.
echo [*] Launching PowerShell logic core...
echo.

:: Launch the main PowerShell script
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0install.ps1"

echo.
echo =====================================================================
echo   SETUP COMPLETED. You may now close this window.
echo =====================================================================
pause
