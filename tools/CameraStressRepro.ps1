# CameraStressRepro.ps1 — Issue #1 reproduction & recovery-validation harness
#
# Goal: accelerate the "PnP enabled but camera unusable" failure (Camera app
# 0xA00F4241 (0xC00D7167 = MF_E_REBOOT_REQUIRED)) that normally takes days of
# HelloFix toggle cycles, and provide a probe that reports the raw Media
# Foundation HRESULT so the CameraRecoveryFailsafe recovery ladder can be tested.
#
# IMPORTANT HONESTY NOTE:
#   MF_E_REBOOT_REQUIRED's raise-site is undocumented; no tool can set it
#   directly. This harness instead compresses the suspected *mechanism* —
#   camera device-identity churn while the Camera Frame Server holds live
#   client sessions — from days into minutes. It may still not reproduce the
#   exact failure; every attempt is logged either way.
#
# Modes:
#   ProbeOnly         Single read-only frame grab. "Is the camera wedged right now?"
#   ActivationChurn   Open/close a live frame reader in a loop (no device toggles).
#                     Stresses Frame Server client session lifecycle.
#   IdleToggleChurn   Disable -> enable cycles with no stream attached + a churn
#                     burst each cycle (mimics accelerated HelloFix toggling).
#   MidStreamToggle   The aggressive one: hold a LIVE frame stream and disable the
#                     device (or its composite parent) mid-stream, re-enable, then
#                     probe. Closest to the known frame-server wedge pattern.
#
# Requirements: Windows PowerShell 5.1 (powershell.exe, NOT pwsh), Windows 10
# 2004+ (pnputil /disable-device), administrator for toggle modes.
# Toggles only the configured/target RGB camera. The IR/Hello camera is never
# touched unless -ToggleParent is used (which briefly re-detects siblings).

#requires -Version 5.1
[CmdletBinding()]
param(
    [ValidateSet('ProbeOnly', 'ActivationChurn', 'IdleToggleChurn', 'MidStreamToggle')]
    [string]$Mode = 'MidStreamToggle',

    [int]$Iterations = 0,               # 0 = mode default
    [string]$InstanceId = '',           # target device instance id; empty = auto-detect (config.txt -> MI_00 heuristic)
    [switch]$ToggleParent,              # toggle the composite parent instead (briefly re-detects IR sibling too)
    [ValidateSet('PnPUtil', 'HelloFix')]
    [string]$ToggleTool = 'PnPUtil',    # pnputil = raw PnP toggle; HelloFix = the app's own --disable/--enable-camera path
    [string]$HelloFixExe = '',          # used with -ToggleTool HelloFix; empty = auto-detect in Program Files
    [int]$MidStreamHoldSeconds = 5,     # how long the live stream runs while the device is disabled
    [int]$FrameTimeoutSeconds = 6,      # probe: max wait for the first frame after a successful start
    [int]$SettleSeconds = 4,            # pause after enable before probing
    [int]$ChurnPerCycle = 20,           # activation churn burst per IdleToggleChurn cycle
    [switch]$UntilFailure,              # stop as soon as the wedge signature (or a persistent failure) appears
    [switch]$NoElevate,                 # do not auto-relaunch elevated
    [string]$LogPath = ''               # defaults to CameraStressRepro_<timestamp>.log next to this script
)

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'

# ---------------------------------------------------------------------------
# Constants
# ---------------------------------------------------------------------------
$script:HR_REBOOT_REQUIRED = [int32]'0xC00D7167'   # MF_E_REBOOT_REQUIRED ("We need to reboot the machine.")
$script:HR_ACCESSDENIED    = [int32]'0x80070005'   # E_ACCESSDENIED (privacy gate)
$script:HR_DEVICE_LOCKED   = [int32]'0xC00D4E24'   # MF_E_VIDEO_DEVICE_LOCKED (busy = device responsive)
$script:HR_START_STREAMING = [int32]'0xC00D3704'   # MF_E_HW_MFT_FAILED_START_STREAMING (busy / resource)
$script:HR_NO_CAPTURE_DEV  = [int32]'0xC00DABE0'   # MF_E_NO_CAPTURE_DEVICES_AVAILABLE

