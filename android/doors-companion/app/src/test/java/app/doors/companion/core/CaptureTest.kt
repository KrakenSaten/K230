// Copyright (c) 2026 Doors authors.
// SPDX-License-Identifier: Apache-2.0
package app.doors.companion.core

import app.doors.companion.ui.Fmt
import app.doors.companion.ui.LogScale
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.cancel
import kotlinx.coroutines.cancelAndJoin
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.launch
import kotlinx.coroutines.runBlocking
import kotlinx.coroutines.withTimeout
import kotlinx.serialization.json.Json
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.jsonObject
import org.junit.After
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test
import java.io.DataInputStream
import java.net.InetAddress
import java.net.ServerSocket
import java.net.Socket
import java.util.concurrent.CopyOnWriteArrayList

/**
 * PacketShark on the phone (gateway.md section 9, ADR-016 amendment 1):
 *
 *  - only counts, rates and sizes are taken from an answer, by name;
 *  - nothing is asked of a unit that does not serve capture, has not granted
 *    the scope, or while no screen shows the figures;
 *  - a command needs `capture_control`, and a start request can only ask for
 *    what a tap on the unit could choose;
 *  - simulated figures always say that they are.
 *
 * The gateway here is played in this JVM, from the contract: no gatewayd
 * serves these methods yet.
 */
class CaptureTest {
    private val scope = CoroutineScope(Dispatchers.Default + Job())
    private val servers = ArrayList<ServerSocket>()

    @After
    fun close() {
        scope.cancel()
        servers.forEach { it.close() }
    }

    private fun json(s: String): JsonObject = Json.parseToJsonElement(s).jsonObject

    private val example = """{"available": true, "state": "capturing", "reason": "none", "iface": "eth0", "keep": "headers",
        "elapsed_ms": 61250, "limit_s": 900, "seen": 48211, "stored": 48211, "held": 0,
        "kernel_drops": 0, "iface_drops": 2, "shed": 5, "bytes": 31022764, "pps": 812, "bps": 4120000, "fill_permille": 310,
        "classes": {"tcp": 39120, "udp": 5210, "dns": 1630, "dhcp": 4, "arp": 61, "icmp": 12, "ipv6": 2101, "other": 73},
        "findings": [0, 3, 0, 1, 0, 0],
        "diag": {"dns_queries": 815, "dns_retries": 4, "dns_answered": 800, "dns_late": 0, "dns_noreply": 10, "dns_rtt_sum_us": 16000000, "tcp_attempts": 40, "tcp_connected": 38, "tcp_timed": 38, "tcp_hs_sum_us": 380000, "tcp_refused": 2},
        "requested_by_companion": false}"""

    // ---- 1. what is taken from an answer ----

    @Test
    fun theContractsExampleIsRead() {
        val s = CaptureStatus.parse(json(example))
        assertTrue(s.available)
        assertEquals(CaptureState.CAPTURING, s.state)
        assertEquals("eth0", s.iface)
        assertFalse(s.fullBytes)
        assertEquals(61250, s.elapsedMs)
        assertEquals(900, s.limitS)
        assertEquals(812, s.pps)
        assertEquals(7, s.drops)
        assertEquals(310, s.fillPermille)
        assertEquals(listOf(39120L, 5210L, 1630L, 4L, 61L, 12L, 2101L, 73L), s.classes)
        assertEquals(3, s.findings[CaptureSignal.TCP_RST.ordinal])
        assertEquals(20_000L, s.diag.dnsRttAvgUs)
        assertEquals(10_000L, s.diag.tcpHsAvgUs)
        assertEquals(2, s.diag.tcpFailures)
    }

    @Test
    fun nothingButTheNamedFiguresIsKept() {
        // A gateway that sent what it never should: none of it has a place to go.
        val leaky = example.dropLast(1) + """, "hosts": ["10.20.30.40"], "rows": [{"src": "fe80::1", "name": "printer.lan"}],
            "iface": "eth0; 10.20.30.40", "reason": "talked to printer.lan", "top_talker": "aa:bb:cc:dd:ee:ff"}"""
        val s = CaptureStatus.parse(json(leaky))
        val all = s.toString()
        for (secret in listOf("10.20.30.40", "fe80", "printer", "aa:bb")) assertFalse("kept $secret", all.contains(secret))
        assertEquals("", s.iface)
        assertEquals("none", s.reason)
    }

