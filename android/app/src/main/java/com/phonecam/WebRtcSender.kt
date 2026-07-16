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

        // Mic capture with the platform AEC/NS (this is a voice mic, not music).
        adm = JavaAudioDeviceModule.builder(appCtx)
            .setUseHardwareAcousticEchoCanceler(true)
            .setUseHardwareNoiseSuppressor(true)
            .createAudioDeviceModule()
        // Hardware H.264 (via MediaCodec) so the PC's H264 offer has a matching encoder; the EGL
        // context is shared with the camera capture path. VP8/VP9 stay enabled as fallback.
        val egl = EglBase.create()
        eglBase = egl
        factory = PeerConnectionFactory.builder()
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
            val src = factory!!.createAudioSource(MediaConstraints())
            audioSource = src
            val track = factory!!.createAudioTrack("mic0", src).apply { setEnabled(true) }
            audioTrack = track
            peer.addTrack(track, listOf("pcam"))
        }

        // Camera → H.264 video track (added after audio to match the PC offer's m-line order).
        if (withVideo) startCamera(peer, egl)

        // --- signaling handshake over TCP ---
        onState(State.CONNECTING)
        val s = connectSignaling()
        if (closed.get()) return
        val out = DataOutputStream(s.getOutputStream())
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

        // Keep the signaling socket open only to notice the PC stopping: it closes the socket on Stop,
        // so a blocking read returning EOF is our cue to shut the stream down at once.
        try {
            s.soTimeout = 0
            while (!closed.get()) {
                if (inp.read() < 0) break   // PC closed the signaling channel
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

    fun stop() {
        if (!closed.compareAndSet(false, true)) return
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
        capturer.initialize(helper, appCtx, vsrc.capturerObserver)
        // The selected Quality preset caps capture; WebRTC then sheds resolution under congestion.
        capturer.startCapture(videoW, videoH, videoFps)
        val vtrack = factory!!.createVideoTrack("cam0", vsrc).apply { setEnabled(true) }
        videoTrack = vtrack
        val sender = peer.addTrack(vtrack, listOf("pcam"))
        // Low-latency intent: on a weak link keep motion smooth and let WebRTC shed resolution
        // rather than framerate. Best-effort — falls back to WebRTC's default if the API differs.
        runCatching {
            val p = sender.parameters
            p.degradationPreference = RtpParameters.DegradationPreference.MAINTAIN_FRAMERATE
            sender.parameters = p
        }
        Log.i(TAG, "webrtc: camera track added ($camName, ${videoW}x${videoH}@${videoFps})")
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

    // --- PeerConnection.Observer ---
    private val observer = object : PeerConnection.Observer {
        override fun onSignalingChange(s: PeerConnection.SignalingState?) {}
        override fun onIceConnectionChange(state: PeerConnection.IceConnectionState?) {
            Log.i(TAG, "webrtc ice: $state")
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
        override fun onIceCandidate(c: IceCandidate?) {}                  // non-trickle: inline in the SDP
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
    private fun sendMsg(out: DataOutputStream, type: Char, payload: ByteArray) {
        out.writeByte(type.code)
        out.writeInt(payload.size)     // DataOutputStream.writeInt is big-endian
        out.write(payload)
        out.flush()
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