if ($Iterations -le 0) {
    switch ($Mode) {
        'ProbeOnly'       { $Iterations = 1 }
        'ActivationChurn' { $Iterations = 300 }
        'IdleToggleChurn' { $Iterations = 60 }
        'MidStreamToggle' { $Iterations = 60 }
    }
}
if (-not $LogPath) {
    $LogPath = Join-Path $PSScriptRoot ('CameraStressRepro_{0:yyyyMMdd_HHmmss}.log' -f (Get-Date))
}

# ---------------------------------------------------------------------------
# Logging
# ---------------------------------------------------------------------------
$script:WedgeCount = 0
function Write-StressLog {
    param([string]$Level, [string]$Message, [ConsoleColor]$Color = 'Gray')
    $line = '[{0:yyyy-MM-dd HH:mm:ss.fff}] [{1,-7}] {2}' -f (Get-Date), $Level, $Message
    try { Add-Content -Path $LogPath -Value $line -Encoding UTF8 } catch { }
    Write-Host $line -ForegroundColor $Color
}

# ---------------------------------------------------------------------------
# Elevation (toggle modes need admin for pnputil)
# ---------------------------------------------------------------------------
function Test-IsAdmin {
    $id = [System.Security.Principal.WindowsIdentity]::GetCurrent()
    (New-Object System.Security.Principal.WindowsPrincipal($id)).IsInRole([System.Security.Principal.WindowsBuiltInRole]::Administrator)
}

if (($Mode -eq 'IdleToggleChurn' -or $Mode -eq 'MidStreamToggle') -and -not (Test-IsAdmin) -and -not $NoElevate) {
    Write-Host "Toggle modes require administrator rights - relaunching elevated..." -ForegroundColor Yellow
    $argList = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $PSCommandPath)
    foreach ($k in $MyInvocation.BoundParameters.Keys) { $argList += ('-{0}' -f $k) }
    Start-Process -FilePath 'powershell.exe' -ArgumentList $argList -Verb RunAs | Out-Null
    exit 0
}

# ---------------------------------------------------------------------------
# WinRT plumbing (MediaCapture == the stack the Camera app uses)
# ---------------------------------------------------------------------------
function Load-WinRtTypes {
    # Canonical WinRT projection literals: "<Type>, <TypeNamespace>, ContentType=WindowsRuntime"
    [Windows.Media.Capture.MediaCapture, Windows.Media.Capture, ContentType = WindowsRuntime] | Out-Null
    [Windows.Media.Capture.MediaCaptureInitializationSettings, Windows.Media.Capture, ContentType = WindowsRuntime] | Out-Null
    [Windows.Media.Capture.MediaCaptureSharingMode, Windows.Media.Capture, ContentType = WindowsRuntime] | Out-Null
    [Windows.Media.Capture.StreamingCaptureMode, Windows.Media.Capture, ContentType = WindowsRuntime] | Out-Null
    [Windows.Media.Capture.Frames.MediaFrameSourceGroup, Windows.Media.Capture.Frames, ContentType = WindowsRuntime] | Out-Null
    [Windows.Media.Capture.Frames.MediaFrameSourceKind, Windows.Media.Capture.Frames, ContentType = WindowsRuntime] | Out-Null
    [Windows.Media.Capture.Frames.MediaFrameReader, Windows.Media.Capture.Frames, ContentType = WindowsRuntime] | Out-Null
    [Windows.Media.Capture.Frames.MediaFrameReaderStartStatus, Windows.Media.Capture.Frames, ContentType = WindowsRuntime] | Out-Null
}

Add-Type -AssemblyName System.Runtime.WindowsRuntime | Out-Null

