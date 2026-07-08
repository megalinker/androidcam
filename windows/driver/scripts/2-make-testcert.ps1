# PhoneCam virtual-mic driver — step 2/4: create + trust a self-signed code-signing cert.
#
# Produces phonecam-test.pfx (used to sign the driver) and installs the public cert into
# LocalMachine\Root and LocalMachine\TrustedPublisher so a test-signed driver will load.
#
# Run ELEVATED, after a reboot into test-signing mode.

#Requires -RunAsAdministrator
param(
    [string]$Subject     = "CN=PhoneCam Test Signing",
    [string]$PfxPassword = "phonecam"
)
$ErrorActionPreference = 'Stop'
$here = $PSScriptRoot

$cert = New-SelfSignedCertificate `
    -Type CodeSigningCert `
    -Subject $Subject `
    -CertStoreLocation "Cert:\LocalMachine\My" `
    -KeyUsage DigitalSignature `
    -KeyExportPolicy Exportable `
    -FriendlyName "PhoneCam Test Signing" `
    -NotAfter (Get-Date).AddYears(5)

$pwd = ConvertTo-SecureString -String $PfxPassword -Force -AsPlainText
$pfx = Join-Path $here "phonecam-test.pfx"
$cer = Join-Path $here "phonecam-test.cer"

Export-PfxCertificate -Cert $cert -FilePath $pfx -Password $pwd | Out-Null
Export-Certificate    -Cert $cert -FilePath $cer | Out-Null

# Trust the cert for driver loading (Root = trust chain, TrustedPublisher = silent driver install).
Import-Certificate -FilePath $cer -CertStoreLocation "Cert:\LocalMachine\Root" | Out-Null
Import-Certificate -FilePath $cer -CertStoreLocation "Cert:\LocalMachine\TrustedPublisher" | Out-Null

Write-Host ""
Write-Host "Created + trusted test cert." -ForegroundColor Green
Write-Host "  PFX:      $pfx  (password: $PfxPassword)"
Write-Host "  Public:   $cer"
Write-Host "Next: build the driver (see ../README.md), then run 3-sign-and-install.ps1." -ForegroundColor Yellow