    @Test
    fun aFigureThatCannotBeIsNotShown() {
        val s = CaptureStatus.parse(json("""{"available": true, "state": "dancing", "pps": -5, "fill_permille": 4000, "keep": "everything", "findings": [1, "x", -2], "classes": {"tcp": "many"}}"""))
        assertEquals(CaptureState.IDLE, s.state)
        assertEquals(0, s.pps)
        assertEquals(1000, s.fillPermille)
        assertFalse(s.fullBytes)
        assertEquals(listOf(1L, 0L, 0L, 0L, 0L, 0L), s.findings)
        assertEquals(0, s.classes[0])
    }

    @Test
    fun notInFrontIsNothingElse() {
        assertFalse(CaptureStatus.parse(json("""{"available": false, "pps": 99}""")).available)
        assertEquals(0, CaptureStatus.parse(json("""{"available": false, "pps": 99}""")).pps)
        // An answer must say it; an event is only sent while PacketShark is there.
        assertFalse(CaptureStatus.parse(json("""{"state": "capturing"}""")).available)
        assertTrue(CaptureStatus.parse(json("""{"state": "capturing"}"""), event = true).available)
    }

    // ---- 2. the graph's seconds ----

    private fun reading(elapsed: Long, tcp: Long, udp: Long = 0, pps: Long = 0, state: CaptureState = CaptureState.CAPTURING, shed: Long = 0) =
        CaptureStatus(available = true, state = state, elapsedMs = elapsed, seen = tcp + udp, pps = pps, shed = shed, classes = listOf(tcp, udp, 0, 0, 0, 0, 0, 0))

    @Test
    fun aSecondIsTheDifferenceOfTwoReadings() {
        val f = CaptureFeed()
        assertFalse(f.take(reading(1000, 100)))
        assertTrue(f.take(reading(2000, 400, udp = 50, pps = 350, shed = 3)))
        assertEquals(1, f.history.size)
        assertEquals(listOf(300L, 50L, 0L, 0L, 0L, 0L, 0L, 0L), f.history[0].classes)
        assertEquals(3, f.history[0].drops)
        // The same moment twice (an answer and an event) is one second.
        assertFalse(f.take(reading(2000, 400, udp = 50)))
        assertFalse(f.take(reading(2100, 420, udp = 50)))
        assertEquals(1, f.history.size)
        // Two seconds apart: a rate, not a sum.
        assertTrue(f.take(reading(4000, 1000, udp = 50)))
        assertEquals(300, f.history[1].classes[0])
    }

    @Test
    fun aGraphBeginsWhenPacketSharkIsOpenedAfterThePhone() {
        // The usual order: the phone's screen first, PacketShark on the unit after it.
        val f = CaptureFeed()
        f.take(CaptureStatus(available = false))
        f.take(CaptureStatus(available = false))
        assertFalse(f.take(reading(1000, 100)))
        assertTrue(f.take(reading(2000, 300)))
        assertTrue(f.take(reading(3000, 600)))
        assertEquals(listOf(200L, 300L), f.history.map { it.classes[0] })
    }

    @Test
    fun anotherCaptureBeginsAnotherGraphAndItIsBounded() {
        val f = CaptureFeed()
        for (i in 1..200) f.take(reading(i * 1000L, i * 10L))
        assertEquals(CaptureFeed.KEPT, f.history.size)
        f.take(reading(1000, 5))
        assertEquals(0, f.history.size)
        f.take(reading(2000, 9))
        assertEquals(1, f.history.size)
        f.take(CaptureStatus(available = false))
        assertEquals(0, f.history.size)
    }

    // ---- 3. how figures are written ----