$script:AsTaskOperation = [System.WindowsRuntimeSystemExtensions].GetMethods() |
    Where-Object { $_.Name -eq 'AsTask' -and $_.IsGenericMethod -and $_.GetParameters().Count -eq 1 -and
                   $_.GetParameters()[0].ParameterType.Name -eq 'IAsyncOperation`1' } |
    Select-Object -First 1
$script:AsTaskAction = [System.WindowsRuntimeSystemExtensions].GetMethods() |
    Where-Object { $_.Name -eq 'AsTask' -and -not $_.IsGenericMethod -and $_.GetParameters().Count -eq 1 -and
                   $_.GetParameters()[0].ParameterType.Name -eq 'IAsyncAction' } |
    Select-Object -First 1

# Await an IAsyncOperation<T>; unwraps the real WinRT exception (with its HResult).
function Await {
    param([object]$Operation, [Type]$ResultType)
    $asTask = $script:AsTaskOperation.MakeGenericMethod($ResultType)
    $task = $asTask.Invoke($null, @($Operation))
    try { $task.Wait(-1) | Out-Null } catch {
        $base = $_.Exception.GetBaseException()
        throw (New-Object System.Exception ('0x{0:X8} {1}' -f $base.HResult, $base.Message), $base)
    }
    return $task.Result
}

# Await an IAsyncAction (no result).
function AwaitAction {
    param([object]$Action)
    $task = $script:AsTaskAction.Invoke($null, @($Action))
    try { $task.Wait(-1) | Out-Null } catch {
        $base = $_.Exception.GetBaseException()
        throw (New-Object System.Exception ('0x{0:X8} {1}' -f $base.HResult, $base.Message), $base)
    }
}

function Get-HResultHex([int]$Hr) { '0x{0:X8}' -f $Hr }

