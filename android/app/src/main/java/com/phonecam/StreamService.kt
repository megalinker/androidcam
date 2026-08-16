package com.phonecam

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.Service
import android.content.Intent
import android.content.pm.ServiceInfo
import android.os.Build
import android.os.Handler
import android.os.HandlerThread
import android.os.IBinder
import android.os.Looper
import android.os.PowerManager
import android.os.SystemClock
import android.util.Log
import androidx.core.app.NotificationCompat

/**
 * Foreground service that streams the phone's camera and/or mic to the PC over WebRTC (DTLS-SRTP).
 * The phone answers the PC's PCAM3 offer with a sendonly Opus mic and/or H.264 camera track; media
 * then flows LAN-direct over UDP. All the WebRTC work lives in [WebRtcSender]; this service owns
 * lifecycle, the foreground notification, the wake lock, and the idle auto-stop.
 *
 * Headless: we never render an on-screen preview — org.webrtc captures straight into the encoder,
 * so no Surface is required. View the feed on the PC receiver instead.
 */
class StreamService : Service() {

    enum class Mode { BOTH, CAMERA_ONLY, MIC_ONLY }

    /** Camera capture presets. Caps the WebRTC capture resolution; the link sheds detail under load. */
    enum class Quality(val label: String, val w: Int, val h: Int, val fps: Int) {
        UHD_2160P30("2160p 30fps (4K)", 3840, 2160, 30),
        FHD_1080P60("1080p 60fps",      1920, 1080, 60),
        FHD_1080P30("1080p 30fps",      1920, 1080, 30),
        HD_720P30  ("720p 30fps",       1280,  720, 30),
    }

    companion object {
        const val ACTION_START = "com.phonecam.action.START"
        const val ACTION_STOP = "com.phonecam.action.STOP"
        const val ACTION_SWITCH_CAMERA = "com.phonecam.action.SWITCH_CAMERA"
        const val EXTRA_MODE = "mode"
        const val EXTRA_QUALITY = "quality"
        // Raw mic: capture without the platform voice AEC/NS (the phone is a standalone remote mic —
        // it has nothing to echo-cancel, and raw sounds fuller). Default true = raw; the desktop app's
        // "Phone-call noise filter" checkbox turns the processing back on for noisy rooms. (F-12)
        const val EXTRA_RAW_MIC = "rawMic"
        // Diagnostics: OFF unless the user turns it on in the phone app, or a caller passes these.
        // `diag` enables the 30 s session sampler; `deepDiag` additionally drops to a 5 s cadence.
        // Battery reporting to the PC is a separate, always-on production feature and is NOT gated
        // by these — it is a single sticky-intent read per minute.
        const val EXTRA_DIAG = "diag"
        const val EXTRA_DEEP_DIAG = "deepDiag"
        /** SharedPreferences key backing the in-app "Record diagnostics" switch. */
        const val PREF_DIAG = "diagEnabled"
        // Measurement only: send video with rotation 0 so the phone does not rotate pixels before
        // encoding. Lets the rotation cost be A/B'd with the phone untouched. See WebRtcSender.
        const val EXTRA_NO_ROTATE = "noRotate"
        const val PREF_NO_ROTATE = "noRotateEnabled"
        // Transport: "webrtc" (default, Wi-Fi) or "usb" (scrcpy-style H.264/PCM over an adb-forwarded socket).
        const val EXTRA_TRANSPORT = "transport"
        // WebRTC signaling target (from the PC's PCAM3 QR).
        const val EXTRA_SIG_HOST = "sigHost"
        const val EXTRA_SIG_PORT = "sigPort"
        const val EXTRA_SIG_SECRET = "sigSecret"
        // USB: the loopback port the phone listens on; the PC reaches it via `adb forward`.
        const val EXTRA_USB_PORT = "usbPort"
        const val DEFAULT_USB_PORT = 27183

        // Auto-stop after this long with no PC connected — the camera/mic run whether or not anyone
        // is watching, so a stream left on with no peer is pure battery waste. Generous enough not to
        // interrupt the normal "start, then open Discord" flow.
        private const val IDLE_TIMEOUT_MS = 5 * 60 * 1000L
        // Once a PC has connected and then dropped, wait only this long before auto-stopping: either the
        // PC is reconnecting (it retries within seconds) or the session is over.
        private const val IDLE_AFTER_DISCONNECT_MS = 2 * 60 * 1000L
        private const val IDLE_CHECK_MS = 30 * 1000L
        // How often the phone tells the PC its battery level. Battery % moves at most ~1 point every
        // few minutes under this workload, so a minute is already far finer than the data; charger
        // plug/unplug is delivered as an event, not waited for. One ~90-byte write on a socket that
        // is open anyway.
        private const val STATUS_PUSH_MS = 60 * 1000L

        val DEFAULT_QUALITY = Quality.FHD_1080P30

        private const val CHANNEL_ID = "phonecam_stream"
        private const val NOTIF_ID = 1
        private const val TAG = "PhoneCam"

        @Volatile var isRunning: Boolean = false
            private set

        // Shown in the status card: the PC we're connected (or connecting) to.
        @Volatile var streamUrl: String? = null
            private set

        // True while the WebRTC link to the PC is up. Drives the "PC connected ✓" UI.
        @Volatile var clientConnected: Boolean = false
            private set

        // True once the link has come up this session (stays true after it drops). Lets the UI say
        // "PC disconnected" rather than a fresh "connecting", and shortens the idle auto-stop.
        @Volatile var everConnected: Boolean = false
            private set
    }