    @Test
    fun figures() {
        assertEquals("48 417", Fmt.count(48417))
        assertEquals("999", Fmt.count(999))
        assertEquals("1 000 000", Fmt.count(1_000_000))
        assertEquals("13k", Fmt.short(12_800))
        assertEquals("3.4M", Fmt.short(3_400_000))
        assertEquals("9.6" to "Mb/s", Fmt.bitrate(9_600_000))
        assertEquals("812" to "b/s", Fmt.bitrate(812))
        assertEquals("04:13", Fmt.clock(253_000))
        assertEquals("1:04:13", Fmt.clock(3_853_000))
        assertEquals("15 min", Fmt.limit(900))
        assertEquals("1 h", Fmt.limit(3600))
        assertEquals("no limit", Fmt.limit(0))
        assertEquals("12 ms", Fmt.latency(12_400))
        assertEquals("840 µs", Fmt.latency(840))
        assertEquals("—", Fmt.latency(null))
        assertEquals("71.2%", Fmt.share(712, 1000))
        assertEquals("—", Fmt.share(1, 0))
        assertEquals("31%", Fmt.permille(310))
    }

    @Test
    fun theScaleIsWholeDecades() {
        val quiet = LogScale(0, 1)
        assertEquals(100, quiet.hi)
        val busy = LogScale(1284, 1)
        assertEquals(10_000, busy.hi)
        assertEquals(listOf(10L, 100L, 1000L, 10_000L), busy.lines())
        assertEquals(0f, busy.at(0))
        assertTrue(busy.at(1) > 0f)
        assertEquals(0.75f, busy.at(1000), 0.001f)
        assertEquals(1f, busy.at(50_000))
    }

    // ---- 4. what is asked of a unit, and when ----

    /** A gateway on loopback that answers from [answers] and remembers every request. */
    private inner class FakeUnit(private val answers: (String, JsonObject) -> String?) {
        val asked = CopyOnWriteArrayList<Pair<String, JsonObject>>()
        private val server = ServerSocket(0, 4, InetAddress.getLoopbackAddress()).also { servers += it }

        @Volatile
        private var peer: Socket? = null

        init {
            Thread {
                try {
                    val s = server.accept()
                    peer = s
                    val i = DataInputStream(s.getInputStream())
                    while (true) {
                        val frame = ByteArray(i.readInt()).also { i.readFully(it) }.toString(Charsets.UTF_8)
                        val o = json(frame)
                        val method = o.string("method") ?: continue
                        val params = o.obj("params") ?: JsonObject(emptyMap())
                        asked += method to params
                        val a = answers(method, params) ?: continue
                        write(if (a.startsWith("!")) """{"id":${o.long("id")},"error":{"code":${a.drop(1)},"message":"x"}}""" else """{"id":${o.long("id")},"result":$a}""")
                    }
                } catch (_: Exception) {
                }
            }.apply { isDaemon = true }.start()
        }

        fun write(frame: String) {
            val b = frame.toByteArray()
            val o = peer!!.getOutputStream()
            synchronized(this) {
                o.write(byteArrayOf((b.size ushr 24).toByte(), (b.size ushr 16).toByte(), (b.size ushr 8).toByte(), b.size.toByte()) + b)
                o.flush()
            }
        }

        fun connect() = GatewayConnection(Socket(InetAddress.getLoopbackAddress(), server.localPort))
        fun methods() = asked.map { it.first }
    }

    private fun hello(vararg capabilities: String) =
        Hello(1, "00", "u", true, null, null, null, emptySet(), capabilities.toSet(), false, null, null)

    private val paired = PairedUnit("00".repeat(32), "u", "127.0.0.1", 1, 0)

    private fun serving(status: String = example): (String, JsonObject) -> String? = { method, params ->
        when (method) {
            "gateway.subscribe" -> """{"topics":${params["topics"]}}"""
            "capture.status" -> status
            // The state when asked, and that the capture was told to change (gateway.md section 9).
            "capture.hold" -> """{"state":"${if (params.bool("on") == true) "capturing" else "hold"}","changing":true}"""
            "capture.stop" -> """{"state":"capturing","changing":true}"""
            "capture.request_start" -> """{"shown":true}"""
            else -> "!1"
        }
    }

    private suspend fun await(what: String, check: () -> Boolean) = withTimeout(5_000) {
        while (!check()) delay(10)
    }.also { assertTrue(what, check()) }

