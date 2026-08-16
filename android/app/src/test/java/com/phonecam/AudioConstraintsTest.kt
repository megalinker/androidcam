package com.phonecam

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * The mic-mode contract.
 *
 * Measured on a Pixel 9 Pro XL: libwebrtc's software audio processing costs ~12.7 percentage points
 * of one CPU core — about a quarter of the app's total CPU. Turning it off in raw-mic mode is the
 * point of this code. But the *other* direction matters just as much: a user who ticked
 * "Phone-call noise filter" did so because their room is noisy, and silently disabling the processing
 * for them would be a quality regression disguised as an optimisation. These tests pin both.
 */
class AudioConstraintsTest {

    private fun map(rawMic: Boolean) = WebRtcSender.audioConstraintPairs(rawMic).toMap()

    @Test fun `raw mic disables the software audio processing`() {
        val c = map(rawMic = true)
        assertTrue("echo cancellation must be off: $c",
            c["googEchoCancellation"] == "false" || c["echoCancellation"] == "false")
        assertTrue("noise suppression must be off: $c",
            c["googNoiseSuppression"] == "false" || c["noiseSuppression"] == "false")
        assertTrue("auto gain must be off: $c",
            c["googAutoGainControl"] == "false" || c["autoGainControl"] == "false")
        assertEquals("false", c["googHighpassFilter"])
    }

    @Test fun `every raw-mic constraint is a disable, never an enable`() {
        // A stray "true" here would silently switch processing ON for the default mic mode.
        for ((k, v) in WebRtcSender.audioConstraintPairs(rawMic = true))
            assertEquals("$k must disable, not enable", "false", v)
    }

    @Test fun `noise filter mode leaves libwebrtc's defaults alone`() {
        // Empty = no constraints applied = all processing enabled, which is what the user asked for.
        assertTrue("filter mode must not disable anything",
            WebRtcSender.audioConstraintPairs(rawMic = false).isEmpty())
    }

    @Test fun `both legacy and modern constraint spellings are set`() {
        // libwebrtc has accepted both across versions and ignores keys it doesn't know, so sending
        // both is the safe play. If only one spelling shipped and that build dropped it, the
        // processing would quietly stay on and the CPU saving would vanish with no visible symptom.
        val keys = WebRtcSender.audioConstraintPairs(rawMic = true).map { it.first }
        assertTrue(keys.toString(), keys.contains("googEchoCancellation"))
        assertTrue(keys.toString(), keys.contains("echoCancellation"))
        assertTrue(keys.toString(), keys.contains("googNoiseSuppression"))
        assertTrue(keys.toString(), keys.contains("noiseSuppression"))
    }

    @Test fun `no duplicate keys`() {
        val keys = WebRtcSender.audioConstraintPairs(rawMic = true).map { it.first }
        assertEquals("duplicate constraint keys: $keys", keys.size, keys.distinct().size)
    }

    @Test fun `no key is empty`() {
        for ((k, _) in WebRtcSender.audioConstraintPairs(rawMic = true))
            assertFalse("empty constraint key", k.isBlank())
    }
}
