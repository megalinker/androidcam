package com.phonecam

import android.Manifest
import android.app.Activity
import android.content.Intent
import android.content.pm.PackageManager
import android.os.Build
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.view.WindowManager
import android.widget.ArrayAdapter
import android.widget.Button
import android.widget.RadioGroup
import android.widget.Spinner
import android.widget.TextView
import androidx.core.app.ActivityCompat
import androidx.core.content.ContextCompat

/**
 * Thin UI: pick a mode (Camera+Mic / Camera / Mic), start/stop the streaming service, and
 * show the RTSP URL to type on the PC. All the real work is in [StreamService].
 */
class MainActivity : Activity() {

    private lateinit var status: TextView
    private lateinit var qualitySpinner: Spinner
    private val prefs by lazy { getSharedPreferences("phonecam", MODE_PRIVATE) }
    private val ui = Handler(Looper.getMainLooper())
    private val poll = object : Runnable {
        override fun run() { refreshStatus(); ui.postDelayed(this, 1000) }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON) // don't sleep while streaming
        setContentView(R.layout.activity_main)
        status = findViewById(R.id.statusText)

        qualitySpinner = findViewById(R.id.qualitySpinner)
        val qualities = StreamService.Quality.values()
        qualitySpinner.adapter = ArrayAdapter(
            this, android.R.layout.simple_spinner_dropdown_item, qualities.map { it.label }
        )

        restoreSelections(qualities)

        findViewById<Button>(R.id.startBtn).setOnClickListener { if (ensurePermissions()) startStreaming() }
        findViewById<Button>(R.id.stopBtn).setOnClickListener { stopStreaming() }
        findViewById<Button>(R.id.switchCamBtn).setOnClickListener { switchCamera() }
    }

    /** Restore the last-used mode + quality from prefs (defaults if none saved). */
    private fun restoreSelections(qualities: Array<StreamService.Quality>) {
        val savedQuality = prefs.getString(KEY_QUALITY, null)
            ?.let { runCatching { StreamService.Quality.valueOf(it) }.getOrNull() }
            ?: StreamService.DEFAULT_QUALITY
        qualitySpinner.setSelection(qualities.indexOf(savedQuality))

        val savedMode = prefs.getString(KEY_MODE, null)
            ?.let { runCatching { StreamService.Mode.valueOf(it) }.getOrNull() }
        val id = when (savedMode) {
            StreamService.Mode.CAMERA_ONLY -> R.id.modeCamera
            StreamService.Mode.MIC_ONLY -> R.id.modeMic
            else -> R.id.modeBoth
        }
        findViewById<RadioGroup>(R.id.modeGroup).check(id)
    }

    private fun selectedMode(): StreamService.Mode =
        when (findViewById<RadioGroup>(R.id.modeGroup).checkedRadioButtonId) {
            R.id.modeCamera -> StreamService.Mode.CAMERA_ONLY
            R.id.modeMic -> StreamService.Mode.MIC_ONLY
            else -> StreamService.Mode.BOTH
        }

    private fun selectedQuality(): StreamService.Quality =
        StreamService.Quality.values()[qualitySpinner.selectedItemPosition]

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

    private fun switchCamera() {
        if (!StreamService.isRunning) return
        startService(Intent(this, StreamService::class.java).apply {
            action = StreamService.ACTION_SWITCH_CAMERA
        })
    }

    private fun stopStreaming() {
        startService(Intent(this, StreamService::class.java).apply { action = StreamService.ACTION_STOP })
    }

    private fun refreshStatus() {
        status.text = if (StreamService.isRunning) {
            "● Streaming (${selectedMode().name})\n\nOn the PC (same Wi-Fi), run:\n  receiver.exe ${StreamService.streamUrl}"
        } else {
            "○ Idle. Pick a mode and press Start.\nEnsure the PC is on the same Wi-Fi network."
        }
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
        if (camOk && micOk) startStreaming()
        else status.text = "Camera and microphone permissions are required."
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
