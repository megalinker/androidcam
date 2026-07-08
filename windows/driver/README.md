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

## Runbook (personal use, test-signed)

Do all of this on Windows (a Win10/11 VM is fine). Steps 1–2 map to the scripts in [`scripts/`](scripts).

1. **Install tools:** Visual Studio 2022 + **WDK** (matching Windows SDK). This gives `msbuild`, `signtool`, `inf2cat`, `pnputil`.

2. **Fork & rename:** clone your chosen base into `windows/driver/src/`. In its `.inf` and resource strings, rename the endpoints to:
   - render/playback → **PhoneCam Audio**
   - capture/mic → **PhoneCam Microphone**
   Keep (or add) the internal **loopback** so render samples appear on the capture pin. Set the format list to include **48000 Hz, 16-bit, 1–2 ch**.

3. **Enable test-signing** (needed once per machine), then reboot:
   ```powershell
   .\scripts\1-enable-testsigning.ps1   # elevated; then REBOOT
   ```

4. **Create + trust a self-signed cert:**
   ```powershell
   .\scripts\2-make-testcert.ps1        # elevated; makes phonecam-test.pfx
   ```

5. **Build** the driver (from the fork's solution / `.vcxproj`), producing `phonecam-audio.sys` + `phonecam-audio.inf`.

6. **Catalog, sign, install:**
   ```powershell
   .\scripts\3-sign-and-install.ps1 -SysPath <build>\phonecam-audio.sys -InfPath <build>\phonecam-audio.inf
   ```

7. **Verify:** Settings → System → Sound should list **PhoneCam Microphone** (input) and **PhoneCam Audio** (output). Then:
   ```powershell
   receiver.exe rtsp://<phone-ip>:8554/ --audio-device "PhoneCam Audio"
   ```
   Pick **PhoneCam Microphone** in Zoom/Teams/Discord.

Remove it later with `.\scripts\4-uninstall.ps1`.

## Requirements & gotchas

- **Signing:** self-signed test cert is fine for your own PC (steps above). **Distribution** needs an EV code-signing certificate + Microsoft Partner Center attestation signing — a different, paid process.
- **Format match:** render at 48 kHz/16-bit to match the receiver and the app's WASAPI shared-mode; watch for drift between the network source clock and the device clock.
- **VM audio:** some VMs virtualize audio oddly; if endpoints don't appear, test on real hardware.
- Keep audio in the same RTSP session as video (already the case) so A/V stay in sync.

## Status

**Not started.** Phase 1 (camera) + the WASAPI render path ship first. This folder holds the plan and the sign/install tooling; the driver `.sys`/`.inf` come from your fork.
