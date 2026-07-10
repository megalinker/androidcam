// PhoneCam GUI — a small windowed launcher (no console) with an embedded live
// camera preview. Compiled to PhoneCam.exe with the .NET Framework csc.exe (see
// assemble.ps1). It drives the bundled receiver.exe (+ adb for the USB tunnel)
// and hosts the receiver's preview window inside itself so you see the feed.
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Drawing;
using System.IO;
using System.Net;
using System.Net.NetworkInformation;
using System.Net.Sockets;
using System.Runtime.InteropServices;
using System.Text;
using System.Text.RegularExpressions;
using System.Threading;
using System.Windows.Forms;

public class PhoneCamGui : Form
{
    // --- Win32 for embedding the receiver's preview window ---
    [DllImport("user32.dll", SetLastError = true)] static extern IntPtr FindWindow(string cls, string title);
    [DllImport("user32.dll")] static extern IntPtr SetParent(IntPtr child, IntPtr parent);
    [DllImport("user32.dll")] static extern int GetWindowLong(IntPtr h, int i);
    [DllImport("user32.dll")] static extern int SetWindowLong(IntPtr h, int i, int v);
    [DllImport("user32.dll")] static extern bool MoveWindow(IntPtr h, int x, int y, int w, int ht, bool repaint);
    const int GWL_STYLE = -16;
    const int WS_CHILD = 0x40000000, WS_VISIBLE = 0x10000000;
    const int WS_CAPTION = 0x00C00000, WS_THICKFRAME = 0x00040000, WS_POPUP = unchecked((int)0x80000000);

    // --- theme (clean, dark) ---
    static readonly Color Bg = Color.FromArgb(27, 29, 33);
    static readonly Color Card = Color.FromArgb(42, 45, 51);
    static readonly Color Line = Color.FromArgb(52, 55, 62);
    static readonly Color Fg = Color.FromArgb(234, 236, 239);
    static readonly Color Sub = Color.FromArgb(138, 143, 150);
    static readonly Color Accent = Color.FromArgb(72, 125, 232);
    static readonly Color Green = Color.FromArgb(72, 190, 128);
    static readonly Color Amber = Color.FromArgb(226, 170, 74);

    [DllImport("dwmapi.dll")] static extern int DwmSetWindowAttribute(IntPtr h, int attr, ref int v, int sz);
    protected override void OnHandleCreated(EventArgs e)
    {
        base.OnHandleCreated(e);
        // Dark title bar only on Windows 11, where it reliably switches the caption text to light.
        // On some Windows 10 builds the attribute darkens the caption but leaves the title text dark
        // (invisible) — which made the version look "missing". A normal (light) title bar is safer
        // there, and the version is shown in the window body regardless.
        if (WindowsBuild() >= 22000)
        { int on = 1; DwmSetWindowAttribute(Handle, 20, ref on, 4); }
    }

    // True OS build number (Environment.OSVersion lies for manifest-less .NET Framework apps).
    static int WindowsBuild()
    {
        try
        {
            using (var k = Microsoft.Win32.Registry.LocalMachine.OpenSubKey(@"SOFTWARE\Microsoft\Windows NT\CurrentVersion"))
            {
                int n; var b = k != null ? k.GetValue("CurrentBuildNumber") as string : null;
                return (b != null && int.TryParse(b, out n)) ? n : 0;
            }
        }
        catch { return 0; }
    }

    RadioButton rbUsb, rbWifi, rbQr;
    LinkLabel linkIp, linkDiag;
    TextBox tbIp;
    CheckBox cbMic, cbFlipH, cbFlipV;
    Button btnStart;
    Label lblStatus, lblDot, tip;
    Panel preview;
    Label previewHint, qrLabel;
    PictureBox qrBox;
    System.Windows.Forms.Timer timer;
    Process recv;
    IntPtr embedded = IntPtr.Zero;
    readonly object logLock = new object();
    readonly List<string> logLines = new List<string>();
    string receiverExe, adbExe, settingsPath, logPath, pairedHost;
    const string Version = "0.4.7";
    const int LocalPort = 18554, PhonePort = 8554;
    bool usbForwarded = false, running = false;
    volatile bool videoSeen = false, reachIssue = false;   // set from receiver stderr, drive the status

