package com.phonecam

import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.os.BatteryManager
import android.os.Build
import android.os.Debug
import android.os.Handler
import android.os.HandlerThread
import android.os.PowerManager
import android.os.SystemClock
import android.system.Os
import android.system.OsConstants
import android.util.Log
import java.io.File
import java.util.ArrayDeque
import java.util.concurrent.atomic.AtomicLong

/**
 * Session diagnostics for the streaming pipeline — built to answer "where is the battery going?"
 * with evidence instead of guesses.
 *
 * Design rules (the measurement must not become the thing being measured):
 *  - **Counters, not logs, on hot paths.** Per-frame work is a single `AtomicLong.incrementAndGet()`
 *    on a preallocated field — no map lookup, no string, no allocation. Frames/bytes are only ever
 *    *summarised* at the sample tick.
 *  - **Low-frequency sampling.** One background thread wakes every [Level.BASIC] = 30 s (DEEP = 5 s),
 *    reads a handful of cheap counters, and writes one line. That is ~2 wakeups/minute against a
 *    pipeline already waking 30–60×/s for frames, i.e. noise.
 *  - **Event-driven where the platform offers it.** Thermal status arrives via
 *    `PowerManager.addThermalStatusListener` and charger connect/disconnect via the
 *    `ACTION_POWER_CONNECTED`/`DISCONNECTED` broadcasts, so nothing polls for them. Battery level is
 *    read from the *sticky* `ACTION_BATTERY_CHANGED` intent (`registerReceiver(null, …)`), which is
 *    the documented no-receiver way to sample it and costs one binder call per tick.
 *  - **Bounded everything.** The in-memory event ring is capped at [MAX_EVENTS]; each session file is
 *    capped at [MAX_FILE_BYTES] and only [MAX_FILES] sessions are kept, so leaving diagnostics on can
 *    never fill storage.
 *  - **Never log media content.** Frame bytes, audio samples, the pairing secret, SSIDs and IP addresses
 *    are never recorded — only counts, sizes and timings.
 *
 * Unavailable platform values are reported as `n/a`, never as a fabricated estimate: several battery
 * properties (`CURRENT_NOW`, `ENERGY_COUNTER`) are optional and device-dependent, and their sign
 * convention is not standardised across vendors.
 *
 * Export: every line also goes to logcat under tag [TAG], so
 * `adb logcat -d -s PhoneCamDiag` always works with no permissions and no file access.
 * See `tools/battery-session.ps1`.
 */
object Diag {

    const val TAG = "PhoneCamDiag"

    enum class Level { OFF, BASIC, DEEP }

    private const val MAX_EVENTS = 400          // in-memory ring (~40 KB worst case)
    private const val MAX_FILE_BYTES = 256L * 1024
    private const val MAX_FILES = 8
    private const val SAMPLE_BASIC_MS = 30_000L
    private const val SAMPLE_DEEP_MS = 5_000L

    // ---------------------------------------------------------------- counters

    /**
     * Hot-path counters. Every field is touched at most once per frame/packet with a single atomic
     * add; they are read only by the sampler. Adding a counter here costs nothing at rest.
     */
    class Counters {
        // capture
        val cameraFrames = AtomicLong()        // frames delivered by the camera into the pipeline
        val cameraDropped = AtomicLong()
        // encoder
        val framesEncoded = AtomicLong()
        val keyFrames = AtomicLong()
        val encodedBytes = AtomicLong()
        val encoderErrors = AtomicLong()
        val encoderReconfigs = AtomicLong()
        // audio
        val audioChunks = AtomicLong()
        val audioBytes = AtomicLong()
        // transport
        val socketWrites = AtomicLong()
        val socketWriteErrors = AtomicLong()
        val reconnects = AtomicLong()
        val keyframeRequests = AtomicLong()    // PLI / 'K' from the PC that we honoured
        val statusPushes = AtomicLong()
        // marks raised from the PC ("I can see the artifact now")
        val problemMarks = AtomicLong()