# ---------------------------------------------------------------------------
# Target selection (RGB camera only; IR/Hello camera is never selected)
# ---------------------------------------------------------------------------
function Resolve-Target {
    $resolved = [pscustomobject]@{ InstanceId = ''; Group = $null; Source = 'heuristic' }

    # 1) explicit parameter
    # 2) HelloFix config.txt device= line (targets the same camera as the app)
    # 3) MediaFrameSourceGroup heuristic: color source whose device id contains MI_00
    # 4) first color-source group (warned)
    $fromConfig = ''
    $configPath = Join-Path $env:APPDATA 'Windows Hello Fix\config.txt'
    if (Test-Path $configPath) {
        foreach ($line in (Get-Content $configPath)) {
            if ($line -match '^\s*device\s*=\s*(.+?)\s*$') { $fromConfig = $Matches[1]; break }
        }
    }

    $groups = Await ([Windows.Media.Capture.Frames.MediaFrameSourceGroup]::FindAllAsync()) `
                    ([System.Collections.Generic.IReadOnlyList[Windows.Media.Capture.Frames.MediaFrameSourceGroup]])

    $colorGroups = @()
    foreach ($g in $groups) {
        $color = $g.SourceInfos | Where-Object { $_.SourceKind -eq [Windows.Media.Capture.Frames.MediaFrameSourceKind]::Color } | Select-Object -First 1
        if ($color) { $colorGroups += [pscustomobject]@{ Group = $g; DeviceId = $color.DeviceInformation.Id } }
    }

    if ($colorGroups.Count -eq 0) {
        throw "No camera with a color frame source found. (If camera privacy is globally off, groups can still enumerate but access will be denied later.)"
    }

    foreach ($candidate in @($InstanceId, $fromConfig)) {
        if ($candidate) {
            $norm = $candidate.ToLowerInvariant().Replace('\', '#')
            foreach ($cg in $colorGroups) {
                if ($cg.DeviceId.ToLowerInvariant().Contains($norm)) {
                    $resolved.InstanceId = $candidate
                    $resolved.Group = $cg.Group
                    $resolved.Source = if ($candidate -eq $InstanceId) { 'parameter' } else { 'config.txt' }
                    return $resolved
                }
            }
        }
    }

    foreach ($cg in $colorGroups) {
        if ($cg.DeviceId.ToLowerInvariant().Contains('mi_00')) {
            $resolved.InstanceId = $cg.DeviceId
            $resolved.Group = $cg.Group
            $resolved.Source = 'MI_00 heuristic'
            return $resolved
        }
    }

    Write-StressLog 'WARN' "Could not match a configured instance id; using first color camera group: $($colorGroups[0].DeviceId)" 'Yellow'
    $resolved.InstanceId = $colorGroups[0].DeviceId
    $resolved.Group = $colorGroups[0].Group
    $resolved.Source = 'first color group'
    return $resolved
}

# ---------------------------------------------------------------------------
# Camera probe — the same classification semantics as CameraRecoveryFailsafe
# ---------------------------------------------------------------------------
function Invoke-CameraProbe {
    # Returns: Verdict, HResult(hex string), Detail, PnpStatus, PnpProblem
    $out = [pscustomobject]@{
        Verdict = 'Unknown'; HResult = ''; Detail = ''; PnpStatus = ''; PnpProblem = ''
    }

    try {
        $dev = Get-PnpDevice -InstanceId $script:TargetInstanceId -ErrorAction Stop
        $out.PnpStatus = "$($dev.Status)"
        $out.PnpProblem = "$($dev.Problem)"
    } catch {
        $out.PnpStatus = 'NotFound'
        $out.PnpProblem = 'CM_PROB_PHANTOM?'
    }

    $mc = $null; $reader = $null
    try {
        $mc = New-Object Windows.Media.Capture.MediaCapture
        $settings = New-Object Windows.Media.Capture.MediaCaptureInitializationSettings
        $settings.SourceGroup = $script:TargetGroup
        $settings.StreamingCaptureMode = [Windows.Media.Capture.StreamingCaptureMode]::Video
        $settings.SharingMode = [Windows.Media.Capture.MediaCaptureSharingMode]::SharedReadOnly
        AwaitAction ($mc.InitializeAsync($settings))

        # PS projects IMapView as KeyValuePair pairs keyed by source-id strings that embed
        # the device-interface GUID: {e5323777-...} = KSCATEGORY_VIDEO_CAMERA (RGB),
        # {24e552d7-...} = KSCATEGORY_SENSOR_CAMERA (IR). Select the RGB source by GUID.
        $colorSource = $null
        foreach ($kv in $mc.FrameSources) {
            if ($kv.Key -like '*e5323777*' -and $kv.Key -notlike '*24e552d7*') { $colorSource = $kv.Value; break }
        }
        if (-not $colorSource) {
            $out.Verdict = 'Failed'; $out.Detail = 'No color frame source on the initialized capture'
            return $out
        }

        $reader = Await ($mc.CreateFrameReaderAsync($colorSource)) ([Windows.Media.Capture.Frames.MediaFrameReader])
        $startStatus = Await ($reader.StartAsync()) ([Windows.Media.Capture.Frames.MediaFrameReaderStartStatus])
        if ("$startStatus" -ne 'Success') {
            $out.Verdict = 'Failed'; $out.Detail = "FrameReaderStart=$startStatus"
            try { AwaitAction ($reader.StopAsync()) } catch { }
            return $out
        }

        $deadline = [DateTime]::UtcNow.AddSeconds($FrameTimeoutSeconds)
        $gotFrame = $false
        while ([DateTime]::UtcNow -lt $deadline) {
            if ($reader.TryAcquireLatestFrame()) { $gotFrame = $true; break }
            Start-Sleep -Milliseconds 100
        }
        try { AwaitAction ($reader.StopAsync()) } catch { }

        if ($gotFrame) { $out.Verdict = 'Healthy'; $out.Detail = 'First frame obtained' }
        else { $out.Verdict = 'Timeout'; $out.Detail = "Start=Success but no frame within ${FrameTimeoutSeconds}s" }
        return $out
    }
    catch {
        $base = $_.Exception.GetBaseException()
        $hr = 0
        if ($base) { $hr = $base.HResult }
        $out.HResult = Get-HResultHex $hr

        if ($hr -eq $script:HR_REBOOT_REQUIRED)   { $out.Verdict = 'RebootRequired'; $out.Detail = 'MF_E_REBOOT_REQUIRED - the Issue #1 signature' }
        elseif ($hr -eq $script:HR_ACCESSDENIED)  { $out.Verdict = 'AccessDenied';    $out.Detail = 'E_ACCESSDENIED - camera privacy setting, NOT a wedge' }
        elseif ($hr -eq $script:HR_DEVICE_LOCKED -or $hr -eq $script:HR_START_STREAMING) {
            $out.Verdict = 'InUse'; $out.Detail = 'Device responds but another client holds it (busy = healthy)'
        }
        elseif ($hr -eq $script:HR_NO_CAPTURE_DEV){ $out.Verdict = 'NoCaptureDevice'; $out.Detail = 'MF reports no capture device available' }
        else { $out.Verdict = 'Failed'; $out.Detail = 'Initialization/activation failure' }
        return $out
    }
    finally {
        $reader = $null; $mc = $null
        [GC]::Collect(); [GC]::WaitForPendingFinalizers()
    }
}

function Show-ProbeResult {
    param([pscustomobject]$Probe, [string]$Tag)
    $color = 'Green'
    if ($Probe.Verdict -eq 'RebootRequired' -or $Probe.Verdict -in @('Failed', 'Timeout')) { $color = 'Red' }
    elseif ($Probe.Verdict -in @('AccessDenied', 'InUse', 'NoCaptureDevice')) { $color = 'Yellow' }
    Write-StressLog 'INFO' ("{0} PnP={1}/{2} CameraStack={3} HRESULT={4} ({5})" -f `
        $Tag, $Probe.PnpStatus, $Probe.PnpProblem, $Probe.Verdict, $Probe.HResult, $Probe.Detail) $color
}

