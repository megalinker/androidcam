package com.phonecam

import android.Manifest
import android.content.ClipData
import android.content.ClipboardManager
import android.content.Context
import android.content.Intent
import android.content.pm.PackageManager
import android.os.Build
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.view.View
import android.widget.TextView
import android.widget.Toast
import androidx.appcompat.app.AppCompatActivity
import androidx.core.app.ActivityCompat
import androidx.core.content.ContextCompat
import com.google.android.material.button.MaterialButton
import com.google.android.material.button.MaterialButtonToggleGroup
import com.google.android.material.card.MaterialCardView
import com.google.android.material.textfield.MaterialAutoCompleteTextView
import com.journeyapps.barcodescanner.ScanContract
import com.journeyapps.barcodescanner.ScanOptions

/**
 * Single-screen UI: pick a mode (Cam + Mic / Camera / Mic) and quality, then scan the QR the
 * PhoneCam app shows on your PC (or one-tap reconnect to the last PC). The phone connects out to
 * the PC over WebRTC (DTLS-SRTP, LAN-direct); the card shows the link status. The heavy lifting is
 * in [StreamService]; this activity only drives it and polls its state once a second.
 */
class MainActivity : AppCompatActivity() {

    private lateinit var modeGroup: MaterialButtonToggleGroup
    private lateinit var qualityInput: MaterialAutoCompleteTextView
    private lateinit var stopBtn: MaterialButton
    private lateinit var scanBtn: MaterialButton
    private lateinit var reconnectBtn: MaterialButton
    private lateinit var statusCard: MaterialCardView
    private lateinit var idleHint: View
    private lateinit var urlText: TextView
    private lateinit var pcStatus: TextView

    private val prefs by lazy { getSharedPreferences("phonecam", MODE_PRIVATE) }
    private val ui = Handler(Looper.getMainLooper())
    private val qualities = StreamService.Quality.values()

    // The PC target from the last scanned QR, held across the permission prompt.
    private var pendingWebrtc: WebrtcTarget? = null
    // A USB request pushed by the PC over adb (port, mode, quality), held across the permission prompt.
    private var pendingUsb: Triple<Int, String?, String?>? = null
    private val scanLauncher = registerForActivityResult(ScanContract()) { result ->
        result.contents?.let { onScanned(it) }
    }

    private val poll = object : Runnable {
        override fun run() { refresh(); ui.postDelayed(this, 1000) }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        // The screen is free to sleep — StreamService holds a partial wake lock so the stream keeps
        // running with the screen off (a lit screen was a big source of the phone getting hot).
        setContentView(R.layout.activity_main)

        modeGroup = findViewById(R.id.modeGroup)
        qualityInput = findViewById(R.id.qualityInput)
        stopBtn = findViewById(R.id.startBtn)
        scanBtn = findViewById(R.id.scanBtn)
        reconnectBtn = findViewById(R.id.reconnectBtn)
        statusCard = findViewById(R.id.statusCard)
        idleHint = findViewById(R.id.idleHint)
        urlText = findViewById(R.id.urlText)
        pcStatus = findViewById(R.id.pcStatus)

        findViewById<com.google.android.material.appbar.MaterialToolbar>(R.id.toolbar).subtitle =
            "v" + BuildConfig.VERSION_NAME

        qualityInput.setSimpleItems(qualities.map { it.label }.toTypedArray())
        restoreSelections()

        stopBtn.setOnClickListener { if (StreamService.isRunning) stopStreaming() }
        scanBtn.setOnClickListener { launchScan() }
        reconnectBtn.setOnClickListener { reconnectWebrtc() }
        urlText.setOnClickListener { copyUrl() }
        findViewById<MaterialButton>(R.id.switchCamBtn).setOnClickListener { switchCamera() }

        handleUsbIntent(intent)   // the PC may have launched us over adb to start USB streaming
    }

    override fun onNewIntent(intent: Intent?) {
        super.onNewIntent(intent)
        setIntent(intent)
        handleUsbIntent(intent)
    }

    /**
     * USB auto-connect: the PC (via bundled adb) launches us with ACTION_USB + the loopback port so we
     * start streaming over the cable with no QR scan. adb has usually pre-granted camera/mic; if not,
     * we prompt and resume from onRequestPermissionsResult.
     */
    private fun handleUsbIntent(intent: Intent?) {
        if (intent?.action != ACTION_USB) return
        pendingUsb = Triple(
            intent.getIntExtra(StreamService.EXTRA_USB_PORT, StreamService.DEFAULT_USB_PORT),
            intent.getStringExtra(KEY_MODE),
            intent.getStringExtra(KEY_QUALITY))
        if (ensurePermissions()) beginUsb()
    }

