package com.phonecam

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.Service
import android.content.Context
import android.content.Intent
import android.content.pm.ServiceInfo
import android.graphics.Bitmap
import android.graphics.Color
import android.os.Build
import android.os.Handler
import android.os.IBinder
import android.os.Looper
import android.os.PowerManager
import android.os.SystemClock
import android.util.Log
import androidx.core.app.NotificationCompat
import com.pedro.common.ConnectChecker
import com.pedro.encoder.input.sources.audio.AudioSource
import com.pedro.encoder.input.sources.audio.MicrophoneSource
import com.pedro.encoder.input.sources.audio.NoAudioSource
import com.pedro.encoder.input.sources.video.BitmapSource
import com.pedro.encoder.input.sources.video.Camera2Source
import com.pedro.encoder.input.sources.video.VideoSource
import com.pedro.rtspserver.RtspServerStream
import com.pedro.rtspserver.server.ClientListener
import com.pedro.rtspserver.server.ServerClient

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
        // Transport: "rtsp" (default, phone = server) or "webrtc" (phone answers the PC's PCAM3
        // offer with a sendonly Opus mic track — mic-only,
        // Phase 3 of docs/webrtc-migration.md).
        const val EXTRA_TRANSPORT = "transport"
        // WebRTC signaling target (PCAM3 QR).
        const val EXTRA_SIG_HOST = "sigHost"
        const val EXTRA_SIG_PORT = "sigPort"
        const val EXTRA_SIG_SECRET = "sigSecret"
        const val PORT = 8554
        // A tiny control port: the PC connects and sends "PCAM-STOP" so pressing Stop on the PC stops
        // the phone at once, instead of leaving it waiting for a reconnect. Only a deliberate PC-side
        // Stop sends this — a mere client drop (blip) does not, so auto-reconnect still works.
        const val CONTROL_PORT = 8555
        private const val CONTROL_STOP = "PCAM-STOP"
        private const val CONTROL_CREDS = "PCAM-CREDS?"   // USB (loopback) asks for the RTSP secret
        const val RTSP_USER = "phonecam"                  // Basic-auth username for the stream
        const val I_FRAME_INTERVAL = 1   // 1s GOP: faster first frame + quicker recovery after a glitch

        // Auto-stop after this long with no PC pulling — the encoder/camera run whether or not anyone
        // is watching, so a stream left on with no viewer is pure battery waste. Generous enough not to
        // interrupt the normal "start, then open Discord" flow.
        private const val IDLE_TIMEOUT_MS = 5 * 60 * 1000L
        // Once a PC has connected and then dropped, wait only this long before auto-stopping: either the
        // PC is reconnecting (it retries within seconds) or the session is over. Stops the phone from
        // "streaming forever" after the PC side is closed, without cutting off a quick reconnect.
        private const val IDLE_AFTER_DISCONNECT_MS = 2 * 60 * 1000L
        private const val IDLE_CHECK_MS = 30 * 1000L

        // Mic-only still has to run the camera+video encoder (the RTSP server won't answer a client
        // until the video encoder emits a keyframe) — but that video is never sent, so encode a tiny,
        // low-fps frame to keep battery/heat down instead of a full 1080p30 stream nobody receives.
        const val MIC_ONLY_W = 176
        const val MIC_ONLY_H = 144
        const val MIC_ONLY_FPS = 10
        const val MIC_ONLY_BITRATE = 80_000

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

        /**
         * A stable per-install secret the PC must present to stop us over Wi-Fi. Created once and kept
         * in prefs, so it survives restarts — that's what lets a saved-device reconnect (which never
         * re-pairs) still carry a valid stop token. Sent to the PC in the pairing handshake.
         */
        @Synchronized   // announce (worker thread) and the listener (main) can both ask at once — create once
        fun controlToken(ctx: Context): String {
            val prefs = ctx.getSharedPreferences("phonecam", Context.MODE_PRIVATE)
            prefs.getString("controlToken", null)?.let { if (it.isNotEmpty()) return it }
            val t = java.util.UUID.randomUUID().toString().replace("-", "").substring(0, 16)
            prefs.edit().putString("controlToken", t).apply()
            return t
        }

        // True while a PC (RTSP client) is pulling the stream. Drives the "PC connected ✓" UI.
        @Volatile var clientConnected: Boolean = false
            private set

        // True once any PC has connected this session (stays true after it drops). Lets the UI say
        // "PC disconnected" rather than a fresh "waiting", and shortens the idle auto-stop.
        @Volatile var everConnected: Boolean = false
            private set
    }

    private var stream: RtspServerStream? = null
    private var webrtcSender: WebRtcSender? = null
    private var wakeLock: PowerManager.WakeLock? = null
    private var controlServer: java.net.ServerSocket? = null

    // Battery guard: auto-stop if no PC connects for IDLE_TIMEOUT_MS. lastClientMs = the last time a
    // client was connected (or the stream start), so the timeout counts from "nobody watching".
    @Volatile private var lastClientMs = 0L
    private val idleHandler = Handler(Looper.getMainLooper())
    private val idleCheck = object : Runnable {
        override fun run() {
            if (!isRunning) return
            val timeout = if (everConnected) IDLE_AFTER_DISCONNECT_MS else IDLE_TIMEOUT_MS
            if (!clientConnected && SystemClock.elapsedRealtime() - lastClientMs > timeout) {
                Log.i(TAG, "no PC for ${timeout / 60000} min — auto-stopping to save battery")
                stopStreaming()
                return
            }
            idleHandler.postDelayed(this, IDLE_CHECK_MS)
        }
    }

    // Headless streaming: no on-screen preview. RootEncoder's startPreview() attaches a second GL
    // surface to the encoder's shared context and, on at least some devices, releases/re-inits that
    // context ("SurfaceManager: GL already released") — which blacks out BOTH the preview and the
    // encoded stream. Capture works reliably without it; view the feed on the PC receiver instead.
    override fun onBind(intent: Intent?): IBinder? = null

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        when (intent?.action) {
            ACTION_STOP -> { stopStreaming(); return START_NOT_STICKY }
            ACTION_SWITCH_CAMERA -> { switchCamera(); return START_STICKY }
        }

        val mode = intent?.getStringExtra(EXTRA_MODE)?.let { runCatching { Mode.valueOf(it) }.getOrNull() }
            ?: Mode.BOTH
        val quality = intent?.getStringExtra(EXTRA_QUALITY)?.let { runCatching { Quality.valueOf(it) }.getOrNull() }
            ?: DEFAULT_QUALITY
        val transport = intent?.getStringExtra(EXTRA_TRANSPORT) ?: "rtsp"
        val sigHost = intent?.getStringExtra(EXTRA_SIG_HOST)
        val sigPort = intent?.getIntExtra(EXTRA_SIG_PORT, 0) ?: 0
        val sigSecret = intent?.getStringExtra(EXTRA_SIG_SECRET)

        // WebRTC is mic-only for now (Phase 3); force the mic foreground type regardless of the toggle.
        startForegroundForMode(mode)   // WebRTC now carries video too, so honor the real mode
        startStreaming(mode, quality, transport, sigHost, sigPort, sigSecret)
        return START_STICKY
    }

    private fun startStreaming(mode: Mode, quality: Quality, transport: String,
                               sigHost: String?, sigPort: Int, sigSecret: String?) {
        if (isRunning) return
        clientConnected = false; everConnected = false

        // WebRTC (mic-only) takes a wholly separate path — org.webrtc, not RootEncoder.
        if (transport == "webrtc") { startWebrtcStreaming(mode, sigHost, sigPort, sigSecret); return }

        // The RTSP server won't answer a client until the video encoder emits its first keyframe
        // (SPS/PPS via onVideoInfo), and a NoVideoSource never produces one. Camera modes use the real
        // camera; MIC_ONLY feeds a frozen 16x16 black bitmap instead, so the encoder still emits
        // keyframes but the camera sensor never powers on (a static frame encodes to ~nothing) — the
        // real battery/heat fix. setOnlyAudio(true) below keeps the dummy video out of the SDP.
        val video: VideoSource = if (mode == Mode.MIC_ONLY)
            BitmapSource(Bitmap.createBitmap(16, 16, Bitmap.Config.ARGB_8888).apply { eraseColor(Color.BLACK) })
        else
            Camera2Source(this)
        val audio: AudioSource = if (mode == Mode.CAMERA_ONLY) NoAudioSource() else MicrophoneSource()

        try {
            // Phone = RTSP server. Constructor arg order is (context, PORT, connectChecker, video, audio).
            val s = RtspServerStream(this, PORT, this, video, audio)

            // Advertise only the relevant track(s) in the SDP for single-medium modes.
            if (mode == Mode.MIC_ONLY) s.getStreamClient().setOnlyAudio(true)
            if (mode == Mode.CAMERA_ONLY) s.getStreamClient().setOnlyVideo(true)

            // Require Basic auth on the stream so a random device on the same Wi-Fi can't pull our
            // camera/mic. The PC gets the password from the QR handshake (or, over USB, from the
            // loopback creds query below). NOTE: RTSP here is unencrypted, so this stops casual access,
            // not a LAN attacker who can sniff packets — but it closes the "open in VLC" hole.
            s.getStreamClient().setAuthorization(RTSP_USER, controlToken(this))

            // Server-side client attach/detach. (The ConnectChecker callbacks below reflect the
            // phone's own encoder session, NOT a PC pulling — so the "PC connected" state must come
            // from here, the RTSP server's client listener.)
            s.getStreamClient().setClientListener(object : ClientListener {
                override fun onClientConnected(client: ServerClient) {
                    clientConnected = true; everConnected = true; lastClientMs = SystemClock.elapsedRealtime()
                    Log.i(TAG, "PC connected (clients=${s.getStreamClient().getNumClients()})")
                }
                override fun onClientDisconnected(client: ServerClient) {
                    clientConnected = false; lastClientMs = SystemClock.elapsedRealtime()   // start the idle countdown
                    Log.i(TAG, "PC disconnected")
                }
                override fun onClientNewBitrate(bitrate: Long, client: ServerClient) { /* adaptive hook */ }
            })

            // Follow the phone's orientation so the picture isn't squished: with rotation = 0 the
            // encoder always emitted a landscape 1920x1080 frame, so holding the phone portrait
            // crushed the tall scene into a wide frame. getCameraOrientation() rotates the frame to
            // match the device (portrait -> portrait output), which keeps the aspect correct.
            val rotation = com.pedro.encoder.input.video.CameraHelper.getCameraOrientation(this)

            // RootEncoder's startStream() starts BOTH encoders regardless of No*Source, so we must
            // prepare BOTH even in single-track modes — otherwise the unused encoder throws
            // "…Encoder not prepared yet" on start. setOnly*/No*Source handle what's actually sent.
            val videoOk = if (mode == Mode.MIC_ONLY)
                s.prepareVideo(MIC_ONLY_W, MIC_ONLY_H, MIC_ONLY_BITRATE, MIC_ONLY_FPS, I_FRAME_INTERVAL, rotation = rotation)
            else
                s.prepareVideo(quality.w, quality.h, quality.bitrate, quality.fps, I_FRAME_INTERVAL, rotation = rotation)
            val audioOk = s.prepareAudio(AUDIO_SAMPLE_RATE, AUDIO_STEREO, AUDIO_BITRATE)
            if (!videoOk || !audioOk) {
                Log.e(TAG, "prepare failed (video=$videoOk audio=$audioOk) — try a lower Quality preset")
                stopSelf()
                return
            }

            s.startStream() // opens the RTSP listening socket; no URL/preview needed
            stream = s
            // Prefer the real Wi-Fi IPv4 for the displayed URL (getEndPointConnection can pick a
            // VPN/cellular address — unreachable, and for IPv6 an unbracketed/malformed URL).
            streamUrl = wifiRtspUrl() ?: s.getStreamClient().getEndPointConnection()
            isRunning = true
            acquireWakeLock()   // keep the CPU/stream alive with the screen off (less heat than forcing it on)
            lastClientMs = SystemClock.elapsedRealtime()
            idleHandler.postDelayed(idleCheck, IDLE_CHECK_MS)   // auto-stop if no PC ever connects
            startControlListener()   // let the PC's Stop button stop us immediately
            Log.i(TAG, "RTSP server up ($mode, ${quality.label}) at $streamUrl")
            updateNotification()
        } catch (e: Exception) {
            // Never crash-loop the service (it is START_STICKY): stop cleanly on any start failure.
            Log.e(TAG, "startStreaming failed", e)
            releaseWakeLock()   // don't leak the CPU lock if we bail after acquiring it
            runCatching { stream?.stopStream() }
            stream = null
            isRunning = false
            stopSelf()
        }
    }

    /**
     * WebRTC mic-only: answer the PC's PCAM3 offer with a sendonly Opus track over DTLS-SRTP.
     * No RootEncoder, no camera, no dummy-video hack — org.webrtc captures the mic directly.
     * The PC is the offerer/listener, so "connected" comes from the WebRTC state callback.
     */
    private fun startWebrtcStreaming(mode: Mode, sigHost: String?, sigPort: Int, sigSecret: String?) {
        if (sigHost.isNullOrEmpty() || sigPort !in 1..65535 || sigSecret.isNullOrEmpty()) {
            Log.e(TAG, "webrtc: missing signaling target (host=$sigHost port=$sigPort)")
            stopSelf(); return
        }
        try {
            val sender = WebRtcSender(applicationContext, sigHost, sigPort, sigSecret,
                mode != Mode.MIC_ONLY, mode != Mode.CAMERA_ONLY) { state ->
                when (state) {
                    WebRtcSender.State.CONNECTED -> {
                        clientConnected = true; everConnected = true
                        lastClientMs = SystemClock.elapsedRealtime()
                        Log.i(TAG, "webrtc: connected to PC")
                    }
                    WebRtcSender.State.CONNECTING -> Log.i(TAG, "webrtc: connecting…")
                    WebRtcSender.State.DISCONNECTED, WebRtcSender.State.FAILED -> {
                        clientConnected = false
                        Log.i(TAG, "webrtc: $state — stopping")
                        idleHandler.post { stopStreaming() }
                    }
                }
            }
            webrtcSender = sender
            sender.start()
            streamUrl = "WebRTC → $sigHost"
            isRunning = true
            acquireWakeLock()
            lastClientMs = SystemClock.elapsedRealtime()
            idleHandler.postDelayed(idleCheck, IDLE_CHECK_MS)
            Log.i(TAG, "webrtc mic push up to $sigHost:$sigPort")
            updateNotification()
        } catch (e: Exception) {
            Log.e(TAG, "startWebrtcStreaming failed", e)
            releaseWakeLock()
            runCatching { webrtcSender?.stop() }
            webrtcSender = null
            isRunning = false
            stopSelf()
        }
    }

    /** Pull URL from the phone's Wi-Fi (wlan) IPv4, skipping VPN/cellular interfaces. Null if none. */
    private fun wifiRtspUrl(): String? = try {
        java.net.NetworkInterface.getNetworkInterfaces().toList()
            .filter { it.isUp && !it.isLoopback && it.name.startsWith("wlan") }
            .flatMap { it.inetAddresses.toList() }
            .filterIsInstance<java.net.Inet4Address>()
            .firstOrNull { !it.isLoopbackAddress }
            ?.hostAddress
            ?.let { "rtsp://$it:$PORT/" }
    } catch (e: Exception) {
        Log.w(TAG, "wifiRtspUrl failed", e); null
    }

    /** Toggle front/back camera on the running stream (no-op in mic-only mode). */
    private fun switchCamera() {
        val cam = stream?.videoSource as? Camera2Source ?: return
        runCatching { cam.switchCamera() }.onFailure { Log.w(TAG, "switchCamera failed", it) }
    }

    // Listen for the PC's "stop now" command on a background thread. One connection, one line: if it's
    // CONTROL_STOP we stop the stream on the main thread. Closing controlServer in stopStreaming()
    // unblocks accept() and ends the loop. Any bind/read failure just degrades to the old behaviour
    // (the phone keeps waiting), so this is best-effort and never crashes the service.
    private fun startControlListener() {
        val myToken = controlToken(this)
        Thread {
            val srv = try {
                java.net.ServerSocket().apply { reuseAddress = true; bind(java.net.InetSocketAddress(CONTROL_PORT)) }
            } catch (e: Exception) {
                Log.w(TAG, "control listener bind failed", e); return@Thread
            }
            controlServer = srv
            Log.i(TAG, "control listener on $CONTROL_PORT")
            try {
                while (isRunning) {
                    val client = try { srv.accept() } catch (e: Exception) { break }
                    try {
                        client.soTimeout = 3000
                        val line = client.getInputStream().bufferedReader(Charsets.UTF_8).readLine()?.trim()
                        val fromLoopback = client.inetAddress?.isLoopbackAddress == true
                        if (fromLoopback && line == CONTROL_CREDS) {
                            // USB has no QR handshake — hand the (loopback-only, trusted) caller the RTSP
                            // secret so it can authenticate the pull. Never answered over Wi-Fi.
                            runCatching {
                                client.getOutputStream().apply { write("$myToken\n".toByteArray(Charsets.UTF_8)); flush() }
                            }
                        } else {
                            // Authorize a stop: a matching token from anywhere, OR a bare stop over loopback
                            // (the USB adb-forward path, already trusted via USB debugging).
                            val authorized = line == "$CONTROL_STOP:$myToken" || (fromLoopback && line == CONTROL_STOP)
                            if (authorized) {
                                Log.i(TAG, "authorized stop from PC — stopping stream immediately")
                                idleHandler.post { stopStreaming() }
                            } else if (line != null && line.startsWith(CONTROL_STOP)) {
                                Log.w(TAG, "ignored unauthorized stop from ${client.inetAddress}")
                            }
                        }
                    } catch (e: Exception) {
                        Log.w(TAG, "control read failed", e)
                    } finally {
                        runCatching { client.close() }
                    }
                }
            } finally {
                runCatching { srv.close() }
            }
        }.apply { isDaemon = true }.start()
    }

    // A partial wake lock keeps the CPU running so the stream survives the screen turning off — which
    // is what we want instead of FLAG_KEEP_SCREEN_ON (a lit screen is a big heat/battery source).
    private fun acquireWakeLock() {
        if (wakeLock?.isHeld == true) return
        val pm = getSystemService(POWER_SERVICE) as PowerManager
        wakeLock = pm.newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "PhoneCam::stream").apply {
            setReferenceCounted(false)
            acquire(4 * 60 * 60 * 1000L)   // 4h safety cap; released explicitly on stop
        }
    }

    private fun releaseWakeLock() {
        wakeLock?.let { if (it.isHeld) it.release() }
        wakeLock = null
    }

    private fun stopStreaming() {
        idleHandler.removeCallbacks(idleCheck)
        runCatching { controlServer?.close() }; controlServer = null   // unblocks the accept() loop
        releaseWakeLock()
        stream?.let { if (it.isStreaming) it.stopStream() }
        runCatching { webrtcSender?.stop() }
        stream = null; webrtcSender = null
        isRunning = false
        streamUrl = null
        clientConnected = false; everConnected = false
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

    // --- ConnectChecker (com.pedro.common). For the RTSP server these describe encoder state. ---
    override fun onConnectionStarted(url: String) { Log.d(TAG, "connection starting: $url") }
    override fun onConnectionSuccess() { Log.i(TAG, "encoder session ready") }
    override fun onConnectionFailed(reason: String) {
        Log.w(TAG, "connection failed: $reason")
    }
    override fun onNewBitrate(bitrate: Long) { /* hook for adaptive bitrate */ }
    override fun onDisconnect() { Log.i(TAG, "encoder session ended") }
    override fun onAuthError() { Log.w(TAG, "auth error") }
    override fun onAuthSuccess() { Log.d(TAG, "auth success") }
}
