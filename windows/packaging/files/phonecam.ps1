<#
  PhoneCam launcher (packaged) - camera only.
  Autodetects: phone plugged in over USB -> starts it + tunnels automatically;
  otherwise asks for the address the phone app shows and connects over Wi-Fi.
#>
param([switch]$Usb, [switch]$Wifi, [string]$Ip, [switch]$Preview, [switch]$WithAudio, [switch]$FlipH, [switch]$FlipV,
      [switch]$Mic, [string]$MicDevice = 'CABLE Input')
$ErrorActionPreference = 'Stop'
$here     = $PSScriptRoot
$receiver = Join-Path $here 'receiver.exe'
$adb      = Join-Path $here 'adb\adb.exe'
$PKG = 'com.phonecam'; $PhonePort = 8554; $LocalPort = 18554

function AdbState { foreach ($l in (& $adb devices 2>$null)) { if ($l -match '^\S+\s+(device|unauthorized)\b') { return $Matches[1] } } return $null }
function PhoneIp  { $o = (& $adb shell "ip -o -f inet addr show wlan0" 2>$null) -join "`n"; if ($o -match 'inet\s+(\d+\.\d+\.\d+\.\d+)') { return $Matches[1] } return $null }
function NodeCenter($xml, $id) {
  $e = [regex]::Escape($id)
  $m = [regex]::Match($xml, 'resource-id="' + $e + '"[^>]*bounds="\[(\d+),(\d+)\]\[(\d+),(\d+)\]"')
  if (-not $m.Success) { $m = [regex]::Match($xml, 'bounds="\[(\d+),(\d+)\]\[(\d+),(\d+)\]"[^>]*resource-id="' + $e + '"') }
  if ($m.Success) { return @([int](([int]$m.Groups[1].Value + [int]$m.Groups[3].Value)/2), [int](([int]$m.Groups[2].Value + [int]$m.Groups[4].Value)/2)) }
  return $null
}
function StartPhone {
  foreach ($p in @('android.permission.CAMERA','android.permission.RECORD_AUDIO','android.permission.POST_NOTIFICATIONS')) { & $adb shell pm grant $PKG $p 2>$null | Out-Null }
  & $adb shell input keyevent KEYCODE_WAKEUP 2>$null | Out-Null
  & $adb shell am start -n "$PKG/.MainActivity" 2>$null | Out-Null
  Start-Sleep 2
  for ($t = 0; $t -lt 3; $t++) {
    & $adb shell uiautomator dump /sdcard/pc_ui.xml 2>$null | Out-Null
    $xml = (& $adb shell cat /sdcard/pc_ui.xml 2>$null) -join "`n"
    $both = NodeCenter $xml "$PKG`:id/modeBoth"; $start = NodeCenter $xml "$PKG`:id/startBtn"
    if ($start) { if ($both) { & $adb shell input tap $both[0] $both[1]; Start-Sleep -Milliseconds 400 }; & $adb shell input tap $start[0] $start[1]; break }
    Start-Sleep -Milliseconds 800
  }
  for ($i = 0; $i -lt 16; $i++) { if (& $adb shell "ss -ltn 2>/dev/null | grep :$PhonePort" 2>$null) { return $true }; Start-Sleep -Milliseconds 500 }
  return $false
}
function Reachable($a, $p) { $c = New-Object Net.Sockets.TcpClient; try { $r = $c.BeginConnect($a, $p, $null, $null); return ($r.AsyncWaitHandle.WaitOne(2500) -and $c.Connected) } catch { return $false } finally { $c.Close() } }

Write-Host ""
Write-Host "  PhoneCam - use your phone as a webcam (camera only)" -ForegroundColor Cyan
Write-Host ""

$state = AdbState
if ($state -eq 'unauthorized') { Write-Host "  Phone seen over USB but not authorized - tap 'Allow USB debugging' on the phone, then re-run." -ForegroundColor Yellow }
$usbOk = ($state -eq 'device')
$mode = if ($Usb) { 'usb' } elseif ($Wifi) { 'wifi' } elseif ($usbOk) { 'usb' } else { 'wifi' }

if ($mode -eq 'usb') {
  Write-Host "  Phone connected by USB - starting the app for you..." -ForegroundColor Green
  if (-not (StartPhone)) { Write-Host "  (Couldn't auto-press Start - open PhoneCam on the phone and press Start.)" -ForegroundColor Yellow }
  & $adb forward "tcp:$LocalPort" "tcp:$PhonePort" | Out-Null
  $url = "rtsp://127.0.0.1:$LocalPort/"
}
else {
  $addr = $Ip
  if (-not $addr) {
    Write-Host "  On the phone: open PhoneCam, pick 'Camera + Mic' or 'Camera only', press Start."
    Write-Host "  It shows an address like:  rtsp://192.168.0.101:8554/"
    $addr = Read-Host "  Type that address (or just the numbers, e.g. 192.168.0.101)"
  }
  if ($addr -match 'rtsp://') { $url = $addr.Trim() } else { $url = "rtsp://$($addr.Trim()):$PhonePort/" }
  $host4 = ($url -replace 'rtsp://([^:/]+).*', '$1')
  if (-not (Reachable $host4 $PhonePort)) {
    Write-Host "  (Can't reach the phone yet - make sure it's on the SAME Wi-Fi, the app is streaming, and any VPN is off. I'll keep trying.)" -ForegroundColor Yellow
  }
}

$rArgs = @($url)
if     ($Mic)            { $rArgs += @('--audio-device', $MicDevice) }  # phone mic -> virtual cable
elseif (-not $WithAudio) { $rArgs += '--no-audio' }                    # camera only (default)
if ($Preview) { $rArgs += '--preview' }
if ($FlipH)   { $rArgs += '--flip-h' }
if ($FlipV)   { $rArgs += '--flip-v' }

Write-Host ""
Write-Host "  Connecting to $url ..." -ForegroundColor Green
Write-Host "  >> Open Zoom / Teams / Meet / Discord / OBS and choose the camera called 'PhoneCam Camera'." -ForegroundColor Cyan
Write-Host "  >> Keep this window open while you use it. Close it to stop." -ForegroundColor Cyan
Write-Host ""
try { & $receiver @rArgs } finally { if ($mode -eq 'usb') { & $adb forward --remove "tcp:$LocalPort" 2>$null | Out-Null } }
