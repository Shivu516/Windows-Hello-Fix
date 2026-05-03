# ==============================================================================
# WINDOWS HELLO CAMERA FIX - REMOVAL LOGIC
# ==============================================================================
Write-Host "[*] Uninstaller Execution Started." -ForegroundColor Cyan

Write-Host "[*] Deregistering Scheduled Tasks..." -ForegroundColor Yellow

if (schtasks /query /tn "Disable Camera On Lock" 2>$null) {
    schtasks /delete /tn "Disable Camera On Lock" /f | Out-Null
    Write-Host "    [+] Removed: Disable Camera On Lock" -ForegroundColor Green
} else {
    Write-Host "    [-] Task 'Disable Camera On Lock' not found. Skipping." -ForegroundColor DarkGray
}

if (schtasks /query /tn "Enable Camera On Unlock" 2>$null) {
    schtasks /delete /tn "Enable Camera On Unlock" /f | Out-Null
    Write-Host "    [+] Removed: Enable Camera On Unlock" -ForegroundColor Green
} else {
    Write-Host "    [-] Task 'Enable Camera On Unlock' not found. Skipping." -ForegroundColor DarkGray
}

$installPath = "$PSScriptRoot\CameraFix"
Write-Host "[*] Locating installation directory: $installPath" -ForegroundColor Yellow

if (Test-Path $installPath) {
    Write-Host "    [*] Directory found. Deleting all scripts and wrappers..." -ForegroundColor Cyan
    Remove-Item -Path $installPath -Recurse -Force
    Write-Host "    [+] System folder successfully purged." -ForegroundColor Green
} else {
    Write-Host "    [-] Installation folder not found. Skipping." -ForegroundColor DarkGray
}

Write-Host "[+] Cleanup Complete. No traces remain." -ForegroundColor Green
Start-Sleep -Seconds 2