    private var webrtcSender: WebRtcSender? = null
    private var usbStreamer: UsbStreamer? = null
    private var wakeLock: PowerManager.WakeLock? = null

    // Battery guard: auto-stop if no PC connects for IDLE_TIMEOUT_MS. lastClientMs = the last time the
    // link was up (or the stream start), so the timeout counts from "nobody watching".
    @Volatile private var lastClientMs = 0L
    private val idleHandler = Handler(Looper.getMainLooper())
    private val idleCheck = object : Runnable {
        override fun run() {
            if (!isRunning) return
            val timeout = if (everConnected) IDLE_AFTER_DISCONNECT_MS else IDLE_TIMEOUT_MS
            if (!clientConnected && SystemClock.elapsedRealtime() - lastClientMs > timeout) {
                Log.i(TAG, "no PC for ${timeout / 60000} min — auto-stopping to save battery")
                Diag.event("idle_autostop", "timeoutMs=$timeout")
                stopStreaming()
                return
            }
            idleHandler.postDelayed(this, IDLE_CHECK_MS)
        }
    }

    // --- device status (battery) -> PC, over whichever control channel the transport already has ---
    //
    // Runs on its own background thread, NOT the main looper: pushing a status writes to a TCP socket,
    // and Android forbids network I/O on the main thread (NetworkOnMainThreadException). The thread is
    // asleep between the once-a-minute ticks.
    private var statusThread: HandlerThread? = null
    @Volatile private var statusHandler: Handler? = null
    private val statusTick = object : Runnable {
        override fun run() {
            if (!isRunning) return
            pushStatus()
            statusHandler?.postDelayed(this, STATUS_PUSH_MS)
        }
    }
    /** Charger plug/unplug is a system broadcast — an event, so nothing has to poll for it. */
    private val powerReceiver = object : android.content.BroadcastReceiver() {
        override fun onReceive(ctx: android.content.Context?, i: Intent?) {
            val connected = i?.action == Intent.ACTION_POWER_CONNECTED
            Diag.onPowerConnectionChanged(connected)
            postStatus()          // onReceive is on the main thread — hop off it before writing
        }
    }
    private var powerReceiverRegistered = false

    /** Request a status push from any thread; it runs on the status thread. */
    fun postStatus() { statusHandler?.post { pushStatus() } }

    /** Read the battery (one sticky-intent lookup) and hand it to the active transport. */
    private fun pushStatus() {
        val json = runCatching {
            PhoneStatus.encode(Diag.readBattery(applicationContext), Diag.sessionId, System.currentTimeMillis())
        }.getOrNull() ?: return
        webrtcSender?.sendStatus(json)
        usbStreamer?.sendStatus(json)
    }

    private fun startStatusReporting() {
        if (statusThread == null) {
            val t = HandlerThread("phonecam-status", android.os.Process.THREAD_PRIORITY_BACKGROUND)
            t.start()
            statusThread = t
            statusHandler = Handler(t.looper)
        }
        if (!powerReceiverRegistered) {
            val f = android.content.IntentFilter().apply {
                addAction(Intent.ACTION_POWER_CONNECTED)
                addAction(Intent.ACTION_POWER_DISCONNECTED)
            }
            androidx.core.content.ContextCompat.registerReceiver(
                this, powerReceiver, f, androidx.core.content.ContextCompat.RECEIVER_NOT_EXPORTED)
            powerReceiverRegistered = true
        }
        statusHandler?.removeCallbacks(statusTick)
        statusHandler?.postDelayed(statusTick, STATUS_PUSH_MS)
    }

