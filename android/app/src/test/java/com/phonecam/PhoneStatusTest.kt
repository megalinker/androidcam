package com.phonecam

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * Wire-format tests for the phone → PC device-status message.
 *
 * The contract these lock down: **an unavailable value is omitted, never invented.** Several battery
 * properties are optional on Android (`CURRENT_NOW`, `ENERGY_COUNTER`, and on some devices even the
 * temperature), and a receiver that cannot tell "unknown" from "0" would happily draw an empty
 * battery for a phone that simply did not report one.
 */
class PhoneStatusTest {

    private fun battery(
        pct: Int = 73, charging: Boolean = false, plug: Int = 0,
        status: Int = 2, tempDeciC: Int = 312,
    ) = Diag.Battery(pct = pct, charging = charging, plug = plug, status = status, tempDeciC = tempDeciC)

    @Test fun `encodes the fields the PC needs`() {
        val j = PhoneStatus.encode(battery(), "a1b2c3d4", 1723200000000L)
        assertTrue(j, j.contains("\"t\":\"status\""))
        assertTrue(j, j.contains("\"v\":1"))
        assertTrue(j, j.contains("\"ts\":1723200000000"))
        assertTrue(j, j.contains("\"sid\":\"a1b2c3d4\""))
        assertTrue(j, j.contains("\"pct\":73"))
        assertTrue(j, j.contains("\"chg\":0"))
        assertTrue(j, j.contains("\"tempC\":31.2"))
        assertTrue(j, j.startsWith("{") && j.endsWith("}"))
    }

    @Test fun `charging is reported as one`() {
        assertTrue(PhoneStatus.encode(battery(charging = true, plug = 2), "s", 1L).contains("\"chg\":1"))
    }

    @Test fun `unknown battery level is omitted, not zeroed`() {
        val j = PhoneStatus.encode(battery(pct = -1), "s", 1L)
        assertFalse("an unknown level must not appear at all: $j", j.contains("\"pct\""))
    }

    @Test fun `unavailable temperature is omitted`() {
        val j = PhoneStatus.encode(battery(tempDeciC = Int.MIN_VALUE), "s", 1L)
        assertFalse("unavailable temperature must not appear: $j", j.contains("tempC"))
    }

    @Test fun `an out-of-range level is treated as unknown`() {
        assertFalse(PhoneStatus.encode(battery(pct = 101), "s", 1L).contains("\"pct\""))
        assertFalse(PhoneStatus.encode(battery(pct = -50), "s", 1L).contains("\"pct\""))
    }

    @Test fun `zero percent is a real reading and is sent`() {
        assertTrue(PhoneStatus.encode(battery(pct = 0), "s", 1L).contains("\"pct\":0"))
    }

    @Test fun `a defaulted battery encodes without any invented values`() {
        // Everything unavailable: the message still has to be well-formed and carry only "chg".
        val j = PhoneStatus.encode(Diag.Battery(), "", 42L)
        assertTrue(j, j.startsWith("{\"t\":\"status\""))
        assertTrue(j, j.endsWith("}"))
        assertFalse(j, j.contains("pct"))
        assertFalse(j, j.contains("tempC"))
        assertFalse(j, j.contains("sid"))
        assertTrue(j, j.contains("\"chg\":0"))
    }

    @Test fun `session id cannot inject quotes into the payload`() {
        val j = PhoneStatus.encode(battery(), "bad\"id\\", 1L)
        assertTrue("the id must be stripped to safe characters: $j", j.contains("\"sid\":\"badid\""))
        assertFalse("no backslash may reach the wire: $j", j.contains("\\"))
        assertEquals("quotes must stay balanced: $j", 0, j.count { it == '"' } % 2)
    }

    @Test fun `rotation and geometry are only sent when the phone has stopped rotating`() {
        // rotation = -1 means "we are still rotating the pixels ourselves", so the PC must not be
        // told to rotate again — that would double-apply it and stand the picture on its side.
        val still = PhoneStatus.encode(battery(), "s", 1L, rotation = -1, videoW = 1280, videoH = 720)
        assertFalse(still, still.contains("\"rot\""))
        val handed = PhoneStatus.encode(battery(), "s", 1L, rotation = 90, videoW = 1280, videoH = 720)
        assertTrue(handed, handed.contains("\"rot\":90"))
        assertTrue(handed, handed.contains("\"vw\":1280"))
        assertTrue(handed, handed.contains("\"vh\":720"))
    }

    @Test fun `zero rotation is still reported`() {
        // 0 is a real answer ("landscape, nothing to do"), distinct from -1 ("I rotated it myself").
        assertTrue(PhoneStatus.encode(battery(), "s", 1L, rotation = 0).contains("\"rot\":0"))
    }

    @Test fun `rotation is normalised into 0-359`() {
        assertTrue(PhoneStatus.encode(battery(), "s", 1L, rotation = 450).contains("\"rot\":90"))
    }

    @Test fun `geometry is omitted when unknown`() {
        val j = PhoneStatus.encode(battery(), "s", 1L, rotation = 90, videoW = 0, videoH = 0)
        assertFalse(j, j.contains("vw"))
        assertFalse(j, j.contains("vh"))
    }

    @Test fun `defaults keep the legacy payload unchanged`() {
        // An unchanged call site must not start emitting the new fields: an older receiver would then
        // see keys it ignores, but more importantly the phone would still be rotating.
        val j = PhoneStatus.encode(battery(), "s", 1L)
        assertFalse(j, j.contains("rot"))
        assertFalse(j, j.contains("vw"))
    }

    @Test fun `hello rotation feature is recognised only when advertised`() {
        assertTrue(PhoneStatus.helloSupportsRotation(
            "{\"feat\":[\"status\",\"mark\",\"keyframe\",\"rotation\"]}"))
        assertFalse(PhoneStatus.helloSupportsRotation(
            "{\"feat\":[\"status\",\"mark\"]}"))
        assertFalse(PhoneStatus.helloSupportsRotation(""))
    }

    @Test fun `hello gating recognises the PC's feature list`() {
        assertTrue(PhoneStatus.helloSupportsStatus(
            "{\"t\":\"hello\",\"app\":\"phonecam\",\"v\":1,\"feat\":[\"status\",\"mark\",\"keyframe\"]}"))
    }

    @Test fun `hello without the feature does not enable pushing`() {
        assertFalse(PhoneStatus.helloSupportsStatus("{\"t\":\"hello\",\"v\":1,\"feat\":[\"mark\"]}"))
        assertFalse(PhoneStatus.helloSupportsStatus(""))
    }

    @Test fun `message type constants match the receiver's wire protocol`() {
        // These bytes are the contract with webrtc_receiver.cpp / usb_receiver.cpp. Changing one
        // silently breaks a mixed-version pair, so pin them.
        assertEquals('V', PhoneStatus.MSG_HELLO)
        assertEquals('B', PhoneStatus.MSG_STATUS)
        assertEquals('M', PhoneStatus.MSG_MARK)
        assertEquals('K', PhoneStatus.MSG_KEYFRAME)
        assertEquals('S', PhoneStatus.USB_STATUS)
        assertEquals(1, PhoneStatus.PROTO_VERSION)
    }
}