    @Test
    fun aUnitThatDoesNotServeCaptureIsAskedNothing() = runBlocking {
        val unit = FakeUnit(serving())
        val conn = unit.connect()
        val capture = GatewayCapture(pollMs = 50)
        capture.watch(true)
        val job = scope.launch { capture.run(paired, conn, hello("status", "mesh"), MutableStateFlow(setOf("capture_view", "capture_control"))) }
        await("not served") { capture.view.value.access == CaptureAccess.NOT_SERVED }
        capture.hold(true)
        capture.requestStart("eth0", 900)
        delay(150)
        assertEquals(emptyList<String>(), unit.methods())
        assertNotNull(capture.view.value.notice)
        job.cancelAndJoin()
        assertEquals(CaptureAccess.OFFLINE, capture.view.value.access)
    }

    @Test
    fun withoutTheScopeOrOffScreenNothingIsRead() = runBlocking {
        val unit = FakeUnit(serving())
        val conn = unit.connect()
        val scopes = MutableStateFlow(setOf("status"))
        val capture = GatewayCapture(pollMs = 50)
        capture.watch(true)
        val job = scope.launch { capture.run(paired, conn, hello("capture"), scopes) }
        await("not granted") { capture.view.value.access == CaptureAccess.NOT_GRANTED }
        delay(120)
        assertEquals(emptyList<String>(), unit.methods())

        // Granted, but no screen shows the figures: still nothing.
        capture.watch(false)
        scopes.value = setOf("capture_view")
        await("waiting") { capture.view.value.access == CaptureAccess.WAITING }
        delay(120)
        assertEquals(emptyList<String>(), unit.methods())

        // On screen: the topic is held and the status read.
        capture.watch(true)
        await("open") { capture.view.value.access == CaptureAccess.OPEN }
        assertEquals("gateway.subscribe", unit.methods().first())
        assertTrue(unit.asked.first().second.strings("topics").contains("capture"))
        assertEquals(812L, capture.view.value.status?.pps)
        assertFalse(capture.view.value.simulated)
        assertFalse(capture.view.value.canControl)

        // Off screen again: the topic is given back and the figures are gone.
        capture.watch(false)
        await("given back") { unit.asked.last().let { it.first == "gateway.subscribe" && "capture" !in it.second.strings("topics") } }
        await("figures gone") { capture.view.value.status == null }
        val before = unit.asked.size
        delay(150)
        assertEquals(before, unit.asked.size)
        job.cancelAndJoin()
    }

    @Test
    fun eventsFillTheGraphAndAStateChangeIsAskedAbout() = runBlocking {
        val unit = FakeUnit(serving())
        val conn = unit.connect()
        val capture = GatewayCapture(pollMs = 2_000)
        capture.watch(true)
        val job = scope.launch { capture.run(paired, conn, hello("capture"), MutableStateFlow(setOf("capture_view"))) }
        await("open") { capture.view.value.access == CaptureAccess.OPEN }
        unit.write("""{"event":"capture.stats","data":${example.replace("\"elapsed_ms\": 61250", "\"elapsed_ms\": 62250").replace("\"tcp\": 39120", "\"tcp\": 39920").replace("\"available\": true, ", "")}}""")
        await("a second on the graph") { capture.view.value.history.size == 1 }
        assertEquals(800, capture.view.value.history[0].classes[0])
        val asks = unit.methods().count { it == "capture.status" }
        unit.write("""{"event":"capture.state","data":{"state":"stopped"}}""")
        await("asked again") { unit.methods().count { it == "capture.status" } > asks }
        assertEquals(CaptureAccess.OPEN, capture.view.value.access)
        job.cancelAndJoin()
    }

