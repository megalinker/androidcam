<#
  PhoneCam launcher — pick the transport and run the receiver, hands-free.

  Default (autodetect): if the phone is connected over USB (adb), it opens a USB
  tunnel and uses that — the most reliable path, since it bypasses Wi-Fi/VPN/AP-
  isolation entirely. Unless -NoStart, it also launches the PhoneCam app and presses
  Start (Camera+Mic) for you over adb. With no phone plugged in, it falls back to
  Wi-Fi using -Ip (or the last IP it learned while plugged in over USB).

  Transport is chosen automatically but you can force it:
    .\phonecam.ps1                      # autodetect, hands-free
    .\phonecam.ps1 -Preview             # + show the local preview window
    .\phonecam.ps1 -Usb                 # force the USB tunnel
    .\phonecam.ps1 -Wifi                # force Wi-Fi (uses learned / -Ip address)
    .\phonecam.ps1 -Wifi -Ip 192.168.0.101
    .\phonecam.ps1 -NoStart             # don't touch the phone; just connect
    .\phonecam.ps1 -DryRun              # show the decision + exact command, run nothing
    .\phonecam.ps1 -Smooth -AudioDevice Realtek   # receiver flags: jitter buffer + target output

  Receiver passthrough switches: -Preview -Smooth -NoAudio -Udp -AudioDevice <name-substr>.
  receiver.exe must already be built (windows\build\Release\receiver.exe).
#>
param(
  [switch]$Usb,
  [switch]$Wifi,
  [string]$Ip,
  [switch]$Preview,
  [switch]$Smooth,
  [switch]$NoAudio,
  [switch]$Udp,
  [string]$AudioDevice,
  [switch]$NoStart,
  [switch]$DryRun,
  [int]$LocalPort = 18554,
  [int]$PhonePort = 8554
)
$ErrorActionPreference = 'Stop'
$PKG = 'com.phonecam'

# ---------------------------------------------------------------- tool resolution
function Resolve-Adb {
  $c = Get-Command adb -ErrorAction SilentlyContinue
  if ($c) { return $c.Source }
  $p = Join-Path $env:LOCALAPPDATA 'Android\Sdk\platform-tools\adb.exe'
  if (Test-Path $p) { return $p }
  throw "adb not found. Install Android platform-tools or add adb to PATH."
}
$adb = Resolve-Adb
$receiver = Resolve-Path (Join-Path $PSScriptRoot '..\build\Release\receiver.exe') -ErrorAction SilentlyContinue
if (-not $receiver) { throw "receiver.exe not found under windows\build\Release. Build it first: cmake --build build --config Release" }
$ipStore = Join-Path $env:LOCALAPPDATA 'PhoneCam\last-ip.txt'

# ---------------------------------------------------------------- adb helpers
function Get-AdbState {
  # 'device' (ready), 'unauthorized' (needs the on-phone Allow prompt), or $null (none)
  foreach ($l in (& $adb devices 2>$null)) {
    if ($l -match '^\S+\s+(device|unauthorized)\b') { return $Matches[1] }
  }
  return $null
}
function Get-PhoneWifiIp {
  $out = (& $adb shell "ip -o -f inet addr show wlan0" 2>$null) -join "`n"
  if ($out -match 'inet\s+(\d+\.\d+\.\d+\.\d+)') { return $Matches[1] }
  return $null
}
function Save-Ip([string]$addr) {
  if (-not $addr) { return }
  New-Item -ItemType Directory -Force -Path (Split-Path $ipStore) | Out-Null
  Set-Content -Path $ipStore -Value $addr -Encoding ascii
}
function Load-Ip { if (Test-Path $ipStore) { (Get-Content $ipStore | Select-Object -First 1).Trim() } }

function Get-NodeCenter([string]$xml, [string]$resId) {
  $esc = [regex]::Escape($resId)
  $m = [regex]::Match($xml, 'resource-id="' + $esc + '"[^>]*bounds="\[(\d+),(\d+)\]\[(\d+),(\d+)\]"')
  if (-not $m.Success) { $m = [regex]::Match($xml, 'bounds="\[(\d+),(\d+)\]\[(\d+),(\d+)\]"[^>]*resource-id="' + $esc + '"') }
  if ($m.Success) {
    return @([int](([int]$m.Groups[1].Value + [int]$m.Groups[3].Value) / 2),
             [int](([int]$m.Groups[2].Value + [int]$m.Groups[4].Value) / 2))
  }
  return $null
}
function Start-PhoneStream {
  Write-Host "[phonecam] starting PhoneCam on the phone (Camera+Mic)..." -ForegroundColor Cyan
  foreach ($p in @('android.permission.CAMERA','android.permission.RECORD_AUDIO','android.permission.POST_NOTIFICATIONS')) {
    & $adb shell pm grant $PKG $p 2>$null | Out-Null
  }
  & $adb shell input keyevent KEYCODE_WAKEUP 2>$null | Out-Null
  & $adb shell am start -n "$PKG/.MainActivity" 2>$null | Out-Null
  Start-Sleep -Seconds 2
  for ($try = 0; $try -lt 3; $try++) {
    & $adb shell uiautomator dump /sdcard/pc_ui.xml 2>$null | Out-Null
    $xml = (& $adb shell cat /sdcard/pc_ui.xml 2>$null) -join "`n"
    $both  = Get-NodeCenter $xml "$PKG`:id/modeBoth"
    $start = Get-NodeCenter $xml "$PKG`:id/startBtn"
    if ($start) {
      if ($both) { & $adb shell input tap $both[0] $both[1]; Start-Sleep -Milliseconds 400 }
      & $adb shell input tap $start[0] $start[1]
      break
    }
    Start-Sleep -Milliseconds 800
  }
  for ($i = 0; $i -lt 16; $i++) {
    if (& $adb shell "ss -ltn 2>/dev/null | grep :$PhonePort" 2>$null) {
      Write-Host "[phonecam] phone RTSP server is up." -ForegroundColor Green; return $true
    }
    Start-Sleep -Milliseconds 500
  }
  Write-Host "[phonecam] warning: couldn't confirm the phone's server came up (it may still connect)." -ForegroundColor Yellow
  return $false
}
function Test-Reachable([string]$addr, [int]$port, [int]$ms = 2500) {
  $c = New-Object System.Net.Sockets.TcpClient
  try { $iar = $c.BeginConnect($addr, $port, $null, $null); return ($iar.AsyncWaitHandle.WaitOne($ms) -and $c.Connected) }
  catch { return $false } finally { $c.Close() }
}

