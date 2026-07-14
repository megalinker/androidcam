package com.phonecam

import android.content.Context
import android.util.Log
import org.webrtc.AudioSource
import org.webrtc.AudioTrack
import org.webrtc.DataChannel
import org.webrtc.IceCandidate
import org.webrtc.MediaConstraints
import org.webrtc.MediaStream
import org.webrtc.PeerConnection
import org.webrtc.PeerConnectionFactory
import org.webrtc.RtpReceiver
import org.webrtc.SdpObserver
import org.webrtc.SessionDescription
import org.webrtc.audio.JavaAudioDeviceModule
import java.io.DataInputStream
import java.io.DataOutputStream
import java.net.InetSocketAddress
import java.net.Socket
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicBoolean

/**
 * Mic-only WebRTC sender — Phase 3 of docs/webrtc-migration.md.
 *
 * Connects to the PC's PCAM3 TCP signaling channel, answers the PC's recvonly Opus offer with a
 * sendonly microphone track, and streams over DTLS-SRTP (LAN-direct, host ICE candidates only).
 * The PC is the WebRTC *offerer*; we *answer* (proven necessary in Phase 1 — a sendonly offer to an
 * unprepared answerer is rejected). Wire protocol mirrors webrtc_receiver.cpp exactly:
 *   [1 byte type][4-byte big-endian length][payload],  S = pairSecret, O = offer, A = answer.
 *
 * org.webrtc handles Oboe/AAudio low-latency capture, Opus, DTLS-SRTP and congestion control; we
 * only drive the signaling handshake and lifecycle. Coexists with the RTSP/SRT paths.
 */
class WebRtcSender(
    private val appCtx: Context,
    private val host: String,
    private val port: Int,
    private val secret: String,
    private val onState: (State) -> Unit,
) {
    enum class State { CONNECTING, CONNECTED, DISCONNECTED, FAILED }

    private var factory: PeerConnectionFactory? = null
    private var pc: PeerConnection? = null
    private var adm: JavaAudioDeviceModule? = null
    private var audioSource: AudioSource? = null
    private var audioTrack: AudioTrack? = null
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
                Log.e(TAG, "webrtc sender failed", e)
                onState(State.FAILED)
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
        factory = PeerConnectionFactory.builder()
            .setAudioDeviceModule(adm)
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
        val src = factory!!.createAudioSource(MediaConstraints())
        audioSource = src
        val track = factory!!.createAudioTrack("mic0", src).apply { setEnabled(true) }
        audioTrack = track
        peer.addTrack(track, listOf("pcam"))

        // --- signaling handshake over TCP ---
        val s = Socket()
        s.tcpNoDelay = true
        s.connect(InetSocketAddress(host, port), 5000)
        socket = s
        val out = DataOutputStream(s.getOutputStream())
        val inp = DataInputStream(s.getInputStream())
        onState(State.CONNECTING)
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
        // Tear down WebRTC off the caller's thread — dispose() blocks on WebRTC's threads.
        Thread {
            runCatching { pc?.dispose() }
            runCatching { audioTrack?.dispose() }
            runCatching { audioSource?.dispose() }
            runCatching { adm?.release() }
            pc = null; audioTrack = null; audioSource = null; adm = null; factory = null
        }.apply { isDaemon = true }.start()
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