function Test-WedgeSignature {
    param([pscustomobject]$Probe)
    # RebootRequired is the definitive Issue #1 signature; Timeout/Failed with PnP
    # OK are treated as wedge-suspect and confirmed by a second probe by the caller.
    return ($Probe.Verdict -eq 'RebootRequired')
}

# ---------------------------------------------------------------------------
# Device toggling
# ---------------------------------------------------------------------------
function Resolve-ToggleDeviceId {
    if (-not $ToggleParent) { return $script:TargetInstanceId }
    $parent = (Get-PnpDeviceProperty -InstanceId $script:TargetInstanceId -KeyName 'DEVPKEY_Device_Parent').Data
    Write-StressLog 'WARN' "ToggleParent: will toggle composite parent '$parent' (briefly re-detects IR sibling too)" 'Yellow'
    return $parent
}

function Resolve-HelloFixExe {
    if ($HelloFixExe) { return $HelloFixExe }
    $candidates = Get-ChildItem 'C:\Program Files\WindowsHelloFix\Windows_Hello_Fix_v*.exe' -ErrorAction SilentlyContinue |
                  Sort-Object Name -Descending | Select-Object -First 1
    if ($candidates) { return $candidates.FullName }
    throw "-ToggleTool HelloFix requested but no Windows_Hello_Fix_v*.exe found in C:\Program Files\WindowsHelloFix (use -HelloFixExe)"
}