    private fun beginUsb() {
        val req = pendingUsb ?: return
        pendingUsb = null
        val (port, modeName, qualityName) = req
        // Honor a mode/quality the PC asked for (else keep the current selection).
        modeName?.let { runCatching { StreamService.Mode.valueOf(it) }.getOrNull() }?.let { m ->
            modeGroup.check(when (m) {
                StreamService.Mode.CAMERA_ONLY -> R.id.modeCamera
                StreamService.Mode.MIC_ONLY -> R.id.modeMic
                else -> R.id.modeBoth
            })
        }
        qualityName?.let { runCatching { StreamService.Quality.valueOf(it) }.getOrNull() }
            ?.let { qualityInput.setText(it.label, false) }
        prefs.edit().putString(KEY_MODE, selectedMode().name).putString(KEY_QUALITY, selectedQuality().name).apply()
        val svc = Intent(this, StreamService::class.java).apply {
            action = StreamService.ACTION_START
            putExtra(StreamService.EXTRA_TRANSPORT, "usb")
            putExtra(StreamService.EXTRA_USB_PORT, port)
            putExtra(StreamService.EXTRA_MODE, selectedMode().name)
            putExtra(StreamService.EXTRA_QUALITY, selectedQuality().name)
        }
        ContextCompat.startForegroundService(this, svc)
        Toast.makeText(this, "USB — streaming to the PC…", Toast.LENGTH_SHORT).show()
    }

    // --- pairing (scan the PC's QR) ---

    private fun launchScan() {
        scanLauncher.launch(
            ScanOptions()
                .setDesiredBarcodeFormats(ScanOptions.QR_CODE)
                .setPrompt("Scan the QR shown in the PhoneCam app on your PC")
                .setBeepEnabled(false)
                .setOrientationLocked(false)
        )
    }

    private fun onScanned(payload: String) {
        val wr = WebrtcTarget.parse(payload)
        if (wr == null) {
            Toast.makeText(this, "That isn't a valid PhoneCam code.", Toast.LENGTH_SHORT).show()
            return
        }
        pendingWebrtc = wr
        if (ensurePermissions()) beginWebrtc()   // else resumed from onRequestPermissionsResult
    }

    /** Answer the scanned PC's PCAM3 offer. The phone connects out; there is no announce-back. */
    private fun beginWebrtc() {
        val wr = pendingWebrtc ?: return
        pendingWebrtc = null
        saveWebrtcTarget(wr)   // remember for one-tap reconnect
        if (!StreamService.isRunning) startStreaming(wr)
        Toast.makeText(this, "Connecting to ${wr.host}…", Toast.LENGTH_SHORT).show()
    }

    /** One-tap reconnect to the last PC — no QR. */
    private fun reconnectWebrtc() {
        savedWebrtcTarget()?.let { pendingWebrtc = it; if (ensurePermissions()) beginWebrtc() }
    }

    private fun saveWebrtcTarget(wr: WebrtcTarget) {
        prefs.edit()
            .putString(KEY_SIG_HOST, wr.host).putInt(KEY_SIG_PORT, wr.port).putString(KEY_SIG_SECRET, wr.secret)
            .apply()
    }

    private fun savedWebrtcTarget(): WebrtcTarget? {
        val host = prefs.getString(KEY_SIG_HOST, null) ?: return null
        val secret = prefs.getString(KEY_SIG_SECRET, null) ?: return null
        val port = prefs.getInt(KEY_SIG_PORT, 0)
        if (host.isEmpty() || secret.isEmpty() || port !in 1..65535) return null
        return WebrtcTarget(host, port, secret)
    }

    private fun startStreaming(wr: WebrtcTarget) {
        // Honor the mode toggle: Cam+Mic streams H.264 video too, Mic-only stays audio-only.
        prefs.edit()
            .putString(KEY_MODE, selectedMode().name)
            .putString(KEY_QUALITY, selectedQuality().name)
            .apply()
        val intent = Intent(this, StreamService::class.java).apply {
            action = StreamService.ACTION_START
            putExtra(StreamService.EXTRA_MODE, selectedMode().name)
            putExtra(StreamService.EXTRA_QUALITY, selectedQuality().name)
            putExtra(StreamService.EXTRA_SIG_HOST, wr.host)
            putExtra(StreamService.EXTRA_SIG_PORT, wr.port)
            putExtra(StreamService.EXTRA_SIG_SECRET, wr.secret)
        }
        ContextCompat.startForegroundService(this, intent)
    }

    private fun copyUrl() {
        val url = StreamService.streamUrl ?: return
        (getSystemService(Context.CLIPBOARD_SERVICE) as ClipboardManager)
            .setPrimaryClip(ClipData.newPlainText("PhoneCam PC", url))
        Toast.makeText(this, "Copied", Toast.LENGTH_SHORT).show()
    }

    /** Restore the last-used mode + quality from prefs (defaults if none saved). */
    private fun restoreSelections() {
        val savedQuality = prefs.getString(KEY_QUALITY, null)
            ?.let { runCatching { StreamService.Quality.valueOf(it) }.getOrNull() }
            ?: StreamService.DEFAULT_QUALITY
        qualityInput.setText(savedQuality.label, false)

        val savedMode = prefs.getString(KEY_MODE, null)
            ?.let { runCatching { StreamService.Mode.valueOf(it) }.getOrNull() }
        modeGroup.check(
            when (savedMode) {
                StreamService.Mode.CAMERA_ONLY -> R.id.modeCamera
                StreamService.Mode.MIC_ONLY -> R.id.modeMic
                else -> R.id.modeBoth
            }
        )
    }

