<#
.SYNOPSIS
  Compare the runs produced by battery-session.ps1 and print a before/after table.

.DESCRIPTION
  A single battery-percentage reading is not a result: 1 % granularity over 15 minutes is a huge
  relative error, and phone thermals drift between runs. This script aggregates repeated runs of the
  same scenario so a conclusion rests on a mean and a spread rather than one number.

  Runs whose phone was charging are excluded from the drain statistics automatically (they are still
  listed, marked, because their pipeline metrics are valid).

.EXAMPLE
  .\tools\battery-compare.ps1
  .\tools\battery-compare.ps1 -Scenario static
#>
[CmdletBinding()]
param(
  [string]$RunsDir = "$PSScriptRoot\..\docs\perf-audit\runs",
  [string]$Scenario,
  [switch]$Csv
)

$ErrorActionPreference = 'Stop'
if (-not (Test-Path $RunsDir)) { throw "No runs directory at $RunsDir — record one with tools\battery-session.ps1." }

$rows = foreach ($f in Get-ChildItem $RunsDir -Recurse -Filter summary.json) {
  $j = Get-Content $f.FullName -Raw | ConvertFrom-Json
  if ($Scenario -and $j.scenario -ne $Scenario) { continue }

  # Pull the pipeline numbers out of the phone's own session_summary line, when present.
  $s = $j.phoneSummary
  function Field([string]$k) {
    if ($s -and $s -match "\b$k=([^\s]+)") { $Matches[1] } else { $null }
  }
  [pscustomobject]@{
    Run          = $f.Directory.Name
    Scenario     = $j.scenario
    Transport    = $j.transport
    Quality      = $j.quality
    Min          = $j.minutes
    BattDelta    = $j.batteryDelta
    PctPerHour   = $j.batteryPctPerHour
    MeanCurrentMA= Field 'avgCurrentMA'
    ChargeUAh    = Field 'chargeDeltaUAh'
    CpuAvg       = Field 'cpuAvg'
    CapFps       = Field 'avgCapFps'
    EncFps       = Field 'avgEncFps'
    EncKbps      = Field 'avgEncKbps'
    TempMaxC     = Field 'tempMaxC'
    ThermalMax   = Field 'thermalMax'
    Charged      = [bool]$j.chargedDuringRun
  }
}

if (-not $rows) { throw "No matching runs found." }

if ($Csv) { $rows | ConvertTo-Csv -NoTypeInformation; return }

Write-Host "`n== Individual runs ==" -ForegroundColor Cyan
$rows | Format-Table -AutoSize

Write-Host "== Aggregated per scenario (charging runs excluded from drain) ==" -ForegroundColor Cyan
$rows | Where-Object { -not $_.Charged } | Group-Object Scenario, Transport, Quality | ForEach-Object {
  $g = $_.Group
  $vals = @($g | Where-Object { $_.PctPerHour -ne 'n/a' -and $null -ne $_.PctPerHour } | ForEach-Object { [double]$_.PctPerHour })
  $ma   = @($g | Where-Object { $_.MeanCurrentMA -and $_.MeanCurrentMA -ne 'n/a' } | ForEach-Object { [double]$_.MeanCurrentMA })
  [pscustomobject]@{
    Group        = $_.Name
    Runs         = $g.Count
    PctPerHourMean = if ($vals) { [math]::Round(($vals | Measure-Object -Average).Average, 2) } else { 'n/a' }
    PctPerHourMin  = if ($vals) { ($vals | Measure-Object -Minimum).Minimum } else { 'n/a' }
    PctPerHourMax  = if ($vals) { ($vals | Measure-Object -Maximum).Maximum } else { 'n/a' }
    MeanCurrentMA  = if ($ma) { [math]::Round(($ma | Measure-Object -Average).Average, 1) } else { 'n/a' }
    EncKbpsMean    = & {
      $k = @($g | Where-Object { $_.EncKbps } | ForEach-Object { [double]$_.EncKbps })
      if ($k) { [math]::Round(($k | Measure-Object -Average).Average, 0) } else { 'n/a' }
    }
  }
} | Format-Table -AutoSize

$n = ($rows | Where-Object { -not $_.Charged }).Count
if ($n -lt 3) {
  Write-Host "Only $n usable run(s). Battery % has 1-point granularity — record at least 3 runs per" -ForegroundColor Yellow
  Write-Host "scenario (ideally 5) before treating a difference as real." -ForegroundColor Yellow
}
