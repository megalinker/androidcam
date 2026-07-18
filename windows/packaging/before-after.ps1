<#
  before-after.ps1 — run ONE instrumented USB-path session and print aggregated per-stage stats.
  Run it twice (baseline vs a flag) and compare. The receiver's PHONECAM_STATS instrumentation does
  the measuring; this just drives a clean phone stream, runs for -Seconds, and parses the [stats] lines.

  Examples (from windows/packaging):
    .\before-after.ps1 -Label baseline
    .\before-after.ps1 -Label lowlat  -LowLatencyAudio     # F-11 A/B (compare AudioWasapiMeanMs)
    .\before-after.ps1 -Label drift   -Drift -Seconds 600  # F-03 A/B (watch AudioWasapiMeanMs trend/drops)

  Needs: a phone on adb (USB debugging) with PhoneCam installed, and a receiver built with the
  instrumentation (this branch). A hard receiver kill can wedge the phone streamer, so each run
  force-restarts the phone app first.
#>
param(
  [string]$Label = "run",
  [int]$Seconds = 60,
  [switch]$Drift,             # PHONECAM_DRIFT (F-03 audio drift compensation)
  [switch]$LowLatencyAudio,   # PHONECAM_LOWLATENCY_AUDIO (F-11 event-driven audio)
  [switch]$RawMic,            # F-12: launch the phone with --ez rawMic true
  [string]$Receiver = (Join-Path $PSScriptRoot '..\build-webrtc\Release\receiver.exe'),
  [string]$Adb = (Join-Path $PSScriptRoot '..\..\dist\PhoneCam\bin\adb\adb.exe')
)
$ErrorActionPreference = 'Continue'
if (-not (Test-Path $Receiver)) { throw "receiver not found: $Receiver (build it first)" }

# Clean phone stream (a prior hard kill can leave the streamer wedged).
& $Adb shell am force-stop com.phonecam | Out-Null
Start-Sleep -Seconds 1
& $Adb forward tcp:27183 tcp:27183 | Out-Null
& $Adb shell input keyevent KEYCODE_WAKEUP | Out-Null
$rawArg = if ($RawMic) { ' --ez rawMic true' } else { '' }
& $Adb shell am start -n com.phonecam/.MainActivity -a com.phonecam.action.USB --ei usbPort 27183 --es mode BOTH$rawArg | Out-Null
Start-Sleep -Seconds 4

$env:PHONECAM_STATS = "1"
if ($Drift) { $env:PHONECAM_DRIFT = "1" } else { Remove-Item Env:\PHONECAM_DRIFT -ErrorAction SilentlyContinue }
if ($LowLatencyAudio) { $env:PHONECAM_LOWLATENCY_AUDIO = "1" } else { Remove-Item Env:\PHONECAM_LOWLATENCY_AUDIO -ErrorAction SilentlyContinue }

$log = Join-Path $env:TEMP ("ba_" + $Label + ".log")
$p = Start-Process $Receiver -ArgumentList "--usb","--usb-port","27183" -NoNewWindow -PassThru `
       -RedirectStandardError $log -RedirectStandardOutput ($log + ".out")
Start-Sleep -Seconds $Seconds
Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
Start-Sleep -Seconds 1

$lines = Get-Content $log
function Med($xs) { if ($xs.Count -eq 0) { return 0 } ($xs | Sort-Object)[[int]($xs.Count / 2)] }
$dec = @(); $sink = @(); $wpad = @(); $adrops = 0; $vdrops = 0
foreach ($l in $lines) {
  if ($l -match 'video\.decode.*p50=([\d.]+)ms')  { $dec  += [double]$Matches[1] }
  if ($l -match 'video\.sink.*p50=([\d.]+)ms')    { $sink += [double]$Matches[1] }
  if ($l -match 'audio\.wasapi.*mean=([\d.]+)ms') { $wpad += [double]$Matches[1] }
  if ($l -match 'audio\.queue.*drops=(\d+)')      { $adrops = [int]$Matches[1] }
  if ($l -match 'video\.queue.*drops=(\d+)')      { $vdrops = [int]$Matches[1] }
}
[pscustomobject]@{
  Label             = $Label
  Seconds           = $Seconds
  Flags             = (@(if($Drift){'DRIFT'}; if($LowLatencyAudio){'LOWLAT'}; if($RawMic){'RAWMIC'}) -join ',')
  Reconnects        = ($lines | Select-String 'h264 decode:').Count
  DecodeP50ms       = (Med $dec)
  SinkP50ms         = (Med $sink)
  AudioWasapiMeanMs = (Med $wpad)
  AudioDrops        = $adrops
  VideoDrops        = $vdrops
} | Format-List