    private fun selectedMode(): StreamService.Mode = when (modeGroup.checkedButtonId) {
        R.id.modeCamera -> StreamService.Mode.CAMERA_ONLY
        R.id.modeMic -> StreamService.Mode.MIC_ONLY
        else -> StreamService.Mode.BOTH
    }

    private fun selectedQuality(): StreamService.Quality =
        qualities.firstOrNull { it.label == qualityInput.text.toString() } ?: StreamService.DEFAULT_QUALITY

    private fun stopStreaming() {
        startService(Intent(this, StreamService::class.java).apply { action = StreamService.ACTION_STOP })
    }

    private fun switchCamera() {
        if (!StreamService.isRunning) return
        startService(Intent(this, StreamService::class.java).apply {
            action = StreamService.ACTION_SWITCH_CAMERA
        })
    }

    /** Mirror the service's state into the UI once a second (stop can also come from the notification). */
    private fun refresh() {
        val running = StreamService.isRunning
        stopBtn.visibility = if (running) View.VISIBLE else View.GONE
        scanBtn.visibility = if (running) View.GONE else View.VISIBLE
        // One-tap reconnect: only useful when idle and we've paired with a PC before.
        reconnectBtn.visibility =
            if (!running && savedWebrtcTarget() != null) View.VISIBLE else View.GONE
        statusCard.visibility = if (running) View.VISIBLE else View.GONE
        idleHint.visibility = if (running) View.GONE else View.VISIBLE

        // Mode + quality are locked in while streaming (they only take effect at start).
        for (i in 0 until modeGroup.childCount) modeGroup.getChildAt(i).isEnabled = !running
        findViewById<View>(R.id.qualityLayout).isEnabled = !running
        qualityInput.isEnabled = !running

        if (!running) return

        urlText.text = StreamService.streamUrl ?: ""
        val connected = StreamService.clientConnected
        // After a PC has connected once and dropped, say so plainly (it's not a fresh "connecting") and
        // make clear the phone will stop itself — so the user needn't hunt for the Stop button.
        val dropped = !connected && StreamService.everConnected
        pcStatus.text = when {
            connected -> "✓ PC connected"
            dropped -> "PC disconnected — waiting to reconnect (auto-stops soon)…"
            else -> "Connecting to the PC…"
        }
        pcStatus.setTextColor(
            ContextCompat.getColor(
                this,
                when {
                    connected -> R.color.pc_green
                    dropped -> R.color.pc_amber
                    else -> R.color.pc_muted
                }
            )
        )
    }

    override fun onResume() { super.onResume(); ui.post(poll) }
    override fun onPause() { super.onPause(); ui.removeCallbacks(poll) }

    // --- runtime permissions ---

    private fun requiredPermissions(): Array<String> {
        val p = mutableListOf(Manifest.permission.CAMERA, Manifest.permission.RECORD_AUDIO)
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            p.add(Manifest.permission.POST_NOTIFICATIONS)
        }
        return p.toTypedArray()
    }

    private fun ensurePermissions(): Boolean {
        val missing = requiredPermissions().filter {
            ContextCompat.checkSelfPermission(this, it) != PackageManager.PERMISSION_GRANTED
        }
        if (missing.isEmpty()) return true
        ActivityCompat.requestPermissions(this, missing.toTypedArray(), REQ_PERMS)
        return false
    }

    override fun onRequestPermissionsResult(
        requestCode: Int, permissions: Array<out String>, grantResults: IntArray
    ) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults)
        if (requestCode != REQ_PERMS) return
        // CAMERA + RECORD_AUDIO are the hard requirements; POST_NOTIFICATIONS is best-effort.
        val camOk = isGranted(permissions, grantResults, Manifest.permission.CAMERA)
        val micOk = isGranted(permissions, grantResults, Manifest.permission.RECORD_AUDIO)
        if (camOk && micOk) {
            when {
                pendingWebrtc != null -> beginWebrtc()
                pendingUsb != null -> beginUsb()
            }
        } else {
            pendingWebrtc = null; pendingUsb = null
            Toast.makeText(this, "Camera and microphone permissions are required.", Toast.LENGTH_LONG).show()
        }
    }

    private fun isGranted(perms: Array<out String>, results: IntArray, name: String): Boolean {
        val i = perms.indexOf(name)
        return i >= 0 && results[i] == PackageManager.PERMISSION_GRANTED
    }

    companion object {
        // The PC launches us with this action over adb to start USB streaming (no QR scan).
        const val ACTION_USB = "com.phonecam.action.USB"
        private const val REQ_PERMS = 1
        private const val KEY_MODE = "mode"
        private const val KEY_QUALITY = "quality"
        private const val KEY_SIG_HOST = "sigHost"
        private const val KEY_SIG_PORT = "sigPort"
        private const val KEY_SIG_SECRET = "sigSecret"
    }
}
