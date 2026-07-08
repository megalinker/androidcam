package com.phonecam

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.Service
import android.content.Intent
import android.content.pm.ServiceInfo
import android.os.Binder
import android.os.Build
import android.os.IBinder
import android.util.Log
import android.view.SurfaceView
import androidx.core.app.NotificationCompat
import com.pedro.common.ConnectChecker
import com.pedro.encoder.input.sources.audio.AudioSource
import com.pedro.encoder.input.sources.audio.MicrophoneSource
import com.pedro.encoder.input.sources.audio.NoAudioSource
import com.pedro.encoder.input.sources.video.Camera2Source
import com.pedro.encoder.input.sources.video.NoVideoSource
import com.pedro.encoder.input.sources.video.VideoSource
import com.pedro.rtspserver.RtspServerStream

/**
 * Foreground service that turns the phone into an RTSP *server*. The PC connects to
 * rtsp://<phone-ip>:8554/ and pulls a single session carrying video and/or audio (kept
 * in one session so A/V stay in sync).
 *
 * Verified against RTSP-Server 1.4.1 (com.pedro.rtspserver) + RootEncoder 2.7.2 (com.pedro.*).
 * Headless: we never call startPreview(); the camera renders to the encoder's internal
 * OpenGL surface, so no on-screen Surface is required.
 */
class StreamService : Service(), ConnectChecker {

    enum class Mode { BOTH, CAMERA_ONLY, MIC_ONLY }

    /** Video presets. All dimensions are multiples of 4 (the PC-side softcam/GDI requirement). */
    enum class Quality(val label: String, val w: Int, val h: Int, val fps: Int, val bitrate: Int) {
        UHD_2160P30("2160p 30fps (4K)", 3840, 2160, 30, 20_000_000),
        FHD_1080P60("1080p 60fps",      1920, 1080, 60,  8_000_000),
        FHD_1080P30("1080p 30fps",      1920, 1080, 30,  6_000_000),
        HD_720P30  ("720p 30fps",       1280,  720, 30,  4_000_000),
    }

    companion object {
        const val ACTION_START = "com.phonecam.action.START"
        const val ACTION_STOP = "com.phonecam.action.STOP"
        const val ACTION_SWITCH_CAMERA = "com.phonecam.action.SWITCH_CAMERA"
        const val EXTRA_MODE = "mode"
        const val EXTRA_QUALITY = "quality"

        const val PORT = 8554
        const val I_FRAME_INTERVAL = 2

        // 48 kHz matches the Windows WASAPI shared-mode rate (Phase 2 virtual mic) → no resample drift.
        const val AUDIO_SAMPLE_RATE = 48_000
        const val AUDIO_BITRATE = 128_000
        const val AUDIO_STEREO = false

        val DEFAULT_QUALITY = Quality.FHD_1080P30

        private const val CHANNEL_ID = "phonecam_stream"
        private const val NOTIF_ID = 1
        private const val TAG = "PhoneCam"

        @Volatile var isRunning: Boolean = false
            private set

        @Volatile var streamUrl: String? = null
            private set
    }

    private var stream: RtspServerStream? = null
    private var hasVideo = false
    private val binder = LocalBinder()

    /** Lets the Activity reach this service instance to attach an on-screen preview. */
    inner class LocalBinder : Binder() {
        val service: StreamService get() = this@StreamService
    }

    override fun onBind(intent: Intent?): IBinder = binder

    /** Attach the Activity's SurfaceView to the running camera stream. Idempotent; main thread. */
    fun attachPreview(surfaceView: SurfaceView) {
        val s = stream ?: return
        if (!hasVideo || s.isOnPreview) return
        runCatching { s.startPreview(surfaceView) }.onFailure { Log.w(TAG, "startPreview failed", it) }
    }

    fun detachPreview() {
        val s = stream ?: return
        if (s.isOnPreview) runCatching { s.stopPreview() }.onFailure { Log.w(TAG, "stopPreview failed", it) }
    }

    fun setPreviewResolution(w: Int, h: Int) {
        runCatching { stream?.getGlInterface()?.setPreviewResolution(w, h) }
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        when (intent?.action) {
            ACTION_STOP -> { stopStreaming(); return START_NOT_STICKY }
            ACTION_SWITCH_CAMERA -> { switchCamera(); return START_STICKY }
        }

        val mode = intent?.getStringExtra(EXTRA_MODE)?.let { runCatching { Mode.valueOf(it) }.getOrNull() }
            ?: Mode.BOTH
        val quality = intent?.getStringExtra(EXTRA_QUALITY)?.let { runCatching { Quality.valueOf(it) }.getOrNull() }
            ?: DEFAULT_QUALITY

        startForegroundForMode(mode)
        startStreaming(mode, quality)
        return START_STICKY
    }

