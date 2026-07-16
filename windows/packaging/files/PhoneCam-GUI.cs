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

    LinkLabel linkDiag;
    CheckBox cbMic;
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
    Label previewHint, qrLabel, vpnHint;
    PictureBox qrBox;
    volatile float micLevel = 0f;   // 0..1 peak from the receiver's [level] lines, drives the mic meter
    NotifyIcon tray;
    bool minimizeToTray = false;
    bool startMinimized = false;    // launched with -tray (autostart) → start hidden in the tray
    bool autoListen = true;         // on launch, auto-start the WebRTC listener so the phone reconnects with one tap (no PC clicks)
    System.Windows.Forms.Timer timer;
    Process recv;
    IntPtr embedded = IntPtr.Zero;
    readonly object logLock = new object();
    readonly List<string> logLines = new List<string>();
    string receiverExe, settingsPath, logPath;
    const string Version = "0.5.2";
    const int WebrtcSigPort = 8891;   // TCP port the PC's WebRTC PCAM3 signaling listener binds
    bool webrtcMode = false;     // this session is a WebRTC (PCAM3) signaling + DTLS-SRTP receive (mic-only)
    string webrtcSecret = "";    // the pairSecret for this WebRTC session (kept out of logs)
    string iceBindIp = "";       // set to the USB-tethering adapter IP when detected (forces media over USB)
    string pairSecret = "";      // persisted secret, so a phone that saved us can reconnect later
    DateTime pairWaitSince = DateTime.MinValue;   // when the WebRTC QR went up (to time the VPN hint)
    bool running = false;
    volatile bool videoSeen = false, audioSeen = false;   // from receiver stderr, drive the status
    volatile bool streamDropped = false;   // the phone's stream ended mid-session (receiver is re-listening)

    // --- auto-relaunch: if receiver.exe dies unexpectedly, relaunch the WebRTC listener ---
    string lastUrl;              // "webrtc" sentinel once a session started (so OnTick relaunches, not stops)
    bool manualStop = false;     // true while the user (or app close) is deliberately stopping — no relaunch
    bool reconnecting = false;   // waiting out the delay between reconnect attempts
    bool wentLive = false;       // did this URL ever produce a live feed? (tunes the give-up message)
    int reconnectAttempts = 0;
    DateTime reconnectAt = DateTime.MinValue;
    const int MaxReconnectAttempts = 30;   // ~60 s at 2 s each, then give up
    const int ReconnectDelayMs = 2000;


    public PhoneCamGui()
    {
        string b = AppDomain.CurrentDomain.BaseDirectory;
        string lad = Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData);
        receiverExe = FindFirst(new[] { Path.Combine(b, "bin", "receiver.exe"), Path.Combine(b, "receiver.exe"), Path.Combine(b, "..", "build", "Release", "receiver.exe") });
        settingsPath = Path.Combine(lad, "PhoneCam", "gui.txt");
        logPath = Path.Combine(lad, "PhoneCam", "phonecam.log");
        try { Directory.CreateDirectory(Path.GetDirectoryName(logPath)); File.WriteAllText(logPath, ""); } catch { }  // fresh log per session
        BuildUi();
        LoadSettings();
        BuildTray();
        Log("PhoneCam v" + Version + " started. receiver=" + (receiverExe ?? "NOT FOUND"));
        timer = new System.Windows.Forms.Timer { Interval = 700 };
        timer.Tick += OnTick;
        FormClosing += (s, e) => { SaveSettings(); StopReceiver();
                                   if (tray != null) { tray.Visible = false; tray.Dispose(); } };
        // Optional: connect immediately on launch (handy for a "start on login" shortcut).
        if (Array.IndexOf(Environment.GetCommandLineArgs(), "-autostart") >= 0)
            Shown += (s, e) => { if (!running) StartReceiver(); };
        // -tray (used by the start-with-Windows entry): come up hidden in the notification area.
        startMinimized = Array.IndexOf(Environment.GetCommandLineArgs(), "-tray") >= 0;
        if (startMinimized) Shown += (s, e) => { WindowState = FormWindowState.Minimized; Hide(); };
        // Auto-listen: on every launch, start WebRTC so the phone can reconnect with one tap.
        if (autoListen) Shown += (s, e) => { if (!running) StartAutoListen(); };
    }

    static string FindFirst(string[] paths)
    { foreach (var p in paths) { try { var f = Path.GetFullPath(p); if (File.Exists(f)) return f; } catch { } } return null; }

    void BuildUi()
    {
        Text = "PhoneCam v" + Version;
        FormBorderStyle = FormBorderStyle.FixedSingle;
        MaximizeBox = false;
        ClientSize = new Size(744, 560);
        StartPosition = FormStartPosition.CenterScreen;
        BackColor = Bg; ForeColor = Fg;
        Font = new Font("Segoe UI", 9.5f);

        // Left control column
        var title = new Label { Text = "PhoneCam", Font = new Font("Segoe UI Semibold", 17f), ForeColor = Fg, Location = new Point(20, 18), AutoSize = true };
        Controls.Add(title);
        Controls.Add(new Label { Text = "v" + Version, ForeColor = Sub, Font = new Font("Segoe UI", 9f), Location = new Point(158, 32), AutoSize = true });

        AddSection("Connect", 72);
        Controls.Add(new Label { Text = "Wi-Fi — scan the QR shown here with the PhoneCam app.\nKeep the phone and PC on the same network.",
            ForeColor = Sub, Location = new Point(24, 106), AutoSize = true, MaximumSize = new Size(224, 0) });

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
        Controls.Add(cbMic); Controls.Add(cbBoost); Controls.Add(cbEq);

        btnStart = new Button { Text = "Start", Location = new Point(22, 362), Size = new Size(224, 40), FlatStyle = FlatStyle.Flat, BackColor = Accent, ForeColor = Color.White, Font = new Font("Segoe UI Semibold", 11f) };
        btnStart.FlatAppearance.BorderSize = 0;
        btnStart.Click += OnStartStop;
        Controls.Add(btnStart);

        lblDot = new Label { Text = "●", ForeColor = Sub, Location = new Point(24, 416), AutoSize = true, Font = new Font("Segoe UI", 11f) };
        lblStatus = new Label { Text = "Idle", ForeColor = Sub, Location = new Point(44, 418), AutoSize = true, MaximumSize = new Size(210, 0) };
        Controls.Add(lblDot); Controls.Add(lblStatus);

        // Live mic level meter (visible only while the mic is streaming).
        micLabel = new Label { Text = "Mic", ForeColor = Sub, Location = new Point(24, 446), AutoSize = true, Font = new Font("Segoe UI", 8.25f), Visible = false };
        micMeter = new Panel { Location = new Point(56, 446), Size = new Size(184, 14), BackColor = Color.FromArgb(20, 22, 25), Visible = false };
        micMeter.Paint += PaintMeter;
        Controls.Add(micLabel); Controls.Add(micMeter);

        tip = new Label { Text = TipText(false), ForeColor = Sub, Location = new Point(22, 474), AutoSize = true, MaximumSize = new Size(236, 0) };
        Controls.Add(tip);

        linkDiag = new LinkLabel { Text = "Copy diagnostics", Location = new Point(22, 522), AutoSize = true, LinkColor = Accent, ActiveLinkColor = Accent, LinkBehavior = LinkBehavior.AlwaysUnderline, Font = new Font("Segoe UI", 9f) };
        linkDiag.LinkClicked += (s, e) => CopyDiagnostics();
        Controls.Add(linkDiag);

        // Right: embedded live preview (also hosts the pairing QR before a phone connects)
        preview = new Panel { Location = new Point(260, 84), Size = new Size(468, 446), BackColor = Color.FromArgb(12, 13, 15), BorderStyle = BorderStyle.None };
        preview.Paint += (s, e) => { using (var pen = new Pen(Line)) e.Graphics.DrawRectangle(pen, 0, 0, preview.Width - 1, preview.Height - 1); };
        previewHint = new Label { Text = "Live preview appears here once you press Start.", ForeColor = Sub, BackColor = Color.FromArgb(12, 13, 15), AutoSize = true, Location = new Point(16, 16) };
        // Centered as one vertical group inside the 468×392 preview panel: the QR block itself
        // sits at the panel's vertical centre, with the caption just above it.
        qrLabel = new Label { Text = "Scan this with the PhoneCam phone app\n(tap “Scan PC QR to connect”)", ForeColor = Fg, BackColor = Color.FromArgb(12, 13, 15), Size = new Size(468, 40), Location = new Point(0, 34), TextAlign = ContentAlignment.MiddleCenter, Visible = false };
        qrBox = new PictureBox { Location = new Point(104, 82), Size = new Size(260, 260), SizeMode = PictureBoxSizeMode.Zoom, BackColor = Color.White, Visible = false };
        // Shown below the QR only if WebRTC stalls — the #1 cause is a VPN hiding the LAN.
        vpnHint = new Label { Text = "Nothing yet? If you use a VPN, turn on “Allow LAN traffic” in it on\nBOTH this PC and the phone — a full tunnel hides local devices.",
            ForeColor = Amber, BackColor = Color.FromArgb(12, 13, 15), Size = new Size(468, 46), Location = new Point(0, 356),
            TextAlign = ContentAlignment.MiddleCenter, Font = new Font("Segoe UI", 8.5f), Visible = false };
        preview.Controls.Add(previewHint); preview.Controls.Add(qrLabel); preview.Controls.Add(qrBox); preview.Controls.Add(vpnHint);
        Controls.Add(preview);

        if (receiverExe == null) { btnStart.Enabled = false; SetStatus(Color.IndianRed, "receiver.exe not found"); }
    }

    void AddSection(string t, int y)
    {
        Controls.Add(new Label { Text = t.ToUpperInvariant(), ForeColor = Sub, Font = new Font("Segoe UI", 7.5f, FontStyle.Bold), Location = new Point(24, y), AutoSize = true });
        Controls.Add(new Panel { BackColor = Line, Location = new Point(24, y + 17), Size = new Size(212, 1) });
    }
    CheckBox Check(string t, int x, int y) { return new CheckBox { Text = t, ForeColor = Fg, Location = new Point(x, y), AutoSize = true }; }

    void SetStatus(Color c, string s) { lblDot.ForeColor = c; lblStatus.ForeColor = c == Sub ? Sub : Fg; lblStatus.Text = s; }

    // --- mic level meter ---
    void PaintMeter(object sender, PaintEventArgs e)
    {
        var g = e.Graphics;
        int w = micMeter.ClientSize.Width, h = micMeter.ClientSize.Height;
        float lvl = micLevel; if (lvl < 0f) lvl = 0f; if (lvl > 1f) lvl = 1f;
        int iw = w - 2, ih = h - 2;   // 1px inset so the fill sits inside the border
        Color c = lvl < 0.7f ? Green : (lvl < 0.9f ? Amber : Color.FromArgb(226, 96, 96));
        using (var b = new SolidBrush(c)) g.FillRectangle(b, 1, 1, (int)(iw * lvl), ih);
        using (var pen = new Pen(Color.FromArgb(60, 64, 72)))
        { g.DrawLine(pen, 1 + (int)(iw * 0.7f), 1, 1 + (int)(iw * 0.7f), 1 + ih); g.DrawLine(pen, 1 + (int)(iw * 0.9f), 1, 1 + (int)(iw * 0.9f), 1 + ih); }
        using (var bp = new Pen(Line)) g.DrawRectangle(bp, 0, 0, w - 1, h - 1);
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
        var miListen = new ToolStripMenuItem("Auto-listen for my phone (WebRTC)") { CheckOnClick = true, Checked = autoListen };
        miListen.Click += (s, e) => { autoListen = miListen.Checked; SaveSettings(); if (autoListen && !running && !reconnecting) StartAutoListen(); };
        var stop = new ToolStripMenuItem("Stop streaming");
        stop.Click += (s, e) => { if (running || reconnecting) { manualStop = true; StopReceiver(); } };
        var exit = new ToolStripMenuItem("Exit"); exit.Click += (s, e) => Close();
        menu.Items.Add(open); menu.Items.Add(new ToolStripSeparator());
        menu.Items.Add(miAuto); menu.Items.Add(miTray); menu.Items.Add(miListen); menu.Items.Add(new ToolStripSeparator());
        menu.Items.Add(stop); menu.Items.Add(exit);
        tray = new NotifyIcon { Icon = SystemIcons.Application, Text = "PhoneCam", Visible = true, ContextMenuStrip = menu };
        tray.DoubleClick += (s, e) => ShowFromTray();
    }

    void ShowFromTray() { Show(); WindowState = FormWindowState.Normal; Activate(); }

    /// <summary>Auto-listen: come up as the WebRTC listener so the phone can one-tap reconnect with
    /// no PC interaction. The receiver keeps re-listening internally, so the PC stays ready between streams.</summary>
    void StartAutoListen()
    {
        if (receiverExe == null || running || reconnecting) return;
        StartReceiver();
    }

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
            ? "In your call app: “PhoneCam Camera” (camera) + “CABLE Output” (mic)."
            : "Pick “PhoneCam Camera” as the webcam in Zoom / Teams / OBS.";
    }

    // Options only take effect at Start, so lock them while streaming — otherwise ticking
    // "Use microphone" mid-stream silently does nothing and never prompts for VB-CABLE.
    void SetInputsEnabled(bool on)
    {
        cbMic.Enabled = on;
        cbBoost.Enabled = on && cbMic.Checked;
        cbEq.Enabled = on && cbMic.Checked;
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
        if (running || reconnecting) { manualStop = true; StopReceiver(); }   // deliberate stop — no relaunch
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
        manualStop = false; reconnecting = false; wentLive = false; reconnectAttempts = 0; webrtcMode = false;
        Log("Start: WebRTC (mic=" + useMic + ")");
        StartWebrtcPairing();
    }


    /// <summary>Shared receiver.exe launch: start the process, wire its stderr (mic meter + live/drop
    /// detection), and flip the UI into the running state. Callers build the arg list and set the
    /// session fields (lastUrl / controlHost / webrtcMode) beforehand.</summary>
    void RunReceiverProcess(List<string> a, bool useMic)
    {
        string args = BuildArgs(a);
        Log("launching receiver: " + Redact(args));
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
                    micLevel = lv; streamDropped = false;   // audio flowing → the stream is live
                    try { if (micMeter.IsHandleCreated) micMeter.BeginInvoke((Action)(() => micMeter.Invalidate())); } catch { }
                }
                return;
            }
            Log("[recv] " + Redact(d));   // mask the password / passphrase if FFmpeg echoes the URL
            if (d.IndexOf("[video]", StringComparison.OrdinalIgnoreCase) >= 0) { videoSeen = true; streamDropped = false; }
            if (d.IndexOf("[audio] rendering", StringComparison.OrdinalIgnoreCase) >= 0) { audioSeen = true; streamDropped = false; }
            // The phone leaving ends the session; the receiver keeps listening for it to come back.
            if (d.IndexOf("session ended", StringComparison.OrdinalIgnoreCase) >= 0) streamDropped = true;
        };
        try { recv.Start(); recv.BeginErrorReadLine(); }
        catch (Exception ex) { Log("receiver start FAILED: " + ex.Message); MessageBox.Show("Failed to start receiver: " + ex.Message, "PhoneCam"); StopReceiver(); return; }

        running = true; embedded = IntPtr.Zero;
        if (!webrtcMode) previewHint.Visible = true;   // WebRTC keeps the QR up until the phone connects
        tip.Text = TipText(useMic);
        SetInputsEnabled(false);
        btnStart.Text = "Stop"; btnStart.BackColor = Color.FromArgb(70, 74, 82);
        timer.Start();
    }

    /// <summary>WebRTC mode: run receiver.exe as a PCAM3
    /// signaling *offerer* and show a QR the phone answers. The phone captures mic → Opus → DTLS-SRTP
    /// straight into the CABLE virtual mic — lower latency + authenticated encryption, no video.</summary>
    void StartWebrtcPairing()
    {
        // Prefer a USB-tethering link when the phone is sharing USB (the PC gets a 192.168.42.x
        // address): media then rides the cable — no Wi-Fi jitter/congestion. Falls back to Wi-Fi.
        string usb = UsbTetherIPv4();
        string ip = usb ?? LocalIPv4();
        if (ip == null) { MessageBox.Show("Couldn't determine this PC's address. Connect to the same Wi-Fi as the phone (or enable USB tethering on the phone), then try again.", "PhoneCam"); return; }
        iceBindIp = usb ?? "";   // when tethering, bind ICE to the USB adapter so media can't slip onto Wi-Fi
        webrtcMode = true;
        // Reuse the stable persisted secret so a phone that saved us can reconnect without re-scanning.
        if (pairSecret.Length < 10) { pairSecret = Guid.NewGuid().ToString("N").Substring(0, 24); SaveSettings(); }
        webrtcSecret = pairSecret;
        string payload = "PCAM3:" + ip + ":" + WebrtcSigPort + ":" + webrtcSecret;
        Log("WebRTC: signaling (offerer) on tcp/" + WebrtcSigPort + (usb != null ? " over USB tethering (" + ip + ")" : "") + " — waiting for the phone to scan");
        try { qrBox.Image = MakeQr(payload); }
        catch (Exception ex) { MessageBox.Show("Couldn't render the QR: " + ex.Message, "PhoneCam"); webrtcMode = false; return; }
        qrLabel.Text = usb != null
            ? "Scan this with the PhoneCam app\n(🔌 USB — lowest latency)"
            : "Scan this with the PhoneCam app\n(⚡ low-latency mic)";
        qrLabel.Visible = true; qrBox.Visible = true;
        pairWaitSince = DateTime.Now; vpnHint.Visible = false;
        SetStatus(Amber, usb != null ? "USB tethering detected — scan the QR with the PhoneCam app…" : "Scan the QR (low-latency mic) with the PhoneCam app…");
        StartWebrtcReceiver();
    }

    /// <summary>Launch (or relaunch, for auto-reconnect) receiver.exe in --webrtc mode.</summary>
    void StartWebrtcReceiver()
    {
        lastUrl = "webrtc"; reconnecting = false;   // sentinel so OnTick relaunches (not stop)
        videoSeen = false; audioSeen = false; micLevel = 0f;
        // Offer video too (+ embed the preview): if the phone streams Cam+Mic it comes through the
        // softcam virtual camera; if it's mic-only the video track just stays idle. One low-latency mode.
        var a = new List<string> {
            "--webrtc", "--webrtc-video", "--preview",
            "--sig-port", WebrtcSigPort.ToString(), "--sig-secret", webrtcSecret,
            "--audio-device", "CABLE Input"
        };
        if (iceBindIp.Length > 0) { a.Add("--ice-bind"); a.Add(iceBindIp); }   // force ICE onto USB tethering
        int bi = cbBoost.SelectedIndex; if (bi < 0 || bi >= BoostDb.Length) bi = 2;
        if (BoostDb[bi] != 0) { a.Add("--mic-gain"); a.Add(BoostDb[bi].ToString()); }
        string eq = SelectedEq();
        if (eq.Length > 0) { a.Add("--eq"); a.Add(eq); }
        RunReceiverProcess(a, true);
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

    /// <summary>
    /// This PC's address on the phone's USB-tethering link, or null if the phone isn't tethering.
    /// Android hands the PC a 192.168.42.x address (phone gateway = .129) over RNDIS/NCM, so that
    /// subnet is a reliable, adapter-name-independent marker of the USB path.
    /// </summary>
    static string UsbTetherIPv4()
    {
        try
        {
            foreach (var ni in NetworkInterface.GetAllNetworkInterfaces())
            {
                if (ni.OperationalStatus != OperationalStatus.Up) continue;
                if (ni.NetworkInterfaceType == NetworkInterfaceType.Loopback) continue;
                foreach (var ua in ni.GetIPProperties().UnicastAddresses)
                    if (ua.Address.AddressFamily == AddressFamily.InterNetwork &&
                        ua.Address.ToString().StartsWith("192.168.42."))
                        return ua.Address.ToString();
            }
        }
        catch { }
        return null;
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
                    ? "Lost the phone — stopped. Press Start to listen again."
                    : "Couldn't reach the phone. Open PhoneCam on it and tap Reconnect, then try again.";
                StopReceiver();
                SetStatus(Amber, msg);
                return;
            }
            reconnectAttempts++;
            SetStatus(Amber, "Reconnecting… (" + reconnectAttempts + ")");
            StartWebrtcReceiver();
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
        // Phone connected: drop the pairing QR.
        if ((hasVideo || audioSeen) && qrBox.Visible)
        { qrLabel.Visible = false; qrBox.Visible = false; if (qrBox.Image != null) { var i = qrBox.Image; qrBox.Image = null; i.Dispose(); } }
        // WebRTC connection stalling? Surface the VPN "allow LAN" hint (the usual culprit).
        bool webRtcStalled = webrtcMode && !hasVideo && !audioSeen && (DateTime.Now - pairWaitSince).TotalSeconds > 12;
        if (vpnHint.Visible != webRtcStalled) vpnHint.Visible = webRtcStalled;
        // Meter is hidden while dropped so a frozen last-value bar can't look "live". Empty it too.
        bool micActive = running && audioSeen && !streamDropped;
        if (micMeter.Visible != micActive) { micMeter.Visible = micActive; micLabel.Visible = micActive; }
        if (streamDropped && micLevel != 0f) { micLevel = 0f; }
        // The phone stopping mid-session leaves a frozen preview, so this must win over the "Live" checks.
        if (streamDropped)
            // In WebRTC mode the receiver keeps listening, so a drop means it is ready for the phone.
            SetStatus(Amber, webrtcMode ? "Phone disconnected — ready to reconnect (tap “Reconnect” on the phone)."
                                     : "Phone stopped — press Stop, or restart it on the phone.");
        else if (hasVideo) SetStatus(Green, "Live — camera ready ✓");   // detail (“pick PhoneCam Camera”) is in the tip below
        else if (audioSeen)
        {
            // Mic-only: there's no video window, so the video-based detection never fires. Audio is up.
            SetStatus(Green, "Live — microphone ready ✓");
            if (embedded == IntPtr.Zero) { previewHint.Text = "Microphone only — no video."; previewHint.Visible = true; }
        }
        else if (webrtcMode)
            SetStatus(Amber, "Waiting for the phone to connect…");
    }

    /// <summary>Mask the session secret before anything reaches the log.</summary>
    string Redact(string s)
    {
        if (webrtcSecret.Length > 0) s = s.Replace(webrtcSecret, "***");
        return s;
    }

    void StopReceiver()
    {
        if (running) Log("stopped");
        timer.Stop();
        reconnecting = false;
        webrtcMode = false;
        embedded = IntPtr.Zero;
        micLevel = 0f;
        if (micMeter != null) { micMeter.Visible = false; micLabel.Visible = false; }
        try { if (recv != null && !recv.HasExited) recv.Kill(); } catch { }
        recv = null;
        running = false;
        if (IsHandleCreated)
        {
            if (qrBox != null) { qrLabel.Visible = false; qrBox.Visible = false; if (qrBox.Image != null) { var i = qrBox.Image; qrBox.Image = null; i.Dispose(); } }
            if (vpnHint != null) vpnHint.Visible = false;
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
                    case "mic": cbMic.Checked = kv[1] == "1"; break;
                    case "srtpass": pairSecret = kv[1]; break;   // migrate the secret from pre-0.5 settings
                    case "pairsecret": pairSecret = kv[1]; break;
                    case "autolisten": autoListen = kv[1] == "1"; break;
                    case "boost": { int bi; if (int.TryParse(kv[1], out bi) && bi >= 0 && bi < BoostDb.Length) cbBoost.SelectedIndex = bi; } break;
                    case "eqcustom": customEq = kv[1]; break;
                    case "eq": { int ei; if (int.TryParse(kv[1], out ei) && ei >= 0 && ei < cbEq.Items.Count) { suppressEqDialog = true; cbEq.SelectedIndex = ei; suppressEqDialog = false; prevEqIndex = ei; } } break;
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
                "mic=" + (cbMic.Checked ? "1" : "0"),
                "pairsecret=" + pairSecret,
                "boost=" + cbBoost.SelectedIndex,
                "eqcustom=" + customEq,
                "eq=" + cbEq.SelectedIndex,
                "tray=" + (minimizeToTray ? "1" : "0"),
                "autolisten=" + (autoListen ? "1" : "0"),
            };
            var wl = (WindowState == FormWindowState.Normal ? Location : RestoreBounds.Location);
            lines.Add("winpos=" + wl.X + "," + wl.Y);
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
