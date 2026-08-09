<#
.SYNOPSIS
  Record a Perfetto system trace with Android battery counters and (where the hardware supports it)
  on-device power-rail measurements, while PhoneCam streams.

.DESCRIPTION
  App-side telemetry can only tell us what the app *thinks* it is doing. This gives an independent,
  platform-level view: battery capacity/current/charge counters, per-rail energy, CPU frequency and
  scheduling, all on one timeline. Open the result at https://ui.perfetto.dev.

  Requirements / honesty about them:
    * Battery counters (`android.power`) work on any Android 10+ device, no root.
    * **Power rails (ODPM) are hardware.** They exist on Pixel 6 and later; on other devices the
      `collect_power_rails` block simply produces nothing. Absence of rail data is *not* evidence of
      zero power use — this script reports which of the two it got.
    * `atrace` categories need no root; the `perfetto` binary is on-device on Android 10+.

  For a per-subsystem power breakdown with a UI, Android Studio's Power Profiler reads the same ODPM
  source; this script is the scriptable equivalent for repeatable runs.

.EXAMPLE
  .\tools\power-trace.ps1 -Seconds 120 -Out .\docs\perf-audit\runs\trace.pftrace
#>
[CmdletBinding()]
param(
  [int]$Seconds = 120,
  [string]$Out = "$PSScriptRoot\..\docs\perf-audit\runs\phonecam-power.pftrace",
  [string]$Adb
)

$ErrorActionPreference = 'Stop'

function Find-Adb {
  if ($Adb -and (Test-Path $Adb)) { return $Adb }
  foreach ($p in @("$PSScriptRoot\..\dist\PhoneCam\bin\adb\adb.exe",
                   "$env:LOCALAPPDATA\Android\Sdk\platform-tools\adb.exe")) {
    if (Test-Path $p) { return (Resolve-Path $p).Path }
  }
  $g = Get-Command adb -ErrorAction SilentlyContinue
  if ($g) { return $g.Source }
  throw "adb not found. Pass -Adb <path>."
}
$adb = Find-Adb

# TraceConfig per the Perfetto "Battery counters and power rails" data-source documentation.
# battery_poll_ms 1000: battery counters change slowly, and a tighter poll only costs power itself.
$config = @'
buffers: { size_kb: 65536 fill_policy: RING_BUFFER }
data_sources: {
  config {
    name: "android.power"
    android_power_config {
      battery_poll_ms: 1000
      battery_counters: BATTERY_COUNTER_CAPACITY_PERCENT
      battery_counters: BATTERY_COUNTER_CHARGE
      battery_counters: BATTERY_COUNTER_CURRENT
      battery_counters: BATTERY_COUNTER_VOLTAGE
      collect_power_rails: true
    }
  }
}
data_sources: {
  config {
    name: "linux.sys_stats"
    sys_stats_config { stat_period_ms: 1000 stat_counters: STAT_CPU_TIMES stat_counters: STAT_FORK_COUNT }
  }
}
data_sources: {
  config {
    name: "linux.process_stats"
    process_stats_config { scan_all_processes_on_start: true proc_stats_poll_ms: 2000 }
  }
}
data_sources: {
  config {
    name: "linux.ftrace"
    ftrace_config {
      ftrace_events: "power/cpu_frequency"
      ftrace_events: "power/cpu_idle"
      ftrace_events: "power/suspend_resume"
      atrace_categories: "camera"
      atrace_categories: "video"
      atrace_categories: "audio"
      atrace_categories: "freq"
      atrace_categories: "idle"
      atrace_apps: "com.phonecam"
    }
  }
}
'@

$durMs = $Seconds * 1000
$config = "duration_ms: $durMs`n" + $config

New-Item -ItemType Directory -Force (Split-Path $Out) | Out-Null
$tmpCfg = Join-Path $env:TEMP 'phonecam-power.cfg'
# Write LF-only: the on-device parser is not fond of CRLF in a textproto piped through the shell.
[IO.File]::WriteAllText($tmpCfg, ($config -replace "`r`n", "`n"))

Write-Host "Recording $Seconds s of power/CPU trace. Start (or keep) the stream running now." -ForegroundColor Cyan
& $adb push $tmpCfg /data/local/tmp/phonecam-power.cfg | Out-Null
& $adb shell "cat /data/local/tmp/phonecam-power.cfg | perfetto --txt -c - -o /data/misc/perfetto-traces/phonecam-power.pftrace"
if ($LASTEXITCODE -ne 0) { throw "perfetto failed on the device (is this Android 10+?)" }
& $adb pull /data/misc/perfetto-traces/phonecam-power.pftrace $Out | Out-Null
& $adb shell rm -f /data/misc/perfetto-traces/phonecam-power.pftrace /data/local/tmp/phonecam-power.cfg | Out-Null

$size = (Get-Item $Out).Length
Write-Host "`nTrace written: $Out ($([math]::Round($size/1MB,1)) MB)"
Write-Host "Open it at https://ui.perfetto.dev and look for:"
Write-Host "  * 'batt.*' counters      — capacity %, charge µAh, current µA over the session"
Write-Host "  * 'power.rails.*' tracks — per-subsystem energy (Pixel 6+ only; absent elsewhere)"
Write-Host "  * cpu frequency / idle   — whether cores are being kept out of deep idle"
Write-Host "  * the com.phonecam slices — camera / codec / audio activity on the same timeline"

# Say plainly which of the two we actually got, so a missing rail track is never read as "no power used".
$rails = & $adb shell "ls /sys/bus/iio/devices/ 2>/dev/null | head -5"
Write-Host "`nODPM probe (iio devices present on this phone): $($rails -join ' ')"
Write-Host "If the trace has no power.rails tracks, this device has no on-device power monitor —"
Write-Host "use the battery counters plus tools\battery-session.ps1 instead. That is a hardware limit,"
Write-Host "not a measurement of zero."