    private fun stopStatusReporting() {
        statusHandler?.removeCallbacks(statusTick)
        statusHandler = null
        statusThread?.quitSafely()
        statusThread = null
        if (powerReceiverRegistered) {
            runCatching { unregisterReceiver(powerReceiver) }
            powerReceiverRegistered = false
        }
    }

    /** OFF unless the user enabled it in the app or the caller (desktop app / test script) asked. */
    private fun diagLevelFor(intent: Intent?): Diag.Level {
        val prefOn = getSharedPreferences("phonecam", MODE_PRIVATE).getBoolean(PREF_DIAG, false)
        val deep = intent?.getBooleanExtra(EXTRA_DEEP_DIAG, false) ?: false
        val on = deep || (intent?.getBooleanExtra(EXTRA_DIAG, prefOn) ?: prefOn)
        return when {
            deep -> Diag.Level.DEEP
            on -> Diag.Level.BASIC
            else -> Diag.Level.OFF
        }
    }

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
        val transport = intent?.getStringExtra(EXTRA_TRANSPORT) ?: "webrtc"
        val rawMic = intent?.getBooleanExtra(EXTRA_RAW_MIC, true) ?: true   // default = raw/fuller mic (better sounding)
        val prefs = getSharedPreferences("phonecam", MODE_PRIVATE)
        val noRotate = intent?.getBooleanExtra(EXTRA_NO_ROTATE, prefs.getBoolean(PREF_NO_ROTATE, false))
            ?: prefs.getBoolean(PREF_NO_ROTATE, false)