        fun snapshotInto(sb: StringBuilder) {
            sb.append(" camFrames=").append(cameraFrames.get())
            sb.append(" encFrames=").append(framesEncoded.get())
            sb.append(" keyFrames=").append(keyFrames.get())
            sb.append(" encBytes=").append(encodedBytes.get())
            sb.append(" encErr=").append(encoderErrors.get())
            sb.append(" encReconf=").append(encoderReconfigs.get())
            sb.append(" aChunks=").append(audioChunks.get())
            sb.append(" aBytes=").append(audioBytes.get())
            sb.append(" sockWr=").append(socketWrites.get())
            sb.append(" sockErr=").append(socketWriteErrors.get())
            sb.append(" reconn=").append(reconnects.get())
            sb.append(" kfReq=").append(keyframeRequests.get())
            sb.append(" marks=").append(problemMarks.get())
        }
    }

    @JvmField val c = Counters()

    // ---------------------------------------------------------------- state

    /**
     * Master switch, read on every hot-path counter call. **Off by default** — with it off there is no
     * sampler thread, no logcat write, no file, no `getStats()` collection and no counter traffic, so
     * diagnostics cost exactly nothing. The user turns it on in the phone app ("Record diagnostics"),
     * or a test script/desktop app passes `--ez diag true`.
     *
     * A plain volatile read is ~1 ns and lets the JIT hoist the check out of per-frame paths.
     */
    @Volatile @JvmField var on: Boolean = false

    @Volatile var level: Level = Level.OFF
        private set

    /** Short random id shared with the PC so both sides' logs line up. */
    @Volatile var sessionId: String = ""
        private set

    @Volatile private var appCtx: Context? = null
    @Volatile private var startElapsed = 0L
    @Volatile private var startWall = 0L
    @Volatile private var running = false

    private val ring = ArrayDeque<String>(MAX_EVENTS)
    private var thread: HandlerThread? = null
    private var handler: Handler? = null
    private var logFile: File? = null
    private var fileBytes = 0L

    // sampler deltas
    private var lastCpuTicks = -1L
    private var lastSampleElapsed = 0L
    private var lastCamFrames = 0L
    private var lastEncFrames = 0L
    private var lastEncBytes = 0L
    private var lastTxBytes = -1L

    // session-scoped extremes / first values (for the summary)
    private var battStartPct = -1
    private var battStartChargeUah = Long.MIN_VALUE
    @Volatile private var battLastPct = -1
    @Volatile private var battLastChargeUah = Long.MIN_VALUE
    @Volatile private var thermalMax = -1
    private var tempMaxDeciC = Int.MIN_VALUE
    private var cpuTicksTotal = 0L
    private var chargedDuringSession = false

    private var thermalListener: PowerManager.OnThermalStatusChangedListener? = null

    // ---------------------------------------------------------------- lifecycle

    /**
     * Begin a diagnostic session. Cheap and idempotent; safe to call from the service's start path.
     *
     * @param lvl [Level.OFF] (the default for normal use) makes this a no-op — nothing is started and
     *   [on] stays false. [Level.BASIC] samples every 30 s. [Level.DEEP] samples every 5 s and enables
     *   the verbose event set; it is a temporary developer mode and does cost measurably more.
     */
    @Synchronized
    fun start(ctx: Context, lvl: Level, meta: String) {
        if (running) stop("restart")
        // Diagnostics off: start nothing, and clear any id left over from an earlier session so the
        // status messages we still send to the PC don't quote a session that isn't being recorded.
        if (lvl == Level.OFF) { level = Level.OFF; on = false; sessionId = ""; return }
        appCtx = ctx.applicationContext
        level = lvl
        on = true
        sessionId = java.lang.Long.toHexString(
            (System.currentTimeMillis() shl 16) xor SystemClock.elapsedRealtimeNanos()
        ).takeLast(8)
        startElapsed = SystemClock.elapsedRealtime()
        startWall = System.currentTimeMillis()
        running = true
        synchronized(ring) { ring.clear() }
        resetCounters()
        lastCpuTicks = -1L; lastTxBytes = -1L
        lastSampleElapsed = startElapsed
        lastCamFrames = 0; lastEncFrames = 0; lastEncBytes = 0
        battStartPct = -1; battStartChargeUah = Long.MIN_VALUE
        thermalMax = -1; tempMaxDeciC = Int.MIN_VALUE; cpuTicksTotal = 0
        chargedDuringSession = false

        openLogFile(appCtx!!)

        val t = HandlerThread("phonecam-diag", android.os.Process.THREAD_PRIORITY_BACKGROUND)
        t.start()
        thread = t
        val h = Handler(t.looper)
        handler = h

        event("session_started", "sid=$sessionId", meta,
            "level=${level.name}", "device=${Build.MANUFACTURER}/${Build.MODEL}",
            "sdk=${Build.VERSION.SDK_INT}", "app=${BuildConfig.VERSION_NAME}")

        h.post {
            val b = readBattery()
            battStartPct = b.pct
            battStartChargeUah = b.chargeUah
            event("battery_state_changed", "phase=start", b.toKv())
            registerThermal()
            sample()   // schedules itself
        }
    }

