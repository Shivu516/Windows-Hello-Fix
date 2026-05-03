# ==============================================================================
# WINDOWS HELLO CAMERA FIX - INSTALLATION LOGIC (PNPUTIL OPTIMIZATION)
# ==============================================================================
Write-Host "[*] Script Execution Started." -ForegroundColor Cyan

# --- 1. GUI CAMERA SELECTION ---
Write-Host "[*] Loading GUI Components..." -ForegroundColor Yellow
Add-Type -AssemblyName System.Windows.Forms
$form = New-Object System.Windows.Forms.Form
$form.Text = "Windows Hello Camera Fix - Device Selector"
$form.Size = New-Object System.Drawing.Size(500, 250)
$form.StartPosition = "CenterScreen"
$form.Topmost = $true

$label = New-Object System.Windows.Forms.Label
$label.Text = "Please select your RGB Camera from the drop-down menu:`n(This is the camera that will be disabled to prioritize IR)"
$label.AutoSize = $true
$label.Location = New-Object System.Drawing.Point(20, 20)
$form.Controls.Add($label)

$dropdown = New-Object System.Windows.Forms.ComboBox
$dropdown.Location = New-Object System.Drawing.Point(20, 60)
$dropdown.Size = New-Object System.Drawing.Size(440, 30)
$dropdown.DropDownStyle = [System.Windows.Forms.ComboBoxStyle]::DropDownList

Write-Host "[*] Scanning system for imaging devices..." -ForegroundColor Yellow
$cameras = Get-PnpDevice -Class Camera | Where-Object { $_.FriendlyName -ne $null }
foreach ($cam in $cameras) { 
    $dropdown.Items.Add("$($cam.FriendlyName) ($($cam.InstanceId))") 
}
$form.Controls.Add($dropdown)

$btn = New-Object System.Windows.Forms.Button
$btn.Text = "Apply Fix"
$btn.Location = New-Object System.Drawing.Point(175, 120)
$btn.Size = New-Object System.Drawing.Size(150, 40)
$btn.Add_Click({ $form.Close() })
$form.Controls.Add($btn)

Write-Host "[*] Waiting for user input..." -ForegroundColor Cyan
$form.ShowDialog() | Out-Null

if ($dropdown.SelectedItem -eq $null) { 
    Write-Host "[!] Setup aborted by user. No camera selected." -ForegroundColor Red
    exit 
}

$rawSelection = $dropdown.SelectedItem.ToString()
$instanceId = $rawSelection.Substring($rawSelection.LastIndexOf("(")+1).Replace(")","")
Write-Host "[+] Target Camera Locked: $instanceId" -ForegroundColor Green
Start-Sleep -Seconds 1

# --- 2. DIRECTORY & HIGH-SPEED SCRIPT GENERATION ---
$installPath = "$PSScriptRoot\CameraFix"
Write-Host "[*] Verifying installation directory at $installPath..." -ForegroundColor Yellow

if (!(Test-Path $installPath)) { 
    Write-Host "    -> Directory not found. Creating..." -ForegroundColor Cyan
    New-Item -ItemType Directory -Path $installPath | Out-Null 
} else {
    Write-Host "    -> Directory exists. Overwriting old files..." -ForegroundColor Cyan
}

Write-Host "[*] Generating high-speed VBS wrappers (bypassing PowerShell delays)..." -ForegroundColor Yellow

# Using native pnputil.exe for millisecond execution times
$vbsDisable = @"
Set WshShell = CreateObject("WScript.Shell")
WshShell.Run "pnputil /disable-device ""$instanceId""", 0, False
"@

$vbsEnable = @"
Set WshShell = CreateObject("WScript.Shell")
WshShell.Run "pnputil /enable-device ""$instanceId""", 0, False
"@

$vbsDisable | Out-File "$installPath\Disable_Camera.vbs" -Encoding ASCII
$vbsEnable | Out-File "$installPath\Enable_Camera.vbs" -Encoding ASCII

