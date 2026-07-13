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
    CheckBox cbMic, cbFlipH, cbFlipV, cbUdp, cbEncrypt;
    ComboBox cbBoost, cbEq;
    string customEq = "";        // user-defined band list ("type:f:q:db;...") from the EQ editor
    int prevEqIndex = 0;         // revert target if the custom editor is cancelled
    bool suppressEqDialog = false;   // don't pop the editor when we set the EQ index programmatically
    static readonly int[] BoostDb = { 0, 6, 12, 18 };   // Off / Low / Med / High
    // EQ dropdown index -> receiver preset name; index 5 ("custom") uses customEq instead.
    static readonly string[] EqPreset = { "", "clarity", "warm", "bright", "podcast", "clarity+", "warm+", "bright+", "podcast+", "custom" };
    Button btnStart;
    Label lblStatus, lblDot, tip, micLabel;
    Panel preview, micMeter;
    Label previewHint, qrLabel, srtHint;
    PictureBox qrBox;
    volatile float micLevel = 0f;   // 0..1 peak from the receiver's [level] lines, drives the mic meter
    bool authIssue = false;         // receiver reported an auth (401) failure
    NotifyIcon tray;
    bool minimizeToTray = false;
    bool startMinimized = false;    // launched with -tray (autostart) → start hidden in the tray
    System.Windows.Forms.Timer timer;
    Process recv;
    IntPtr embedded = IntPtr.Zero;
    readonly object logLock = new object();
    readonly List<string> logLines = new List<string>();
    string receiverExe, adbExe, settingsPath, logPath, pairedHost;
    const string Version = "0.4.23";
    const string RtspUser = "phonecam";   // Basic-auth username the phone expects
    const int LocalPort = 18554, PhonePort = 8554;
    const int PhoneControlPort = 8555, LocalControlPort = 18555;   // "stop the phone now" channel (USB uses the forward)
    const int SrtPort = 8890;    // UDP port the PC's SRT listener binds in encrypted mode
    bool srtMode = false;        // this session is an encrypted SRT listen (phone pushes to us)
    string srtPass = "";         // the SRT passphrase for this session (kept out of logs)
    DateTime srtWaitSince = DateTime.MinValue;   // when the encrypted QR went up (to time the VPN hint)
    bool usbForwarded = false, running = false;
    volatile bool videoSeen = false, audioSeen = false, reachIssue = false;   // from receiver stderr, drive the status

    // --- auto-reconnect: if receiver.exe dies unexpectedly, relaunch it against the same URL ---
    string lastUrl;              // the phone's rtsp URL we're (re)connecting to
    bool lastUseMic;             // the mic choice for that session, so reconnects match
    string controlHost; int controlPort = PhoneControlPort;   // where to send "stop the phone now"
    string phoneToken = "";      // the phone's stop-authorization token for this session (empty over USB/manual-IP)
    bool manualStop = false;     // true while the user (or app close) is deliberately stopping — no reconnect
    bool reconnecting = false;   // waiting out the delay between reconnect attempts
    bool wentLive = false;       // did this URL ever produce a live feed? (tunes the give-up message)
    int reconnectAttempts = 0;
    DateTime reconnectAt = DateTime.MinValue;
    const int MaxReconnectAttempts = 30;   // ~60 s at 2 s each, then give up
    const int ReconnectDelayMs = 2000;

    // --- saved devices: name -> rtsp URL, captured at pairing so you can reconnect without the QR ---
    readonly List<string[]> devices = new List<string[]>();   // each = { name, url }
    Panel devicesPanel;

    // --- Wi-Fi QR pairing (the phone scans a code we show and announces its pull URL back) ---
    TcpListener pairListener;
    Thread pairThread;
    string pairToken;
    bool pendingUseMic;
    static readonly Regex reTok = new Regex("\"tok\"\\s*:\\s*\"([^\"]*)\"");
    static readonly Regex reUrl = new Regex("\"url\"\\s*:\\s*\"([^\"]*)\"");
    static readonly Regex reName = new Regex("\"name\"\\s*:\\s*\"([^\"]*)\"");
    static readonly Regex reCtok = new Regex("\"ctok\"\\s*:\\s*\"([^\"]*)\"");

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
        RebuildDevices(); devicesPanel.Visible = true;   // show saved devices in the idle preview panel
        BuildTray();
        Log("PhoneCam v" + Version + " started. receiver=" + (receiverExe ?? "NOT FOUND") + " adb=" + (adbExe ?? "none"));
        timer = new System.Windows.Forms.Timer { Interval = 700 };
        timer.Tick += OnTick;
        FormClosing += (s, e) => { SaveSettings(); if (running || reconnecting) SendPhoneStop(true); StopReceiver();
                                   if (tray != null) { tray.Visible = false; tray.Dispose(); } };
        // Optional: connect immediately on launch (handy for a "start on login" shortcut).
        if (Array.IndexOf(Environment.GetCommandLineArgs(), "-autostart") >= 0)
            Shown += (s, e) => { if (!running) StartReceiver(); };
        // -tray (used by the start-with-Windows entry): come up hidden in the notification area.
        startMinimized = Array.IndexOf(Environment.GetCommandLineArgs(), "-tray") >= 0;
        if (startMinimized) Shown += (s, e) => { WindowState = FormWindowState.Minimized; Hide(); };
    }

    static string FindFirst(string[] paths)
    { foreach (var p in paths) { try { var f = Path.GetFullPath(p); if (File.Exists(f)) return f; } catch { } } return null; }

    void BuildUi()
    {
        Text = "PhoneCam v" + Version;
        FormBorderStyle = FormBorderStyle.FixedSingle;
        MaximizeBox = false;
        ClientSize = new Size(744, 570);
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
        cbMic = Check("Use microphone", 22, 222);
        cbBoost = new ComboBox { DropDownStyle = ComboBoxStyle.DropDownList, Location = new Point(150, 219), Width = 96, FlatStyle = FlatStyle.Flat, BackColor = Card, ForeColor = Fg };
        cbBoost.Items.AddRange(new object[] { "Boost: Off", "Boost: Low", "Boost: Med", "Boost: High" });
        cbBoost.SelectedIndex = 2;   // Medium (~+12 dB) by default — phone mics are quiet
        cbEq = new ComboBox { DropDownStyle = ComboBoxStyle.DropDownList, Location = new Point(150, 247), Width = 96, FlatStyle = FlatStyle.Flat, BackColor = Card, ForeColor = Fg };
        cbEq.Items.AddRange(new object[] { "EQ: Off", "EQ: Clarity", "EQ: Warm", "EQ: Bright", "EQ: Podcast",
                                           "EQ: Clarity+", "EQ: Warm+", "EQ: Bright+", "EQ: Podcast+", "EQ: Custom…" });
        cbEq.SelectedIndex = 0;
        cbEq.SelectedIndexChanged += OnEqChanged;
        cbMic.CheckedChanged += (s, e) => { cbBoost.Enabled = cbMic.Checked; cbEq.Enabled = cbMic.Checked; };
        cbBoost.Enabled = cbMic.Checked; cbEq.Enabled = cbMic.Checked;
        cbFlipH = Check("Flip left / right", 22, 278);
        cbFlipV = Check("Flip up / down", 22, 304);
        cbUdp = Check("Lower latency (Wi-Fi) — may glitch", 22, 330);
        cbEncrypt = Check("Encrypted (🔒 Wi-Fi QR only)", 22, 354);
        cbEncrypt.CheckedChanged += (s, e) => { if (cbEncrypt.Checked) cbUdp.Checked = false; };
        Controls.Add(cbMic); Controls.Add(cbBoost); Controls.Add(cbEq); Controls.Add(cbFlipH); Controls.Add(cbFlipV); Controls.Add(cbUdp); Controls.Add(cbEncrypt);

        btnStart = new Button { Text = "Start", Location = new Point(22, 386), Size = new Size(224, 40), FlatStyle = FlatStyle.Flat, BackColor = Accent, ForeColor = Color.White, Font = new Font("Segoe UI Semibold", 11f) };
        btnStart.FlatAppearance.BorderSize = 0;
        btnStart.Click += OnStartStop;
        Controls.Add(btnStart);

        lblDot = new Label { Text = "●", ForeColor = Sub, Location = new Point(24, 440), AutoSize = true, Font = new Font("Segoe UI", 11f) };
        lblStatus = new Label { Text = "Idle", ForeColor = Sub, Location = new Point(44, 442), AutoSize = true, MaximumSize = new Size(220, 0) };
        Controls.Add(lblDot); Controls.Add(lblStatus);

        // Live mic level meter (visible only while the mic is streaming).
        micLabel = new Label { Text = "Mic", ForeColor = Sub, Location = new Point(24, 468), AutoSize = true, Font = new Font("Segoe UI", 8.25f), Visible = false };
        micMeter = new Panel { Location = new Point(56, 469), Size = new Size(180, 12), BackColor = Color.FromArgb(20, 22, 25), Visible = false };
        micMeter.Paint += PaintMeter;
        Controls.Add(micLabel); Controls.Add(micMeter);

        tip = new Label { Text = TipText(false), ForeColor = Sub, Location = new Point(22, 492), AutoSize = true };
        Controls.Add(tip);

        linkDiag = new LinkLabel { Text = "Copy diagnostics", Location = new Point(22, 540), AutoSize = true, LinkColor = Accent, ActiveLinkColor = Accent, LinkBehavior = LinkBehavior.AlwaysUnderline, Font = new Font("Segoe UI", 9f) };
        linkDiag.LinkClicked += (s, e) => CopyDiagnostics();
        Controls.Add(linkDiag);

        // Right: embedded live preview (also hosts the pairing QR before a phone connects)
        preview = new Panel { Location = new Point(260, 84), Size = new Size(468, 462), BackColor = Color.FromArgb(12, 13, 15), BorderStyle = BorderStyle.None };
        preview.Paint += (s, e) => { using (var pen = new Pen(Line)) e.Graphics.DrawRectangle(pen, 0, 0, preview.Width - 1, preview.Height - 1); };
        previewHint = new Label { Text = "Live preview appears here once you press Start.", ForeColor = Sub, BackColor = Color.FromArgb(12, 13, 15), AutoSize = true, Location = new Point(16, 16) };
        // Centered as one vertical group inside the 468×392 preview panel: the QR block itself
        // sits at the panel's vertical centre, with the caption just above it.
        qrLabel = new Label { Text = "Scan this with the PhoneCam phone app\n(tap “Scan PC QR to connect”)", ForeColor = Fg, BackColor = Color.FromArgb(12, 13, 15), Size = new Size(468, 40), Location = new Point(0, 34), TextAlign = ContentAlignment.MiddleCenter, Visible = false };
        qrBox = new PictureBox { Location = new Point(104, 82), Size = new Size(260, 260), SizeMode = PictureBoxSizeMode.Zoom, BackColor = Color.White, Visible = false };
        // Shown below the QR only if an encrypted connection stalls — the #1 cause is a VPN hiding the LAN.
        srtHint = new Label { Text = "Nothing yet? If you use a VPN, turn on “Allow LAN traffic” in it on\nBOTH this PC and the phone — a full tunnel hides local devices.",
            ForeColor = Amber, BackColor = Color.FromArgb(12, 13, 15), Size = new Size(468, 46), Location = new Point(0, 356),
            TextAlign = ContentAlignment.MiddleCenter, Font = new Font("Segoe UI", 8.5f), Visible = false };
        // Saved-devices list — fills the empty preview panel while idle so you can reconnect without a QR.
        devicesPanel = new Panel { Location = new Point(0, 0), Size = preview.ClientSize, BackColor = Color.FromArgb(12, 13, 15), Visible = false };
        preview.Controls.Add(previewHint); preview.Controls.Add(qrLabel); preview.Controls.Add(qrBox); preview.Controls.Add(srtHint); preview.Controls.Add(devicesPanel);
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

    // --- mic level meter ---
    void PaintMeter(object sender, PaintEventArgs e)
    {
        var g = e.Graphics;
        int w = micMeter.ClientSize.Width, h = micMeter.ClientSize.Height;
        float lvl = micLevel; if (lvl < 0f) lvl = 0f; if (lvl > 1f) lvl = 1f;
        Color c = lvl < 0.7f ? Green : (lvl < 0.9f ? Amber : Color.FromArgb(226, 96, 96));
        using (var b = new SolidBrush(c)) g.FillRectangle(b, 0, 0, (int)(w * lvl), h);
        using (var pen = new Pen(Color.FromArgb(60, 64, 72)))
        { g.DrawLine(pen, (int)(w * 0.7f), 0, (int)(w * 0.7f), h); g.DrawLine(pen, (int)(w * 0.9f), 0, (int)(w * 0.9f), h); }
    }

    // --- system tray + start-with-Windows ---
    const string RunKey = @"Software\Microsoft\Windows\CurrentVersion\Run";
    const string RunName = "PhoneCam";

    void BuildTray()
    {
        var menu = new ContextMenuStrip();
        var open = new ToolStripMenuItem("Open PhoneCam"); open.Click += (s, e) => ShowFromTray();
        var miAuto = new ToolStripMenuItem("Start with Windows") { CheckOnClick = true, Checked = AutostartEnabled() };
        miAuto.Click += (s, e) => SetAutostart(miAuto.Checked);
        var miTray = new ToolStripMenuItem("Minimize to tray") { CheckOnClick = true, Checked = minimizeToTray };
        miTray.Click += (s, e) => { minimizeToTray = miTray.Checked; SaveSettings(); };
        var stop = new ToolStripMenuItem("Stop streaming");
        stop.Click += (s, e) => { if (running || reconnecting) { manualStop = true; SendPhoneStop(); StopReceiver(); } };
        var exit = new ToolStripMenuItem("Exit"); exit.Click += (s, e) => Close();
        menu.Items.Add(open); menu.Items.Add(new ToolStripSeparator());
        menu.Items.Add(miAuto); menu.Items.Add(miTray); menu.Items.Add(new ToolStripSeparator());
        menu.Items.Add(stop); menu.Items.Add(exit);
        tray = new NotifyIcon { Icon = SystemIcons.Application, Text = "PhoneCam", Visible = true, ContextMenuStrip = menu };
        tray.DoubleClick += (s, e) => ShowFromTray();
    }

    void ShowFromTray() { Show(); WindowState = FormWindowState.Normal; Activate(); }

    protected override void OnResize(EventArgs e)
    {
        base.OnResize(e);
        if (minimizeToTray && tray != null && WindowState == FormWindowState.Minimized) Hide();
    }

    bool AutostartEnabled()
    {
        try { using (var k = Microsoft.Win32.Registry.CurrentUser.OpenSubKey(RunKey))
              return k != null && k.GetValue(RunName) != null; }
        catch { return false; }
    }

    void SetAutostart(bool on)
    {
        try
        {
            using (var k = Microsoft.Win32.Registry.CurrentUser.OpenSubKey(RunKey, true)
                        ?? Microsoft.Win32.Registry.CurrentUser.CreateSubKey(RunKey))
            {
                if (on) k.SetValue(RunName, "\"" + Application.ExecutablePath + "\" -tray");
                else k.DeleteValue(RunName, false);
            }
            Log("start-with-Windows " + (on ? "enabled" : "disabled"));
        }
        catch (Exception ex) { Log("autostart set failed: " + ex.Message); }
    }

    static bool OnScreen(int x, int y)
    {
        var r = SystemInformation.VirtualScreen;
        return x >= r.Left - 8 && y >= r.Top - 8 && x < r.Right - 40 && y < r.Bottom - 40;
    }

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
        cbMic.Enabled = on; cbFlipH.Enabled = on; cbFlipV.Enabled = on; cbUdp.Enabled = on; cbEncrypt.Enabled = on;
        cbBoost.Enabled = on && cbMic.Checked;
        cbEq.Enabled = on && cbMic.Checked;
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

    void OnStartStop(object sender, EventArgs e)
    {
        if (running || reconnecting) { manualStop = true; SendPhoneStop(); StopReceiver(); }   // deliberate stop — tell the phone, don't auto-reconnect
        else StartReceiver();
    }

    // "EQ: Custom…" opens the band editor; presets just select. On cancel, revert to the last choice.
    void OnEqChanged(object sender, EventArgs e)
    {
        if (suppressEqDialog) return;
        int idx = cbEq.SelectedIndex;
        if (idx >= 0 && idx < EqPreset.Length && EqPreset[idx] == "custom")
        {
            using (var dlg = new EqEditorForm(customEq))
            {
                if (dlg.ShowDialog(this) == DialogResult.OK) { customEq = dlg.Result; prevEqIndex = idx; }
                else { suppressEqDialog = true; cbEq.SelectedIndex = prevEqIndex; suppressEqDialog = false; }
            }
        }
        else prevEqIndex = idx;
    }

    /// <summary>The --eq value for the current selection: a preset name, or the custom band list.</summary>
    string SelectedEq()
    {
        int i = cbEq.SelectedIndex;
        if (i <= 0 || i >= EqPreset.Length) return "";
        return EqPreset[i] == "custom" ? customEq : EqPreset[i];
    }

    // Returns false if the caller should abort the start (user cancelled, or we launched the installer).
    // On return, useMic may be flipped off if the user declined VB-CABLE.
    bool PrepareMic(ref bool useMic)
    {
        if (!useMic || VbCableInstalled()) return true;
        var r = MessageBox.Show(
            "Using the phone as a microphone needs the free VB-CABLE audio driver (by VB-Audio).\n\nDownload and install it now?",
            "PhoneCam — microphone setup", MessageBoxButtons.YesNoCancel, MessageBoxIcon.Question);
        if (r == DialogResult.Cancel) return false;
        if (r == DialogResult.Yes) { InstallVbCable(); return false; }  // install, then press Start again
        useMic = false;                                                // No -> just the camera
        return true;
    }

    void StartReceiver()
    {
        bool useMic = cbMic.Checked;
        if (!PrepareMic(ref useMic)) return;

        // Fresh user-initiated start: clear any leftover reconnect/stop state.
        // No token for USB/manual-IP (no handshake) — USB stops are authorized by loopback on the phone.
        manualStop = false; reconnecting = false; wentLive = false; reconnectAttempts = 0; phoneToken = ""; srtMode = false;
        Log("Start: mode=" + (rbUsb.Checked ? "usb" : rbQr.Checked ? "qr" : "wifi-ip") + " mic=" + useMic + " enc=" + (rbQr.Checked && cbEncrypt.Checked));
        // Encrypted Wi-Fi: we become the SRT listener and the phone pushes to us (AES). QR carries our endpoint.
        if (rbQr.Checked && cbEncrypt.Checked) { StartSrtPairing(useMic); return; }
        // Wi-Fi QR pairing: show a code, let the phone scan it and announce its URL to us.
        if (rbQr.Checked) { StartQrPairing(useMic); return; }

        string url;
        if (rbUsb.Checked)
        {
            if (adbExe == null) { MessageBox.Show("adb not found (needed for USB). Use Wi-Fi instead.", "PhoneCam"); return; }
            if (!Regex.IsMatch(Adb("devices"), @"\bdevice\b")) { MessageBox.Show("No phone detected over USB.\n\nEnable Developer Options → USB debugging, plug in, tap “Allow”. Or use Wi-Fi.", "PhoneCam"); return; }
            Adb("forward tcp:" + LocalPort + " tcp:" + PhonePort);
            Adb("forward tcp:" + LocalControlPort + " tcp:" + PhoneControlPort);   // so Stop can reach the phone over USB
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
        lastUrl = url; lastUseMic = useMic; reconnecting = false;   // remember the target for auto-reconnect
        // Where to send "stop now": the phone's IP over Wi-Fi, or the forwarded loopback port over USB.
        // In SRT mode the phone is the client, so there's no stop channel — it stops when we (the listener) do.
        controlHost = srtMode ? "" : HostOf(url);
        controlPort = usbForwarded ? LocalControlPort : PhoneControlPort;
        // USB has no QR handshake, so fetch the phone's RTSP secret over the (loopback-only) control port.
        if (usbForwarded && phoneToken.Length == 0) phoneToken = QueryUsbCreds();
        pairedHost = HostOf(url);
        videoSeen = false; audioSeen = false; reachIssue = false; authIssue = false; micLevel = 0f;
        var a = new List<string> { url, "--preview" };   // --preview so we can embed the feed
        if (cbFlipH.Checked) a.Add("--flip-h");
        if (cbFlipV.Checked) a.Add("--flip-v");
        // UDP avoids TCP's retransmit stalls (lower/steadier latency on busy Wi-Fi) but can't ride the
        // USB adb tunnel, which is TCP-only — so only over Wi-Fi/QR.
        if (cbUdp.Checked && !usbForwarded && !srtMode) a.Add("--udp");
        if (useMic)
        {
            a.Add("--audio-device"); a.Add("CABLE Input");
            int bi = cbBoost.SelectedIndex; if (bi < 0 || bi >= BoostDb.Length) bi = 2;
            if (BoostDb[bi] != 0) { a.Add("--mic-gain"); a.Add(BoostDb[bi].ToString()); }
            string eq = SelectedEq();
            if (eq.Length > 0) { a.Add("--eq"); a.Add(eq); }
        }
        else a.Add("--no-audio");
        // Stream is Basic-auth protected on the phone; present the credentials (blank over USB before the
        // creds query succeeds / manual-IP, where the phone may not require auth).
        if (phoneToken.Length > 0) { a.Add("--rtsp-user"); a.Add(RtspUser); a.Add("--rtsp-pass"); a.Add(phoneToken); }

        string args = BuildArgs(a);
        Log("launching receiver: " + Redact(args));   // never log the RTSP password / SRT passphrase
        var psi = new ProcessStartInfo(receiverExe, args) { UseShellExecute = false, CreateNoWindow = true, RedirectStandardError = true, RedirectStandardOutput = true, StandardErrorEncoding = Encoding.UTF8 };
        recv = new Process { StartInfo = psi };
        recv.ErrorDataReceived += (s, ev) =>
        {
            if (ev.Data == null) return;
            string d = ev.Data;
            // Mic-meter feed at ~10 Hz: parse the level, but don't log it (it would flood diagnostics).
            if (d.StartsWith("[level]"))
            {
                float lv; var parts = d.Split(' ');
                if (parts.Length >= 2 && float.TryParse(parts[1], System.Globalization.NumberStyles.Float, System.Globalization.CultureInfo.InvariantCulture, out lv))
                {
                    micLevel = lv;
                    try { if (micMeter.IsHandleCreated) micMeter.BeginInvoke((Action)(() => micMeter.Invalidate())); } catch { }
                }
                return;
            }
            Log("[recv] " + Redact(d));   // mask the password / passphrase if FFmpeg echoes the URL
            if (d.IndexOf("[video]", StringComparison.OrdinalIgnoreCase) >= 0) videoSeen = true;
            if (d.IndexOf("[audio] rendering", StringComparison.OrdinalIgnoreCase) >= 0) audioSeen = true;
            if (d.IndexOf("401", StringComparison.Ordinal) >= 0 || d.IndexOf("Unauthorized", StringComparison.OrdinalIgnoreCase) >= 0)
            { authIssue = true; reachIssue = true; }
            if (d.IndexOf("reconnect", StringComparison.OrdinalIgnoreCase) >= 0 || d.IndexOf("unreachable", StringComparison.OrdinalIgnoreCase) >= 0
                || d.IndexOf("refused", StringComparison.OrdinalIgnoreCase) >= 0 || d.IndexOf("timed out", StringComparison.OrdinalIgnoreCase) >= 0
                || d.IndexOf("failed", StringComparison.OrdinalIgnoreCase) >= 0) reachIssue = true;
        };
        try { recv.Start(); recv.BeginErrorReadLine(); }
        catch (Exception ex) { Log("receiver start FAILED: " + ex.Message); MessageBox.Show("Failed to start receiver: " + ex.Message, "PhoneCam"); StopReceiver(); return; }

        running = true; embedded = IntPtr.Zero;
        devicesPanel.Visible = false;
        if (!srtMode) previewHint.Visible = true;   // in SRT mode the QR stays up until the phone connects
        tip.Text = TipText(useMic);
        SetInputsEnabled(false);
        btnStart.Text = "Stop"; btnStart.BackColor = Color.FromArgb(70, 74, 82);
        timer.Start();
    }

    /// <summary>Encrypted mode: run receiver.exe as an SRT listener and show a QR the phone pushes to.
    /// No TCP announce-back — the phone connects out to us and pushes an AES stream.</summary>
    void StartSrtPairing(bool useMic)
    {
        string ip = LocalIPv4();
        if (ip == null) { MessageBox.Show("Couldn't determine this PC's Wi-Fi address. Use USB, or turn off Encrypted.", "PhoneCam"); return; }
        srtMode = true;
        srtPass = Guid.NewGuid().ToString("N").Substring(0, 24);   // 24-hex SRT passphrase (min 10, max 79)
        string listenUrl = "srt://0.0.0.0:" + SrtPort + "?mode=listener&passphrase=" + srtPass + "&pbkeylen=16&latency=120000";
        string payload = "PCAM2:" + ip + ":" + SrtPort + ":" + srtPass;
        Log("SRT: listening (encrypted) on udp/" + SrtPort + " — waiting for the phone to scan");
        try { qrBox.Image = MakeQr(payload); }
        catch (Exception ex) { MessageBox.Show("Couldn't render the QR: " + ex.Message, "PhoneCam"); srtMode = false; return; }
        qrLabel.Text = "Scan this with the PhoneCam app\n(encrypted 🔒)";
        qrLabel.Visible = true; qrBox.Visible = true;
        srtWaitSince = DateTime.Now; srtHint.Visible = false;
        SetStatus(Amber, "Scan the QR (encrypted) with the PhoneCam app…");
        StartReceiverWithUrl(listenUrl, useMic);   // launches the listener + sets running/Stop/timer
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
        previewHint.Visible = false; devicesPanel.Visible = false; qrLabel.Visible = true; qrBox.Visible = true;
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
                    var mt = reTok.Match(line); var mu = reUrl.Match(line); var mn = reName.Match(line); var mc = reCtok.Match(line);
                    if (mt.Success && mt.Groups[1].Value == pairToken && mu.Success)
                    {
                        try { var ok = Encoding.UTF8.GetBytes("OK\n"); ns.Write(ok, 0, ok.Length); } catch { }
                        // org.json (Android) escapes '/' as '\/', and we extract with a regex rather
                        // than a JSON parser — so unescape, else the receiver gets rtsp:\/\/… and fails
                        // with "Failed to resolve hostname \".
                        string url = mu.Groups[1].Value.Replace("\\/", "/");
                        string name = mn.Success ? mn.Groups[1].Value.Replace("\\/", "/") : "";
                        string ctok = mc.Success ? mc.Groups[1].Value : "";
                        Log("pairing OK: phone stream = " + url + (name.Length > 0 ? " (" + name + ")" : ""));
                        BeginInvoke((Action)(() => OnPaired(url, name, ctok)));
                    }
                    else { Log("pairing: token mismatch / bad payload from " + peer); BeginInvoke((Action)(() => SetStatus(Amber, "A device tried to pair but the code didn't match."))); }
                }
            }
            catch { /* listener stopped (Stop pressed) or timed out */ }
        }) { IsBackground = true };
        pairThread.Start();
    }

    void OnPaired(string url, string name, string token)
    {
        try { if (pairListener != null) { pairListener.Stop(); pairListener = null; } } catch { }
        qrLabel.Visible = false; qrBox.Visible = false;
        if (qrBox.Image != null) { var img = qrBox.Image; qrBox.Image = null; img.Dispose(); }
        SaveDevice(name, url, token);   // remember this phone (+ its stop token) so next time you can skip the QR
        phoneToken = token;             // authorize a later Stop of this session
        previewHint.Text = "Connecting…"; SetStatus(Amber, "Phone paired (" + HostOf(url) + ") — connecting…");
        manualStop = false; wentLive = false;
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
        // Between reconnect attempts recv is null while we wait out the delay, then relaunch.
        if (reconnecting)
        {
            if (DateTime.Now < reconnectAt) return;
            reconnecting = false;
            if (reconnectAttempts >= MaxReconnectAttempts)
            {
                string msg = wentLive
                    ? "Lost the phone — stopped. Press Start, or pick a saved device, to reconnect."
                    : "Couldn't reach the phone. Open PhoneCam on it and press Start, then try again.";
                StopReceiver();
                SetStatus(Amber, msg);
                return;
            }
            reconnectAttempts++;
            SetStatus(Amber, "Reconnecting… (" + reconnectAttempts + ")");
            StartReceiverWithUrl(lastUrl, lastUseMic);
            return;
        }

        if (recv == null) return;

        if (recv.HasExited)
        {
            recv = null; embedded = IntPtr.Zero;
            // A deliberate stop (button / app close), or we never had a URL to retry: just stop.
            if (manualStop || lastUrl == null) { StopReceiver(); if (!manualStop) SetStatus(Sub, "Stopped"); return; }
            // Otherwise the feed dropped while the user still wants it — schedule an auto-reconnect.
            reconnecting = true;
            reconnectAt = DateTime.Now.AddMilliseconds(ReconnectDelayMs);
            videoSeen = false; audioSeen = false;
            SetStatus(Amber, "Connection lost — reconnecting…");
            previewHint.Text = "Reconnecting…"; previewHint.Visible = true;
            return;
        }

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

        bool hasVideo = videoSeen || embedded != IntPtr.Zero;
        // Back to a live feed → this URL is good; refill the reconnect budget for the next blip.
        if (hasVideo || audioSeen) { wentLive = true; reconnectAttempts = 0; }
        // Phone connected → drop the pairing QR (SRT has no OnPaired step to do it).
        if ((hasVideo || audioSeen) && qrBox.Visible)
        { qrLabel.Visible = false; qrBox.Visible = false; if (qrBox.Image != null) { var i = qrBox.Image; qrBox.Image = null; i.Dispose(); } }
        // Encrypted connection stalling? Surface the VPN "allow LAN" hint (the usual culprit).
        bool srtStalled = srtMode && !hasVideo && !audioSeen && (DateTime.Now - srtWaitSince).TotalSeconds > 12;
        if (srtHint.Visible != srtStalled) srtHint.Visible = srtStalled;
        bool micActive = running && audioSeen;
        if (micMeter.Visible != micActive) { micMeter.Visible = micActive; micLabel.Visible = micActive; }
        if (hasVideo) SetStatus(Green, "Live — select “PhoneCam Camera” in your app");
        else if (audioSeen)
        {
            // Mic-only: there's no video window, so the video-based detection never fires. Audio is up.
            SetStatus(Green, "Live (mic) — pick “CABLE Output” as your microphone");
            if (embedded == IntPtr.Zero) { previewHint.Text = "Microphone only — no video."; previewHint.Visible = true; }
        }
        else if (srtMode)
            SetStatus(Amber, "Waiting for the phone to connect (encrypted)…");
        else if (authIssue)
            SetStatus(Amber, "Phone requires pairing — connect with the QR or a saved device, not a typed IP.");
        else if (reachIssue)
            SetStatus(Amber, rbUsb.Checked ? "Waiting for the phone (press Start in the app)…"
                : "Can't reach the phone" + (pairedHost != null ? " at " + pairedHost : "") + " — same Wi-Fi? VPN off? Firewall?");
    }

    /// <summary>Ask the phone to stop streaming now (deliberate PC-side Stop). Async by default so an
    /// unreachable/old phone can't hang the UI; pass wait=true on app-close so it isn't cut off by exit.</summary>
    void SendPhoneStop(bool wait = false)
    {
        string host = controlHost; int port = controlPort;
        if (string.IsNullOrEmpty(host)) return;
        // Authenticated over Wi-Fi (token from pairing / saved device); bare over USB (phone trusts loopback).
        string cmd = phoneToken.Length > 0 ? "PCAM-STOP:" + phoneToken + "\n" : "PCAM-STOP\n";
        Log("sending stop to phone " + host + ":" + port);
        Action send = delegate
        {
            try
            {
                using (var c = new TcpClient())
                {
                    var ar = c.BeginConnect(host, port, null, null);
                    if (!ar.AsyncWaitHandle.WaitOne(1500)) return;   // phone unreachable or too old — give up quietly
                    c.EndConnect(ar);
                    var b = Encoding.UTF8.GetBytes(cmd);
                    var ns = c.GetStream(); ns.Write(b, 0, b.Length); ns.Flush();
                }
            }
            catch { }
        };
        if (wait) send();
        else ThreadPool.QueueUserWorkItem(delegate { send(); });
    }

    /// <summary>Mask secrets (RTSP password / SRT passphrase) before anything reaches the log.</summary>
    string Redact(string s)
    {
        if (phoneToken.Length > 0) s = s.Replace(phoneToken, "***");
        if (srtPass.Length > 0) s = s.Replace(srtPass, "***");
        return s;
    }

    /// <summary>Over USB, ask the phone (via the loopback-forwarded control port) for its RTSP secret so
    /// we can authenticate the pull. The phone only answers this on loopback. "" if unavailable.</summary>
    string QueryUsbCreds()
    {
        try
        {
            using (var c = new TcpClient())
            {
                var ar = c.BeginConnect("127.0.0.1", LocalControlPort, null, null);
                if (!ar.AsyncWaitHandle.WaitOne(700)) return "";
                c.EndConnect(ar);
                c.ReceiveTimeout = 700;
                var ns = c.GetStream();
                var q = Encoding.UTF8.GetBytes("PCAM-CREDS?\n");
                ns.Write(q, 0, q.Length); ns.Flush();
                string line = new StreamReader(ns, Encoding.UTF8).ReadLine();
                return (line ?? "").Trim();
            }
        }
        catch { return ""; }
    }

    void StopReceiver()
    {
        if (running) Log("stopped");
        timer.Stop();
        reconnecting = false;
        srtMode = false;
        embedded = IntPtr.Zero;
        micLevel = 0f;
        if (micMeter != null) { micMeter.Visible = false; micLabel.Visible = false; }
        try { if (pairListener != null) { pairListener.Stop(); pairListener = null; } } catch { }
        try { if (recv != null && !recv.HasExited) recv.Kill(); } catch { }
        recv = null;
        if (usbForwarded) { Adb("forward --remove tcp:" + LocalPort); usbForwarded = false; }
        running = false;
        if (IsHandleCreated)
        {
            if (qrBox != null) { qrLabel.Visible = false; qrBox.Visible = false; if (qrBox.Image != null) { var i = qrBox.Image; qrBox.Image = null; i.Dispose(); } }
            if (srtHint != null) srtHint.Visible = false;
            SetInputsEnabled(true);
            tip.Text = TipText(false);
            btnStart.Text = "Start"; btnStart.BackColor = Accent;
            previewHint.Text = "Live preview appears here once you press Start."; previewHint.Visible = false;
            RebuildDevices(); devicesPanel.Visible = true;   // back to idle — offer one-click reconnect
            if (lblStatus.Text != "Stopped") SetStatus(Sub, "Idle");
        }
    }

    // --- saved devices ---

    /// <summary>Remember (or refresh) a phone as a named device for one-click reconnect. De-dupes by name and URL.</summary>
    void SaveDevice(string name, string url, string token)
    {
        if (string.IsNullOrEmpty(url)) return;
        if (string.IsNullOrEmpty(name)) name = HostOf(url);
        name = name.Replace('\t', ' ').Trim();
        devices.RemoveAll(d => d[0] == name || d[1] == url);
        devices.Insert(0, new[] { name, url, token ?? "" });
        while (devices.Count > 8) devices.RemoveAt(devices.Count - 1);
        SaveSettings();
        if (!running && !reconnecting) RebuildDevices();
    }

    void ForgetDevice(string name)
    {
        devices.RemoveAll(d => d[0] == name);
        SaveSettings();
        RebuildDevices();
    }

    /// <summary>Connect straight to a saved phone's URL — no QR. Reuses the current Options (mic/EQ/flip).</summary>
    void ConnectToDevice(string url, string token)
    {
        if (receiverExe == null || running || reconnecting) return;
        bool useMic = cbMic.Checked;
        if (!PrepareMic(ref useMic)) return;
        manualStop = false; reconnecting = false; wentLive = false; reconnectAttempts = 0;
        usbForwarded = false;   // saved devices are always Wi-Fi (a LAN rtsp URL), never the USB tunnel
        phoneToken = token ?? "";   // the saved stop token authorizes stopping this phone
        Log("connect to saved device: " + url + " mic=" + useMic);
        previewHint.Text = "Connecting…";
        SetStatus(Amber, "Connecting to " + HostOf(url) + "…");
        StartReceiverWithUrl(url, useMic);
    }

    /// <summary>Repaint the saved-devices list shown in the (idle) preview panel.</summary>
    void RebuildDevices()
    {
        if (devicesPanel == null) return;
        var back = devicesPanel.BackColor;
        devicesPanel.Controls.Clear();
        devicesPanel.Controls.Add(new Label { Text = "Saved devices", ForeColor = Sub,
            Font = new Font("Segoe UI", 9f, FontStyle.Bold), Location = new Point(16, 14), AutoSize = true, BackColor = back });
        if (devices.Count == 0)
        {
            devicesPanel.Controls.Add(new Label {
                Text = "Pair once with the QR and your phone is saved here,\nso next time you can reconnect with one click.",
                ForeColor = Sub, Location = new Point(16, 42), AutoSize = true, BackColor = back });
            return;
        }
        int y = 44, rowW = devicesPanel.ClientSize.Width - 24;
        foreach (var d in devices)
        {
            string name = d[0], url = d[1], token = d.Length > 2 ? d[2] : "";
            var row = new Panel { Location = new Point(12, y), Size = new Size(rowW, 50), BackColor = Card };
            row.Controls.Add(new Label { Text = name, ForeColor = Fg, Font = new Font("Segoe UI Semibold", 10f),
                Location = new Point(12, 7), AutoSize = true, BackColor = Card });
            row.Controls.Add(new Label { Text = HostOf(url), ForeColor = Sub, Font = new Font("Segoe UI", 8f),
                Location = new Point(12, 28), AutoSize = true, BackColor = Card });
            var conn = new Button { Text = "Connect", Size = new Size(84, 30), Location = new Point(rowW - 84 - 44, 10),
                FlatStyle = FlatStyle.Flat, BackColor = Accent, ForeColor = Color.White };
            conn.FlatAppearance.BorderSize = 0;
            conn.Click += (s, e) => ConnectToDevice(url, token);
            var forget = new Button { Text = "✕", Size = new Size(30, 30), Location = new Point(rowW - 36, 10),
                FlatStyle = FlatStyle.Flat, BackColor = Card, ForeColor = Sub };
            forget.FlatAppearance.BorderColor = Line;
            forget.Click += (s, e) => ForgetDevice(name);
            row.Controls.Add(conn); row.Controls.Add(forget);
            devicesPanel.Controls.Add(row);
            y += 58;
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
                    case "udp": cbUdp.Checked = kv[1] == "1"; break;
                    case "encrypt": cbEncrypt.Checked = kv[1] == "1"; break;
                    case "boost": { int bi; if (int.TryParse(kv[1], out bi) && bi >= 0 && bi < BoostDb.Length) cbBoost.SelectedIndex = bi; } break;
                    case "eqcustom": customEq = kv[1]; break;
                    case "eq": { int ei; if (int.TryParse(kv[1], out ei) && ei >= 0 && ei < cbEq.Items.Count) { suppressEqDialog = true; cbEq.SelectedIndex = ei; suppressEqDialog = false; prevEqIndex = ei; } } break;
                    case "device": { var t = kv[1].Split('\t'); if (t.Length >= 2 && t[0].Length > 0 && t[1].Length > 0) devices.Add(new[] { t[0], t[1], t.Length > 2 ? t[2] : "" }); } break;
                    case "tray": minimizeToTray = kv[1] == "1"; break;
                    case "winpos": {
                        var xy = kv[1].Split(','); int wx, wy;
                        if (xy.Length == 2 && int.TryParse(xy[0], out wx) && int.TryParse(xy[1], out wy) && OnScreen(wx, wy))
                        { StartPosition = FormStartPosition.Manual; Location = new Point(wx, wy); }
                    } break;
                }
            }
        } catch { }
    }

    void SaveSettings()
    {
        try
        {
            Directory.CreateDirectory(Path.GetDirectoryName(settingsPath));
            var lines = new List<string> {
                "conn=" + (rbQr.Checked ? "qr" : rbWifi.Checked ? "wifi" : "usb"),
                "ip=" + tbIp.Text.Trim(),
                "mic=" + (cbMic.Checked ? "1" : "0"),
                "flipH=" + (cbFlipH.Checked ? "1" : "0"),
                "flipV=" + (cbFlipV.Checked ? "1" : "0"),
                "udp=" + (cbUdp.Checked ? "1" : "0"),
                "encrypt=" + (cbEncrypt.Checked ? "1" : "0"),
                "boost=" + cbBoost.SelectedIndex,
                "eqcustom=" + customEq,
                "eq=" + cbEq.SelectedIndex,
                "tray=" + (minimizeToTray ? "1" : "0"),
            };
            var wl = (WindowState == FormWindowState.Normal ? Location : RestoreBounds.Location);
            lines.Add("winpos=" + wl.X + "," + wl.Y);
            foreach (var d in devices) lines.Add("device=" + d[0] + "\t" + d[1] + "\t" + (d.Length > 2 ? d[2] : ""));
            File.WriteAllLines(settingsPath, lines);
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

// Custom EQ editor — an unlimited list of bands. Serializes to the receiver's "type:f:q:db;..." spec.
public class EqEditorForm : Form
{
    static readonly Color Bg = Color.FromArgb(27, 29, 33);
    static readonly Color Card = Color.FromArgb(42, 45, 51);
    static readonly Color Line = Color.FromArgb(60, 64, 72);
    static readonly Color Fg = Color.FromArgb(234, 236, 239);
    static readonly Color Accent = Color.FromArgb(72, 125, 232);
    static readonly string[] TypeNames = { "Peak", "High-pass", "Low-pass", "Low-shelf", "High-shelf" };
    static readonly string[] TypeCodes = { "peak", "hp", "lp", "ls", "hs" };

    readonly DataGridView grid;
    public string Result { get; private set; }

    public EqEditorForm(string bandList)
    {
        Result = "";
        Text = "Custom EQ";
        FormBorderStyle = FormBorderStyle.FixedDialog;
        MaximizeBox = false; MinimizeBox = false;
        StartPosition = FormStartPosition.CenterParent;
        ClientSize = new Size(470, 328);
        BackColor = Bg; ForeColor = Fg;
        Font = new Font("Segoe UI", 9.5f);

        Controls.Add(new Label { Text = "Add as many bands as you like. Gain is ignored for high/low-pass.", ForeColor = Color.FromArgb(138, 143, 150), Location = new Point(16, 12), AutoSize = true });

        grid = new DataGridView
        {
            Location = new Point(16, 38), Size = new Size(438, 216),
            BackgroundColor = Card, ForeColor = Fg, BorderStyle = BorderStyle.FixedSingle,
            GridColor = Line, RowHeadersVisible = false, AllowUserToAddRows = false,
            AllowUserToResizeRows = false, SelectionMode = DataGridViewSelectionMode.FullRowSelect,
            AutoSizeColumnsMode = DataGridViewAutoSizeColumnsMode.Fill, EnableHeadersVisualStyles = false
        };
        grid.ColumnHeadersDefaultCellStyle.BackColor = Card;
        grid.ColumnHeadersDefaultCellStyle.ForeColor = Fg;
        grid.DefaultCellStyle.BackColor = Card;
        grid.DefaultCellStyle.ForeColor = Fg;
        grid.DefaultCellStyle.SelectionBackColor = Accent;
        grid.DefaultCellStyle.SelectionForeColor = Color.White;
        grid.DataError += (s, e) => { e.ThrowException = false; };
        var typeCol = new DataGridViewComboBoxColumn { HeaderText = "Type", Name = "type", FlatStyle = FlatStyle.Flat, Width = 108 };
        typeCol.Items.AddRange(TypeNames);
        grid.Columns.Add(typeCol);
        grid.Columns.Add(new DataGridViewTextBoxColumn { HeaderText = "Freq (Hz)", Name = "freq" });
        grid.Columns.Add(new DataGridViewTextBoxColumn { HeaderText = "Q", Name = "q" });
        grid.Columns.Add(new DataGridViewTextBoxColumn { HeaderText = "Gain (dB)", Name = "gain" });
        Controls.Add(grid);

        var add = Btn("Add band", 16, 266, 92, Card);
        add.Click += (s, e) => grid.Rows.Add("Peak", "1000", "1.0", "0");
        var rem = Btn("Remove", 116, 266, 84, Card);
        rem.Click += (s, e) => { if (grid.CurrentRow != null) grid.Rows.Remove(grid.CurrentRow); };
        var ok = Btn("OK", 278, 266, 84, Accent); ok.ForeColor = Color.White; ok.FlatAppearance.BorderSize = 0; ok.DialogResult = DialogResult.OK;
        var cancel = Btn("Cancel", 370, 266, 84, Card); cancel.DialogResult = DialogResult.Cancel;
        Controls.Add(add); Controls.Add(rem); Controls.Add(ok); Controls.Add(cancel);
        AcceptButton = ok; CancelButton = cancel;

        LoadBands(bandList);
        FormClosing += (s, e) => { if (DialogResult == DialogResult.OK) Result = Serialize(); };
    }

    static Button Btn(string t, int x, int y, int w, Color bg)
    {
        var b = new Button { Text = t, Location = new Point(x, y), Size = new Size(w, 30), FlatStyle = FlatStyle.Flat, BackColor = bg, ForeColor = Fg };
        b.FlatAppearance.BorderColor = Line;
        return b;
    }

    void LoadBands(string spec)
    {
        if (string.IsNullOrEmpty(spec) || spec.Trim().Length == 0)
        {
            grid.Rows.Add("High-pass", "80", "0.7", "0");
            grid.Rows.Add("Peak", "300", "1.0", "-2.5");
            grid.Rows.Add("Peak", "3500", "1.0", "3");
            return;
        }
        foreach (var band in spec.Split(';'))
        {
            var p = band.Split(':');
            if (p.Length < 1 || p[0].Trim().Length == 0) continue;
            int ti = Array.IndexOf(TypeCodes, p[0].Trim().ToLowerInvariant());
            grid.Rows.Add(ti >= 0 ? TypeNames[ti] : "Peak",
                          p.Length > 1 ? p[1] : "1000", p.Length > 2 ? p[2] : "1.0", p.Length > 3 ? p[3] : "0");
        }
    }

    string Serialize()
    {
        var sb = new StringBuilder();
        foreach (DataGridViewRow r in grid.Rows)
        {
            var tv = r.Cells["type"].Value;
            int ti = Array.IndexOf(TypeNames, tv != null ? tv.ToString() : "Peak"); if (ti < 0) ti = 0;
            string f = Cell(r, "freq", "1000"), q = Cell(r, "q", "1.0"), g = Cell(r, "gain", "0");
            if (f.Length == 0) continue;
            if (sb.Length > 0) sb.Append(';');
            sb.Append(TypeCodes[ti]).Append(':').Append(f).Append(':').Append(q).Append(':').Append(g);
        }
        return sb.ToString();
    }

    static string Cell(DataGridViewRow r, string name, string dflt)
    {
        var v = r.Cells[name].Value;
        string s = v != null ? v.ToString().Trim() : "";
        return s.Length == 0 ? dflt : s;
    }
}
