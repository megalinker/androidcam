<#
  Assemble the distributable payload folder (dist/PhoneCam) from build outputs +
  bundled adb + VC++ redists + the packaging files. Idempotent (skips re-downloads).
#>
param(
  [string]$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path,
  [string]$ApkPath,
  [string]$OutDir
)
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
$win = Join-Path $RepoRoot 'windows'
if (-not $OutDir) { $OutDir = Join-Path $RepoRoot 'dist\PhoneCam' }
foreach ($d in @('bin','bin\softcam\x64','bin\softcam\Win32','bin\adb','redist')) { New-Item -ItemType Directory -Force (Join-Path $OutDir $d) | Out-Null }

$rel = Join-Path $win 'build\Release'
Copy-Item (Join-Path $rel 'receiver.exe') (Join-Path $OutDir 'bin') -Force
foreach ($d in 'avformat-63','avcodec-63','avutil-61','swscale-10','swresample-7') { Copy-Item (Join-Path $rel "$d.dll") (Join-Path $OutDir 'bin') -Force }
Copy-Item (Join-Path $win 'third_party\softcam\dist\bin\x64\softcam.dll')   (Join-Path $OutDir 'bin\softcam\x64')   -Force
Copy-Item (Join-Path $win 'third_party\softcam\dist\bin\Win32\softcam.dll') (Join-Path $OutDir 'bin\softcam\Win32') -Force
Copy-Item (Join-Path $PSScriptRoot 'files\phonecam.ps1') (Join-Path $OutDir 'bin') -Force
Copy-Item (Join-Path $PSScriptRoot 'files\PhoneCam.bat') $OutDir -Force
Copy-Item (Join-Path $PSScriptRoot 'files\README.txt')   $OutDir -Force

# Compile the windowed launcher PhoneCam.exe. Prefer the Roslyn csc from the installed
# Visual Studio (modern C#, so we can compile the vendored QRCoder source for the Wi-Fi
# pairing QR); the in-box .NET Framework csc is too old for QRCoder.
$csc = $null
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (Test-Path $vswhere) {
    $vs = & $vswhere -latest -products * -property installationPath
    if ($vs) { $c = Get-ChildItem (Join-Path $vs 'MSBuild\*\Bin\Roslyn\csc.exe') -ErrorAction SilentlyContinue | Select-Object -First 1; if ($c) { $csc = $c.FullName } }
}
if (-not $csc) { throw 'Roslyn csc.exe not found (need Visual Studio 2019/2022 Build Tools) — cannot build PhoneCam.exe.' }
$guiSrc = @(Join-Path $PSScriptRoot 'files\PhoneCam-GUI.cs') + @((Get-ChildItem (Join-Path $PSScriptRoot 'files\qrcoder\*.cs')).FullName)
& $csc /nologo /target:winexe "/out:$(Join-Path $OutDir 'PhoneCam.exe')" `
    /reference:System.Windows.Forms.dll /reference:System.Drawing.dll `
    /reference:System.IO.Compression.dll /reference:System.IO.Compression.FileSystem.dll `
    $guiSrc
if ($LASTEXITCODE -ne 0) { throw 'PhoneCam.exe (GUI) compile failed' }

if ($ApkPath) { Copy-Item $ApkPath (Join-Path $OutDir 'PhoneCam.apk') -Force }
elseif (-not (Test-Path (Join-Path $OutDir 'PhoneCam.apk'))) { Write-Warning 'No APK present (pass -ApkPath); the installer build needs PhoneCam.apk.' }

$adbDst = Join-Path $OutDir 'bin\adb'
if (-not (Test-Path (Join-Path $adbDst 'adb.exe'))) {
  $t = Join-Path $env:TEMP 'pc-pt'; New-Item -ItemType Directory -Force $t | Out-Null
  $z = Join-Path $t 'pt.zip'
  Invoke-WebRequest 'https://dl.google.com/android/repository/platform-tools-latest-windows.zip' -OutFile $z -UseBasicParsing
  Expand-Archive $z $t -Force
  foreach ($f in 'adb.exe','AdbWinApi.dll','AdbWinUsbApi.dll') { Copy-Item (Join-Path $t "platform-tools\$f") $adbDst -Force }
}
$rd = Join-Path $OutDir 'redist'
if (-not (Test-Path (Join-Path $rd 'vc_redist.x64.exe'))) { Invoke-WebRequest 'https://aka.ms/vs/17/release/vc_redist.x64.exe' -OutFile (Join-Path $rd 'vc_redist.x64.exe') -UseBasicParsing }
if (-not (Test-Path (Join-Path $rd 'vc_redist.x86.exe'))) { Invoke-WebRequest 'https://aka.ms/vs/17/release/vc_redist.x86.exe' -OutFile (Join-Path $rd 'vc_redist.x86.exe') -UseBasicParsing }
Write-Host "Assembled payload at $OutDir"