    @Synchronized
    fun stop(reason: String) {
        if (!running) { on = false; level = Level.OFF; return }
        running = false
        on = false
        val h = handler
        handler = null
        unregisterThermal()
        val summary = buildSummary(reason)
        // Emit the summary on the diag thread so the final battery read stays off the caller's thread,
        // then tear the thread down.
        if (h != null) {
            h.post {
                rawEmit(summary)
                flushFile()
                thread?.quitSafely()
                thread = null
            }
        } else {
            rawEmit(summary); flushFile(); thread?.quitSafely(); thread = null
        }
    }

    fun isRunning(): Boolean = running

    private fun resetCounters() {
        c.cameraFrames.set(0); c.cameraDropped.set(0)
        c.framesEncoded.set(0); c.keyFrames.set(0); c.encodedBytes.set(0)
        c.encoderErrors.set(0); c.encoderReconfigs.set(0)
        c.audioChunks.set(0); c.audioBytes.set(0)
        c.socketWrites.set(0); c.socketWriteErrors.set(0); c.reconnects.set(0)
        c.keyframeRequests.set(0); c.statusPushes.set(0); c.problemMarks.set(0)
    }

    // ---------------------------------------------------------------- events

    /**
     * Record a structured event. Called at pipeline milestones (start/stop/connect/error), never
     * per frame. Cost: one small StringBuilder + a logcat write.
     */
    fun event(type: String, vararg kv: String) {
        if (!on) return
        val sb = StringBuilder(64)
        sb.append(SystemClock.elapsedRealtime() - startElapsed).append("ms ").append(type)
        for (s in kv) if (s.isNotEmpty()) sb.append(' ').append(s)
        rawEmit(sb.toString())
    }

    /** Deep-debug-only event: compiled in, but silent unless the session was started with deep=true. */
    fun deep(type: String, vararg kv: String) {
        if (!on || level != Level.DEEP) return
        event(type, *kv)
    }

    private fun rawEmit(line: String) {
        Log.i(TAG, line)
        synchronized(ring) {
            if (ring.size >= MAX_EVENTS) ring.removeFirst()
            ring.addLast(line)
        }
        appendFile(line)
    }

    /** The in-memory ring, oldest first — for the in-app "share diagnostics" action. */
    fun recentEvents(): List<String> = synchronized(ring) { ring.toList() }

    // -- test seams (JVM unit tests; no effect on the shipped paths) --
    internal fun clearForTest() { synchronized(ring) { ring.clear() } }
    internal fun maxEventsForTest() = MAX_EVENTS

    // ---------------------------------------------------------------- sampler hooks

    /**
     * Extra work to run on the (low-frequency) sample tick — used by the transports to pull their
     * own stack's statistics, e.g. `PeerConnection.getStats()`. Runs on the diag thread; keep it
     * non-blocking. Registering a hook adds no wakeups: it rides the existing tick.
     */
    private val samplers = java.util.concurrent.CopyOnWriteArrayList<Runnable>()

    fun addSampler(r: Runnable) { samplers.add(r) }
    fun removeSampler(r: Runnable) { samplers.remove(r) }

    // ---------------------------------------------------------------- sampling