        startForegroundForMode(mode)
        if (!isRunning) {
            Diag.start(applicationContext, diagLevelFor(intent),
                "mode=$mode quality=${quality.name} transport=$transport rawMic=$rawMic")
        }
        if (transport == "usb") {
            val usbPort = intent?.getIntExtra(EXTRA_USB_PORT, DEFAULT_USB_PORT) ?: DEFAULT_USB_PORT
            startUsbStreaming(mode, quality, usbPort, rawMic)   // rotation strip is WebRTC-only
        } else {
            val sigHost = intent?.getStringExtra(EXTRA_SIG_HOST)
            val sigPort = intent?.getIntExtra(EXTRA_SIG_PORT, 0) ?: 0
            val sigSecret = intent?.getStringExtra(EXTRA_SIG_SECRET)
            startStreaming(mode, quality, sigHost, sigPort, sigSecret, rawMic, noRotate)
        }
        return START_STICKY
    }

    /**
     * USB transport: the phone listens on a loopback port and streams H.264 + PCM over it; the PC
     * reaches it through `adb forward` (no WebRTC, no tethering). "connected" comes from the socket
     * accept. All the media work is in [UsbStreamer].
     */
    private fun startUsbStreaming(mode: Mode, quality: Quality, port: Int, rawMic: Boolean) {
        if (isRunning) return
        clientConnected = false; everConnected = false
        try {
            val streamer = UsbStreamer(applicationContext, port,
                mode != Mode.MIC_ONLY, mode != Mode.CAMERA_ONLY, quality.w, quality.h, quality.fps, rawMic) { connected ->
                clientConnected = connected
                if (connected) everConnected = true
                lastClientMs = SystemClock.elapsedRealtime()
                Diag.event(if (connected) "pc_connected" else "pc_disconnected", "transport=usb")
                if (connected) postStatus()   // first reading right away
            }
            usbStreamer = streamer
            streamer.start()
            streamUrl = "USB (adb :$port)"
            isRunning = true
            acquireWakeLock()
            lastClientMs = SystemClock.elapsedRealtime()
            idleHandler.postDelayed(idleCheck, IDLE_CHECK_MS)
            startStatusReporting()
            Log.i(TAG, "usb streamer up ($mode, ${quality.label}) on 127.0.0.1:$port")
            updateNotification()
        } catch (e: Exception) {
            Log.e(TAG, "startUsbStreaming failed", e)
            Diag.event("start_failed", "transport=usb", "err=${e.javaClass.simpleName}")
            Diag.stop("start_failed")
            stopStatusReporting()
            releaseWakeLock()
            runCatching { usbStreamer?.stop() }
            usbStreamer = null
            isRunning = false
            stopSelf()
        }
    }

    /**
     * Answer the PC's PCAM3 offer: sendonly Opus mic and/or H.264 camera over DTLS-SRTP. The PC is the
     * offerer/listener, so "connected" comes from the WebRTC state callback. A dropped signaling socket
     * (the PC pressed Stop) surfaces as DISCONNECTED and stops us at once.
     */
    private fun startStreaming(mode: Mode, quality: Quality,
                               sigHost: String?, sigPort: Int, sigSecret: String?, rawMic: Boolean,
                               noRotate: Boolean = false) {
        if (isRunning) return
        clientConnected = false; everConnected = false

        if (sigHost.isNullOrEmpty() || sigPort !in 1..65535 || sigSecret.isNullOrEmpty()) {
            Log.e(TAG, "missing signaling target (host=$sigHost port=$sigPort) — scan the PC QR")
            stopSelf(); return
        }
        try {
            val sender = WebRtcSender(applicationContext, sigHost, sigPort, sigSecret,
                mode != Mode.MIC_ONLY, mode != Mode.CAMERA_ONLY, quality.w, quality.h, quality.fps, rawMic,
                noRotate) { state ->
                when (state) {
                    WebRtcSender.State.CONNECTED -> {
                        clientConnected = true; everConnected = true
                        lastClientMs = SystemClock.elapsedRealtime()
                        Log.i(TAG, "webrtc: connected to PC")
                        Diag.event("pc_connected", "transport=webrtc")
                    }
                    WebRtcSender.State.CONNECTING -> Log.i(TAG, "webrtc: connecting…")
                    WebRtcSender.State.DISCONNECTED, WebRtcSender.State.FAILED -> {
                        clientConnected = false
                        Log.i(TAG, "webrtc: $state — stopping")
                        Diag.event("pc_disconnected", "transport=webrtc", "state=$state")
                        idleHandler.post { stopStreaming() }
                    }
                }
            }
            // The PC announces device-status support on the signaling socket; send the first reading
            // the moment it does, so the desktop shows a battery level without waiting a minute.
            sender.onStatusChannelReady = { postStatus() }
            webrtcSender = sender
            sender.start()
            streamUrl = sigHost
            isRunning = true
            acquireWakeLock()   // keep the CPU/stream alive with the screen off (less heat than forcing it on)
            lastClientMs = SystemClock.elapsedRealtime()
            idleHandler.postDelayed(idleCheck, IDLE_CHECK_MS)   // auto-stop if no PC ever connects
            startStatusReporting()
            Log.i(TAG, "webrtc push up ($mode, ${quality.label}) to $sigHost:$sigPort")
            updateNotification()
        } catch (e: Exception) {
            // Never crash-loop the service (it is START_STICKY): stop cleanly on any start failure.
            Log.e(TAG, "startStreaming failed", e)
            Diag.event("start_failed", "transport=webrtc", "err=${e.javaClass.simpleName}")
            Diag.stop("start_failed")
            stopStatusReporting()
            releaseWakeLock()   // don't leak the CPU lock if we bail after acquiring it
            runCatching { webrtcSender?.stop() }
            webrtcSender = null
            isRunning = false
            stopSelf()
        }
    }

    /** Toggle front/back camera on the running stream (no-op in mic-only mode). */
    private fun switchCamera() {
        webrtcSender?.switchCamera()
        usbStreamer?.switchCamera()
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
        Diag.event("wakelock_acquired", "type=PARTIAL", "timeoutMs=${4 * 60 * 60 * 1000L}")
    }

    private fun releaseWakeLock() {
        val held = wakeLock?.isHeld == true
        wakeLock?.let { if (it.isHeld) it.release() }
        wakeLock = null
        if (held) Diag.event("wakelock_released")
    }

    private fun stopStreaming() {
        val wasRunning = isRunning
        idleHandler.removeCallbacks(idleCheck)
        stopStatusReporting()
        releaseWakeLock()
        runCatching { webrtcSender?.stop() }
        runCatching { usbStreamer?.stop() }
        // Closed last, so the teardown events above (wakelock release, transport stop) are still
        // recorded and the summary is genuinely the final line of the session.
        if (wasRunning) Diag.stop("stream_stopped")
        webrtcSender = null
        usbStreamer = null
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
        val text = streamUrl?.let { "Streaming to $it" } ?: "Starting…"
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
}
