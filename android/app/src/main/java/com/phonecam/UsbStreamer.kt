package com.phonecam

import android.Manifest
import android.annotation.SuppressLint
import android.content.Context
import android.content.pm.PackageManager
import android.hardware.camera2.CameraCaptureSession
import android.hardware.camera2.CameraCharacteristics
import android.hardware.camera2.CameraDevice
import android.hardware.camera2.CameraManager
import android.hardware.camera2.CaptureRequest
import android.media.AudioFormat
import android.media.AudioRecord
import android.media.MediaCodec
import android.media.MediaCodecInfo
import android.media.MediaFormat
import android.media.MediaRecorder
import android.os.Handler
import android.os.HandlerThread
import android.util.Log
import android.view.Surface
import androidx.core.content.ContextCompat
import org.json.JSONObject
import java.io.BufferedOutputStream
import java.io.OutputStream
import java.net.InetSocketAddress
import java.net.ServerSocket
import java.net.Socket
import java.nio.ByteBuffer
import java.util.concurrent.atomic.AtomicBoolean

/**
 * USB media path — a scrcpy-style low-latency pipe over the cable, no WebRTC and no tethering.
 *
 * The camera feeds a MediaCodec H.264 encoder directly through its input Surface (GPU path, no CPU
 * copy); the mic is captured as raw PCM (USB has bandwidth to spare, so we skip an audio codec and
 * its latency). Both are framed onto a single TCP socket that the PC reaches through `adb forward`.
 * The phone LISTENS (like the old RTSP server did) and the PC connects through the forwarded port.
 *
 * Wire frame:  [1B type][8B ptsUs BE][4B len BE][payload]
 *   'H' one JSON header (codec, geometry, audio rate/channels) sent first
 *   'V' H.264 Annex-B (the codec-config SPS/PPS buffer is sent as the first 'V' frame)
 *   'A' interleaved PCM S16LE
 * Frames are written under one lock so the video and audio threads never interleave a payload.
 */
