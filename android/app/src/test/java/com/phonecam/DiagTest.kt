package com.phonecam

import org.junit.After
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * Tests for the diagnostics core that do not need a device: the bounded event ring, the
 * off-by-default master switch, and the "unavailable stays unavailable" rendering of battery values.
 *
 * The bound matters for a real reason: diagnostics may be left on for an hour-long session, and an
 * unbounded ring (or an unbounded file) would be a memory/storage leak in a battery tool.
 */
class DiagTest {

    @After fun tearDown() {
        Diag.on = false
        Diag.clearForTest()
    }

    @Test fun `events are dropped on the floor while diagnostics are off`() {
        Diag.on = false
        Diag.clearForTest()
        repeat(50) { Diag.event("noise", "i=$it") }
        assertTrue("nothing may be recorded when the switch is off", Diag.recentEvents().isEmpty())
    }

    @Test fun `the event ring is bounded and keeps the newest events`() {
        Diag.on = true
        Diag.clearForTest()
        val n = 1200
        repeat(n) { Diag.event("e", "i=$it") }
        val got = Diag.recentEvents()
        assertTrue("ring must be bounded, got ${got.size}", got.size <= Diag.maxEventsForTest())
        assertEquals(Diag.maxEventsForTest(), got.size)
        // Oldest-first ordering, and the tail is the most recent event — that is what a post-mortem
        // needs, since the interesting thing is whatever happened just before the session ended.
        assertTrue("last event should be the newest: ${got.last()}", got.last().contains("i=${n - 1}"))
        assertTrue("first retained event should be the (n - cap)th: ${got.first()}",
            got.first().contains("i=${n - Diag.maxEventsForTest()}"))
    }

    @Test fun `deep events are silent at the basic level`() {
        Diag.on = true
        Diag.clearForTest()
        Diag.deep("verbose", "x=1")
        assertTrue("deep events must not fire unless deep debugging was requested",
            Diag.recentEvents().isEmpty())
    }

    @Test fun `a fully unavailable battery renders every field as n slash a`() {
        val kv = Diag.Battery().toKv()
        assertTrue(kv, kv.contains("batt=n/a"))
        assertTrue(kv, kv.contains("tempC=n/a"))
        assertTrue(kv, kv.contains("mV=n/a"))
        assertTrue(kv, kv.contains("chargeUAh=n/a"))
        assertTrue(kv, kv.contains("currentUA=n/a"))
        assertTrue(kv, kv.contains("energyNWh=n/a"))
        // "not plugged in" is a real reading and stays numeric; only the sentinel fields go to n/a.
        assertTrue(kv, kv.contains("chg=0"))
        assertTrue(kv, kv.contains("plug=0"))
        for (f in listOf("batt", "tempC", "mV", "chargeUAh", "currentUA", "energyNWh"))
            assertFalse("$f must not render a number when unavailable: $kv",
                Regex("\\b$f=-?\\d").containsMatchIn(kv))
    }

    @Test fun `a populated battery renders real values`() {
        val kv = Diag.Battery(
            pct = 64, charging = true, plug = 2, status = 2, tempDeciC = 305,
            voltageMv = 4012, chargeUah = 3_250_000L, currentUa = -450_000, energyNwh = 12_000_000L
        ).toKv()
        assertTrue(kv, kv.contains("batt=64"))
        assertTrue(kv, kv.contains("chg=1"))
        assertTrue(kv, kv.contains("tempC=30.5"))
        assertTrue(kv, kv.contains("mV=4012"))
        assertTrue(kv, kv.contains("chargeUAh=3250000"))
        assertTrue(kv, kv.contains("currentUA=-450000"))
    }

    @Test fun `counters start at zero and are additive`() {
        Diag.c.framesEncoded.set(0)
        Diag.c.encodedBytes.set(0)
        assertEquals(0L, Diag.c.framesEncoded.get())
        repeat(10) { Diag.c.framesEncoded.incrementAndGet() }
        Diag.c.encodedBytes.addAndGet(4096)
        assertEquals(10L, Diag.c.framesEncoded.get())
        assertEquals(4096L, Diag.c.encodedBytes.get())
        val sb = StringBuilder()
        Diag.c.snapshotInto(sb)
        assertTrue(sb.toString(), sb.contains("encFrames=10"))
        assertTrue(sb.toString(), sb.contains("encBytes=4096"))
    }
}
