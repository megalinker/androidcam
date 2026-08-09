<#
.SYNOPSIS
  Run ONE instrumented PhoneCam streaming session and print a battery/thermal/pipeline summary.

.DESCRIPTION
  This is the repeatable unit of the battery benchmark. It:
    1. resets the phone's diagnostic state (batterystats, logcat buffer),
    2. records the battery counters BEFORE,
    3. starts a streaming session with phone-side diagnostics enabled,
    4. streams for -Minutes,
    5. stops, records the counters AFTER,
    6. pulls the phone's structured diagnostics and prints a one-screen summary.

  No root required. Nothing here is PhoneCam-specific magic — every measurement comes from a
  documented Android facility:
    * `dumpsys battery`            — level, temperature, charging state
    * BatteryManager charge counter (via the app's own session_summary line) — the trustworthy µAh delta
    * `dumpsys batterystats`       — per-uid CPU/wakelock/radio accounting (Battery Historian input)
    * `dumpsys thermalservice`     — thermal status
    * the app's `PhoneCamDiag` logcat stream — capture/encode/bitrate/CPU/thermal samples

.PARAMETER Transport
  wifi | usb.

  ** wifi is the only transport that yields a valid battery number. ** Over USB the cable charges the
  phone, so the battery delta is meaningless — use -Transport usb only for pipeline metrics (fps,
  bitrate, dropped frames), never for drain.

.PARAMETER Scenario
  A free-text label recorded in the summary (e.g. "static", "motion", "idle-connected"). Keep the
  scene, brightness, room and thermal state constant between the runs you intend to compare.

.EXAMPLE
  # Baseline: 15 minutes of a static scene over Wi-Fi, phone unplugged.
  .\tools\battery-session.ps1 -Transport wifi -Minutes 15 -Scenario static

.EXAMPLE
  # Connected but NOT streaming — the control that separates "the workload" from "our overhead".
  .\tools\battery-session.ps1 -Transport wifi -Minutes 15 -Scenario idle-connected -NoStream
#>
[CmdletBinding()]
param(
  [ValidateSet('wifi', 'usb')] [string]$Transport = 'wifi',
  [int]$Minutes = 15,
  [string]$Scenario = 'unlabelled',
  [ValidateSet('BOTH', 'CAMERA_ONLY', 'MIC_ONLY')] [string]$Mode = 'BOTH',
  [string]$Quality = 'FHD_1080P30',
  [switch]$Deep,                       # 5 s sampling instead of 30 s (costs more; use sparingly)
  [switch]$NoStream,                   # control run: app open, nothing streaming
  [string]$OutDir = "$PSScriptRoot\..\docs\perf-audit\runs",
  [string]$Adb
)

$ErrorActionPreference = 'Stop'
$pkg = 'com.phonecam'

function Find-Adb {
  if ($Adb -and (Test-Path $Adb)) { return $Adb }
  $c = @(
    "$PSScriptRoot\..\dist\PhoneCam\bin\adb\adb.exe",
    "$env:LOCALAPPDATA\Android\Sdk\platform-tools\adb.exe"
  )
  foreach ($p in $c) { if (Test-Path $p) { return (Resolve-Path $p).Path } }
  $g = Get-Command adb -ErrorAction SilentlyContinue
  if ($g) { return $g.Source }
  throw "adb not found. Pass -Adb <path>."
}

$adb = Find-Adb
function Adb { & $adb @args 2>&1 }

# ---------------------------------------------------------------- preflight
$devices = (Adb devices) -split "`n" | Where-Object { $_ -match "`tdevice$" }
if ($devices.Count -eq 0) { throw "No authorized device. Enable USB debugging and accept the prompt." }
if ($devices.Count -gt 1) { throw "More than one device attached; disconnect the others." }
$serial = ($devices[0] -split "`t")[0].Trim()

New-Item -ItemType Directory -Force $OutDir | Out-Null
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$runDir = Join-Path $OutDir "$stamp-$Transport-$Scenario"
New-Item -ItemType Directory -Force $runDir | Out-Null

function Get-BatterySnapshot {
  $d = (Adb shell dumpsys battery) -join "`n"
  $get = { param($k) if ($d -match "(?m)^\s*$k[:=]\s*(-?\d+)") { [int]$Matches[1] } else { $null } }
  [pscustomobject]@{
    Level       = & $get 'level'
    Scale       = & $get 'scale'
    TempDeciC   = & $get 'temperature'
    VoltageMv   = & $get 'voltage'
    AcPowered   = $d -match 'AC powered: true'
    UsbPowered  = $d -match 'USB powered: true'
    ChargeUah   = & $get 'Charge counter'
    Raw         = $d
  }
}

Write-Host "== PhoneCam battery session ==" -ForegroundColor Cyan
Write-Host "device=$serial transport=$Transport scenario=$Scenario mode=$Mode quality=$Quality minutes=$Minutes"
Write-Host "output: $runDir"

$before = Get-BatterySnapshot
if ($Transport -eq 'wifi' -and ($before.AcPowered -or $before.UsbPowered)) {
  Write-Warning "The phone is CHARGING. A battery delta measured while plugged in is meaningless."
  Write-Warning "Unplug the charger (keep only the data connection you need for adb, or use adb over Wi-Fi)."
}
if ($Transport -eq 'usb') {
  Write-Warning "USB transport charges the phone: treat the battery delta in this run as INVALID."
  Write-Warning "Use USB runs for pipeline metrics (fps/bitrate/drops) and Wi-Fi runs for drain."
}

# ---------------------------------------------------------------- reset counters
Write-Host "`n[1/6] resetting diagnostic state…"
Adb shell dumpsys batterystats --reset | Out-Null
Adb logcat -c | Out-Null
Adb shell am force-stop $pkg | Out-Null
foreach ($p in 'android.permission.CAMERA', 'android.permission.RECORD_AUDIO', 'android.permission.POST_NOTIFICATIONS') {
  Adb shell pm grant $pkg $p 2>&1 | Out-Null
}
Start-Sleep -Seconds 2

# ---------------------------------------------------------------- start
$deepArg = if ($Deep) { 'true' } else { 'false' }
$recvProc = $null
if ($NoStream) {
  Write-Host "[2/6] control run: launching the app WITHOUT streaming…"
  Adb shell monkey -p $pkg -c android.intent.category.LAUNCHER 1 2>&1 | Out-Null
}
elseif ($Transport -eq 'usb') {
  Write-Host "[2/6] starting the USB session…"
  Adb forward --remove tcp:27183 2>&1 | Out-Null
  Adb forward tcp:27183 tcp:27183 | Out-Null
  Adb shell am start -n "$pkg/.MainActivity" -a com.phonecam.action.USB `
    --ei usbPort 27183 --es mode $Mode --es quality $Quality --ez diag true --ez deepDiag $deepArg | Out-Null
  $rx = Join-Path $PSScriptRoot '..\windows\build-webrtc\Release\receiver.exe'
  if (-not (Test-Path $rx)) { $rx = Join-Path $PSScriptRoot '..\dist\PhoneCam\bin\receiver.exe' }
  if (-not (Test-Path $rx)) { throw "receiver.exe not found — build it first (see docs/build-and-run.md)." }
  $env:PHONECAM_STATS = '1'
  $recvProc = Start-Process -FilePath $rx -ArgumentList '--usb', '--usb-port', '27183' `
    -RedirectStandardError (Join-Path $runDir 'receiver.log') `
    -RedirectStandardOutput (Join-Path $runDir 'receiver.out') -PassThru -NoNewWindow
}
else {
  Write-Host "[2/6] Wi-Fi run: start the desktop PhoneCam app and pair the phone NOW."
  Write-Host "      (Turn ON 'Record battery diagnostics' in the phone app, or tick"
  Write-Host "       'Record phone diagnostics' in the desktop app before pressing Connect.)"
  Read-Host  "      Press Enter once the desktop app shows the stream as live"
}

# ---------------------------------------------------------------- stream
Write-Host "[3/6] streaming for $Minutes minute(s). Keep the scene and screen state constant…"
$deadline = (Get-Date).AddMinutes($Minutes)
while ((Get-Date) -lt $deadline) {
  $left = [int]($deadline - (Get-Date)).TotalSeconds
  Write-Progress -Activity 'Streaming' -Status "$left s remaining" -PercentComplete (100 - 100 * $left / ($Minutes * 60))
  Start-Sleep -Seconds 5
}
Write-Progress -Activity 'Streaming' -Completed

# ---------------------------------------------------------------- stop + collect
Write-Host "[4/6] stopping…"
if ($recvProc -and -not $recvProc.HasExited) { Stop-Process -Id $recvProc.Id -Force; Start-Sleep -Seconds 1 }
Adb shell am force-stop $pkg | Out-Null
Start-Sleep -Seconds 3
$after = Get-BatterySnapshot

Write-Host "[5/6] collecting logs…"
(Adb logcat -d -s PhoneCamDiag:I) -join "`n" | Set-Content (Join-Path $runDir 'phonecam-diag.log') -Encoding utf8
(Adb shell dumpsys batterystats) -join "`n"   | Set-Content (Join-Path $runDir 'batterystats.txt') -Encoding utf8
(Adb shell dumpsys thermalservice) -join "`n" | Set-Content (Join-Path $runDir 'thermal.txt') -Encoding utf8
$before.Raw | Set-Content (Join-Path $runDir 'battery-before.txt') -Encoding utf8
$after.Raw  | Set-Content (Join-Path $runDir 'battery-after.txt') -Encoding utf8
# The phone also keeps its own bounded copy; pull it when the platform allows.
Adb pull "/sdcard/Android/data/$pkg/files/diag" (Join-Path $runDir 'phone-diag-files') 2>&1 | Out-Null

# ---------------------------------------------------------------- summarise
Write-Host "[6/6] summary`n"
$diag = Get-Content (Join-Path $runDir 'phonecam-diag.log') -Raw -ErrorAction SilentlyContinue
$summaryLine = if ($diag) { ($diag -split "`n" | Where-Object { $_ -match 'session_summary' } | Select-Object -Last 1) } else { $null }

$pctDelta = if ($null -ne $before.Level -and $null -ne $after.Level) { $before.Level - $after.Level } else { $null }
$hours = $Minutes / 60.0

$report = [ordered]@{
  timestamp      = $stamp
  device         = $serial
  transport      = $Transport
  scenario       = $Scenario
  mode           = $Mode
  quality        = $Quality
  minutes        = $Minutes
  batteryStart   = $before.Level
  batteryEnd     = $after.Level
  batteryDelta   = $pctDelta
  batteryPctPerHour = if ($null -ne $pctDelta -and $hours -gt 0) { [math]::Round($pctDelta / $hours, 2) } else { 'n/a' }
  tempStartC     = if ($null -ne $before.TempDeciC) { $before.TempDeciC / 10.0 } else { 'n/a' }
  tempEndC       = if ($null -ne $after.TempDeciC) { $after.TempDeciC / 10.0 } else { 'n/a' }
  chargedDuringRun = ($before.AcPowered -or $before.UsbPowered -or $after.AcPowered -or $after.UsbPowered)
  phoneSummary   = $summaryLine
}
$report | ConvertTo-Json -Depth 4 | Set-Content (Join-Path $runDir 'summary.json') -Encoding utf8

$report.GetEnumerator() | ForEach-Object { '{0,-18} {1}' -f $_.Key, $_.Value }

if ($report.chargedDuringRun) {
  Write-Host "`n!! The phone was on power during this run — the battery delta is NOT a drain measurement." -ForegroundColor Yellow
}
if (-not $summaryLine) {
  Write-Host "`n!! No session_summary from the phone. Diagnostics were probably off:" -ForegroundColor Yellow
  Write-Host "   turn on 'Record battery diagnostics' in the phone app (or pass --ez diag true)." -ForegroundColor Yellow
}
Write-Host "`nArtifacts: $runDir"
Write-Host "Battery Historian input: $runDir\batterystats.txt (or run 'adb bugreport' for the full zip)"
