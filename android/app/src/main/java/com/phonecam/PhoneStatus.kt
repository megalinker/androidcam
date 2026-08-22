package com.phonecam

/**
 * The tiny phone → PC device-status message (currently just battery), and the PC → phone hello that
 * gates it.
 *
 * It rides the **control channel that already exists** for each transport — no extra socket, no extra
 * connection, no extra wakeup:
 *  - **Wi-Fi / WebRTC:** the PCAM3 signaling TCP socket. It is already open for the whole session (the
 *    phone blocks on it to notice the PC pressing Stop), so a status message is one small write on an
 *    idle connection. Framing is the existing `[1B type][4B BE len][payload]`, new type `'B'`.
 *  - **USB:** the existing media socket's frame format `[1B type][8B ptsUs][4B len][payload]`, new
 *    type `'S'`.
 *
 * **Backward compatibility.** Older receivers ignore unknown frame types, and older phones never send
 * `'B'` because they never see the PC's hello. Concretely:
 *  - new PC + old phone: the PC sends `'V'` (hello) *after* the SDP offer; the old phone is by then in
 *    its "read until EOF" loop and discards the bytes harmlessly. No status is ever displayed.
 *  - old PC + new phone: no hello ever arrives, so the phone never sends `'B'`. On USB the phone does
 *    send `'S'`, which the old receiver's frame dispatcher drops (it only handles `H`/`V`/`A`).
 *
 * Serialization is hand-rolled (no `org.json`) so it is pure JVM code and unit-testable without a
 * device. The payload carries no identifiers beyond the session id — no IPs, no SSIDs, no secrets.
 */
object PhoneStatus {

    /** Bumped only on an incompatible payload change; receivers must ignore unknown/larger values. */
    const val PROTO_VERSION = 1

    /** PCAM3 signaling message types added by this feature (see webrtc_receiver.cpp). */
    const val MSG_HELLO = 'V'    // PC -> phone: "I understand device status"
    const val MSG_STATUS = 'B'   // phone -> PC: device status
    const val MSG_MARK = 'M'     // PC -> phone: the user pressed "Mark video problem"
    const val MSG_KEYFRAME = 'K' // PC -> phone: please emit a keyframe now

    /** USB media-socket frame type for the same status payload. */
    const val USB_STATUS = 'S'

    /**
     * Encode a status message. Unavailable fields are **omitted** rather than sent as a made-up
     * number, so the PC can distinguish "unknown" from "0".
     */
    fun encode(
        b: Diag.Battery, sessionId: String, tsMs: Long,
        /** Rotation the PC must apply because we are no longer applying it (-1 = we still rotate). */
        rotation: Int = -1,
        /** Our capture geometry BEFORE rotation, so the PC can size the virtual camera from it. */
        videoW: Int = 0, videoH: Int = 0,
    ): String {
        val sb = StringBuilder(112)
        sb.append("{\"t\":\"status\",\"v\":").append(PROTO_VERSION)
        sb.append(",\"ts\":").append(tsMs)
        if (sessionId.isNotEmpty()) sb.append(",\"sid\":\"").append(sanitize(sessionId)).append('"')
        if (b.pct in 0..100) sb.append(",\"pct\":").append(b.pct)
        sb.append(",\"chg\":").append(if (b.charging) 1 else 0)
        if (b.plug != 0) sb.append(",\"plug\":").append(b.plug)
        if (b.status != 1 /* BATTERY_STATUS_UNKNOWN */) sb.append(",\"st\":").append(b.status)
        if (b.tempDeciC != Int.MIN_VALUE)
            sb.append(",\"tempC\":").append(String.format(java.util.Locale.US, "%.1f", b.tempDeciC / 10.0))
        if (rotation >= 0) sb.append(",\"rot\":").append(((rotation % 360) + 360) % 360)
        if (videoW > 0 && videoH > 0)
            sb.append(",\"vw\":").append(videoW).append(",\"vh\":").append(videoH)
        sb.append('}')
        return sb.toString()
    }

    /** True if the PC's hello advertises the device-status feature. */
    fun helloSupportsStatus(payload: String): Boolean =
        payload.contains("\"status\"") || payload.contains("status")

    /**
     * True if the PC will apply the image rotation itself.
     *
     * When it does, we stop rotating frames before encoding and just report the angle. Measured on a
     * Pixel 9 Pro XL, that rotation cost ~6 points of one CPU core at 720p and ~42 at 1080p - by far
     * the largest avoidable draw found. A PC that does not advertise this keeps getting pre-rotated
     * frames exactly as before.
     */
    fun helloSupportsRotation(payload: String): Boolean = payload.contains("\"rotation\"")

    /** Guard against a stray quote/backslash ever reaching the wire from an id we generated. */
    private fun sanitize(s: String): String {
        for (ch in s) if (!(ch.isLetterOrDigit() || ch == '-' || ch == '_')) return s.filter {
            it.isLetterOrDigit() || it == '-' || it == '_'
        }
        return s
    }
}
