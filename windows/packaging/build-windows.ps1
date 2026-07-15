<#
  Reproducible Windows build for the PhoneCam release:
  downloads the FFmpeg shared SDK, clones + renames + builds softcam, and builds
  the softcam-enabled receiver. Used by CI and reproducible locally.
#>
param(
  [string]$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path,
  [string]$Toolset = '',   # e.g. 'v143' on VS2022 CI; empty keeps the project default
  [string]$VcpkgRoot = $env:VCPKG_INSTALLATION_ROOT
)
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
$win = Join-Path $RepoRoot 'windows'
$tp  = Join-Path $win 'third_party'

if (-not $VcpkgRoot) {
  $candidate = Join-Path $HOME 'vcpkg'
  if (Test-Path (Join-Path $candidate 'vcpkg.exe')) { $VcpkgRoot = $candidate }
}
$vcpkg = if ($VcpkgRoot) { Join-Path $VcpkgRoot 'vcpkg.exe' } else { $null }
if (-not $vcpkg -or -not (Test-Path $vcpkg)) { throw 'vcpkg.exe not found; set VCPKG_INSTALLATION_ROOT or pass -VcpkgRoot.' }

Write-Host '== WebRTC dependencies (libdatachannel + OpenSSL-backed SRTP) =='
$overlay = Join-Path $win 'vcpkg-overlays'
& $vcpkg install 'libdatachannel[core,srtp,ws]:x64-windows' 'libsrtp[openssl]:x64-windows' "--overlay-ports=$overlay"
if ($LASTEXITCODE) { throw 'vcpkg WebRTC dependency install failed' }

Write-Host '== FFmpeg shared SDK =='
$ff = Join-Path $tp 'ffmpeg'
if (-not (Test-Path (Join-Path $ff 'include'))) {
  $z = Join-Path $env:TEMP 'ffmpeg-shared.zip'
  Invoke-WebRequest 'https://github.com/BtbN/FFmpeg-Builds/releases/download/latest/ffmpeg-master-latest-win64-gpl-shared.zip' -OutFile $z -UseBasicParsing
  $ex = Join-Path $env:TEMP 'ffx'
  Expand-Archive $z $ex -Force
  $r = Get-ChildItem $ex -Directory | Select-Object -First 1
  New-Item -ItemType Directory -Force $ff | Out-Null
  foreach ($s in 'include','lib','bin') { Copy-Item (Join-Path $r.FullName $s) (Join-Path $ff $s) -Recurse -Force }
}

Write-Host '== softcam (clone, rename to "PhoneCam Camera", build x64 + Win32) =='
$sc = Join-Path $tp 'softcam'
if (-not (Test-Path (Join-Path $sc 'softcam.sln'))) {
  git clone --depth 1 https://github.com/tshino/softcam $sc
  if ($LASTEXITCODE) { throw 'git clone softcam failed' }
}
$scCpp = Join-Path $sc 'src\softcam\softcam.cpp'
$txt = (Get-Content $scCpp -Raw) -replace 'L"DirectShow Softcam"', 'L"PhoneCam Camera"'
[IO.File]::WriteAllText($scCpp, $txt, (New-Object System.Text.UTF8Encoding($false)))
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$msbuild = & $vswhere -latest -requires Microsoft.Component.MSBuild -find 'MSBuild\**\Bin\MSBuild.exe' | Select-Object -First 1
if (-not $msbuild) { throw 'MSBuild not found' }
$ts = @(); if ($Toolset) { $ts = @("/p:PlatformToolset=$Toolset") }
foreach ($plat in 'x64','Win32') {
  & $msbuild (Join-Path $sc 'softcam.sln') /t:softcam /p:Configuration=Release /p:Platform=$plat @ts /m /v:minimal /nologo
  if ($LASTEXITCODE) { throw "softcam $plat build failed" }
}

Write-Host '== receiver (softcam + WebRTC enabled) =='
$scLib = Join-Path $sc 'src\softcamcore\x64\Release\softcamcore.lib'
$bld = Join-Path $win 'build'
cmake -S $win -B $bld -A x64 -DWITH_SOFTCAM=ON -DWITH_WEBRTC=ON `
  "-DSOFTCAM_ROOT=$sc" "-DSOFTCAM_LIB=$scLib" `
  "-DCMAKE_TOOLCHAIN_FILE=$(Join-Path $VcpkgRoot 'scripts\buildsystems\vcpkg.cmake')"
if ($LASTEXITCODE) { throw 'cmake configure failed' }
cmake --build $bld --config Release
if ($LASTEXITCODE) { throw 'receiver build failed' }
foreach ($name in 'datachannel.dll','juice.dll','srtp2.dll','libcrypto-3-x64.dll','libssl-3-x64.dll') {
  if (-not (Test-Path (Join-Path $bld "Release\$name"))) { throw "WebRTC runtime missing after build: $name" }
}
Write-Host 'Windows build complete.'