    // --- Wi-Fi QR pairing (the phone scans a code we show and announces its pull URL back) ---
    TcpListener pairListener;
    Thread pairThread;
    string pairToken;
    bool pendingUseMic;
    static readonly Regex reTok = new Regex("\"tok\"\\s*:\\s*\"([^\"]*)\"");
    static readonly Regex reUrl = new Regex("\"url\"\\s*:\\s*\"([^\"]*)\"");

    public PhoneCamGui()
    {
        string b = AppDomain.CurrentDomain.BaseDirectory;
        string lad = Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData);
        receiverExe = FindFirst(new[] { Path.Combine(b, "bin", "receiver.exe"), Path.Combine(b, "receiver.exe"), Path.Combine(b, "..", "build", "Release", "receiver.exe") });
        adbExe = FindFirst(new[] { Path.Combine(b, "bin", "adb", "adb.exe"), Path.Combine(lad, "Android", "Sdk", "platform-tools", "adb.exe") });
        settingsPath = Path.Combine(lad, "PhoneCam", "gui.txt");
        logPath = Path.Combine(lad, "PhoneCam", "phonecam.log");
        try { Directory.CreateDirectory(Path.GetDirectoryName(logPath)); File.WriteAllText(logPath, ""); } catch { }  // fresh log per session
        BuildUi();
        LoadSettings();
        Log("PhoneCam v" + Version + " started. receiver=" + (receiverExe ?? "NOT FOUND") + " adb=" + (adbExe ?? "none"));
        timer = new System.Windows.Forms.Timer { Interval = 700 };
        timer.Tick += OnTick;
        FormClosing += (s, e) => { SaveSettings(); StopReceiver(); };
        // Optional: connect immediately on launch (handy for a "start on login" shortcut).
        if (Array.IndexOf(Environment.GetCommandLineArgs(), "-autostart") >= 0)
            Shown += (s, e) => { if (!running) StartReceiver(); };
    }

    static string FindFirst(string[] paths)
    { foreach (var p in paths) { try { var f = Path.GetFullPath(p); if (File.Exists(f)) return f; } catch { } } return null; }

    void BuildUi()
    {
        Text = "PhoneCam v" + Version;
        FormBorderStyle = FormBorderStyle.FixedSingle;
        MaximizeBox = false;
        ClientSize = new Size(744, 500);
        StartPosition = FormStartPosition.CenterScreen;
        BackColor = Bg; ForeColor = Fg;
        Font = new Font("Segoe UI", 9.5f);

        // Left control column
        var title = new Label { Text = "PhoneCam", Font = new Font("Segoe UI Semibold", 17f), ForeColor = Fg, Location = new Point(20, 18), AutoSize = true };
        Controls.Add(title);
        Controls.Add(new Label { Text = "v" + Version, ForeColor = Sub, Font = new Font("Segoe UI", 9f), Location = new Point(158, 32), AutoSize = true });

        AddSection("Connect", 72);
        rbUsb = Radio("USB cable", 22, 108, true);
        rbQr = Radio("Wi-Fi — scan QR", 22, 134, false);
        // Manual IP entry is a hidden fallback, revealed by the link below.
        rbWifi = Radio("Wi-Fi — type IP", 22, 160, false); rbWifi.Visible = false;
        tbIp = new TextBox { Location = new Point(140, 158), Width = 106, Enabled = false, Visible = false, BackColor = Card, ForeColor = Fg, BorderStyle = BorderStyle.FixedSingle };
        rbWifi.CheckedChanged += (s, e) => tbIp.Enabled = rbWifi.Checked;
        linkIp = new LinkLabel { Text = "Type an IP address instead", Location = new Point(24, 162), AutoSize = true, LinkColor = Sub, ActiveLinkColor = Accent, LinkBehavior = LinkBehavior.HoverUnderline, Font = new Font("Segoe UI", 8.25f) };
        linkIp.LinkClicked += (s, e) => RevealTypeIp();
        Controls.Add(rbUsb); Controls.Add(rbQr); Controls.Add(rbWifi); Controls.Add(tbIp); Controls.Add(linkIp);

        AddSection("Options", 198);
        cbMic = Check("Use phone microphone", 22, 222);
        cbFlipH = Check("Flip left / right", 22, 248);
        cbFlipV = Check("Flip up / down", 22, 274);
        Controls.Add(cbMic); Controls.Add(cbFlipH); Controls.Add(cbFlipV);

        btnStart = new Button { Text = "Start", Location = new Point(22, 318), Size = new Size(224, 40), FlatStyle = FlatStyle.Flat, BackColor = Accent, ForeColor = Color.White, Font = new Font("Segoe UI Semibold", 11f) };
        btnStart.FlatAppearance.BorderSize = 0;
        btnStart.Click += OnStartStop;
        Controls.Add(btnStart);

        lblDot = new Label { Text = "●", ForeColor = Sub, Location = new Point(24, 372), AutoSize = true, Font = new Font("Segoe UI", 11f) };
        lblStatus = new Label { Text = "Idle", ForeColor = Sub, Location = new Point(44, 374), AutoSize = true, MaximumSize = new Size(220, 0) };
        Controls.Add(lblDot); Controls.Add(lblStatus);

        tip = new Label { Text = TipText(false), ForeColor = Sub, Location = new Point(22, 424), AutoSize = true };
        Controls.Add(tip);

        linkDiag = new LinkLabel { Text = "Copy diagnostics", Location = new Point(22, 472), AutoSize = true, LinkColor = Accent, ActiveLinkColor = Accent, LinkBehavior = LinkBehavior.AlwaysUnderline, Font = new Font("Segoe UI", 9f) };
        linkDiag.LinkClicked += (s, e) => CopyDiagnostics();
        Controls.Add(linkDiag);

        // Right: embedded live preview (also hosts the pairing QR before a phone connects)
        preview = new Panel { Location = new Point(260, 84), Size = new Size(468, 392), BackColor = Color.FromArgb(12, 13, 15), BorderStyle = BorderStyle.None };
        preview.Paint += (s, e) => { using (var pen = new Pen(Line)) e.Graphics.DrawRectangle(pen, 0, 0, preview.Width - 1, preview.Height - 1); };
        previewHint = new Label { Text = "Live preview appears here once you press Start.", ForeColor = Sub, BackColor = Color.FromArgb(12, 13, 15), AutoSize = true, Location = new Point(16, 16) };
        // Centered as one vertical group inside the 468×392 preview panel: the QR block itself
        // sits at the panel's vertical centre, with the caption just above it.
        qrLabel = new Label { Text = "Scan this with the PhoneCam phone app\n(tap “Scan PC QR to connect”)", ForeColor = Fg, BackColor = Color.FromArgb(12, 13, 15), Size = new Size(468, 40), Location = new Point(0, 34), TextAlign = ContentAlignment.MiddleCenter, Visible = false };
        qrBox = new PictureBox { Location = new Point(104, 82), Size = new Size(260, 260), SizeMode = PictureBoxSizeMode.Zoom, BackColor = Color.White, Visible = false };
        preview.Controls.Add(previewHint); preview.Controls.Add(qrLabel); preview.Controls.Add(qrBox);
        Controls.Add(preview);

        if (receiverExe == null) { btnStart.Enabled = false; SetStatus(Color.IndianRed, "receiver.exe not found"); }
    }

    void AddSection(string t, int y)
    {
        Controls.Add(new Label { Text = t.ToUpperInvariant(), ForeColor = Sub, Font = new Font("Segoe UI", 7.5f, FontStyle.Bold), Location = new Point(24, y), AutoSize = true });
        Controls.Add(new Panel { BackColor = Line, Location = new Point(24, y + 17), Size = new Size(212, 1) });
    }
    /// <summary>Reveal the hidden manual-IP fallback (and select it).</summary>
    void RevealTypeIp() { linkIp.Visible = false; rbWifi.Visible = true; tbIp.Visible = true; rbWifi.Checked = true; }

    RadioButton Radio(string t, int x, int y, bool on) { return new RadioButton { Text = t, ForeColor = Fg, Location = new Point(x, y), AutoSize = true, Checked = on, FlatStyle = FlatStyle.Standard }; }
    CheckBox Check(string t, int x, int y) { return new CheckBox { Text = t, ForeColor = Fg, Location = new Point(x, y), AutoSize = true }; }

    void SetStatus(Color c, string s) { lblDot.ForeColor = c; lblStatus.ForeColor = c == Sub ? Sub : Fg; lblStatus.Text = s; }

    static string TipText(bool mic)
    {
        return mic
            ? "In your call app pick “PhoneCam Camera” as the\ncamera and “CABLE Output” as the microphone."
            : "Then pick “PhoneCam Camera” as the\nwebcam in Zoom / Teams / OBS.";
    }

    // Connect + Options only take effect at Start, so lock them while streaming — otherwise ticking
    // "Use microphone" mid-stream silently does nothing and never prompts for VB-CABLE.
    void SetInputsEnabled(bool on)
    {
        rbUsb.Enabled = on; rbQr.Enabled = on; rbWifi.Enabled = on; linkIp.Enabled = on;
        tbIp.Enabled = on && rbWifi.Checked;
        cbMic.Enabled = on; cbFlipH.Enabled = on; cbFlipV.Enabled = on;
    }

    string Adb(string args)
    {
        if (adbExe == null) return "";
        try { var psi = new ProcessStartInfo(adbExe, args) { UseShellExecute = false, CreateNoWindow = true, RedirectStandardOutput = true, RedirectStandardError = true };
              var p = Process.Start(psi); string o = p.StandardOutput.ReadToEnd(); p.WaitForExit(4000); return o; } catch { return ""; }
    }

    // Timestamped rolling log — the source for "Copy diagnostics" and a file the user can share.
    void Log(string s)
    {
        string line = DateTime.Now.ToString("HH:mm:ss ") + s;
        lock (logLock)
        {
            logLines.Add(line);
            if (logLines.Count > 500) logLines.RemoveRange(0, logLines.Count - 500);
            try { File.AppendAllText(logPath, line + Environment.NewLine); } catch { }   // append, don't rewrite
        }
    }

    void CopyDiagnostics()
    {
        var sb = new StringBuilder();
        sb.AppendLine("=== PhoneCam diagnostics ===");
        sb.AppendLine("version: " + Version);
        sb.AppendLine("time: " + DateTime.Now);
        sb.AppendLine("os: " + Environment.OSVersion + (Environment.Is64BitOperatingSystem ? " x64" : " x86"));
        sb.AppendLine("receiver: " + (receiverExe ?? "NOT FOUND"));
        sb.AppendLine("chosen LAN IP: " + (LocalIPv4() ?? "none"));
        try
        {
            sb.AppendLine("interfaces (up):");
            foreach (var ni in NetworkInterface.GetAllNetworkInterfaces())
            {
                if (ni.OperationalStatus != OperationalStatus.Up) continue;
                foreach (var ua in ni.GetIPProperties().UnicastAddresses)
                    if (ua.Address.AddressFamily == AddressFamily.InterNetwork)
                        sb.AppendLine("  " + ua.Address + "  (" + ni.Name + " / " + ni.NetworkInterfaceType + ")");
            }
        }
        catch { }
        sb.AppendLine("--- log ---");
        lock (logLock) foreach (var l in logLines) sb.AppendLine(l);
        try { Clipboard.SetText(sb.ToString()); MessageBox.Show("Diagnostics copied to the clipboard — paste them to share.", "PhoneCam"); }
        catch (Exception e) { try { File.WriteAllText(logPath, sb.ToString()); } catch { } MessageBox.Show("Couldn't copy to clipboard (" + e.Message + ").\nThe log is at:\n" + logPath, "PhoneCam"); }
    }

    static string HostOf(string url)
    {
        try { int a = url.IndexOf("://", StringComparison.Ordinal); if (a < 0) return url; a += 3;
              int b = url.IndexOfAny(new[] { ':', '/' }, a); return b < 0 ? url.Substring(a) : url.Substring(a, b - a); }
        catch { return url; }
    }

    static bool VbCableInstalled()
    {
        try { string drv = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.System), "drivers");
              return Directory.Exists(drv) && Directory.GetFiles(drv, "vbaudio_cable*.sys").Length > 0; } catch { return false; }
    }

    void InstallVbCable()
    {
        try {
            Cursor = Cursors.WaitCursor;
            string tmp = Path.Combine(Path.GetTempPath(), "PhoneCam-VBCABLE"); Directory.CreateDirectory(tmp);
            string zip = Path.Combine(tmp, "vbcable.zip");
            System.Net.ServicePointManager.SecurityProtocol = System.Net.SecurityProtocolType.Tls12;
            using (var wc = new System.Net.WebClient())
                wc.DownloadFile("https://download.vb-audio.com/Download_CABLE/VBCABLE_Driver_Pack43.zip", zip);
            string ex = Path.Combine(tmp, "pkg"); if (Directory.Exists(ex)) Directory.Delete(ex, true);
            System.IO.Compression.ZipFile.ExtractToDirectory(zip, ex);
            Cursor = Cursors.Default;
            string setup = Path.Combine(ex, "VBCABLE_Setup_x64.exe");
            if (!File.Exists(setup)) { MessageBox.Show("Downloaded VB-CABLE but couldn't find its installer.", "PhoneCam"); return; }
            Process.Start(new ProcessStartInfo(setup) { UseShellExecute = true, Verb = "runas" });
            MessageBox.Show("VB-CABLE (by VB-Audio) is opening. Click “Install Driver”, accept the Windows prompt,\nthen come back and press Start.", "PhoneCam — microphone setup");
        } catch (Exception e) {
            Cursor = Cursors.Default;
            if (MessageBox.Show("Couldn't fetch VB-CABLE automatically:\n" + e.Message + "\n\nOpen the VB-CABLE download page in your browser instead?", "PhoneCam", MessageBoxButtons.YesNo) == DialogResult.Yes)
                try { Process.Start(new ProcessStartInfo("https://vb-audio.com/Cable/") { UseShellExecute = true }); } catch { }
        }
    }

    void OnStartStop(object sender, EventArgs e) { if (running) StopReceiver(); else StartReceiver(); }

    void StartReceiver()
    {
        bool useMic = cbMic.Checked;
        if (useMic && !VbCableInstalled())
        {
            var r = MessageBox.Show(
                "Using the phone as a microphone needs the free VB-CABLE audio driver (by VB-Audio).\n\nDownload and install it now?",
                "PhoneCam — microphone setup", MessageBoxButtons.YesNoCancel, MessageBoxIcon.Question);
            if (r == DialogResult.Cancel) return;
            if (r == DialogResult.Yes) { InstallVbCable(); return; }  // install, then press Start again
            useMic = false;                                          // No -> just the camera
        }

        Log("Start: mode=" + (rbUsb.Checked ? "usb" : rbQr.Checked ? "qr" : "wifi-ip") + " mic=" + useMic);
        // Wi-Fi QR pairing: show a code, let the phone scan it and announce its URL to us.
        if (rbQr.Checked) { StartQrPairing(useMic); return; }

        string url;
        if (rbUsb.Checked)
        {
            if (adbExe == null) { MessageBox.Show("adb not found (needed for USB). Use Wi-Fi instead.", "PhoneCam"); return; }
            if (!Regex.IsMatch(Adb("devices"), @"\bdevice\b")) { MessageBox.Show("No phone detected over USB.\n\nEnable Developer Options → USB debugging, plug in, tap “Allow”. Or use Wi-Fi.", "PhoneCam"); return; }
            Adb("forward tcp:" + LocalPort + " tcp:" + PhonePort);
            usbForwarded = true;
            url = "rtsp://127.0.0.1:" + LocalPort + "/";
        }
        else
        {
            string ip = tbIp.Text.Trim();
            if (ip.Length == 0) { MessageBox.Show("Enter the address shown on the phone (e.g. 192.168.0.101).", "PhoneCam"); return; }
            url = ip.StartsWith("rtsp://", StringComparison.OrdinalIgnoreCase) ? ip : "rtsp://" + ip + ":" + PhonePort + "/";
        }

        previewHint.Text = rbUsb.Checked ? "Open PhoneCam on the phone and press Start…" : "Connecting…";
        SetStatus(Amber, rbUsb.Checked ? "Waiting for the phone…" : "Connecting…");
        StartReceiverWithUrl(url, useMic);
    }

    /// <summary>Launch receiver.exe against a concrete rtsp URL and begin embedding its preview.</summary>
    void StartReceiverWithUrl(string url, bool useMic)
    {
        pairedHost = HostOf(url);
        videoSeen = false; reachIssue = false;
        var a = new List<string> { url, "--preview" };   // --preview so we can embed the feed
        if (cbFlipH.Checked) a.Add("--flip-h");
        if (cbFlipV.Checked) a.Add("--flip-v");
        if (useMic) { a.Add("--audio-device"); a.Add("CABLE Input"); } else a.Add("--no-audio");

        string args = BuildArgs(a);
        Log("launching receiver: " + args);
        var psi = new ProcessStartInfo(receiverExe, args) { UseShellExecute = false, CreateNoWindow = true, RedirectStandardError = true, RedirectStandardOutput = true, StandardErrorEncoding = Encoding.UTF8 };
        recv = new Process { StartInfo = psi };
        recv.ErrorDataReceived += (s, ev) =>
        {
            if (ev.Data == null) return;
            Log("[recv] " + ev.Data);
            if (ev.Data.IndexOf("[video]", StringComparison.OrdinalIgnoreCase) >= 0) videoSeen = true;
            if (ev.Data.IndexOf("reconnect", StringComparison.OrdinalIgnoreCase) >= 0 || ev.Data.IndexOf("unreachable", StringComparison.OrdinalIgnoreCase) >= 0
                || ev.Data.IndexOf("refused", StringComparison.OrdinalIgnoreCase) >= 0 || ev.Data.IndexOf("timed out", StringComparison.OrdinalIgnoreCase) >= 0
                || ev.Data.IndexOf("failed", StringComparison.OrdinalIgnoreCase) >= 0) reachIssue = true;
        };
        try { recv.Start(); recv.BeginErrorReadLine(); }
        catch (Exception ex) { Log("receiver start FAILED: " + ex.Message); MessageBox.Show("Failed to start receiver: " + ex.Message, "PhoneCam"); StopReceiver(); return; }

        running = true; embedded = IntPtr.Zero;
        previewHint.Visible = true;
        tip.Text = TipText(useMic);
        SetInputsEnabled(false);
        btnStart.Text = "Stop"; btnStart.BackColor = Color.FromArgb(70, 74, 82);
        timer.Start();
    }

    /// <summary>Show a pairing QR and wait (off the UI thread) for the phone to announce its URL.</summary>
    void StartQrPairing(bool useMic)
    {
        string ip = LocalIPv4();
        if (ip == null) { MessageBox.Show("Couldn't determine this PC's Wi-Fi address. Use “type IP” instead.", "PhoneCam"); return; }
        try { pairListener = new TcpListener(IPAddress.Any, 0); pairListener.Start(); }
        catch (Exception ex) { MessageBox.Show("Couldn't open a pairing port: " + ex.Message, "PhoneCam"); return; }

        int port = ((IPEndPoint)pairListener.LocalEndpoint).Port;
        pairToken = Guid.NewGuid().ToString("N").Substring(0, 6);
        string payload = "PCAM1:" + ip + ":" + port + ":" + pairToken;
        Log("Wi-Fi pairing: advertising " + ip + ":" + port + " — waiting for the phone to scan");
        try { qrBox.Image = MakeQr(payload); }
        catch (Exception ex) { MessageBox.Show("Couldn't render the QR: " + ex.Message, "PhoneCam"); StopReceiver(); return; }

        pendingUseMic = useMic;
        running = true;
        previewHint.Visible = false; qrLabel.Visible = true; qrBox.Visible = true;
        tip.Text = TipText(useMic);
        SetInputsEnabled(false);
        btnStart.Text = "Stop"; btnStart.BackColor = Color.FromArgb(70, 74, 82);
        SetStatus(Amber, "Scan the QR with the PhoneCam app…");

        var ln = pairListener;
        pairThread = new Thread(() =>
        {
            try
            {
                using (var client = ln.AcceptTcpClient())
                using (var ns = client.GetStream())
                {
                    client.ReceiveTimeout = 10000;
                    string peer = "?"; try { peer = ((IPEndPoint)client.Client.RemoteEndPoint).Address.ToString(); } catch { }
                    string line = new StreamReader(ns, Encoding.UTF8).ReadLine() ?? "";
                    Log("pairing: connection from " + peer + " -> " + (line.Length > 160 ? line.Substring(0, 160) : line));
                    var mt = reTok.Match(line); var mu = reUrl.Match(line);
                    if (mt.Success && mt.Groups[1].Value == pairToken && mu.Success)
                    {
                        try { var ok = Encoding.UTF8.GetBytes("OK\n"); ns.Write(ok, 0, ok.Length); } catch { }
                        // org.json (Android) escapes '/' as '\/', and we extract with a regex rather
                        // than a JSON parser — so unescape, else the receiver gets rtsp:\/\/… and fails
                        // with "Failed to resolve hostname \".
                        string url = mu.Groups[1].Value.Replace("\\/", "/");
                        Log("pairing OK: phone stream = " + url);
                        BeginInvoke((Action)(() => OnPaired(url)));
                    }
                    else { Log("pairing: token mismatch / bad payload from " + peer); BeginInvoke((Action)(() => SetStatus(Amber, "A device tried to pair but the code didn't match."))); }
                }
            }
            catch { /* listener stopped (Stop pressed) or timed out */ }
        }) { IsBackground = true };
        pairThread.Start();
    }

    void OnPaired(string url)
    {
        try { if (pairListener != null) { pairListener.Stop(); pairListener = null; } } catch { }
        qrLabel.Visible = false; qrBox.Visible = false;
        if (qrBox.Image != null) { var img = qrBox.Image; qrBox.Image = null; img.Dispose(); }
        previewHint.Text = "Connecting…"; SetStatus(Amber, "Phone paired (" + HostOf(url) + ") — connecting…");
        StartReceiverWithUrl(url, pendingUseMic);
    }

    static Bitmap MakeQr(string text)
    {
        var data = new QRCoder.QRCodeGenerator().CreateQrCode(text, QRCoder.QRCodeGenerator.ECCLevel.M);
        byte[] png = new QRCoder.PngByteQRCode(data).GetGraphic(8);
        using (var ms = new MemoryStream(png)) using (var tmp = new Bitmap(ms)) return new Bitmap(tmp);
    }

    /// <summary>
    /// This PC's LAN IPv4 (the address the phone reaches us on). Prefers a real private-LAN
    /// address (192.168/10/172.16) and skips APIPA + CGNAT/VPN (100.64/10, e.g. Tailscale/
    /// Windscribe) — those are unreachable from the phone over the local network.
    /// </summary>
    static string LocalIPv4()
    {
        // A real LAN adapter has a default gateway; host-only virtual adapters (VirtualBox/VMware/
        // Hyper-V/WSL/ICS) don't. So prefer a private-LAN address whose interface has a gateway.
        var gwLan = new List<string>();   // RFC1918 + has gateway  → the real LAN
        var gwAny = new List<string>();   // anything else with a gateway
        var noGw = new List<string>();    // RFC1918 without a gateway (virtual host-only nets)
        try
        {
            foreach (var ni in NetworkInterface.GetAllNetworkInterfaces())
            {
                if (ni.OperationalStatus != OperationalStatus.Up) continue;
                if (ni.NetworkInterfaceType == NetworkInterfaceType.Loopback ||
                    ni.NetworkInterfaceType == NetworkInterfaceType.Tunnel) continue;
                var props = ni.GetIPProperties();
                bool hasGw = false;
                foreach (var g in props.GatewayAddresses)
                    if (g.Address.AddressFamily == AddressFamily.InterNetwork && g.Address.ToString() != "0.0.0.0") hasGw = true;
                foreach (var ua in props.UnicastAddresses)
                {
                    if (ua.Address.AddressFamily != AddressFamily.InterNetwork) continue;
                    string ip = ua.Address.ToString();
                    if (ip.StartsWith("169.254.") || IsCgnat(ip)) continue;   // APIPA / VPN
                    if (hasGw && IsPrivateLan(ip)) gwLan.Add(ip);
                    else if (hasGw) gwAny.Add(ip);
                    else if (IsPrivateLan(ip)) noGw.Add(ip);
                }
            }
        }
        catch { }
        if (gwLan.Count > 0) return gwLan[0];
        if (gwAny.Count > 0) return gwAny[0];
        if (noGw.Count > 0) return noGw[0];
        // Last resort: whatever address the default route would use.
        try { using (var s = new Socket(AddressFamily.InterNetwork, SocketType.Dgram, ProtocolType.Udp))
            { s.Connect("8.8.8.8", 65530); return ((IPEndPoint)s.LocalEndPoint).Address.ToString(); } }
        catch { return null; }
    }

    static bool IsPrivateLan(string ip)
    {
        if (ip.StartsWith("192.168.") || ip.StartsWith("10.")) return true;
        if (ip.StartsWith("172."))
        { var p = ip.Split('.'); int o; if (p.Length > 1 && int.TryParse(p[1], out o)) return o >= 16 && o <= 31; }
        return false;
    }

    static bool IsCgnat(string ip)  // 100.64.0.0/10 — carrier-grade NAT range VPNs like to use
    {
        if (!ip.StartsWith("100.")) return false;
        var p = ip.Split('.'); int o;
        return p.Length > 1 && int.TryParse(p[1], out o) && o >= 64 && o <= 127;
    }

    static string BuildArgs(List<string> a)
    { var sb = new StringBuilder(); foreach (var s in a) { if (sb.Length > 0) sb.Append(' '); if (s.IndexOf(' ') >= 0) sb.Append('"').Append(s).Append('"'); else sb.Append(s); } return sb.ToString(); }

    void OnTick(object sender, EventArgs e)
    {
        if (recv == null) return;
        if (recv.HasExited) { StopReceiver(); SetStatus(Sub, "Stopped"); return; }

        // once the receiver's preview window exists, suck it into our panel
        if (embedded == IntPtr.Zero)
        {
            IntPtr hwnd = FindWindow("PhoneCamPreview", null);
            if (hwnd != IntPtr.Zero)
            {
                int st = GetWindowLong(hwnd, GWL_STYLE);
                st = (st & ~(WS_CAPTION | WS_THICKFRAME | WS_POPUP)) | WS_CHILD | WS_VISIBLE;
                SetWindowLong(hwnd, GWL_STYLE, st);
                SetParent(hwnd, preview.Handle);
                MoveWindow(hwnd, 0, 0, preview.ClientSize.Width, preview.ClientSize.Height, true);
                embedded = hwnd;
                previewHint.Visible = false;
            }
        }
        else
        {
            MoveWindow(embedded, 0, 0, preview.ClientSize.Width, preview.ClientSize.Height, true);
        }

        if (videoSeen || embedded != IntPtr.Zero) SetStatus(Green, "Live — select “PhoneCam Camera” in your app");
        else if (reachIssue)
            SetStatus(Amber, rbUsb.Checked ? "Waiting for the phone (press Start in the app)…"
                : "Can't reach the phone" + (pairedHost != null ? " at " + pairedHost : "") + " — same Wi-Fi? VPN off? Firewall?");
    }

    void StopReceiver()
    {
        if (running) Log("stopped");
        timer.Stop();
        embedded = IntPtr.Zero;
        try { if (pairListener != null) { pairListener.Stop(); pairListener = null; } } catch { }
        try { if (recv != null && !recv.HasExited) recv.Kill(); } catch { }
        recv = null;
        if (usbForwarded) { Adb("forward --remove tcp:" + LocalPort); usbForwarded = false; }
        running = false;
        if (IsHandleCreated)
        {
            if (qrBox != null) { qrLabel.Visible = false; qrBox.Visible = false; if (qrBox.Image != null) { var i = qrBox.Image; qrBox.Image = null; i.Dispose(); } }
            SetInputsEnabled(true);
            tip.Text = TipText(false);
            btnStart.Text = "Start"; btnStart.BackColor = Accent;
            previewHint.Text = "Live preview appears here once you press Start."; previewHint.Visible = true;
            if (lblStatus.Text != "Stopped") SetStatus(Sub, "Idle");
        }
    }

    void LoadSettings()
    {
        try
        {
            if (!File.Exists(settingsPath)) return;
            foreach (var line in File.ReadAllLines(settingsPath))
            {
                var kv = line.Split(new[] { '=' }, 2); if (kv.Length != 2) continue;
                switch (kv[0]) {
                    case "conn": if (kv[1] == "wifi") RevealTypeIp(); else if (kv[1] == "qr") rbQr.Checked = true; break;
                    case "ip": tbIp.Text = kv[1]; break;
                    case "mic": cbMic.Checked = kv[1] == "1"; break;
                    case "flipH": cbFlipH.Checked = kv[1] == "1"; break;
                    case "flipV": cbFlipV.Checked = kv[1] == "1"; break;
                }
            }
        } catch { }
    }

    void SaveSettings()
    {
        try
        {
            Directory.CreateDirectory(Path.GetDirectoryName(settingsPath));
            File.WriteAllLines(settingsPath, new[] {
                "conn=" + (rbQr.Checked ? "qr" : rbWifi.Checked ? "wifi" : "usb"),
                "ip=" + tbIp.Text.Trim(),
                "mic=" + (cbMic.Checked ? "1" : "0"),
                "flipH=" + (cbFlipH.Checked ? "1" : "0"),
                "flipV=" + (cbFlipV.Checked ? "1" : "0"),
            });
        } catch { }
    }

    [STAThread]
    static void Main()
    {
        Application.EnableVisualStyles();
        Application.SetCompatibleTextRenderingDefault(false);
        Application.Run(new PhoneCamGui());
    }
}
