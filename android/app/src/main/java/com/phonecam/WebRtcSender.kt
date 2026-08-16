package com.phonecam

import android.content.Context
import android.util.Log
import org.webrtc.AudioSource
import org.webrtc.AudioTrack
import org.webrtc.Camera2Enumerator
import org.webrtc.DataChannel
import org.webrtc.DefaultVideoDecoderFactory
import org.webrtc.DefaultVideoEncoderFactory
import org.webrtc.EglBase
import org.webrtc.IceCandidate
import org.webrtc.MediaConstraints
import org.webrtc.MediaStream
import org.webrtc.PeerConnection
import org.webrtc.PeerConnectionFactory
import org.webrtc.RtpParameters
import org.webrtc.RtpReceiver
import org.webrtc.SdpObserver
import org.webrtc.SessionDescription
import org.webrtc.SurfaceTextureHelper
import org.webrtc.VideoCapturer
import org.webrtc.VideoSource
import org.webrtc.VideoTrack
import org.webrtc.audio.JavaAudioDeviceModule
import java.io.DataInputStream
import java.io.DataOutputStream
import java.net.InetSocketAddress
import java.net.Socket
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicBoolean

/**
 * WebRTC sender — Opus mic + optional H.264 camera.
 *
 * Connects to the PC's PCAM3 TCP signaling channel, answers the PC's recvonly Opus offer with a
 * sendonly microphone track, and streams over DTLS-SRTP (LAN-direct, host ICE candidates only).
 * The PC is the WebRTC *offerer*; we *answer* (proven necessary in Phase 1 — a sendonly offer to an
 * unprepared answerer is rejected). Wire protocol mirrors webrtc_receiver.cpp exactly:
 *   [1 byte type][4-byte big-endian length][payload],  S = pairSecret, O = offer, A = answer.
 *
 * org.webrtc handles Oboe/AAudio low-latency capture, Opus, DTLS-SRTP and congestion control; we
 * only drive the signaling handshake and lifecycle.
 */