    private fun startStreaming(mode: Mode, quality: Quality) {
        if (isRunning) return

        val video: VideoSource = if (mode == Mode.MIC_ONLY) NoVideoSource() else Camera2Source(this)
        val audio: AudioSource = if (mode == Mode.CAMERA_ONLY) NoAudioSource() else MicrophoneSource()

        // Phone = RTSP server. Constructor arg order is (context, PORT, connectChecker, video, audio).
        val s = RtspServerStream(this, PORT, this, video, audio)

        // Advertise only the relevant track(s) in the SDP for single-medium modes.
        if (mode == Mode.MIC_ONLY) s.getStreamClient().setOnlyAudio(true)
        if (mode == Mode.CAMERA_ONLY) s.getStreamClient().setOnlyVideo(true)

        val videoOk = mode == Mode.MIC_ONLY ||
            s.prepareVideo(quality.w, quality.h, quality.bitrate, quality.fps, I_FRAME_INTERVAL, rotation = 0)
        val audioOk = mode == Mode.CAMERA_ONLY ||
            s.prepareAudio(AUDIO_SAMPLE_RATE, AUDIO_STEREO, AUDIO_BITRATE)

        if (!videoOk || !audioOk) {
            Log.e(TAG, "prepare failed (video=$videoOk audio=$audioOk) — try a lower Quality preset")
            stopSelf()
            return
        }

        s.startStream() // opens the RTSP listening socket; no URL/preview needed
        stream = s
        hasVideo = mode != Mode.MIC_ONLY
        streamUrl = s.getStreamClient().getEndPointConnection() // "rtsp://<phone-ip>:8554/"
        isRunning = true
        Log.i(TAG, "RTSP server up ($mode, ${quality.label}) at $streamUrl")
        updateNotification()
    }

    /** Toggle front/back camera on the running stream (no-op in mic-only mode). */
    private fun switchCamera() {
        val cam = stream?.videoSource as? Camera2Source ?: return
        runCatching { cam.switchCamera() }.onFailure { Log.w(TAG, "switchCamera failed", it) }
    }

    private fun stopStreaming() {
        stream?.let {
            if (it.isOnPreview) runCatching { it.stopPreview() }
            if (it.isStreaming) it.stopStream()
        }
        stream = null
        hasVideo = false
        isRunning = false
        streamUrl = null
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.N) {
            stopForeground(STOP_FOREGROUND_REMOVE)
        } else {
            @Suppress("DEPRECATION") stopForeground(true)
        }
        stopSelf()
    }

    override fun onDestroy() {
        stopStreaming()
        super.onDestroy()
    }

    // --- foreground notification ---

    private fun startForegroundForMode(mode: Mode) {
        createChannel()
        val notif = buildNotification()
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
            var type = 0
            if (mode != Mode.MIC_ONLY) type = type or ServiceInfo.FOREGROUND_SERVICE_TYPE_CAMERA
            if (mode != Mode.CAMERA_ONLY) type = type or ServiceInfo.FOREGROUND_SERVICE_TYPE_MICROPHONE
            startForeground(NOTIF_ID, notif, type)
        } else {
            startForeground(NOTIF_ID, notif)
        }
    }

    private fun buildNotification(): Notification {
        val text = streamUrl?.let { "Streaming at $it" } ?: "Starting…"
        return NotificationCompat.Builder(this, CHANNEL_ID)
            .setContentTitle(getString(R.string.notif_title))
            .setContentText(text)
            .setSmallIcon(android.R.drawable.ic_menu_camera)
            .setOngoing(true)
            .build()
    }

    private fun updateNotification() {
        (getSystemService(NOTIFICATION_SERVICE) as NotificationManager)
            .notify(NOTIF_ID, buildNotification())
    }

    private fun createChannel() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            val ch = NotificationChannel(
                CHANNEL_ID, getString(R.string.notif_channel), NotificationManager.IMPORTANCE_LOW
            )
            (getSystemService(NOTIFICATION_SERVICE) as NotificationManager).createNotificationChannel(ch)
        }
    }

    // --- ConnectChecker (com.pedro.common). For a server these fire as clients attach/detach. ---
    override fun onConnectionStarted(url: String) { Log.d(TAG, "client connecting: $url") }
    override fun onConnectionSuccess() { Log.i(TAG, "client connected") }
    override fun onConnectionFailed(reason: String) { Log.w(TAG, "connection failed: $reason") }
    override fun onNewBitrate(bitrate: Long) { /* hook for adaptive bitrate */ }
    override fun onDisconnect() { Log.i(TAG, "client disconnected") }
    override fun onAuthError() { Log.w(TAG, "auth error") }
    override fun onAuthSuccess() { Log.d(TAG, "auth success") }
}