    private fun sample() {
        if (!running) return
        val now = SystemClock.elapsedRealtime()
        val dtMs = (now - lastSampleElapsed).coerceAtLeast(1)
        lastSampleElapsed = now

        val sb = StringBuilder(220)
        sb.append(now - startElapsed).append("ms sample")

        // --- battery (sticky intent + BatteryManager properties; no receiver, no wakeups) ---
        val b = readBattery()
        battLastPct = b.pct
        battLastChargeUah = b.chargeUah
        if (b.charging) chargedDuringSession = true
        if (b.tempDeciC != Int.MIN_VALUE && b.tempDeciC > tempMaxDeciC) tempMaxDeciC = b.tempDeciC
        if (battStartPct < 0 && b.pct >= 0) battStartPct = b.pct
        if (battStartChargeUah == Long.MIN_VALUE) battStartChargeUah = b.chargeUah
        sb.append(' ').append(b.toKv())

        // --- process CPU (own /proc/self/stat — no root, no other-process access) ---
        var cpuPct = -1.0
        val ticks = readSelfCpuTicks()
        if (ticks >= 0) {
            if (lastCpuTicks >= 0) {
                val d = ticks - lastCpuTicks
                cpuTicksTotal += d
                val cpuMs = d * 1000.0 / clockTicksPerSec
                cpuPct = 100.0 * cpuMs / dtMs
                sb.append(" cpu=").append(fmt1(cpuPct)).append('%')
            }
            lastCpuTicks = ticks
        } else sb.append(" cpu=n/a")
        sb.append(" threads=").append(readSelfThreads())

        // --- observed pipeline rates over this window (from the hot-path counters) ---
        val cam = c.cameraFrames.get(); val enc = c.framesEncoded.get(); val bytes = c.encodedBytes.get()
        val capFps = (cam - lastCamFrames) * 1000.0 / dtMs
        sb.append(" capFps=").append(fmt1(capFps))
        sb.append(" encFps=").append(fmt1((enc - lastEncFrames) * 1000.0 / dtMs))
        sb.append(" encKbps=").append(fmt1((bytes - lastEncBytes) * 8.0 / dtMs))
        // CPU normalised per captured frame. Raw cpu% is not comparable between runs whose frame rate
        // differs — dim light alone drops the sensor to 23 fps and takes CPU down with it, which is
        // exactly what made the first rotation A/B unreadable. This is the number to compare.
        if (cpuPct >= 0 && capFps > 1.0) sb.append(" cpuPerFps=").append(fmt2(cpuPct / capFps))
        lastCamFrames = cam; lastEncFrames = enc; lastEncBytes = bytes

        // --- thermal (status is event-driven; headroom is a forecast the platform rate-limits) ---
        val pm = appCtx?.getSystemService(Context.POWER_SERVICE) as? PowerManager
        if (pm != null && Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
            val st = runCatching { pm.currentThermalStatus }.getOrDefault(-1)
            if (st > thermalMax) thermalMax = st
            sb.append(" thermal=").append(st)
        }
        if (pm != null && Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
            val hr = runCatching { pm.getThermalHeadroom(0) }.getOrDefault(Float.NaN)
            sb.append(" headroom=").append(if (hr.isNaN()) "n/a" else fmt2(hr.toDouble()))
        }

        // --- memory / GC pressure (cheap runtime stats, no allocation tracking) ---
        val rt = Runtime.getRuntime()
        sb.append(" heapKB=").append((rt.totalMemory() - rt.freeMemory()) / 1024)
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.M) {
            runCatching { Debug.getRuntimeStat("art.gc.gc-count") }.getOrNull()
                ?.let { sb.append(" gcCount=").append(it) }
        }

        // --- radio bytes for this uid (Wi-Fi path; loopback/USB is not counted by TrafficStats) ---
        val tx = runCatching { android.net.TrafficStats.getUidTxBytes(android.os.Process.myUid()) }
            .getOrDefault(-1L)
        if (tx >= 0) {
            if (lastTxBytes >= 0) sb.append(" uidTxKbps=").append(fmt1((tx - lastTxBytes) * 8.0 / dtMs))
            lastTxBytes = tx
        }