function Invoke-DeviceToggle {
    param([bool]$Enable, [string]$DeviceId)
    if ($ToggleTool -eq 'HelloFix') {
        # HelloFix acts on its own configured camera; only meaningful when it matches the target.
        $arg = if ($Enable) { '--enable-camera' } else { '--disable-camera' }
        $p = Start-Process -FilePath $script:HelloFixPath -ArgumentList $arg -Wait -PassThru -WindowStyle Hidden
        Write-StressLog 'INFO' ("HelloFix {0} exit={1}" -f $arg, $p.ExitCode)
        return ($p.ExitCode -eq 0)
    }
    $verb = if ($Enable) { '/enable-device' } else { '/disable-device' }
    $out = & pnputil $verb $DeviceId 2>&1
    $ok = ($LASTEXITCODE -eq 0)
    if (-not $ok) { Write-StressLog 'WARN' "pnputil $verb failed: $($out -join ' ')" 'Yellow' }
    return $ok
}

function Wait-PnpSettle {
    param([string]$DeviceId, [int]$TimeoutSeconds = 25)
    $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
    while ([DateTime]::UtcNow -lt $deadline) {
        $dev = Get-PnpDevice -InstanceId $DeviceId -ErrorAction SilentlyContinue
        if ($dev -and "$($dev.Status)" -eq 'OK') { return $true }
        Start-Sleep -Milliseconds 500
    }
    return $false
}

# ---------------------------------------------------------------------------
# One activation (open -> frame -> close) with no device interaction
# ---------------------------------------------------------------------------
function Invoke-ActivationOnce {
    try {
        $probe = Invoke-CameraProbe
        return $probe
    } catch {
        return [pscustomobject]@{ Verdict = 'Failed'; HResult = ''; Detail = "$_"; PnpStatus = ''; PnpProblem = '' }
    }
}

# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------
Load-WinRtTypes
Write-Host ''
Write-Host '=== CameraStressRepro — Issue #1 (0xA00F4241 / MF_E_REBOOT_REQUIRED) harness ===' -ForegroundColor Cyan
Write-Host ("Mode={0} Iterations={1} ToggleTool={2} ToggleParent={3} Log={4}" -f $Mode, $Iterations, $ToggleTool, $ToggleParent, $LogPath)
if ($Mode -eq 'MidStreamToggle' -or $Mode -eq 'IdleToggleChurn') {
    Write-Host 'WARNING: this intentionally stress-tests the camera device stack. If a real wedge appears, a reboot may be needed.' -ForegroundColor Yellow
}
Write-Host ''

$target = Resolve-Target
$script:TargetGroup = $target.Group
$script:TargetInstanceId = $target.InstanceId
Write-StressLog 'INFO' ("Target camera ({0}): {1}" -f $target.Source, $target.InstanceId) 'Cyan'

if ($ToggleTool -eq 'HelloFix') {
    $script:HelloFixPath = Resolve-HelloFixExe
    Write-StressLog 'INFO' "HelloFix toggle tool: $script:HelloFixPath"
}
$script:ToggleId = Resolve-ToggleDeviceId
Write-StressLog 'INFO' "Toggle device id: $script:ToggleId"

$baseline = Invoke-CameraProbe
Show-ProbeResult $baseline '[baseline]'
if ($baseline.Verdict -ne 'Healthy' -and $Mode -ne 'ProbeOnly') {
    Write-StressLog 'WARN' "Baseline is not healthy - results may be confounded. Fix the camera state first (or continue anyway)." 'Yellow'
}
if ($Mode -eq 'ProbeOnly') {
    Write-Host ''
    Write-Host 'ProbeOnly complete. Verdict meanings:' -ForegroundColor Cyan
    Write-Host '  Healthy        camera stack works (PnP + MF both fine)'
    Write-Host '  RebootRequired MF_E_REBOOT_REQUIRED - THE Issue #1 signature'
    Write-Host '  AccessDenied   privacy setting blocks camera access (not a wedge)'
    Write-Host '  InUse          another client holds the camera (device responsive)'
    Write-Host '  Timeout        stream started but no frames - wedge suspect'
    Write-Host '  Failed         initialization failure - see HRESULT'
    exit 0
}