    @Test
    fun commandsNeedControlAndARequestAsksOnlyForWhatATapCould() = runBlocking {
        val unit = FakeUnit(serving())
        val conn = unit.connect()
        val scopes = MutableStateFlow(setOf("capture_view"))
        var clock = 1_000_000L
        val capture = GatewayCapture(now = { clock }, pollMs = 5_000)
        capture.watch(true)
        val job = scope.launch { capture.run(paired, conn, hello("capture"), scopes) }
        await("open") { capture.view.value.access == CaptureAccess.OPEN }

        // View only: nothing is sent.
        capture.hold(true)
        capture.stop()
        assertFalse(unit.methods().any { it == "capture.hold" || it == "capture.stop" })
        assertNotNull(capture.view.value.notice)

        scopes.value = setOf("capture_view", "capture_control")
        await("control") { capture.view.value.canControl && capture.view.value.access == CaptureAccess.OPEN }
        capture.hold(true)
        assertEquals(CaptureState.HOLD, capture.view.value.status?.state)
        assertNull(capture.view.value.busy)

        // Running: a request is not sent. Nor is one for full packets' cousin, an interface or a limit the unit does not offer.
        capture.requestStart("eth0", 900)
        capture.stop()
        assertEquals(CaptureState.STOPPED, capture.view.value.status?.state)
        capture.requestStart("eth0", 0)
        capture.requestStart("eth0", 7200)
        capture.requestStart("lo", 900)
        capture.requestStart("eth0 full", 900)
        assertFalse(unit.methods().contains("capture.request_start"))

        capture.requestStart("wlan0", 300)
        assertEquals(1, unit.methods().count { it == "capture.request_start" })
        assertEquals(setOf("iface", "limit_s"), unit.asked.last { it.first == "capture.request_start" }.second.keys)
        assertEquals(StartRequest.SHOWN, capture.view.value.request)

        // One every 30 seconds, kept here before the gateway has to say so.
        clock += 10_000
        capture.requestStart("wlan0", 300)
        assertEquals(1, unit.methods().count { it == "capture.request_start" })
        clock += 25_000
        capture.requestStart("wlan0", 300)
        assertEquals(2, unit.methods().count { it == "capture.request_start" })
        job.cancelAndJoin()
    }

    @Test
    fun aRefusalIsSaidAndOneFeaturesTopicsDoNotEndAnothers() = runBlocking {
        val unit = FakeUnit { method, params ->
            when (method) {
                "gateway.subscribe" -> """{"topics":${params["topics"]}}"""
                "capture.status" -> example.replace("capturing", "idle")
                else -> "!3"
            }
        }
        val conn = unit.connect()
        conn.subscribe(add = setOf("mesh", "mesh.activity"))
        val capture = GatewayCapture(pollMs = 5_000)
        capture.watch(true)
        val job = scope.launch { capture.run(paired, conn, hello("capture"), MutableStateFlow(setOf("capture_view", "capture_control"))) }
        await("open") { capture.view.value.access == CaptureAccess.OPEN }
        assertEquals(setOf("mesh", "mesh.activity", "capture"), unit.asked.last { it.first == "gateway.subscribe" }.second.strings("topics"))
        capture.requestStart("eth0", 900)
        assertTrue(capture.view.value.notice!!.contains("refused"))
        assertEquals(StartRequest.NONE, capture.view.value.request)
        capture.watch(false)
        await("mesh kept") { unit.asked.last().second.strings("topics") == setOf("mesh", "mesh.activity") }
        job.cancelAndJoin()
    }

    @Test
    fun whatBecomesOfARequestIsSaidByTheStateEvent() = runBlocking {
        val idle = example.replace("capturing", "idle")
        for ((event, want) in listOf(
            """{"state":"idle","reason":"refused","iface":"","keep":"headers","limit_s":0}""" to StartRequest.REFUSED,
            """{"state":"idle","reason":"timeout","iface":"","keep":"headers","limit_s":0}""" to StartRequest.TIMED_OUT,
            """{"state":"capturing","reason":"none","iface":"eth0","keep":"headers","limit_s":900}""" to StartRequest.STARTED,
        )) {
            val unit = FakeUnit(serving(idle))
            val conn = unit.connect()
            val capture = GatewayCapture(pollMs = 5_000)
            capture.watch(true)
            val job = scope.launch { capture.run(paired, conn, hello("capture"), MutableStateFlow(setOf("capture_view", "capture_control"))) }
            await("open") { capture.view.value.access == CaptureAccess.OPEN }
            capture.requestStart("eth0", 900)
            assertEquals(StartRequest.SHOWN, capture.view.value.request)
            // The status read after it is plain setup: only the event says what happened.
            unit.write("""{"event":"capture.state","data":$event}""")
            await("$want") { capture.view.value.request == want }
            job.cancelAndJoin()
        }
    }

