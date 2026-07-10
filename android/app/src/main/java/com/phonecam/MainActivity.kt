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
import android.os.SystemClock
import android.view.View
import android.view.WindowManager
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
 * Single-screen UI: pick a mode (Cam + Mic / Camera / Mic) and quality, press Start, and the
 * card shows the pull address (tap to copy) plus a live "PC connected" indicator. The heavy
 * lifting is in [StreamService]; this activity only drives it and polls its state once a second.
 */
class MainActivity : AppCompatActivity() {

    private lateinit var modeGroup: MaterialButtonToggleGroup
    private lateinit var qualityInput: MaterialAutoCompleteTextView
    private lateinit var startBtn: MaterialButton
    private lateinit var statusCard: MaterialCardView
    private lateinit var idleHint: View
    private lateinit var urlText: TextView
    private lateinit var pcStatus: TextView

    private val prefs by lazy { getSharedPreferences("phonecam", MODE_PRIVATE) }
    private val ui = Handler(Looper.getMainLooper())
    private val qualities = StreamService.Quality.values()

    // Wi-Fi pairing: the PC target from the last scanned QR, held across the permission prompt.
    private var pendingTarget: PcTarget? = null
    // A note shown in the status card while streaming but not yet connected (e.g. announce failed).
    private var pairingNote: String? = null
    private val scanLauncher = registerForActivityResult(ScanContract()) { result ->
        result.contents?.let { onScanned(it) }
    }

    private val poll = object : Runnable {
        override fun run() { refresh(); ui.postDelayed(this, 1000) }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON) // don't sleep while streaming
        setContentView(R.layout.activity_main)

        modeGroup = findViewById(R.id.modeGroup)
        qualityInput = findViewById(R.id.qualityInput)
        startBtn = findViewById(R.id.startBtn)
        statusCard = findViewById(R.id.statusCard)
        idleHint = findViewById(R.id.idleHint)
        urlText = findViewById(R.id.urlText)
        pcStatus = findViewById(R.id.pcStatus)

        qualityInput.setSimpleItems(qualities.map { it.label }.toTypedArray())
        restoreSelections()

        startBtn.setOnClickListener {
            if (StreamService.isRunning) stopStreaming()
            else if (ensurePermissions()) startStreaming()
        }
        findViewById<MaterialButton>(R.id.scanBtn).setOnClickListener { launchScan() }
        urlText.setOnClickListener { copyUrl() }
        findViewById<MaterialButton>(R.id.switchCamBtn).setOnClickListener { switchCamera() }
    }

    // --- Wi-Fi pairing (scan the PC's QR) ---

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
        val target = PcTarget.parse(payload)
        if (target == null) {
            Toast.makeText(this, "That isn't a PhoneCam PC code.", Toast.LENGTH_SHORT).show()
            return
        }
        pendingTarget = target
        if (ensurePermissions()) beginPairing()   // else resumed from onRequestPermissionsResult
    }

    /** Start streaming (if needed), then announce our pull URL to the scanned PC on a worker thread. */
    private fun beginPairing() {
        val target = pendingTarget ?: return
        pendingTarget = null
        pairingNote = null
        if (!StreamService.isRunning) startStreaming()
        Toast.makeText(this, "Pairing with the PC…", Toast.LENGTH_SHORT).show()
        val mode = selectedMode().name
        Thread {
            val url = waitForStreamUrl(6000)
            if (url == null) {
                ui.post { pairingNote = "Couldn't start the camera stream — try a lower Quality." }
                return@Thread
            }
            val ok = PcLink.announce(target, url, mode)
            ui.post {
                pairingNote = if (ok) null
                else "Reached out to the PC at ${target.host} but it didn't answer — is PhoneCam open on the PC, on the same Wi‑Fi, with its firewall allowing it?"
                Toast.makeText(
                    this,
                    if (ok) "Sent to PC — it should connect now." else "Couldn't reach the PC at ${target.host}.",
                    Toast.LENGTH_LONG
                ).show()
            }
        }.start()
    }

    private fun waitForStreamUrl(timeoutMs: Long): String? {
        val deadline = SystemClock.elapsedRealtime() + timeoutMs
        while (SystemClock.elapsedRealtime() < deadline) {
            StreamService.streamUrl?.let { return it }
            Thread.sleep(150)
        }
        return null
    }

    private fun copyUrl() {
        val url = StreamService.streamUrl ?: return
        (getSystemService(Context.CLIPBOARD_SERVICE) as ClipboardManager)
            .setPrimaryClip(ClipData.newPlainText("PhoneCam address", url))
        Toast.makeText(this, "Address copied", Toast.LENGTH_SHORT).show()
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

    private fun startStreaming() {
        prefs.edit()
            .putString(KEY_MODE, selectedMode().name)
            .putString(KEY_QUALITY, selectedQuality().name)
            .apply()
        val intent = Intent(this, StreamService::class.java).apply {
            action = StreamService.ACTION_START
            putExtra(StreamService.EXTRA_MODE, selectedMode().name)
            putExtra(StreamService.EXTRA_QUALITY, selectedQuality().name)
        }
        ContextCompat.startForegroundService(this, intent)
    }

    private fun stopStreaming() {
        startService(Intent(this, StreamService::class.java).apply { action = StreamService.ACTION_STOP })
    }

    private fun switchCamera() {
        if (!StreamService.isRunning) return
        startService(Intent(this, StreamService::class.java).apply {
            action = StreamService.ACTION_SWITCH_CAMERA
        })
    }

    /** Mirror the service's state into the UI once a second (start/stop can come from the notification). */
    private fun refresh() {
        val running = StreamService.isRunning
        startBtn.text = if (running) "Stop streaming" else "Start streaming"
        findViewById<View>(R.id.scanBtn).visibility = if (running) View.GONE else View.VISIBLE
        statusCard.visibility = if (running) View.VISIBLE else View.GONE
        idleHint.visibility = if (running) View.GONE else View.VISIBLE

        // Mode + quality are locked in while streaming (they only take effect at start).
        for (i in 0 until modeGroup.childCount) modeGroup.getChildAt(i).isEnabled = !running
        findViewById<View>(R.id.qualityLayout).isEnabled = !running
        qualityInput.isEnabled = !running

        if (!running) { pairingNote = null; return }

        urlText.text = StreamService.streamUrl ?: ""
        val connected = StreamService.clientConnected
        if (connected) pairingNote = null
        pcStatus.text = when {
            connected -> "✓ PC connected"
            pairingNote != null -> pairingNote!!
            else -> "Waiting for the PC to connect…"
        }
        pcStatus.setTextColor(
            ContextCompat.getColor(
                this,
                when {
                    connected -> R.color.pc_green
                    pairingNote != null -> R.color.pc_amber
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
            if (pendingTarget != null) beginPairing() else startStreaming()
        } else {
            pendingTarget = null
            pcStatus.text = "Camera and microphone permissions are required."
        }
    }

    private fun isGranted(perms: Array<out String>, results: IntArray, name: String): Boolean {
        val i = perms.indexOf(name)
        return i >= 0 && results[i] == PackageManager.PERMISSION_GRANTED
    }

    companion object {
        private const val REQ_PERMS = 1
        private const val KEY_MODE = "mode"
        private const val KEY_QUALITY = "quality"
    }
}