$stopNow = $false
for ($i = 1; $i -le $Iterations; $i++) {
    if ($stopNow) { break }
    Write-StressLog 'INFO' ("--- iteration {0}/{1} ({2}) ---" -f $i, $Iterations, $Mode)

    switch ($Mode) {

        'ActivationChurn' {
            $probe = Invoke-ActivationOnce
            Show-ProbeResult $probe ('[churn {0}]' -f $i)
            if ($probe.Verdict -eq 'RebootRequired') { $script:WedgeCount++ }
            if ($UntilFailure -and (Test-WedgeSignature $probe)) { $stopNow = $true }
        }

        'IdleToggleChurn' {
            if (-not (Invoke-DeviceToggle -Enable:$false -DeviceId $script:ToggleId)) { Write-StressLog 'WARN' 'disable failed - continuing' 'Yellow' }
            Start-Sleep -Seconds $SettleSeconds
            if (-not (Invoke-DeviceToggle -Enable:$true -DeviceId $script:ToggleId)) { Write-StressLog 'WARN' 'enable failed - continuing' 'Yellow' }
            Wait-PnpSettle -DeviceId $script:ToggleId | Out-Null
            for ($c = 0; $c -lt $ChurnPerCycle; $c++) {
                $probe = Invoke-ActivationOnce
                if ($probe.Verdict -ne 'Healthy') { Show-ProbeResult $probe ('[cycle {0} churn {1}]' -f $i, $c) }
            }
            $final = Invoke-CameraProbe
            Show-ProbeResult $final ('[cycle {0} final]' -f $i)
            if ($final.Verdict -eq 'RebootRequired') { $script:WedgeCount++ }
            if ($UntilFailure -and (Test-WedgeSignature $final)) { $stopNow = $true }
        }

        'MidStreamToggle' {
            # 1) start a LIVE stream and confirm frames are flowing
            $mc = $null; $reader = $null
            try {
                $mc = New-Object Windows.Media.Capture.MediaCapture
                $settings = New-Object Windows.Media.Capture.MediaCaptureInitializationSettings
                $settings.SourceGroup = $script:TargetGroup
                $settings.StreamingCaptureMode = [Windows.Media.Capture.StreamingCaptureMode]::Video
                $settings.SharingMode = [Windows.Media.Capture.MediaCaptureSharingMode]::SharedReadOnly
                AwaitAction ($mc.InitializeAsync($settings))
                $colorSource = $null
                foreach ($kv in $mc.FrameSources) {
                    if ($kv.Key -like '*e5323777*' -and $kv.Key -notlike '*24e552d7*') { $colorSource = $kv.Value; break }
                }
                if (-not $colorSource) { throw 'no color frame source after initialization' }
                $reader = Await ($mc.CreateFrameReaderAsync($colorSource)) ([Windows.Media.Capture.Frames.MediaFrameReader])
                $startStatus = Await ($reader.StartAsync()) ([Windows.Media.Capture.Frames.MediaFrameReaderStartStatus])
                if ("$startStatus" -ne 'Success') { throw "FrameReaderStart=$startStatus" }
                $gotFrame = $false
                $deadline = [DateTime]::UtcNow.AddSeconds($FrameTimeoutSeconds)
                while ([DateTime]::UtcNow -lt $deadline) {
                    if ($reader.TryAcquireLatestFrame()) { $gotFrame = $true; break }
                    Start-Sleep -Milliseconds 100
                }
                if (-not $gotFrame) { throw 'stream did not produce frames before toggle' }
                Write-StressLog 'INFO' "[stream] LIVE - frames flowing; disabling device mid-stream now"

                # 2) yank the device WHILE the frame-server client session is alive
                Invoke-DeviceToggle -Enable:$false -DeviceId $script:ToggleId | Out-Null
                Start-Sleep -Seconds $MidStreamHoldSeconds

                # 3) bring the device back (stream session still attached across the identity change)
                Invoke-DeviceToggle -Enable:$true -DeviceId $script:ToggleId | Out-Null
                $settled = Wait-PnpSettle -DeviceId $script:ToggleId
                Write-StressLog 'INFO' ("[stream] device re-enabled, PnP settled={0}; stopping stream client" -f $settled)
            } catch {
                $base = $_.Exception.GetBaseException()
                Write-StressLog 'WARN' ("[stream] stream setup/teardown issue: 0x{0:X8} {1}" -f $base.HResult, $base.Message) 'Yellow'
            }
            finally {
                try { if ($reader) { AwaitAction ($reader.StopAsync()) } } catch { }
                $reader = $null; $mc = $null
                [GC]::Collect(); [GC]::WaitForPendingFinalizers()
            }

            # 4) probe the camera stack after the mid-stream identity churn
            Start-Sleep -Seconds $SettleSeconds
            $probe = Invoke-CameraProbe
            Show-ProbeResult $probe ('[cycle {0} post-toggle]' -f $i)
            if ($probe.Verdict -eq 'RebootRequired') { $script:WedgeCount++ }
            if ($probe.Verdict -in @('Timeout', 'Failed')) {
                # confirm persistence before treating as the wedge
                Start-Sleep -Seconds 3
                $confirm = Invoke-CameraProbe
                Show-ProbeResult $confirm ('[cycle {0} confirm ]' -f $i)
                if ($confirm.Verdict -in @('Timeout', 'Failed', 'RebootRequired')) {
                    $script:WedgeCount++
                    if ($UntilFailure) { $stopNow = $true }
                }
            }
            elseif ($UntilFailure -and (Test-WedgeSignature $probe)) { $stopNow = $true }
        }
    }

    if (-not $stopNow -and $i -lt $Iterations) { Start-Sleep -Seconds 1 }
}

