package com.phonecam

import android.Manifest
import android.content.Intent
import android.content.pm.PackageManager
import android.graphics.Bitmap
import android.graphics.Color
import android.os.Build
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.view.View
import android.view.WindowManager
import android.widget.ImageView
import android.widget.TextView
import androidx.appcompat.app.AppCompatActivity
import androidx.core.app.ActivityCompat
import androidx.core.content.ContextCompat
import com.google.android.material.button.MaterialButton
import com.google.android.material.button.MaterialButtonToggleGroup
import com.google.android.material.card.MaterialCardView
import com.google.android.material.textfield.MaterialAutoCompleteTextView
import com.google.zxing.BarcodeFormat
import com.google.zxing.qrcode.QRCodeWriter

/**
 * Single-screen UI: pick a mode (Cam + Mic / Camera / Mic) and quality, press Start, and the
 * card shows a QR of the pull URL plus a live "PC connected" indicator. The heavy lifting is in
 * [StreamService]; this activity only drives it and polls its state once a second.
 */
class MainActivity : AppCompatActivity() {

    private lateinit var modeGroup: MaterialButtonToggleGroup
    private lateinit var qualityInput: MaterialAutoCompleteTextView
    private lateinit var startBtn: MaterialButton
    private lateinit var statusCard: MaterialCardView
    private lateinit var idleHint: View
    private lateinit var urlText: TextView
    private lateinit var pcStatus: TextView
    private lateinit var qrImage: ImageView

    private val prefs by lazy { getSharedPreferences("phonecam", MODE_PRIVATE) }
    private val ui = Handler(Looper.getMainLooper())
    private val qualities = StreamService.Quality.values()
    private var lastQrUrl: String? = null

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
        qrImage = findViewById(R.id.qrImage)

        qualityInput.setSimpleItems(qualities.map { it.label }.toTypedArray())
        restoreSelections()

        startBtn.setOnClickListener {
            if (StreamService.isRunning) stopStreaming()
            else if (ensurePermissions()) startStreaming()
        }
        findViewById<MaterialButton>(R.id.switchCamBtn).setOnClickListener { switchCamera() }
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
        statusCard.visibility = if (running) View.VISIBLE else View.GONE
        idleHint.visibility = if (running) View.GONE else View.VISIBLE

        // Mode + quality are locked in while streaming (they only take effect at start).
        for (i in 0 until modeGroup.childCount) modeGroup.getChildAt(i).isEnabled = !running
        findViewById<View>(R.id.qualityLayout).isEnabled = !running
        qualityInput.isEnabled = !running

        if (!running) { lastQrUrl = null; return }

        val url = StreamService.streamUrl ?: ""
        urlText.text = url
        if (url.isNotEmpty() && url != lastQrUrl) {
            qrImage.setImageBitmap(qr(url, 480))
            lastQrUrl = url
        }
        val connected = StreamService.clientConnected
        pcStatus.text = if (connected) "✓ PC connected" else "Waiting for the PC to connect…"
        pcStatus.setTextColor(ContextCompat.getColor(this, if (connected) R.color.pc_green else R.color.pc_muted))
    }

    override fun onResume() { super.onResume(); ui.post(poll) }
    override fun onPause() { super.onPause(); ui.removeCallbacks(poll) }

    /** Black-on-white QR of [text] at [size]px (setPixels in one shot — no per-pixel jank). */
    private fun qr(text: String, size: Int): Bitmap {
        val matrix = QRCodeWriter().encode(text, BarcodeFormat.QR_CODE, size, size)
        val pixels = IntArray(size * size)
        for (y in 0 until size) {
            val row = y * size
            for (x in 0 until size) pixels[row + x] = if (matrix.get(x, y)) Color.BLACK else Color.WHITE
        }
        return Bitmap.createBitmap(size, size, Bitmap.Config.RGB_565)
            .apply { setPixels(pixels, 0, size, 0, 0, size, size) }
    }

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
        if (camOk && micOk) startStreaming()
        else pcStatus.text = "Camera and microphone permissions are required."
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