# ---------------------------------------------------------------- decide transport
if ($Usb -and $Wifi) { throw "Choose one of -Usb or -Wifi, not both." }
$state = Get-AdbState
$usbAvailable = ($state -eq 'device')
if ($state -eq 'unauthorized') {
  Write-Host "[phonecam] a phone is plugged in but USB debugging isn't authorized -- tap 'Allow' on the phone (and 'Always allow')." -ForegroundColor Yellow
}
$mode = if ($Usb) { 'usb' } elseif ($Wifi) { 'wifi' } elseif ($usbAvailable) { 'usb' } else { 'wifi' }
if ($mode -eq 'usb' -and -not $usbAvailable) {
  throw "USB requested but no authorized phone is connected. Plug in + allow USB debugging, or use: -Wifi -Ip <phone-ip>"
}

# resolve the phone's Wi-Fi IP (for the Wi-Fi URL, and to remember for later)
$phoneIp = $Ip
if (-not $phoneIp -and $usbAvailable) { $phoneIp = Get-PhoneWifiIp; if ($phoneIp) { Save-Ip $phoneIp } }
if (-not $phoneIp) { $phoneIp = Load-Ip }

# auto-start the phone app (only possible while connected over adb)
if (-not $NoStart) {
  if ($usbAvailable) { if (-not $DryRun) { Start-PhoneStream | Out-Null } else { Write-Host "[phonecam] (dry-run) would start PhoneCam over adb" -ForegroundColor DarkGray } }
  else { Write-Host "[phonecam] no adb device -- press Start on the phone yourself." -ForegroundColor Yellow }
}

# ---------------------------------------------------------------- build URL + tunnel
$forwardSet = $false
if ($mode -eq 'usb') {
  if (-not $DryRun) { & $adb forward "tcp:$LocalPort" "tcp:$PhonePort" | Out-Null; $forwardSet = $true }
  $url = "rtsp://127.0.0.1:$LocalPort/"
  Write-Host "[phonecam] transport: USB tunnel  $url  ->  phone:$PhonePort" -ForegroundColor Green
  if ($phoneIp) { Write-Host "[phonecam] (Wi-Fi URL, once LAN is reachable: rtsp://${phoneIp}:$PhonePort/ )" -ForegroundColor DarkGray }
} else {
  if (-not $phoneIp) { throw "Wi-Fi mode needs the phone's IP. Pass -Ip <addr>, or connect over USB once so I can learn it." }
  $url = "rtsp://${phoneIp}:$PhonePort/"
  Write-Host "[phonecam] transport: Wi-Fi  $url" -ForegroundColor Green
  if (Test-Reachable $phoneIp $PhonePort) {
    Write-Host "[phonecam] phone is reachable over Wi-Fi." -ForegroundColor Green
  } else {
    Write-Host "[phonecam] warning: can't reach ${phoneIp}:$PhonePort (VPN killswitch / AP-isolation / firewall, or the phone isn't streaming yet). The receiver will keep retrying." -ForegroundColor Yellow
  }
}

# ---------------------------------------------------------------- run receiver
$rArgs = @($url)
if ($Preview)     { $rArgs += '--preview' }
if ($Smooth)      { $rArgs += '--smooth' }
if ($NoAudio)     { $rArgs += '--no-audio' }
if ($Udp)         { $rArgs += '--udp' }
if ($AudioDevice) { $rArgs += @('--audio-device', $AudioDevice) }
Write-Host "[phonecam] receiver.exe $($rArgs -join ' ')" -ForegroundColor Cyan
if ($DryRun) { Write-Host "[phonecam] (dry-run) not launching." -ForegroundColor DarkGray; return }

try { & $receiver @rArgs }
finally { if ($forwardSet) { & $adb forward --remove "tcp:$LocalPort" 2>$null | Out-Null } }