Write-Host ''
Write-Host '=== SUMMARY ===' -ForegroundColor Cyan
Write-StressLog 'INFO' ("Completed. Mode={0} Iterations={1} WedgeSignatures={2}" -f $Mode, $Iterations, $script:WedgeCount)
if ($script:WedgeCount -gt 0) {
    Write-Host ''
    Write-Host 'WEDGE SIGNATURE OBSERVED. Now test recovery WITHOUT rebooting:' -ForegroundColor Red
    Write-Host '  1. Make sure the HelloFix daemon is running with monitoring ON (config.txt monitoring=1).' -ForegroundColor Red
    Write-Host '  2. Trigger a recovery pass: lock the session (Win+L) and unlock it, or wait <=10 min for the backup poll.' -ForegroundColor Red
    Write-Host '  3. Watch %APPDATA%\Windows Hello Fix\diagnostic.log for CameraRecovery_* lines' -ForegroundColor Red
    Write-Host '     (HealthCheck -> RungStart -> RungResult -> Recovered | RecoveryExhausted).' -ForegroundColor Red
    Write-Host '  4. Re-check with:  powershell -File tools\CameraStressRepro.ps1 -Mode ProbeOnly' -ForegroundColor Red
    Write-Host '  5. Also confirm in the Camera app (expect 0xA00F4241 (0xC00D7167) before recovery).' -ForegroundColor Red
    Write-Host '  6. Collect evidence: this log + diagnostic.log + which rung (1-4) recovered the camera.' -ForegroundColor Red
} else {
    Write-Host 'No wedge signature in this run. To push harder:' -ForegroundColor Green
    Write-Host '  - increase -Iterations (e.g. 300) or run overnight with -UntilFailure'
    if ($Mode -ne 'MidStreamToggle') {
        Write-Host '  - try the aggressive mode: -Mode MidStreamToggle -UntilFailure'
        Write-Host '  - escalate scope (also re-detects the IR sibling briefly): -ToggleParent'
    }
    Write-Host "  Log: $LogPath"
}
