# PhoneCam — Virtual Microphone driver (Phase 2)

The **hard** half of the project: a kernel-mode virtual audio device so Windows apps see a selectable **PhoneCam Microphone**. Windows 10 has no user-mode way to add a mic, so this is a driver.

Good news from the Phase-1 design: the receiver already **renders** decoded audio to a WASAPI endpoint, so this driver only needs to be a **loopback** — it does not need any custom IPC with the app.

## Approach: fork, don't write from scratch

Fork one of these (both are the standard starting points — don't hand-roll PortCls):
- **[`VirtualDrivers/Virtual-Audio-Driver`](https://github.com/VirtualDrivers/Virtual-Audio-Driver)** (MIT) — purpose-built virtual audio device; the most direct start.
- **Microsoft `SysVAD`** (WDK sample) — well documented, but only generates a test tone, so you'd add real render→capture loopback.

It's a **WDM/PortCls WaveRT audio miniport** exposing a virtual speaker + virtual mic that share a ring buffer. (Not an APO — that's in-pipeline DSP, no device. Not AVStream — that's for cameras.)

## Data flow (loopback model — no custom IPC needed)

```
receiver.exe --WASAPI render--> [virtual "PhoneCam Audio" speaker] --driver loopback--> [virtual "PhoneCam Microphone"] --> Zoom/Teams/Discord
```

The receiver **already** renders the decoded phone audio to a WASAPI endpoint (`src/wasapi_sink.cpp`). So the driver doesn't need any custom IOCTL/shared-memory bridge with the app — it just needs to be a standard **loopback** device: a render endpoint whose samples are internally copied to a capture endpoint (exactly what `Virtual-Audio-Driver` provides). Run the receiver with `--audio-device "PhoneCam Audio"` to target the render side.

The driver's internal ring buffer should run at **48 kHz** to match both the phone stream and the app's WASAPI shared-mode format, minimizing resample drift.

## The driver is built here

The fork lives in [`src/`](src) (Virtual-Audio-Driver, MIT), **already rebranded**
to **PhoneCam Audio** (render) / **PhoneCam Microphone** (capture) and with a real
**render→capture loopback** implemented in [`src/Source/Main/loopback.h`](src/Source/Main/loopback.h) /
[`loopback.cpp`](src/Source/Main/loopback.cpp) (the base driver's mic was a
tone/silence generator). Both endpoints default to **48 kHz / 16-bit / 2ch** so the
loopback is a correct byte copy in shared mode.

### Build

Needs **Visual Studio 2019 or 2022 + the WDK matching your Windows SDK** (here:
WDK 10.0.19041). One-time: install the WDK, then install its VS extension
`C:\Program Files (x86)\Windows Kits\10\Vsix\VS2019\WDK.vsix` into your VS instance
(`VSIXInstaller.exe /quiet /admin WDK.vsix`) so the `WindowsKernelModeDriver10.0`
toolset is available. Then:

```powershell
$msb = "C:\Program Files (x86)\Microsoft Visual Studio\2019\Enterprise\MSBuild\Current\Bin\MSBuild.exe"
& $msb windows\driver\src\VirtualAudioDriver.sln /t:Rebuild `
    /p:Configuration=Release /p:Platform=x64 /p:SpectreMitigation=false
```

`SpectreMitigation=false` avoids needing the Spectre-mitigated CRT libs; install
those (VS Installer → individual components) and drop the flag for a hardened build.
Output: `windows\driver\src\x64\Release\package\VirtualAudioDriver.{sys,inf,cat}`.

## Install on your PC (test-signed — needs Secure Boot OFF)

> A self-test-signed kernel driver only loads when **test-signing mode** is on, and
> that mode **does not take effect while Secure Boot is enabled**. Turn Secure Boot
> **off in your UEFI/BIOS first**. (Bring-up carries some BSOD risk — a restore point
> is wise.)

1. **Enable test-signing**, then reboot:
   ```powershell
   .\scripts\1-enable-testsigning.ps1   # elevated; then REBOOT
   ```
2. **Create + trust a test cert:**
   ```powershell
   .\scripts\2-make-testcert.ps1        # elevated; makes phonecam-test.pfx
   ```
3. **Sign + install the built package:**
   ```powershell
   .\scripts\3-sign-and-install.ps1 `
     -SysPath ..\src\x64\Release\package\VirtualAudioDriver.sys `
     -InfPath ..\src\x64\Release\package\VirtualAudioDriver.inf
   ```
4. **Verify:** Settings → System → Sound should list **PhoneCam Microphone** (input)
   and **PhoneCam Audio** (output). Then run the receiver into the render endpoint and
   pick the mic in your app:
   ```powershell
   .\build\Release\receiver.exe --sig-port 8891 --sig-secret <hex> --audio-device "PhoneCam Audio"
   ```
   (or just start PhoneCam.exe with the driver installed and point its audio device at
   "PhoneCam Audio"). Pick **PhoneCam Microphone** in Zoom/Teams/Discord. Talk near the
   phone — you should hear it come through as mic input.

Remove it later with `.\scripts\4-uninstall.ps1`.

> **Production path (no Secure-Boot-off):** to load without test-signing, the driver
> must be **Microsoft-attestation-signed** via the Partner Center (needs an EV cert).
> That's the real "no bandaids" distribution route; test-signing is the personal-PC route.

## Requirements & gotchas

- **Signing:** self-signed test cert is fine for your own PC (steps above). **Distribution** needs an EV code-signing certificate + Microsoft Partner Center attestation signing — a different, paid process.
- **Format match:** render at 48 kHz/16-bit to match the receiver and the app's WASAPI shared-mode; watch for drift between the network source clock and the device clock.
- **VM audio:** some VMs virtualize audio oddly; if endpoints don't appear, test on real hardware.
- A/V sync is handled by WebRTC (shared RTP clock + RTCP), so this driver only has to render cleanly at the matched format.

## Status

**Built + rebranded + loopback implemented.** The driver compiles (VS2019 + WDK
10.0.19041) and passes the WDK signability test, producing a test-signed
`VirtualAudioDriver.{sys,inf,cat}`. **Not yet validated on hardware** — loading a
kernel driver needs the Secure-Boot-off + test-sign + reboot cycle above, which
couldn't be exercised in the build environment. The loopback design is a
48 kHz/16-bit/2ch spin-lock-guarded ring FIFO (`loopback.cpp`); the first on-device
test should confirm audio actually flows speaker→mic and watch for clock drift
between the network source and the device clock.
