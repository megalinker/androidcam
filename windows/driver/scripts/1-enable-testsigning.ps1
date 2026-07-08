# PhoneCam virtual-mic driver — step 1/4: enable kernel test-signing.
#
# Windows 10/11 refuses to load an unsigned (or self-signed, untrusted) kernel driver.
# For PERSONAL use you don't need an EV cert — you enable "test signing" mode and sign the
# driver with your own cert (steps 2-3). This shows a small "Test Mode" desktop watermark.
#
# Run this in an ELEVATED PowerShell, then REBOOT.
# To revert later:  bcdedit /set testsigning off   (then reboot)

#Requires -RunAsAdministrator
$ErrorActionPreference = 'Stop'

bcdedit /set testsigning on
Write-Host ""
Write-Host "Test-signing enabled. REBOOT now for it to take effect." -ForegroundColor Yellow
Write-Host "After reboot, run 2-make-testcert.ps1." -ForegroundColor Yellow
