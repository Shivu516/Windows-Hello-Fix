@echo off
setlocal enabledelayedexpansion
color 0C

echo =====================================================================
echo.
echo      WINDOWS HELLO CAMERA FIX - REMOVAL TOOL
echo.
echo =====================================================================
echo.
echo [*] Preparing to remove all modifications...
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
echo [*] Launching cleanup logic...
echo.

powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0uninstall.ps1"

echo.
echo =====================================================================
echo   REMOVAL COMPLETED. Your system is back to default.
echo =====================================================================
pause
