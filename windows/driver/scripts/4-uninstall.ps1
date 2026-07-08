# PhoneCam virtual-mic driver — step 4/4 (optional): remove the driver.
#
# Windows renames third-party INFs to oemNN.inf when installed. Run with no args to list them,
# find the PhoneCam entry, then re-run with -InfName oemNN.inf.
#
# Run ELEVATED.

#Requires -RunAsAdministrator
param([string]$InfName)
$ErrorActionPreference = 'Stop'

if (-not $InfName) {
    Write-Host "Installed third-party drivers (find the PhoneCam / virtual-audio entry):" -ForegroundColor Cyan
    pnputil /enum-drivers
    Write-Host ""
    Write-Host "Re-run: .\4-uninstall.ps1 -InfName oemNN.inf" -ForegroundColor Yellow
    return
}

pnputil /delete-driver $InfName /uninstall /force
Write-Host "Removed $InfName. (To also leave test-signing mode: bcdedit /set testsigning off, then reboot.)" -ForegroundColor Green