# --- 3. AUDIT POLICY CONFIGURATION ---
Write-Host "[*] Enabling Windows Security Auditing for Lock/Unlock Events..." -ForegroundColor Yellow
auditpol /set /subcategory:"Other Logon/Logoff Events" /success:enable | Out-Null
Write-Host "    [+] Security Auditing configured." -ForegroundColor Green

# --- 4. TASK REGISTRATION (EVENT ID XML) ---
Write-Host "[*] Cleaning up any existing tasks..." -ForegroundColor Yellow
schtasks /delete /tn "Disable Camera On Lock" /f 2>$null
schtasks /delete /tn "Enable Camera On Unlock" /f 2>$null

Write-Host "[*] Building XML configurations for Event Triggers..." -ForegroundColor Yellow

# XML for LOCK (Event ID 4800)
$xmlDisable = @"
<?xml version="1.0" encoding="UTF-16"?>
<Task version="1.2" xmlns="http://schemas.microsoft.com/windows/2004/02/mit/task">
  <Principals><Principal><UserId>S-1-5-18</UserId><RunLevel>HighestAvailable</RunLevel></Principal></Principals>
  <Triggers>
    <EventTrigger>
      <Enabled>true</Enabled>
      <Subscription>&lt;QueryList&gt;&lt;Query Id="0" Path="Security"&gt;&lt;Select Path="Security"&gt;*[System[Provider[@Name='Microsoft-Windows-Security-Auditing'] and (EventID=4800)]]&lt;/Select&gt;&lt;/Query&gt;&lt;/QueryList&gt;</Subscription>
    </EventTrigger>
  </Triggers>
  <Settings><ExecutionTimeLimit>PT1H</ExecutionTimeLimit></Settings>
  <Actions>
    <Exec>
      <Command>wscript.exe</Command>
      <Arguments>"$installPath\Disable_Camera.vbs"</Arguments>
    </Exec>
  </Actions>
</Task>
"@

# XML for UNLOCK (Event ID 4801)
$xmlEnable = @"
<?xml version="1.0" encoding="UTF-16"?>
<Task version="1.2" xmlns="http://schemas.microsoft.com/windows/2004/02/mit/task">
  <Principals><Principal><UserId>S-1-5-18</UserId><RunLevel>HighestAvailable</RunLevel></Principal></Principals>
  <Triggers>
    <EventTrigger>
      <Enabled>true</Enabled>
      <Subscription>&lt;QueryList&gt;&lt;Query Id="0" Path="Security"&gt;&lt;Select Path="Security"&gt;*[System[Provider[@Name='Microsoft-Windows-Security-Auditing'] and (EventID=4801)]]&lt;/Select&gt;&lt;/Query&gt;&lt;/QueryList&gt;</Subscription>
    </EventTrigger>
  </Triggers>
  <Settings><ExecutionTimeLimit>PT1H</ExecutionTimeLimit></Settings>
  <Actions>
    <Exec>
      <Command>wscript.exe</Command>
      <Arguments>"$installPath\Enable_Camera.vbs"</Arguments>
    </Exec>
  </Actions>
</Task>
"@

$xmlDisable | Out-File "$env:TEMP\disable_cam.xml" -Encoding Unicode
$xmlEnable | Out-File "$env:TEMP\enable_cam.xml" -Encoding Unicode

Write-Host "[*] Registering tasks into Windows Task Scheduler..." -ForegroundColor Yellow
schtasks /create /tn "Disable Camera On Lock" /xml "$env:TEMP\disable_cam.xml" /f | Out-Null
schtasks /create /tn "Enable Camera On Unlock" /xml "$env:TEMP\enable_cam.xml" /f | Out-Null

Write-Host "[+] SUCCESS! System integration complete." -ForegroundColor Green
Write-Host "    Tasks are now optimized to run instantaneously using pnputil." -ForegroundColor Green
Start-Sleep -Seconds 3
