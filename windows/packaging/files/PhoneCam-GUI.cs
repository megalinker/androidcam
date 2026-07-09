// PhoneCam GUI — a small windowed launcher (no console) with an embedded live
// camera preview. Compiled to PhoneCam.exe with the .NET Framework csc.exe (see
// assemble.ps1). It drives the bundled receiver.exe (+ adb for the USB tunnel)
// and hosts the receiver's preview window inside itself so you see the feed.
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Drawing;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;
using System.Text.RegularExpressions;
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
        int on = 1;                                   // dark title bar (DWMWA_USE_IMMERSIVE_DARK_MODE)
        if (DwmSetWindowAttribute(Handle, 20, ref on, 4) != 0) DwmSetWindowAttribute(Handle, 19, ref on, 4);
    }

    RadioButton rbUsb, rbWifi;
    TextBox tbIp;
    CheckBox cbMic, cbFlipH, cbFlipV;
    Button btnStart;
    Label lblStatus, lblDot;
    Panel preview;
    Label previewHint;
    Timer timer;
    Process recv;
    IntPtr embedded = IntPtr.Zero;
    readonly object logLock = new object();
    readonly List<string> logLines = new List<string>();
    string receiverExe, adbExe, settingsPath;
    const int LocalPort = 18554, PhonePort = 8554;
    bool usbForwarded = false, running = false;

    public PhoneCamGui()
    {
        string b = AppDomain.CurrentDomain.BaseDirectory;
        string lad = Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData);
        receiverExe = FindFirst(new[] { Path.Combine(b, "bin", "receiver.exe"), Path.Combine(b, "receiver.exe"), Path.Combine(b, "..", "build", "Release", "receiver.exe") });
        adbExe = FindFirst(new[] { Path.Combine(b, "bin", "adb", "adb.exe"), Path.Combine(lad, "Android", "Sdk", "platform-tools", "adb.exe") });
        settingsPath = Path.Combine(lad, "PhoneCam", "gui.txt");
        BuildUi();
        LoadSettings();
        timer = new Timer { Interval = 700 };
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
        Text = "PhoneCam";
        FormBorderStyle = FormBorderStyle.FixedSingle;
        MaximizeBox = false;
        ClientSize = new Size(744, 452);
        StartPosition = FormStartPosition.CenterScreen;
        BackColor = Bg; ForeColor = Fg;
        Font = new Font("Segoe UI", 9.5f);

        // Left control column
        var title = new Label { Text = "PhoneCam", Font = new Font("Segoe UI Semibold", 17f), ForeColor = Fg, Location = new Point(20, 16), AutoSize = true };
        var sub = new Label { Text = "Your phone as a webcam", ForeColor = Sub, Location = new Point(22, 50), AutoSize = true };
        Controls.Add(title); Controls.Add(sub);

        AddSection("Connect", 84);
        rbUsb = Radio("USB cable", 22, 108, true);
        rbWifi = Radio("Wi-Fi", 22, 134, false);
        tbIp = new TextBox { Location = new Point(96, 132), Width = 150, Enabled = false, BackColor = Card, ForeColor = Fg, BorderStyle = BorderStyle.FixedSingle };
        rbWifi.CheckedChanged += (s, e) => tbIp.Enabled = rbWifi.Checked;
        Controls.Add(rbUsb); Controls.Add(rbWifi); Controls.Add(tbIp);

        AddSection("Options", 172);
        cbMic = Check("Use phone microphone", 22, 196);
        cbFlipH = Check("Flip left / right", 22, 222);
        cbFlipV = Check("Flip up / down", 22, 248);
        Controls.Add(cbMic); Controls.Add(cbFlipH); Controls.Add(cbFlipV);

        btnStart = new Button { Text = "Start", Location = new Point(22, 292), Size = new Size(224, 40), FlatStyle = FlatStyle.Flat, BackColor = Accent, ForeColor = Color.White, Font = new Font("Segoe UI Semibold", 11f) };
        btnStart.FlatAppearance.BorderSize = 0;
        btnStart.Click += OnStartStop;
        Controls.Add(btnStart);

        lblDot = new Label { Text = "●", ForeColor = Sub, Location = new Point(24, 346), AutoSize = true, Font = new Font("Segoe UI", 11f) };
        lblStatus = new Label { Text = "Idle", ForeColor = Sub, Location = new Point(44, 348), AutoSize = true, MaximumSize = new Size(220, 0) };
        Controls.Add(lblDot); Controls.Add(lblStatus);

        var tip = new Label { Text = "Then pick “PhoneCam Camera” as the\nwebcam in Zoom / Teams / OBS.", ForeColor = Sub, Location = new Point(22, 402), AutoSize = true };
        Controls.Add(tip);

        // Right: embedded live preview
        preview = new Panel { Location = new Point(260, 84), Size = new Size(468, 348), BackColor = Color.FromArgb(12, 13, 15), BorderStyle = BorderStyle.None };
        preview.Paint += (s, e) => { using (var pen = new Pen(Line)) e.Graphics.DrawRectangle(pen, 0, 0, preview.Width - 1, preview.Height - 1); };
        previewHint = new Label { Text = "Live preview appears here once you press Start.", ForeColor = Sub, BackColor = Color.FromArgb(12, 13, 15), AutoSize = true, Location = new Point(16, 16) };
        preview.Controls.Add(previewHint);
        Controls.Add(preview);

        if (receiverExe == null) { btnStart.Enabled = false; SetStatus(Color.IndianRed, "receiver.exe not found"); }
    }

    void AddSection(string t, int y)
    {
        Controls.Add(new Label { Text = t.ToUpperInvariant(), ForeColor = Sub, Font = new Font("Segoe UI", 7.5f, FontStyle.Bold), Location = new Point(24, y), AutoSize = true });
        Controls.Add(new Panel { BackColor = Line, Location = new Point(24, y + 17), Size = new Size(212, 1) });
    }
    RadioButton Radio(string t, int x, int y, bool on) { return new RadioButton { Text = t, ForeColor = Fg, Location = new Point(x, y), AutoSize = true, Checked = on, FlatStyle = FlatStyle.Standard }; }
    CheckBox Check(string t, int x, int y) { return new CheckBox { Text = t, ForeColor = Fg, Location = new Point(x, y), AutoSize = true }; }

    void SetStatus(Color c, string s) { lblDot.ForeColor = c; lblStatus.ForeColor = c == Sub ? Sub : Fg; lblStatus.Text = s; }

    string Adb(string args)
    {
        if (adbExe == null) return "";
        try { var psi = new ProcessStartInfo(adbExe, args) { UseShellExecute = false, CreateNoWindow = true, RedirectStandardOutput = true, RedirectStandardError = true };
              var p = Process.Start(psi); string o = p.StandardOutput.ReadToEnd(); p.WaitForExit(4000); return o; } catch { return ""; }
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

        var a = new List<string> { url, "--preview" };   // --preview so we can embed the feed
        if (cbFlipH.Checked) a.Add("--flip-h");
        if (cbFlipV.Checked) a.Add("--flip-v");
        if (useMic) { a.Add("--audio-device"); a.Add("CABLE Input"); } else a.Add("--no-audio");

        var psi = new ProcessStartInfo(receiverExe, BuildArgs(a)) { UseShellExecute = false, CreateNoWindow = true, RedirectStandardError = true, RedirectStandardOutput = true };
        lock (logLock) logLines.Clear();
        recv = new Process { StartInfo = psi };
        recv.ErrorDataReceived += (s, ev) => { if (ev.Data != null) lock (logLock) logLines.Add(ev.Data); };
        try { recv.Start(); recv.BeginErrorReadLine(); }
        catch (Exception ex) { MessageBox.Show("Failed to start receiver: " + ex.Message, "PhoneCam"); return; }

        running = true; embedded = IntPtr.Zero;
        previewHint.Text = rbUsb.Checked ? "Open PhoneCam on the phone and press Start…" : "Connecting…";
        previewHint.Visible = true;
        btnStart.Text = "Stop"; btnStart.BackColor = Color.FromArgb(70, 74, 82);
        SetStatus(Amber, rbUsb.Checked ? "Waiting for the phone…" : "Connecting…");
        timer.Start();
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

        string blob; lock (logLock) blob = string.Join("\n", logLines);
        if (blob.Contains("[video]") || embedded != IntPtr.Zero) SetStatus(Green, "Live — select “PhoneCam Camera” in your app");
        else if (blob.Contains("reconnecting") || blob.Contains("unreachable") || blob.Contains("failed"))
            SetStatus(Amber, rbUsb.Checked ? "Waiting for the phone (press Start in the app)…" : "Can't reach the phone — same Wi-Fi? VPN off?");
    }

    void StopReceiver()
    {
        timer.Stop();
        embedded = IntPtr.Zero;
        try { if (recv != null && !recv.HasExited) recv.Kill(); } catch { }
        recv = null;
        if (usbForwarded) { Adb("forward --remove tcp:" + LocalPort); usbForwarded = false; }
        running = false;
        if (IsHandleCreated)
        {
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
                    case "conn": if (kv[1] == "wifi") { rbWifi.Checked = true; } break;
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
                "conn=" + (rbWifi.Checked ? "wifi" : "usb"),
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