        rawEmit(sb.toString())
        flushFile()

        for (s in samplers) runCatching { s.run() }

        handler?.postDelayed({ sample() },
            if (level == Level.DEEP) SAMPLE_DEEP_MS else SAMPLE_BASIC_MS)
    }

    // ---------------------------------------------------------------- battery

    /**
     * A battery reading. Fields that the platform does not expose on this device are left at their
     * sentinel (`-1` / `Int.MIN_VALUE` / `Long.MIN_VALUE`) and render as `n/a`.
     */
    data class Battery(
        val pct: Int = -1,
        val charging: Boolean = false,
        val plug: Int = 0,
        val status: Int = BatteryManager.BATTERY_STATUS_UNKNOWN,
        val tempDeciC: Int = Int.MIN_VALUE,
        val voltageMv: Int = Int.MIN_VALUE,
        val chargeUah: Long = Long.MIN_VALUE,
        val currentUa: Int = Int.MIN_VALUE,
        val energyNwh: Long = Long.MIN_VALUE,
    ) {
        fun toKv(): String {
            val sb = StringBuilder(96)
            sb.append("batt=").append(if (pct < 0) "n/a" else pct.toString())
            sb.append(" chg=").append(if (charging) 1 else 0)
            sb.append(" plug=").append(plug)
            sb.append(" battStatus=").append(status)
            sb.append(" tempC=").append(if (tempDeciC == Int.MIN_VALUE) "n/a" else fmt1(tempDeciC / 10.0))
            sb.append(" mV=").append(if (voltageMv == Int.MIN_VALUE) "n/a" else voltageMv.toString())
            sb.append(" chargeUAh=").append(if (chargeUah == Long.MIN_VALUE) "n/a" else chargeUah.toString())
            sb.append(" currentUA=").append(if (currentUa == Int.MIN_VALUE) "n/a" else currentUa.toString())
            sb.append(" energyNWh=").append(if (energyNwh == Long.MIN_VALUE) "n/a" else energyNwh.toString())
            return sb.toString()
        }
    }

    /**
     * Read the battery without registering a receiver: `ACTION_BATTERY_CHANGED` is sticky, so
     * `registerReceiver(null, filter)` returns the last broadcast immediately. This is the pattern the
     * Android "Monitor the battery level and charging state" guide prescribes for sampling, and it
     * causes no wakeups of its own.
     */
    fun readBattery(): Battery {
        val ctx = appCtx ?: return Battery()
        return readBattery(ctx)
    }

    fun readBattery(ctx: Context): Battery {
        val i: Intent? = runCatching {
            ctx.registerReceiver(null, IntentFilter(Intent.ACTION_BATTERY_CHANGED))
        }.getOrNull()
        var pct = -1
        var status = BatteryManager.BATTERY_STATUS_UNKNOWN
        var plug = 0
        var temp = Int.MIN_VALUE
        var volt = Int.MIN_VALUE
        if (i != null) {
            val level = i.getIntExtra(BatteryManager.EXTRA_LEVEL, -1)
            val scale = i.getIntExtra(BatteryManager.EXTRA_SCALE, -1)
            if (level >= 0 && scale > 0) pct = (level * 100 / scale).coerceIn(0, 100)
            status = i.getIntExtra(BatteryManager.EXTRA_STATUS, BatteryManager.BATTERY_STATUS_UNKNOWN)
            plug = i.getIntExtra(BatteryManager.EXTRA_PLUGGED, 0)
            temp = i.getIntExtra(BatteryManager.EXTRA_TEMPERATURE, Int.MIN_VALUE)
            volt = i.getIntExtra(BatteryManager.EXTRA_VOLTAGE, Int.MIN_VALUE)
        }
        val bm = ctx.getSystemService(Context.BATTERY_SERVICE) as? BatteryManager
        if (pct < 0 && bm != null) pct = intProp(bm, BatteryManager.BATTERY_PROPERTY_CAPACITY)
        val chargeUah = longProp(bm, BatteryManager.BATTERY_PROPERTY_CHARGE_COUNTER)
        val currentUa = intProp(bm, BatteryManager.BATTERY_PROPERTY_CURRENT_NOW).let {
            if (it == -1) Int.MIN_VALUE else it   // -1 is also used by some HALs for "unsupported"
        }
        val energyNwh = longProp(bm, BatteryManager.BATTERY_PROPERTY_ENERGY_COUNTER)
        // BATTERY_PLUGGED_* is 0 when running on battery; STATUS_CHARGING/FULL can lag the plug state,
        // so treat "plugged into anything" as charging for UI purposes and report both fields.
        val charging = plug != 0 ||
            status == BatteryManager.BATTERY_STATUS_CHARGING ||
            status == BatteryManager.BATTERY_STATUS_FULL
        return Battery(pct, charging, plug, status, temp, volt, chargeUah, currentUa, energyNwh)
    }

    private fun intProp(bm: BatteryManager?, id: Int): Int {
        if (bm == null) return Int.MIN_VALUE
        val v = runCatching { bm.getIntProperty(id) }.getOrDefault(Int.MIN_VALUE)
        return if (v == Int.MIN_VALUE) Int.MIN_VALUE else v
    }

    private fun longProp(bm: BatteryManager?, id: Int): Long {
        if (bm == null) return Long.MIN_VALUE
        val v = runCatching { bm.getLongProperty(id) }.getOrDefault(Long.MIN_VALUE)
        // Several HALs report 0 for an unimplemented counter; 0 µAh is not a plausible reading on a
        // running phone, so treat it as unavailable rather than as data.
        return if (v == Long.MIN_VALUE || v == 0L) Long.MIN_VALUE else v
    }

    /** Called by the service when the charger is plugged/unplugged (an event, not a poll). */
    fun onPowerConnectionChanged(connected: Boolean) {
        if (!running) return
        handler?.post {
            val b = readBattery()
            if (b.charging) chargedDuringSession = true
            event("battery_state_changed", "phase=power", "connected=${if (connected) 1 else 0}", b.toKv())
        }
    }

    // ---------------------------------------------------------------- thermal

    private fun registerThermal() {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.Q) return
        val pm = appCtx?.getSystemService(Context.POWER_SERVICE) as? PowerManager ?: return
        runCatching {
            val l = PowerManager.OnThermalStatusChangedListener { status ->
                if (status > thermalMax) thermalMax = status
                event("thermal_state_changed", "status=$status")
            }
            pm.addThermalStatusListener(l)
            thermalListener = l
            thermalMax = pm.currentThermalStatus
        }
    }

    private fun unregisterThermal() {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.Q) return
        val l = thermalListener ?: return
        thermalListener = null
        val pm = appCtx?.getSystemService(Context.POWER_SERVICE) as? PowerManager ?: return
        runCatching { pm.removeThermalStatusListener(l) }
    }

    // ---------------------------------------------------------------- summary

    /**
     * The one line that answers "was this session's drain reasonable?". Battery-% deltas over short
     * sessions are coarse (1 % granularity), so the charge counter (µAh) is reported too when the
     * device exposes it — that is the trustworthy number.
     */
    fun buildSummary(reason: String): String {
        val durMs = (SystemClock.elapsedRealtime() - startElapsed).coerceAtLeast(1)
        val b = runCatching { readBattery() }.getOrDefault(Battery())
        val endPct = if (b.pct >= 0) b.pct else battLastPct
        val endCharge = if (b.chargeUah != Long.MIN_VALUE) b.chargeUah else battLastChargeUah
        val hours = durMs / 3_600_000.0

        val sb = StringBuilder(320)
        sb.append(durMs).append("ms session_summary sid=").append(sessionId)
        sb.append(" reason=").append(reason)
        sb.append(" durationS=").append(durMs / 1000)
        sb.append(" battStart=").append(if (battStartPct < 0) "n/a" else battStartPct.toString())
        sb.append(" battEnd=").append(if (endPct < 0) "n/a" else endPct.toString())
        if (battStartPct >= 0 && endPct >= 0) {
            val d = battStartPct - endPct
            sb.append(" battDelta=").append(d)
            sb.append(" battPctPerHour=").append(if (hours > 0.02) fmt1(d / hours) else "n/a")
        } else sb.append(" battDelta=n/a battPctPerHour=n/a")
        if (battStartChargeUah != Long.MIN_VALUE && endCharge != Long.MIN_VALUE) {
            val dUah = battStartChargeUah - endCharge
            sb.append(" chargeDeltaUAh=").append(dUah)
            sb.append(" avgCurrentMA=").append(if (hours > 0.02) fmt1(dUah / 1000.0 / hours) else "n/a")
        } else sb.append(" chargeDeltaUAh=n/a avgCurrentMA=n/a")
        // A session that spent any time on the charger cannot be used as a drain measurement.
        sb.append(" charged=").append(if (chargedDuringSession) 1 else 0)
        sb.append(" tempMaxC=").append(if (tempMaxDeciC == Int.MIN_VALUE) "n/a" else fmt1(tempMaxDeciC / 10.0))
        sb.append(" thermalMax=").append(thermalMax)
        sb.append(" cpuAvg=").append(
            fmt1(100.0 * (cpuTicksTotal * 1000.0 / clockTicksPerSec) / durMs)).append('%')
        sb.append(" avgCapFps=").append(fmt1(c.cameraFrames.get() * 1000.0 / durMs))
        sb.append(" avgEncFps=").append(fmt1(c.framesEncoded.get() * 1000.0 / durMs))
        sb.append(" avgEncKbps=").append(fmt1(c.encodedBytes.get() * 8.0 / durMs))
        c.snapshotInto(sb)
        return sb.toString()
    }

    // ---------------------------------------------------------------- bounded file sink

    private fun openLogFile(ctx: Context) {
        logFile = null; fileBytes = 0
        runCatching {
            val dir = File(ctx.getExternalFilesDir(null) ?: ctx.filesDir, "diag")
            if (!dir.exists() && !dir.mkdirs()) return
            // Keep only the newest MAX_FILES-1 sessions before adding this one.
            dir.listFiles()?.sortedByDescending { it.lastModified() }?.drop(MAX_FILES - 1)
                ?.forEach { it.delete() }
            logFile = File(dir, "session-$sessionId.log")
        }
    }

    private fun appendFile(line: String) {
        val f = logFile ?: return
        if (fileBytes >= MAX_FILE_BYTES) return   // hard cap: diagnostics can never fill storage
        runCatching {
            f.appendText(line + "\n")
            fileBytes += line.length + 1
            if (fileBytes >= MAX_FILE_BYTES) f.appendText("--- diagnostics file cap reached ---\n")
        }
    }

    private fun flushFile() { /* appendText writes through; kept for symmetry with buffered sinks */ }

    // ---------------------------------------------------------------- /proc helpers

    private val clockTicksPerSec: Long by lazy {
        runCatching { Os.sysconf(OsConstants._SC_CLK_TCK) }.getOrDefault(100L).coerceAtLeast(1L)
    }

    /** utime+stime for THIS process, in clock ticks. Reading our own /proc entry needs no permission. */
    private fun readSelfCpuTicks(): Long = runCatching {
        val s = File("/proc/self/stat").readText()
        // Field 2 (comm) may contain spaces; everything after the final ')' is space-separated.
        val after = s.substring(s.lastIndexOf(')') + 2)
        val f = after.split(' ')
        f[11].toLong() + f[12].toLong()   // utime, stime (fields 14/15 overall)
    }.getOrDefault(-1L)

    private fun readSelfThreads(): Int = runCatching {
        val s = File("/proc/self/stat").readText()
        val after = s.substring(s.lastIndexOf(')') + 2)
        after.split(' ')[17].toInt()      // num_threads (field 20)
    }.getOrDefault(-1)

    // ---------------------------------------------------------------- formatting

    private fun fmt1(v: Double): String =
        if (v.isNaN() || v.isInfinite()) "n/a" else String.format(java.util.Locale.US, "%.1f", v)

    private fun fmt2(v: Double): String =
        if (v.isNaN() || v.isInfinite()) "n/a" else String.format(java.util.Locale.US, "%.2f", v)
}