class WebRtcSender(
    private val appCtx: Context,
    private val host: String,
    private val port: Int,
    private val secret: String,
    private val withVideo: Boolean,
    private val withAudio: Boolean,
    private val videoW: Int,
    private val videoH: Int,
    private val videoFps: Int,
    private val rawMic: Boolean = false,        // true = disable HW AEC/NS (clean remote mic) (F-12)
    // Measurement only: send frames with rotation 0 so libwebrtc does not rotate pixels before
    // encoding. Off by default; see CountingCapturerObserver.
    private val stripRotation: Boolean = false,
    private val onState: (State) -> Unit,
) {
    enum class State { CONNECTING, CONNECTED, DISCONNECTED, FAILED }

    private var factory: PeerConnectionFactory? = null
    private var pc: PeerConnection? = null
    private var adm: JavaAudioDeviceModule? = null
    private var audioSource: AudioSource? = null
    private var audioTrack: AudioTrack? = null
    private var eglBase: EglBase? = null
    private var videoCapturer: VideoCapturer? = null
    private var videoSource: VideoSource? = null
    private var videoTrack: VideoTrack? = null
    private var surfaceHelper: SurfaceTextureHelper? = null
    private var socket: Socket? = null
    private var worker: Thread? = null

    private val started = AtomicBoolean(false)
    private val closed = AtomicBoolean(false)
    private val gatheringComplete = CountDownLatch(1)

    // --- device-status side channel on the signaling socket (see PhoneStatus.kt) ---
    private val sendLock = Any()
    private var sigOut: DataOutputStream? = null
    /** Set once the PC's hello arrives; until then we send nothing (old receivers don't read). */
    @Volatile private var statusSupported = false
    /** Invoked when the PC announces status support, so the service can push the first value at once. */
    @Volatile var onStatusChannelReady: (() -> Unit)? = null

    /** Counts capture frames + observes the real capture geometry, at one atomic add per frame. */
    private var captureStats: CountingCapturerObserver? = null
    private val statsSampler = Runnable { pollWebrtcStats() }

    fun start() {
        if (!started.compareAndSet(false, true)) return
        worker = Thread {
            try {
                run()
            } catch (e: Exception) {
                if (!closed.get()) {
                    Log.e(TAG, "webrtc sender failed", e)
                    onState(State.FAILED)
                }
                stop()
            }
        }.apply { isDaemon = true; name = "webrtc-sender"; start() }
    }

    private fun run() {
        ensureFactoryInit(appCtx)

        // Mic capture. Default: platform AEC/NS (voice mic). rawMic mode disables them — the phone is a
        // standalone remote mic with no local playback to echo-cancel, so AEC is dead weight. (F-12)
        adm = JavaAudioDeviceModule.builder(appCtx)
            .setUseHardwareAcousticEchoCanceler(!rawMic)
            .setUseHardwareNoiseSuppressor(!rawMic)
            .createAudioDeviceModule()
        // Hardware H.264 (via MediaCodec) so the PC's H264 offer has a matching encoder; the EGL
        // context is shared with the camera capture path. VP8/VP9 stay enabled as fallback.
        val egl = EglBase.create()
        eglBase = egl
        // Enumerate ALL local interfaces via the native path (getifaddrs), NOT Android's
        // ConnectivityManager network monitor. The monitor only reports networks the phone itself
        // uses for egress and omits the USB-tethering *downstream* interface (rndis0/ncm0 at
        // 192.168.42.129) — so with it on, the phone never gathers a USB-tethering ICE candidate and
        // WebRTC-over-USB can't form a pair. Disabling it trades mid-session network-change handling
        // (irrelevant for short LAN sessions) for seeing the USB interface. Ignore cellular/VPN so
        // media can only ever take a LAN or USB path, never metered mobile data.
        val opts = PeerConnectionFactory.Options().apply {
            disableNetworkMonitor = true
            networkIgnoreMask = PeerConnectionFactory.Options.ADAPTER_TYPE_CELLULAR or
                PeerConnectionFactory.Options.ADAPTER_TYPE_VPN or
                PeerConnectionFactory.Options.ADAPTER_TYPE_LOOPBACK
        }
        factory = PeerConnectionFactory.builder()
            .setOptions(opts)
            .setAudioDeviceModule(adm)
            .setVideoEncoderFactory(DefaultVideoEncoderFactory(egl.eglBaseContext, true, true))
            .setVideoDecoderFactory(DefaultVideoDecoderFactory(egl.eglBaseContext))
            .createPeerConnectionFactory()

        // No STUN/TURN: LAN-direct, host candidates only. The DTLS fingerprints in the SDP are the
        // pinned identity; pairSecret gated the signaling channel.
        val cfg = PeerConnection.RTCConfiguration(emptyList()).apply {
            sdpSemantics = PeerConnection.SdpSemantics.UNIFIED_PLAN
            continualGatheringPolicy = PeerConnection.ContinualGatheringPolicy.GATHER_ONCE
        }
        val peer = factory!!.createPeerConnection(cfg, observer)
            ?: throw IllegalStateException("createPeerConnection returned null")
        pc = peer

        // Sendonly mic track. addTrack before setRemoteDescription: Unified Plan associates this
        // transceiver with the offer's audio m-line, and the negotiated answer direction becomes
        // sendonly (our sendrecv ∩ their recvonly-offer).
        // Mic track only when the mode wants it — Camera-only must NOT leak the mic.
        if (withAudio) {
            val constraints = MediaConstraints().apply {
                for ((k, v) in audioConstraintPairs(rawMic)) mandatory.add(MediaConstraints.KeyValuePair(k, v))
            }
            val src = factory!!.createAudioSource(constraints)
            audioSource = src
            val track = factory!!.createAudioTrack("mic0", src).apply { setEnabled(true) }
            audioTrack = track
            peer.addTrack(track, listOf("pcam"))
            Diag.event("audio_started", "transport=webrtc", "rawMic=${if (rawMic) 1 else 0}",
                "apm=${if (rawMic) "off" else "on"}")
        }

        // Camera → H.264 video track (added after audio to match the PC offer's m-line order).
        // The track must exist before setRemoteDescription (Unified Plan matches it to the offer's
        // video m-line), but the CAMERA does not have to be running yet — see startCapture() below.
        if (withVideo) startCamera(peer, egl)

        // --- signaling handshake over TCP ---
        onState(State.CONNECTING)
        val s = connectSignaling()
        if (closed.get()) return
        // Open the camera only now. connectSignaling() retries every 2 s for as long as the stream is
        // up (the service's idle guard allows 5 minutes), and until this point there is nowhere for a
        // frame to go — so starting capture earlier ran the sensor, ISP and GPU texture path at full
        // rate, throwing every frame away. This is the single largest avoidable draw on the Wi-Fi
        // path when the desktop app isn't listening yet (the "reconnect to last PC" flow). It costs no
        // user-visible latency: camera open overlaps the SDP exchange, ICE and the DTLS handshake.
        startCapture()
        val out = DataOutputStream(s.getOutputStream())
        sigOut = out
        val inp = DataInputStream(s.getInputStream())
        sendMsg(out, 'S', secret.toByteArray(Charsets.UTF_8))

        val (type, payload) = recvMsg(inp) ?: throw IllegalStateException("no offer from PC")
        if (type != 'O') throw IllegalStateException("expected offer 'O', got '$type'")
        Log.i(TAG, "webrtc: got offer, answering")
        setRemote(SessionDescription(SessionDescription.Type.OFFER, String(payload, Charsets.UTF_8)))

        val answer = createAnswer()
        setLocal(answer)

        // Non-trickle: wait for gathering to finish, then send the localDescription with candidates inline.
        if (!gatheringComplete.await(6, TimeUnit.SECONDS))
            Log.w(TAG, "webrtc: ICE gathering slow — sending answer with candidates gathered so far")
        val local = pc?.localDescription ?: throw IllegalStateException("no local description")
        sendMsg(out, 'A', local.description.toByteArray(Charsets.UTF_8))
        Log.i(TAG, "webrtc: answer sent — media negotiating")
        Diag.event("transport_connected", "transport=webrtc", "sdpBytes=${local.description.length}")
        if (Diag.on) {
            Diag.addSampler(statsSampler)
            pollWebrtcStats()   // prime it, so the first 30 s sample already has real encode numbers
        }

        // The signaling socket stays open for the rest of the session. Historically it was only read
        // to notice EOF (the PC pressing Stop); it is now also the control channel: the PC may send a
        // hello ('V'), a problem mark ('M') or a keyframe hint ('K'), and we push device status ('B')
        // back on it. Anything unknown is skipped by length, so either side can add message types.
        try {
            s.soTimeout = 0
            while (!closed.get()) {
                val (t, payload) = recvMsg(inp) ?: break   // EOF: PC closed the signaling channel
                handleControl(t, payload)
            }
        } catch (e: Exception) {
            // socket closed under us — treat as PC gone
        }
        if (!closed.get()) {
            Log.i(TAG, "webrtc: signaling closed (PC stopped) — stopping")
            onState(State.DISCONNECTED)
            stop()
        }
    }

    /** PC → phone control messages on the signaling socket. Unknown types are ignored by design. */
    private fun handleControl(type: Char, payload: ByteArray) {
        when (type) {
            PhoneStatus.MSG_HELLO -> {
                val body = String(payload, Charsets.UTF_8)
                if (PhoneStatus.helloSupportsStatus(body)) {
                    statusSupported = true
                    Diag.event("status_channel_ready", "peer=pc")
                    runCatching { onStatusChannelReady?.invoke() }
                }
            }
            PhoneStatus.MSG_MARK -> {
                Diag.c.problemMarks.incrementAndGet()
                // Correlates the PC operator's "I can see it now" with this phone's event stream.
                Diag.event("problem_mark", "source=pc", "note=" + String(payload, Charsets.UTF_8).take(64))
            }
            PhoneStatus.MSG_KEYFRAME -> {
                // WebRTC signals keyframe requests natively over RTCP (PLI); this is only informational.
                Diag.c.keyframeRequests.incrementAndGet()
                Diag.deep("keyframe_requested", "source=pc-control")
            }
            else -> Diag.deep("control_unknown", "type=$type", "len=${payload.size}")
        }
    }

    /**
     * Push one device-status message. No-op until the PC says it understands them, so an older
     * receiver never sees bytes it would not read. One ~90-byte write per minute.
     */
    fun sendStatus(json: String): Boolean {
        if (!statusSupported || closed.get()) return false
        val out = sigOut ?: return false
        return runCatching {
            sendMsg(out, PhoneStatus.MSG_STATUS, json.toByteArray(Charsets.UTF_8))
            Diag.c.statusPushes.incrementAndGet()
            true
        }.getOrElse {
            Diag.c.socketWriteErrors.incrementAndGet()
            false
        }
    }

    fun stop() {
        if (!closed.compareAndSet(false, true)) return
        Diag.removeSampler(statsSampler)
        statusSupported = false
        runCatching { socket?.close() }
        worker?.interrupt()
        // Tear down WebRTC off the caller's thread — dispose() blocks on WebRTC's threads.
        Thread {
            runCatching { videoCapturer?.stopCapture() }
            runCatching { pc?.dispose() }
            runCatching { audioTrack?.dispose() }
            runCatching { audioSource?.dispose() }
            runCatching { videoTrack?.dispose() }
            runCatching { videoSource?.dispose() }
            runCatching { videoCapturer?.dispose() }
            runCatching { surfaceHelper?.dispose() }
            runCatching { adm?.release() }
            runCatching { eglBase?.release() }
            pc = null; audioTrack = null; audioSource = null; adm = null; factory = null
            videoTrack = null; videoSource = null; videoCapturer = null; surfaceHelper = null; eglBase = null
        }.apply { isDaemon = true }.start()
    }

    /** Start Camera2 capture → a sendonly H.264 video track (back camera preferred, for webcam use). */
    private fun startCamera(peer: PeerConnection, egl: EglBase) {
        val enumerator = Camera2Enumerator(appCtx)
        val names = enumerator.deviceNames
        val camName = names.firstOrNull { enumerator.isBackFacing(it) } ?: names.firstOrNull()
        if (camName == null) { Log.w(TAG, "webrtc: no camera available — audio only"); return }
        val capturer = enumerator.createCapturer(camName, null)
        videoCapturer = capturer
        val helper = SurfaceTextureHelper.create("CaptureThread", egl.eglBaseContext)
        surfaceHelper = helper
        val vsrc = factory!!.createVideoSource(false)   // isScreencast = false
        videoSource = vsrc
        // Count capture frames and record the geometry the camera ACTUALLY produced (which can differ
        // from the request — Camera2Enumerator snaps to a supported format). One atomic add per frame.
        // Only inserted when diagnostics are on, so the production capture path is unchanged.
        val obs = if (Diag.on || stripRotation)
                      CountingCapturerObserver(vsrc.capturerObserver, stripRotation).also { captureStats = it }
                  else vsrc.capturerObserver
        capturer.initialize(helper, appCtx, obs)
        Diag.event("camera_prepared", "transport=webrtc", "cam=$camName",
            "reqW=$videoW", "reqH=$videoH", "reqFps=$videoFps")
        val vtrack = factory!!.createVideoTrack("cam0", vsrc).apply { setEnabled(true) }
        videoTrack = vtrack
        val sender = peer.addTrack(vtrack, listOf("pcam"))
        // Low-latency intent: on a weak link keep motion smooth and let WebRTC shed resolution rather
        // than framerate. Also cut the initial low-res ramp by raising the congestion controller's start
        // estimate and capping max; min is left unset so a weak link can still shed bitrate and never
        // starve audio. Best-effort — falls back to WebRTC's defaults if the API differs. (F-13)
        runCatching {
            val maxBps = (videoW.toLong() * videoH * videoFps / 8).toInt().coerceIn(2_000_000, 20_000_000)
            peer.setBitrate(null, 2_000_000, maxBps)   // (min, start, max) — bump the BWE start estimate
            val p = sender.parameters
            p.degradationPreference = RtpParameters.DegradationPreference.MAINTAIN_FRAMERATE
            p.encodings.firstOrNull()?.let { it.maxBitrateBps = maxBps }
            sender.parameters = p
        }
        Log.i(TAG, "webrtc: camera track added ($camName, ${videoW}x${videoH}@${videoFps})")
    }

    /**
     * Actually open the camera and start delivering frames. Split out of [startCamera] so the sensor
     * only spins up once we have a PC on the other end of the signaling socket. The selected Quality
     * preset caps capture; WebRTC then sheds resolution under congestion.
     */
    private fun startCapture() {
        val capturer = videoCapturer ?: return
        runCatching { capturer.startCapture(videoW, videoH, videoFps) }
            .onSuccess { Diag.event("camera_started", "transport=webrtc", "w=$videoW", "h=$videoH", "fps=$videoFps") }
            .onFailure { Log.e(TAG, "webrtc: startCapture failed", it); Diag.event("camera_start_failed", "err=${it.javaClass.simpleName}") }
    }

    /** Toggle front/back camera on the running capture (no-op in audio-only mode). */
    fun switchCamera() {
        (videoCapturer as? org.webrtc.CameraVideoCapturer)?.let {
            runCatching { it.switchCamera(null) }.onFailure { e -> Log.w(TAG, "switchCamera failed", e) }
        }
    }

    /** Keep a saved-PC reconnect alive when the phone starts before the desktop listener. */
    private fun connectSignaling(): Socket {
        while (!closed.get()) {
            val candidate = Socket().apply { tcpNoDelay = true }
            socket = candidate
            try {
                candidate.connect(InetSocketAddress(host, port), SIGNAL_CONNECT_TIMEOUT_MS)
                Log.i(TAG, "webrtc: signaling connected to $host:$port")
                return candidate
            } catch (e: Exception) {
                runCatching { candidate.close() }
                if (closed.get()) break
                Log.i(TAG, "webrtc: PC not listening yet; retrying")
                Thread.sleep(SIGNAL_RETRY_MS)
            }
        }
        throw InterruptedException("WebRTC sender stopped")
    }

    /**
     * Pass-through [org.webrtc.CapturerObserver] that only counts. Sits between the capturer and the
     * VideoSource so we can report *observed* capture fps/resolution instead of the requested one —
     * the single most useful number for "is the camera doing more work than we transmit?".
     */
    private class CountingCapturerObserver(
        private val delegate: org.webrtc.CapturerObserver,
        /**
         * Measurement mode: hand every frame downstream with rotation 0, so libwebrtc has no rotation
         * to bake into the pixels before encoding.
         *
         * Why this exists: the camera sensor is landscape-native, so a phone held in portrait makes
         * libwebrtc rotate every frame 90° before encode (visible as capGeom 1280x720 but an encoded
         * 720x1280). libdatachannel never offers the `urn:3gpp:video-orientation` extension, so the
         * rotation cannot be signalled in RTP and has to be applied to the image.
         *
         * Trying to remove that by physically holding the phone sideways does not give a clean
         * measurement — frame rate, lighting and thermal state all move at the same time, which is
         * exactly what confounded the first attempt. Toggling it here changes ONE variable with the
         * phone untouched, which is the only way to price it honestly.
         *
         * The received image arrives sideways while this is on; the receiver's Rotate control fixes
         * the view. That is why it lives under Diagnostics and is off by default.
         */
        private val stripRotation: Boolean,
    ) : org.webrtc.CapturerObserver {
        @Volatile var lastW = 0
        @Volatile var lastH = 0
        override fun onCapturerStarted(success: Boolean) {
            Diag.event("camera_capture_started", "ok=${if (success) 1 else 0}",
                "stripRotation=${if (stripRotation) 1 else 0}")
            delegate.onCapturerStarted(success)
        }
        override fun onCapturerStopped() {
            Diag.event("camera_capture_stopped")
            delegate.onCapturerStopped()
        }
        override fun onFrameCaptured(frame: org.webrtc.VideoFrame) {
            if (Diag.on) {   // one volatile read when diagnostics are off; nothing else
                Diag.c.cameraFrames.incrementAndGet()
                lastW = frame.buffer.width; lastH = frame.buffer.height
            }
            if (!stripRotation || frame.rotation == 0) {
                delegate.onFrameCaptured(frame)
                return
            }
            // Re-wrap the SAME buffer with rotation 0. VideoFrame's constructor does not take a
            // reference, so retain before and release after: the capturer's own reference is
            // untouched and the buffer cannot be freed under the consumer.
            frame.buffer.retain()
            val unrotated = org.webrtc.VideoFrame(frame.buffer, 0, frame.timestampNs)
            try { delegate.onFrameCaptured(unrotated) } finally { unrotated.release() }
        }
    }

    /**
     * Pull the WebRTC stack's own statistics rather than reinventing them (W3C `RTCStatsReport`).
     * Runs on the diagnostics sample tick (30 s by default), so the cost is one async collection per
     * half-minute. `outbound-rtp` tells us what we really encoded and sent, `remote-inbound-rtp`
     * carries the PC's loss/jitter/RTT report, and `encoderImplementation` proves hardware vs software.
     */
    private fun pollWebrtcStats() {
        val peer = pc ?: return
        runCatching {
            peer.getStats { report ->
                val sb = StringBuilder(220)
                var any = false
                for (s in report.statsMap.values) {
                    val m = s.members
                    when (s.type) {
                        "outbound-rtp" -> {
                            any = true
                            val kind = (m["kind"] ?: m["mediaType"])?.toString() ?: "?"
                            sb.append(" out.").append(kind).append("[")
                            sb.append("bytes=").append(m["bytesSent"])
                            sb.append(" pkts=").append(m["packetsSent"])
                            if (kind == "video") {
                                // Mirror the stack's cumulative encode counters into our own, so the
                                // sample line's encFps/encKbps are real on this path too. libwebrtc
                                // owns the encoder here (unlike the USB path, where we drive
                                // MediaCodec ourselves and count in its callback), so without this
                                // they sat at zero and read as "the encoder is doing nothing".
                                (m["framesEncoded"] as? Number)?.let { Diag.c.framesEncoded.set(it.toLong()) }
                                (m["keyFramesEncoded"] as? Number)?.let { Diag.c.keyFrames.set(it.toLong()) }
                                (m["bytesSent"] as? Number)?.let { Diag.c.encodedBytes.set(it.toLong()) }
                                sb.append(" frames=").append(m["framesEncoded"])
                                sb.append(" key=").append(m["keyFramesEncoded"])
                                sb.append(" fps=").append(m["framesPerSecond"])
                                sb.append(" ").append(m["frameWidth"]).append("x").append(m["frameHeight"])
                                sb.append(" target=").append(m["targetBitrate"])
                                sb.append(" limit=").append(m["qualityLimitationReason"])
                                sb.append(" enc=").append(m["encoderImplementation"])
                                sb.append(" pli=").append(m["pliCount"])
                                sb.append(" nack=").append(m["nackCount"])
                                sb.append(" fir=").append(m["firCount"])
                                sb.append(" encTime=").append(m["totalEncodeTime"])
                            }
                            sb.append("]")
                        }
                        "remote-inbound-rtp" -> {
                            any = true
                            sb.append(" rin[").append("lost=").append(m["packetsLost"])
                            sb.append(" frac=").append(m["fractionLost"])
                            sb.append(" jitter=").append(m["jitter"])
                            sb.append(" rtt=").append(m["roundTripTime"]).append("]")
                        }
                        "candidate-pair" -> {
                            if (m["nominated"] == true || m["state"] == "succeeded") {
                                any = true
                                sb.append(" pair[rtt=").append(m["currentRoundTripTime"])
                                sb.append(" avail=").append(m["availableOutgoingBitrate"]).append("]")
                            }
                        }
                    }
                }
                if (any) {
                    val cs = captureStats
                    if (cs != null) sb.append(" capGeom=").append(cs.lastW).append("x").append(cs.lastH)
                    Diag.event("webrtc_stats", sb.toString().trim())
                }
            }
        }
    }

    // --- PeerConnection.Observer ---
    private val observer = object : PeerConnection.Observer {
        override fun onSignalingChange(s: PeerConnection.SignalingState?) {}
        override fun onIceConnectionChange(state: PeerConnection.IceConnectionState?) {
            Log.i(TAG, "webrtc ice: $state")
            Diag.event("ice_state", "state=$state")
            when (state) {
                PeerConnection.IceConnectionState.CONNECTED,
                PeerConnection.IceConnectionState.COMPLETED -> onState(State.CONNECTED)
                PeerConnection.IceConnectionState.FAILED -> { onState(State.FAILED); stop() }
                PeerConnection.IceConnectionState.DISCONNECTED,
                PeerConnection.IceConnectionState.CLOSED -> onState(State.DISCONNECTED)
                else -> {}
            }
        }
        override fun onConnectionChange(state: PeerConnection.PeerConnectionState?) {
            if (state == PeerConnection.PeerConnectionState.FAILED) { onState(State.FAILED); stop() }
        }
        override fun onIceConnectionReceivingChange(receiving: Boolean) {}
        override fun onIceGatheringChange(state: PeerConnection.IceGatheringState?) {
            if (state == PeerConnection.IceGatheringState.COMPLETE) gatheringComplete.countDown()
        }
        override fun onIceCandidate(c: IceCandidate?) {                   // non-trickle (inline in the SDP); logged to see which interfaces we gather (Wi-Fi vs USB rndis0)
            c?.let { Log.i(TAG, "ice cand: ${it.sdp}") }
        }
        override fun onIceCandidatesRemoved(c: Array<out IceCandidate>?) {}
        override fun onAddStream(s: MediaStream?) {}
        override fun onRemoveStream(s: MediaStream?) {}
        override fun onDataChannel(d: DataChannel?) {}
        override fun onRenegotiationNeeded() {}
        override fun onAddTrack(r: RtpReceiver?, s: Array<out MediaStream>?) {}
    }

    // --- SdpObserver bridges (WebRTC is async; block the worker thread with a latch) ---
    private fun setRemote(sdp: SessionDescription) = awaitSet("setRemote") { obs -> pc!!.setRemoteDescription(obs, sdp) }
    private fun setLocal(sdp: SessionDescription)  = awaitSet("setLocal")  { obs -> pc!!.setLocalDescription(obs, sdp) }

    private fun awaitSet(what: String, call: (SdpObserver) -> Unit) {
        val latch = CountDownLatch(1)
        var err: String? = null
        call(object : SdpObserver {
            override fun onCreateSuccess(p0: SessionDescription?) {}
            override fun onSetSuccess() { latch.countDown() }
            override fun onCreateFailure(p0: String?) { err = p0; latch.countDown() }
            override fun onSetFailure(p0: String?) { err = p0; latch.countDown() }
        })
        if (!latch.await(6, TimeUnit.SECONDS)) throw IllegalStateException("$what timed out")
        err?.let { throw IllegalStateException("$what: $it") }
    }

    private fun createAnswer(): SessionDescription {
        val latch = CountDownLatch(1)
        var result: SessionDescription? = null
        var err: String? = null
        pc!!.createAnswer(object : SdpObserver {
            override fun onCreateSuccess(sdp: SessionDescription?) { result = sdp; latch.countDown() }
            override fun onSetSuccess() {}
            override fun onCreateFailure(p0: String?) { err = p0; latch.countDown() }
            override fun onSetFailure(p0: String?) {}
        }, MediaConstraints())
        if (!latch.await(6, TimeUnit.SECONDS)) throw IllegalStateException("createAnswer timed out")
        return result ?: throw IllegalStateException("createAnswer: $err")
    }

    // --- PCAM3 framing (mirrors webrtc_receiver.cpp): [type][4-byte BE len][payload] ---
    // Serialized: the handshake writes come from the worker thread and status pushes from the
    // service's status thread. They cannot actually overlap (statusSupported is only set after the
    // answer is sent), but framing corruption is not a failure mode worth reasoning about twice.
    private fun sendMsg(out: DataOutputStream, type: Char, payload: ByteArray) {
        synchronized(sendLock) {
            out.writeByte(type.code)
            out.writeInt(payload.size)     // DataOutputStream.writeInt is big-endian
            out.write(payload)
            out.flush()
        }
    }

    private fun recvMsg(inp: DataInputStream): Pair<Char, ByteArray>? {
        val type = inp.read()
        if (type < 0) return null
        val len = inp.readInt()
        if (len < 0 || len > 10_000_000) return null
        val buf = ByteArray(len)
        inp.readFully(buf)
        return Pair(type.toChar(), buf)
    }

    companion object {
        private const val TAG = "PhoneCam"
        private const val SIGNAL_CONNECT_TIMEOUT_MS = 3000
        private const val SIGNAL_RETRY_MS = 2000L
        private val factoryInited = AtomicBoolean(false)

        /**
         * Audio-source constraints for the two mic modes.
         *
         * `rawMic = true` (the default — the desktop app's "Phone-call noise filter" unchecked) turns
         * libwebrtc's **software** audio processing off: echo cancellation, noise suppression, auto
         * gain and the high-pass filter. Measured on a Pixel 9 Pro XL, that processing costs ~12.7
         * percentage points of one CPU core — about a quarter of the app's total CPU — and a phone
         * used as a standalone remote mic has no local playback to echo-cancel, so AEC in particular
         * is pure overhead. This is the software half of the same decision F-12 already made for the
         * *hardware* AEC/NS on the audio device module.
         *
         * `rawMic = false` leaves everything on: the user ticked the box precisely because they are in
         * a noisy room and want the processing.
         *
         * Both the legacy `goog*` names and the modern short names are set — libwebrtc has accepted
         * both spellings across versions, and an unrecognised key is ignored rather than fatal.
         */
        fun audioConstraintPairs(rawMic: Boolean): List<Pair<String, String>> {
            if (!rawMic) return emptyList()   // keep libwebrtc's defaults = all processing enabled
            val off = "false"
            return listOf(
                "googEchoCancellation" to off, "echoCancellation" to off,
                "googNoiseSuppression" to off, "noiseSuppression" to off,
                "googAutoGainControl" to off,  "autoGainControl" to off,
                "googHighpassFilter" to off,
                "googTypingNoiseDetection" to off,
            )
        }

        /** PeerConnectionFactory.initialize must run once per process before any factory is built. */
        private fun ensureFactoryInit(ctx: Context) {
            if (factoryInited.compareAndSet(false, true)) {
                PeerConnectionFactory.initialize(
                    PeerConnectionFactory.InitializationOptions.builder(ctx.applicationContext)
                        .createInitializationOptions()
                )
            }
        }
    }
}
