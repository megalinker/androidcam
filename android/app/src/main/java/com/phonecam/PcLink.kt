package com.phonecam

import android.util.Log
import org.json.JSONObject
import java.net.InetSocketAddress
import java.net.Socket

/**
 * Wi-Fi pairing by scanning a QR shown on the PC. The PC displays a code carrying its own
 * LAN address + a control port + a one-time token; the phone scans it and *announces* its
 * RTSP pull URL back to that port. The PC then connects to the phone (the pipeline direction
 * is unchanged: phone = server, PC = client). This just removes the "type the IP" step.
 */
data class PcTarget(val host: String, val port: Int, val token: String) {
    companion object {
        /** Parse "PCAM1:<host>:<port>:<token>" (host is IPv4, so ':' is a safe delimiter). */
        fun parse(raw: String?): PcTarget? {
            val parts = raw?.trim()?.split(":") ?: return null
            if (parts.size != 4 || parts[0] != "PCAM1") return null
            val port = parts[2].toIntOrNull() ?: return null
            if (parts[1].isEmpty() || parts[3].isEmpty() || port !in 1..65535) return null
            return PcTarget(parts[1], port, parts[3])
        }
    }
}

/**
 * Encrypted (SRT) pairing. Here the direction flips: the PC runs an SRT *listener* and the phone
 * *pushes* an AES-encrypted stream to it, so the QR carries the PC's endpoint + a passphrase.
 */
data class SrtTarget(val host: String, val port: Int, val passphrase: String) {
    companion object {
        /** Parse "PCAM2:<host>:<port>:<passphrase>" (passphrase is hex, so ':' stays a safe delimiter). */
        fun parse(raw: String?): SrtTarget? {
            val parts = raw?.trim()?.split(":") ?: return null
            if (parts.size != 4 || parts[0] != "PCAM2") return null
            val port = parts[2].toIntOrNull() ?: return null
            if (parts[1].isEmpty() || parts[3].isEmpty() || port !in 1..65535) return null
            return SrtTarget(parts[1], port, parts[3])
        }
    }
}

/**
 * WebRTC (PCAM3) pairing. Like SRT, the phone connects out to the PC — but here the QR carries the
 * PC's TCP *signaling* endpoint + a pairSecret; over that channel the two exchange SDP offer/answer
 * (DTLS-SRTP fingerprints + LAN host candidates) and media then flows LAN-direct over UDP.
 */
data class WebrtcTarget(val host: String, val port: Int, val secret: String) {
    companion object {
        /** Parse "PCAM3:<host>:<port>:<secret>" (host is IPv4, secret is hex — ':' stays a safe delimiter). */
        fun parse(raw: String?): WebrtcTarget? {
            val parts = raw?.trim()?.split(":") ?: return null
            if (parts.size != 4 || parts[0] != "PCAM3") return null
            val port = parts[2].toIntOrNull() ?: return null
            if (parts[1].isEmpty() || parts[3].isEmpty() || port !in 1..65535) return null
            return WebrtcTarget(parts[1], port, parts[3])
        }
    }
}

object PcLink {
    private const val TAG = "PhoneCam"

    /**
     * Open a short TCP connection to the PC and send one JSON line with our pull URL. Blocking —
     * call from a background thread. Returns true if the PC accepted the connection and bytes.
     */
    fun announce(target: PcTarget, url: String, mode: String, name: String, ctok: String): Boolean = try {
        Socket().use { s ->
            s.connect(InetSocketAddress(target.host, target.port), 4000)
            val line = JSONObject()
                .put("v", 1).put("tok", target.token).put("url", url).put("mode", mode).put("name", name)
                .put("ctok", ctok)   // our stop-authorization token, so the PC can stop us later
                .toString() + "\n"
            s.getOutputStream().apply { write(line.toByteArray(Charsets.UTF_8)); flush() }
            // Best-effort read of the PC's "OK" ack; delivery already succeeded above.
            runCatching { s.soTimeout = 3000; s.getInputStream().read() }
            Log.i(TAG, "announced $url to ${target.host}:${target.port}")
            true
        }
    } catch (e: Exception) {
        Log.w(TAG, "announce to ${target.host}:${target.port} failed", e); false
    }
}