    @Test
    fun aHoldThatChangesNothingIsShownAsTheUnitSaysItIs() = runBlocking {
        val unit = FakeUnit { method, params ->
            when (method) {
                "gateway.subscribe" -> """{"topics":${params["topics"]}}"""
                "capture.status" -> example
                // Already held by a tap on the unit, which this phone has not read yet.
                "capture.hold" -> """{"state":"hold","changing":false}"""
                "capture.request_start" -> "!5"
                else -> "!1"
            }
        }
        val conn = unit.connect()
        val capture = GatewayCapture(pollMs = 5_000)
        capture.watch(true)
        val job = scope.launch { capture.run(paired, conn, hello("capture"), MutableStateFlow(setOf("capture_view", "capture_control"))) }
        await("open") { capture.view.value.access == CaptureAccess.OPEN }
        // The phone reads "capturing" and asks to resume; the unit says it is held and nothing changed.
        capture.hold(false)
        assertEquals(CaptureState.HOLD, capture.view.value.status?.state)
        job.cancelAndJoin()
    }

    @Test
    fun aGrantDuringTheSessionIsTriedAndAGatewayWithoutCaptureIsNotServed() = runBlocking {
        // The hello named no capture capability because the phone held no capture scope then.
        val unit = FakeUnit(serving())
        val conn = unit.connect()
        val scopes = MutableStateFlow(setOf("status"))
        val capture = GatewayCapture(pollMs = 5_000)
        capture.watch(true)
        val job = scope.launch { capture.run(paired, conn, hello("status", "mesh"), scopes) }
        await("not granted") { capture.view.value.access == CaptureAccess.NOT_GRANTED }
        assertTrue(unit.methods().isEmpty())
        scopes.value = setOf("status", "capture_view")
        await("open once granted") { capture.view.value.access == CaptureAccess.OPEN }
        job.cancelAndJoin()

        // The same against a gateway that knows no capture method: asked once, then not served.
        val old = FakeUnit { method, params -> if (method == "gateway.subscribe") """{"topics":${params["topics"]}}""" else "!1" }
        val conn2 = old.connect()
        val scopes2 = MutableStateFlow(setOf("status"))
        val capture2 = GatewayCapture(pollMs = 200)
        capture2.watch(true)
        val job2 = scope.launch { capture2.run(paired, conn2, hello("status", "mesh"), scopes2) }
        await("not granted") { capture2.view.value.access == CaptureAccess.NOT_GRANTED }
        scopes2.value = setOf("status", "capture_view")
        await("not served") { capture2.view.value.access == CaptureAccess.NOT_SERVED }
        delay(700)
        assertEquals("asked once, not again and again", 1, old.methods().count { it == "capture.status" })
        job2.cancelAndJoin()
    }

    // ---- 5. the simulation ----

    @Test
    fun simulatedFiguresAlwaysSaySo() = runBlocking {
        val sim = SimulatedCapture(scope, tickMs = 20)
        assertTrue(sim.view.value.simulated)
        sim.watch(true)
        val v = withTimeout(5_000) { sim.view.first { it.history.size >= SimulatedCapture.PREFILL_S && it.status?.state == CaptureState.CAPTURING } }
        assertTrue(v.simulated)
        assertTrue(v.history.all { it.classes.sum() > 0 })
        sim.hold(true)
        assertEquals(CaptureState.HOLD, sim.view.value.status?.state)
        sim.stop()
        assertEquals(CaptureState.STOPPED, sim.view.value.status?.state)
        sim.requestStart("eth0", 1)
        assertNotNull(sim.view.value.notice)
        sim.requestStart("wlan0", 300)
        assertEquals(StartRequest.SHOWN, sim.view.value.request)
        val started = withTimeout(5_000) { sim.view.first { it.request == StartRequest.STARTED && it.status?.state == CaptureState.CAPTURING } }
        assertTrue(started.simulated && started.status!!.requestedByCompanion)
        assertEquals("wlan0", started.status!!.iface)
        sim.watch(false)
    }
}
