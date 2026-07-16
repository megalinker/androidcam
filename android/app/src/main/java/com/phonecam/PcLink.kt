package com.phonecam

/**
 * WebRTC (PCAM3) pairing target. The phone connects out to the PC; the QR carries the PC's TCP
 * *signaling* endpoint + a pairSecret. Over that channel the two exchange SDP offer/answer
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
