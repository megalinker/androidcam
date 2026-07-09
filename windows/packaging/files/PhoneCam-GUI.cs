// PhoneCam GUI — a small windowed launcher (no console) so using the phone as a
// webcam is point-and-click. Compiled to PhoneCam.exe with the .NET Framework
// csc.exe (present on every Win10/11); see assemble.ps1. It just drives the
// bundled receiver.exe (and adb for the USB tunnel) — all the real work lives there.
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Drawing;
using System.IO;
using System.Text;
using System.Text.RegularExpressions;
using System.Windows.Forms;

public class PhoneCamGui : Form
{
    RadioButton rbUsb, rbWifi;
    TextBox tbIp;
    CheckBox cbPreview, cbMic, cbFlipH, cbFlipV;
    Button btnStart;
    Label lblDot, lblStatus;
    Timer timer;
    Process recv;
    readonly object logLock = new object();
    readonly List<string> logLines = new List<string>();
    string receiverExe, adbExe;
    const int LocalPort = 18554, PhonePort = 8554;
    bool usbForwarded = false, running = false;

    public PhoneCamGui()
    {
        string b = AppDomain.CurrentDomain.BaseDirectory;
        string lad = Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData);
        receiverExe = FindFirst(new[] {
            Path.Combine(b, "bin", "receiver.exe"),
            Path.Combine(b, "receiver.exe"),
            Path.Combine(b, "..", "build", "Release", "receiver.exe") });
        adbExe = FindFirst(new[] {
            Path.Combine(b, "bin", "adb", "adb.exe"),
            Path.Combine(lad, "Android", "Sdk", "platform-tools", "adb.exe") });
        BuildUi();
        timer = new Timer { Interval = 800 };
        timer.Tick += OnTick;
        FormClosing += (s, e) => StopReceiver();
    }

    static string FindFirst(string[] paths)
    {
        foreach (var p in paths) { try { var f = Path.GetFullPath(p); if (File.Exists(f)) return f; } catch { } }
        return null;
    }

    void BuildUi()
    {
        Text = "PhoneCam";
        FormBorderStyle = FormBorderStyle.FixedSingle;
        MaximizeBox = false;
        ClientSize = new Size(384, 336);
        StartPosition = FormStartPosition.CenterScreen;
        Font = new Font("Segoe UI", 9f);

        Controls.Add(new Label { Text = "PhoneCam", Font = new Font("Segoe UI", 15f, FontStyle.Bold), Location = new Point(16, 12), AutoSize = true });
        Controls.Add(new Label { Text = "Connect the phone by:", Location = new Point(16, 52), AutoSize = true });

        rbUsb = new RadioButton { Text = "USB cable", Location = new Point(26, 74), AutoSize = true, Checked = true };
        rbWifi = new RadioButton { Text = "Wi-Fi  —  address:", Location = new Point(26, 98), AutoSize = true };
        tbIp = new TextBox { Location = new Point(160, 96), Width = 150, Enabled = false };
        rbWifi.CheckedChanged += (s, e) => tbIp.Enabled = rbWifi.Checked;
        Controls.Add(rbUsb); Controls.Add(rbWifi); Controls.Add(tbIp);

        cbPreview = new CheckBox { Text = "Show preview window", Location = new Point(26, 134), AutoSize = true };
        cbMic = new CheckBox { Text = "Use phone microphone (needs VB-CABLE)", Location = new Point(26, 156), AutoSize = true };
        cbFlipH = new CheckBox { Text = "Flip left/right", Location = new Point(26, 178), AutoSize = true };
        cbFlipV = new CheckBox { Text = "Flip up/down", Location = new Point(170, 178), AutoSize = true };
        Controls.Add(cbPreview); Controls.Add(cbMic); Controls.Add(cbFlipH); Controls.Add(cbFlipV);

        btnStart = new Button { Text = "Start", Location = new Point(26, 214), Size = new Size(104, 34) };
        btnStart.Click += OnStartStop;
        Controls.Add(btnStart);

        lblDot = new Label { Text = "●", ForeColor = Color.Gray, Location = new Point(146, 220), AutoSize = true, Font = new Font("Segoe UI", 12f) };
        lblStatus = new Label { Text = "Idle", Location = new Point(168, 222), AutoSize = true };
        Controls.Add(lblDot); Controls.Add(lblStatus);

        Controls.Add(new Label { Text = "Then choose “PhoneCam Camera” as the webcam in Zoom / Teams / OBS.", Location = new Point(16, 268), Size = new Size(356, 56), ForeColor = Color.DimGray });

        if (receiverExe == null) { btnStart.Enabled = false; SetStatus(Color.Red, "receiver.exe not found"); }
    }

    void SetStatus(Color c, string s) { lblDot.ForeColor = c; lblStatus.Text = s; }

    string Adb(string args)
    {
        if (adbExe == null) return "";
        try {
            var psi = new ProcessStartInfo(adbExe, args) { UseShellExecute = false, CreateNoWindow = true, RedirectStandardOutput = true, RedirectStandardError = true };
            var p = Process.Start(psi); string o = p.StandardOutput.ReadToEnd(); p.WaitForExit(4000); return o;
        } catch { return ""; }
    }

    void OnStartStop(object sender, EventArgs e) { if (running) StopReceiver(); else StartReceiver(); }

    void StartReceiver()
    {
        string url;
        if (rbUsb.Checked)
        {
            if (adbExe == null) { MessageBox.Show("adb not found (needed for USB). Use Wi-Fi, or install Android platform-tools.", "PhoneCam"); return; }
            if (!Regex.IsMatch(Adb("devices"), @"\bdevice\b"))
            { MessageBox.Show("No phone detected over USB.\n\nEnable Developer Options → USB debugging, plug in, and tap “Allow”. Or use Wi-Fi.", "PhoneCam"); return; }
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

        var a = new List<string> { url };
        if (cbPreview.Checked) a.Add("--preview");
        if (cbFlipH.Checked) a.Add("--flip-h");
        if (cbFlipV.Checked) a.Add("--flip-v");
        if (cbMic.Checked) { a.Add("--audio-device"); a.Add("CABLE Input"); } else a.Add("--no-audio");

        var psi = new ProcessStartInfo(receiverExe, BuildArgs(a)) { UseShellExecute = false, CreateNoWindow = true, RedirectStandardError = true, RedirectStandardOutput = true };
        lock (logLock) logLines.Clear();
        recv = new Process { StartInfo = psi };
        recv.ErrorDataReceived += (s, ev) => { if (ev.Data != null) lock (logLock) logLines.Add(ev.Data); };
        try { recv.Start(); recv.BeginErrorReadLine(); }
        catch (Exception ex) { MessageBox.Show("Failed to start receiver: " + ex.Message, "PhoneCam"); return; }

        running = true;
        btnStart.Text = "Stop";
        SetStatus(Color.Goldenrod, rbUsb.Checked ? "Open PhoneCam on the phone, press Start…" : "Connecting…");
        timer.Start();
    }

    static string BuildArgs(List<string> a)
    {
        var sb = new StringBuilder();
        foreach (var s in a) { if (sb.Length > 0) sb.Append(' '); if (s.IndexOf(' ') >= 0) sb.Append('"').Append(s).Append('"'); else sb.Append(s); }
        return sb.ToString();
    }

    void OnTick(object sender, EventArgs e)
    {
        if (recv == null) return;
        if (recv.HasExited) { StopReceiver(); SetStatus(Color.Gray, "Stopped"); return; }
        string blob; lock (logLock) blob = string.Join("\n", logLines);
        if (blob.Contains("[video]")) SetStatus(Color.ForestGreen, "Live — select “PhoneCam Camera” in your app");
        else if (blob.Contains("reconnecting") || blob.Contains("unreachable") || blob.Contains("failed"))
            SetStatus(Color.Goldenrod, rbUsb.Checked ? "Waiting for the phone (press Start in the app)…"
                                                     : "Can't reach the phone — same Wi-Fi? VPN off?");
    }

    void StopReceiver()
    {
        timer.Stop();
        try { if (recv != null && !recv.HasExited) recv.Kill(); } catch { }
        recv = null;
        if (usbForwarded) { Adb("forward --remove tcp:" + LocalPort); usbForwarded = false; }
        running = false;
        if (IsHandleCreated) { btnStart.Text = "Start"; if (lblStatus.Text != "Stopped") SetStatus(Color.Gray, "Idle"); }
    }

    [STAThread]
    static void Main()
    {
        Application.EnableVisualStyles();
        Application.SetCompatibleTextRenderingDefault(false);
        Application.Run(new PhoneCamGui());
    }
}