class UsbStreamer(
    private val appCtx: Context,
    private val port: Int,
    private val withVideo: Boolean,
    private val withAudio: Boolean,
    private val videoW: Int,
    private val videoH: Int,
    private val videoFps: Int,
    private val onClient: (Boolean) -> Unit,   // true once a PC connects, false when it drops
) {
    private val closed = AtomicBoolean(false)
    private var serverSocket: ServerSocket? = null
    private var socket: Socket? = null
    private var out: OutputStream? = null
    private val writeLock = Any()

    private var acceptThread: Thread? = null

    // video
    private var encoder: MediaCodec? = null
    private var encThread: HandlerThread? = null   // owns the encoder callback (must NOT be the main thread — it does socket writes)
    private var inputSurface: Surface? = null
    private var camera: CameraDevice? = null
    private var captureSession: CameraCaptureSession? = null
    private var camThread: HandlerThread? = null
    private var camHandler: Handler? = null
    @Volatile private var useBackCamera = true   // toggled by switchCamera() (manual)

    // audio
    private var audioThread: Thread? = null

    fun start() {
        acceptThread = Thread {
            try {
                val srv = ServerSocket()
                srv.reuseAddress = true
                srv.bind(InetSocketAddress("127.0.0.1", port))   // adb forward reaches loopback on the device
                serverSocket = srv
                Log.i(TAG, "usb: listening on 127.0.0.1:$port (waiting for the PC through adb)")
                while (!closed.get()) {
                    val s = try { srv.accept() } catch (e: Exception) { break }
                    handleClient(s)   // one client at a time; returns when it drops
                    if (!closed.get()) { onClient(false) }
                }
            } catch (e: Exception) {
                if (!closed.get()) Log.e(TAG, "usb: accept loop failed", e)
            }
        }.apply { isDaemon = true; name = "usb-accept"; start() }
    }

    private fun handleClient(s: Socket) {
        try {
            s.tcpNoDelay = true
            socket = s
            out = BufferedOutputStream(s.getOutputStream(), 64 * 1024)
            Log.i(TAG, "usb: PC connected")
            onClient(true)

            // Header first, so the PC can configure its decoders/sinks before any media.
            val meta = JSONObject()
                .put("v", 1)
                .put("vcodec", if (withVideo) "h264" else "none")
                .put("w", videoW).put("h", videoH).put("fps", videoFps)
                .put("arate", AUDIO_RATE).put("achannels", 1)
                .put("audio", if (withAudio) "pcm_s16le" else "none")
                .toString()
            writeFrame('H', 0, meta.toByteArray(Charsets.UTF_8))

            if (withVideo) startVideo()
            if (withAudio) startAudio()

            // Block until the PC drops the socket (read returns EOF) or we're stopped.
            val inp = s.getInputStream()
            while (!closed.get()) { if (inp.read() < 0) break }
        } catch (e: Exception) {
            if (!closed.get()) Log.w(TAG, "usb: client ended", e)
        } finally {
            stopVideo()
            stopAudio()
            runCatching { out?.flush() }
            runCatching { s.close() }
            socket = null; out = null
            Log.i(TAG, "usb: PC disconnected")
        }
    }

    // --- framing ---
    private val hdr = ByteArray(13)
    private fun writeFrame(type: Char, ptsUs: Long, payload: ByteArray, offset: Int = 0, len: Int = payload.size) {
        val o = out ?: return
        synchronized(writeLock) {
            hdr[0] = type.code.toByte()
            var p = ptsUs
            for (i in 8 downTo 1) { hdr[i] = (p and 0xff).toByte(); p = p ushr 8 }
            hdr[9] = (len ushr 24).toByte(); hdr[10] = (len ushr 16).toByte()
            hdr[11] = (len ushr 8).toByte(); hdr[12] = len.toByte()
            o.write(hdr)
            o.write(payload, offset, len)
            o.flush()   // flush per frame: this is a latency path, not a throughput one
        }
    }

    // --- video: Camera2 -> encoder input Surface -> H.264 Annex-B frames ---
    @SuppressLint("MissingPermission")
    private fun startVideo() {
        if (ContextCompat.checkSelfPermission(appCtx, Manifest.permission.CAMERA)
            != PackageManager.PERMISSION_GRANTED) {
            Log.e(TAG, "usb: no camera permission"); return
        }
        val fmt = MediaFormat.createVideoFormat(MediaFormat.MIMETYPE_VIDEO_AVC, videoW, videoH).apply {
            setInteger(MediaFormat.KEY_COLOR_FORMAT, MediaCodecInfo.CodecCapabilities.COLOR_FormatSurface)
            setInteger(MediaFormat.KEY_BIT_RATE, bitrateFor(videoW, videoH, videoFps))
            setInteger(MediaFormat.KEY_FRAME_RATE, videoFps)
            setInteger(MediaFormat.KEY_I_FRAME_INTERVAL, 1)   // 1s GOP: fast first frame + quick recovery
            setInteger(MediaFormat.KEY_BITRATE_MODE, MediaCodecInfo.EncoderCapabilities.BITRATE_MODE_CBR)
            // Low-latency levers (best-effort; ignored where unsupported).
            runCatching { setInteger(MediaFormat.KEY_LATENCY, 1) }
            runCatching { setInteger(MediaFormat.KEY_MAX_B_FRAMES, 0) }
            if (android.os.Build.VERSION.SDK_INT >= android.os.Build.VERSION_CODES.R)
                runCatching { setInteger(MediaFormat.KEY_LOW_LATENCY, 1) }
        }
        val enc = MediaCodec.createEncoderByType(MediaFormat.MIMETYPE_VIDEO_AVC)
        // The callback writes encoded frames to the socket, so it must run on a background thread —
        // MediaCodec would otherwise dispatch it on the main looper (NetworkOnMainThreadException).
        val et = HandlerThread("usb-enc").apply { start() }
        encThread = et
        enc.setCallback(object : MediaCodec.Callback() {
            override fun onInputBufferAvailable(codec: MediaCodec, index: Int) {}   // Surface input: none
            override fun onOutputBufferAvailable(codec: MediaCodec, index: Int, info: MediaCodec.BufferInfo) {
                try {
                    val buf: ByteBuffer? = codec.getOutputBuffer(index)
                    if (buf != null && info.size > 0) {
                        buf.position(info.offset); buf.limit(info.offset + info.size)
                        val bytes = ByteArray(info.size)
                        buf.get(bytes)
                        writeFrame('V', info.presentationTimeUs, bytes)   // Annex-B (incl. the CODEC_CONFIG buffer)
                    }
                } catch (e: Exception) {
                    if (!closed.get()) Log.w(TAG, "usb: encoder output failed", e)
                } finally {
                    runCatching { codec.releaseOutputBuffer(index, false) }
                }
            }
            override fun onError(codec: MediaCodec, e: MediaCodec.CodecException) {
                Log.e(TAG, "usb: encoder error", e)
            }
            override fun onOutputFormatChanged(codec: MediaCodec, format: MediaFormat) {}
        }, Handler(et.looper))
        enc.configure(fmt, null, null, MediaCodec.CONFIGURE_FLAG_ENCODE)
        inputSurface = enc.createInputSurface()
        enc.start()
        encoder = enc
        openCamera()
    }

    @SuppressLint("MissingPermission")
    private fun openCamera() {
        if (camThread == null) {
            val t = HandlerThread("usb-cam").apply { start() }
            camThread = t; camHandler = Handler(t.looper)
        }
        val h = camHandler ?: return
        val mgr = appCtx.getSystemService(Context.CAMERA_SERVICE) as CameraManager
        val camId = pickCamera(mgr, useBackCamera) ?: run { Log.e(TAG, "usb: no camera"); return }
        mgr.openCamera(camId, object : CameraDevice.StateCallback() {
            override fun onOpened(device: CameraDevice) {
                camera = device
                val surface = inputSurface ?: return
                @Suppress("DEPRECATION")
                device.createCaptureSession(listOf(surface), object : CameraCaptureSession.StateCallback() {
                    override fun onConfigured(session: CameraCaptureSession) {
                        if (closed.get()) return
                        captureSession = session
                        // Try a fixed-fps request; if the device rejects that AE range, retry without it
                        // (a rejected request = a repeating capture that never starts = zero frames).
                        val req = device.createCaptureRequest(CameraDevice.TEMPLATE_RECORD).apply {
                            addTarget(surface)
                            set(CaptureRequest.CONTROL_AE_TARGET_FPS_RANGE, android.util.Range(videoFps, videoFps))
                        }
                        val ok = runCatching { session.setRepeatingRequest(req.build(), null, h) }.isSuccess
                        if (!ok) {
                            val plain = device.createCaptureRequest(CameraDevice.TEMPLATE_RECORD).apply { addTarget(surface) }
                            runCatching { session.setRepeatingRequest(plain.build(), null, h) }
                                .onFailure { Log.e(TAG, "usb: setRepeatingRequest failed", it) }
                        }
                        Log.i(TAG, "usb: camera streaming ${videoW}x${videoH}@${videoFps}")
                    }
                    override fun onConfigureFailed(session: CameraCaptureSession) {
                        Log.e(TAG, "usb: capture session config failed")
                    }
                }, h)
            }
            override fun onDisconnected(device: CameraDevice) { runCatching { device.close() }; camera = null }
            override fun onError(device: CameraDevice, error: Int) {
                Log.e(TAG, "usb: camera error $error"); runCatching { device.close() }; camera = null
            }
        }, h)
    }

    /** Flip between back and front lens on the running stream (MANUAL — driven by the user's button). */
    fun switchCamera() {
        val h = camHandler ?: return
        h.post {
            if (closed.get()) return@post
            useBackCamera = !useBackCamera
            runCatching { captureSession?.close() }; captureSession = null
            runCatching { camera?.close() }; camera = null
            openCamera()   // reopens on the other lens, same encoder input surface
            Log.i(TAG, "usb: switched to ${if (useBackCamera) "back" else "front"} camera")
        }
    }

    private fun pickCamera(mgr: CameraManager, back: Boolean): String? {
        return try {
            val want = if (back) CameraCharacteristics.LENS_FACING_BACK else CameraCharacteristics.LENS_FACING_FRONT
            val ids = mgr.cameraIdList
            ids.firstOrNull { mgr.getCameraCharacteristics(it).get(CameraCharacteristics.LENS_FACING) == want }
                ?: ids.firstOrNull { mgr.getCameraCharacteristics(it).get(CameraCharacteristics.LENS_FACING) == CameraCharacteristics.LENS_FACING_BACK }
                ?: ids.firstOrNull()
        } catch (e: Exception) { Log.e(TAG, "usb: camera enumerate failed", e); null }
    }

    private fun stopVideo() {
        runCatching { captureSession?.close() }; captureSession = null
        runCatching { camera?.close() }; camera = null
        runCatching { encoder?.stop() }
        runCatching { encoder?.release() }; encoder = null
        runCatching { encThread?.quitSafely() }; encThread = null
        runCatching { inputSurface?.release() }; inputSurface = null
        runCatching { camThread?.quitSafely() }; camThread = null; camHandler = null
    }

    // --- audio: AudioRecord -> raw PCM frames ---
    @SuppressLint("MissingPermission")
    private fun startAudio() {
        if (ContextCompat.checkSelfPermission(appCtx, Manifest.permission.RECORD_AUDIO)
            != PackageManager.PERMISSION_GRANTED) {
            Log.e(TAG, "usb: no mic permission"); return
        }
        audioThread = Thread {
            var rec: AudioRecord? = null
            try {
                val minBuf = AudioRecord.getMinBufferSize(
                    AUDIO_RATE, AudioFormat.CHANNEL_IN_MONO, AudioFormat.ENCODING_PCM_16BIT)
                val bufSize = maxOf(minBuf, AUDIO_RATE / 25 * 2)   // ~40ms floor
                rec = AudioRecord(
                    MediaRecorder.AudioSource.VOICE_COMMUNICATION,   // platform AEC/NS, like the WebRTC path
                    AUDIO_RATE, AudioFormat.CHANNEL_IN_MONO, AudioFormat.ENCODING_PCM_16BIT, bufSize)
                if (rec.state != AudioRecord.STATE_INITIALIZED) { Log.e(TAG, "usb: AudioRecord init failed"); return@Thread }
                rec.startRecording()
                val chunk = ByteArray(AUDIO_RATE / 50 * 2)   // ~20ms chunks
                var samples = 0L
                while (!closed.get() && socket != null) {
                    val n = rec.read(chunk, 0, chunk.size)
                    if (n <= 0) { if (n == 0) continue else break }
                    val ptsUs = samples * 1_000_000L / AUDIO_RATE
                    writeFrame('A', ptsUs, chunk, 0, n)
                    samples += n / 2
                }
            } catch (e: Exception) {
                if (!closed.get()) Log.w(TAG, "usb: audio failed", e)
            } finally {
                runCatching { rec?.stop() }
                runCatching { rec?.release() }
            }
        }.apply { isDaemon = true; name = "usb-audio"; start() }
    }

    private fun stopAudio() {
        audioThread?.interrupt(); audioThread = null
    }

    fun stop() {
        if (!closed.compareAndSet(false, true)) return
        runCatching { serverSocket?.close() }; serverSocket = null
        runCatching { socket?.close() }
        stopVideo(); stopAudio()
    }

    private fun bitrateFor(w: Int, h: Int, fps: Int): Int {
        // Generous over USB (no congestion): ~0.1 bit/pixel/frame, capped to sane bounds.
        val bpp = (w.toLong() * h * fps / 10).toInt()
        return bpp.coerceIn(2_000_000, 25_000_000)
    }

    companion object {
        private const val TAG = "PhoneCam"
        const val AUDIO_RATE = 48_000   // matches the PC WASAPI shared-mode rate
    }
}
