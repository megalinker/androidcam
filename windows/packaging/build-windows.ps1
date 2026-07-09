<#
  Reproducible Windows build for the PhoneCam release:
  downloads the FFmpeg shared SDK, clones + renames + builds softcam, and builds
  the softcam-enabled receiver. Used by CI and reproducible locally.
#>
param(
  [string]$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path,
  [string]$Toolset = ''    # e.g. 'v143' on VS2022 CI; empty keeps the project default
)
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
$win = Join-Path $RepoRoot 'windows'
$tp  = Join-Path $win 'third_party'

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

Write-Host '== receiver (softcam-enabled) =='
$scLib = Join-Path $sc 'src\softcamcore\x64\Release\softcamcore.lib'
$bld = Join-Path $win 'build'
cmake -S $win -B $bld -A x64 -DWITH_SOFTCAM=ON "-DSOFTCAM_ROOT=$sc" "-DSOFTCAM_LIB=$scLib"
if ($LASTEXITCODE) { throw 'cmake configure failed' }
cmake --build $bld --config Release
if ($LASTEXITCODE) { throw 'receiver build failed' }
Write-Host 'Windows build complete.'
