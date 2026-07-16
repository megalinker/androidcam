# PhoneCam virtual-mic driver — step 3/4: catalog, sign, and install the built driver.
#
# Prereqs: test-signing on + rebooted (step 1), test cert created (step 2), and the driver
# BUILT (you have phonecam-audio.sys + phonecam-audio.inf from the Virtual-Audio-Driver fork).
# Run from an ELEVATED "x64 Native Tools" / EWDK prompt so signtool, inf2cat, pnputil resolve.
#
# Usage:
#   .\3-sign-and-install.ps1 -SysPath ..\build\phonecam-audio.sys -InfPath ..\build\phonecam-audio.inf

#Requires -RunAsAdministrator
param(
    [Parameter(Mandatory = $true)][string]$SysPath,   # built .sys
    [Parameter(Mandatory = $true)][string]$InfPath,   # driver .inf (same folder as .sys)
    [string]$Pfx         = (Join-Path $PSScriptRoot "phonecam-test.pfx"),
    [string]$PfxPassword = "phonecam",
    [string]$OsTarget    = "10_X64"                    # inf2cat target; 10_X64 covers Win10/11 x64
)
$ErrorActionPreference = 'Stop'

foreach ($p in @($SysPath, $InfPath, $Pfx)) {
    if (-not (Test-Path $p)) { throw "Not found: $p" }
}
$drvDir = Split-Path -Parent (Resolve-Path $InfPath)

Write-Host "1/3  Building catalog (inf2cat)..." -ForegroundColor Cyan
inf2cat /driver:"$drvDir" /os:$OsTarget /verbose
$cat = Get-ChildItem -Path $drvDir -Filter *.cat | Select-Object -First 1
if (-not $cat) { throw "inf2cat produced no .cat in $drvDir" }

Write-Host "2/3  Signing .sys and .cat with the test cert..." -ForegroundColor Cyan
signtool sign /fd SHA256 /f $Pfx /p $PfxPassword "$SysPath"
signtool sign /fd SHA256 /f $Pfx /p $PfxPassword "$($cat.FullName)"

Write-Host "3/3  Installing driver (pnputil)..." -ForegroundColor Cyan
pnputil /add-driver "$InfPath" /install

Write-Host ""
Write-Host "Done. Look in Settings > System > Sound for 'PhoneCam Microphone' (input)" -ForegroundColor Green
Write-Host "and 'PhoneCam Audio' (output). Then run the receiver with:" -ForegroundColor Green
Write-Host '   receiver.exe --sig-port 8891 --sig-secret <hex> --audio-device "PhoneCam Audio"'
