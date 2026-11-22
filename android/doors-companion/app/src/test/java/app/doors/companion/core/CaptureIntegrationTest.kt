// Copyright (c) 2026 Doors authors.
// SPDX-License-Identifier: Apache-2.0
package app.doors.companion.core

import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.async
import kotlinx.coroutines.cancelAndJoin
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch
import kotlinx.coroutines.runBlocking
import kotlinx.coroutines.withContext
import kotlinx.coroutines.withTimeout
import kotlinx.serialization.json.Json
import kotlinx.serialization.json.JsonArray
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.JsonPrimitive
import kotlinx.serialization.json.buildJsonObject
import kotlinx.serialization.json.put
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Assume.assumeTrue
import org.junit.Test
import java.io.BufferedReader
import java.io.InputStreamReader
import java.net.InetSocketAddress
import java.net.Socket

/**
 * The PacketShark room's reader against the real gatewayd in front of the real
 * shell with PacketShark (the simulator, on synthetic traffic), through the
 * harness (harness/run-gateway.sh with DOORS_SHELL_BIN and
 * DOORS_CAPTURE_HELPER). Skipped without a harness, and without a shell
 * behind it.
 *
 * A "tap" is the simulator's own finger on its panel, asked for through the
 * harness: the one thing here that a person at the unit would do. Nothing the
 * phone sends starts a capture.
 */
class CaptureIntegrationTest {
    private val ctlAddr = System.getProperty("doors.gateway.ctl").orEmpty()

    private val transport = object : Transport {
        override suspend fun connect(host: String, port: Int, timeoutMs: Int): Socket = withContext(Dispatchers.IO) {
            var last: java.io.IOException? = null
            repeat(8) {
                try {
                    return@withContext Socket().apply { connect(InetSocketAddress(host, port), timeoutMs) }
                } catch (e: java.net.ConnectException) {
                    last = e
                    Thread.sleep(400)
                }
            }
            throw last!!
        }

        override suspend fun probe(port: Int): List<String> = emptyList()
        override fun available() = true
    }

    private fun ctl(method: String, params: JsonObject? = null): JsonObject {
        val (h, p) = ctlAddr.split(":")
        Socket(h, p.toInt()).use { s ->
            s.soTimeout = 30_000
            s.getOutputStream().write((buildJsonObject {
                put("method", method)
                if (params != null) put("params", params)
            }.toString() + "\n").toByteArray())
            val out = Json.parseToJsonElement(BufferedReader(InputStreamReader(s.getInputStream())).readLine()) as JsonObject
            if (out["error"] != null) throw AssertionError("$method: $out")
            return out
        }
    }

    /** One request at the unit itself, on the shell's own socket. */
    private fun unit(method: String, params: JsonObject? = null): JsonObject =
        ctl("harness.unit", buildJsonObject {
            put("method", method)
            if (params != null) put("params", params)
        }).obj("result")!!

    private fun tap(text: String) = unit("shell.tap", buildJsonObject { put("text", text) }).bool("tapped") == true
    private fun unitStatus() = unit("capture.status")
    private fun helpers() = ctl("harness.shell", buildJsonObject { put("op", "helpers") }).obj("result")!!.long("helpers")

    private suspend fun until(what: String, ms: Long = 20_000, cond: () -> Boolean) {
        val end = System.currentTimeMillis() + ms
        while (!cond()) {
            if (System.currentTimeMillis() > end) throw AssertionError("never: $what")
            delay(50)
        }
    }

    private class Phone(val session: GatewaySession, val capture: GatewayCapture, val fingerprint: String) {
        val view get() = capture.view.value
    }

    private suspend fun pairPhone(scope: CoroutineScope, name: String, scopes: List<String>): Pair<Phone, PairedUnit> {
        val id = TestIdentity.create()
        val capture = GatewayCapture(pollMs = 1_500)
        val session = GatewaySession(transport, id, "test", features = listOf(capture), backoff = { 300 })
        val code = PairingUri.parse(ctl("gateway.pair_begin").obj("result")!!.string("uri")!!)
        var waited = false
        val pairing = scope.async { session.pair(code, name) { waited = true } }
        withTimeout(10_000) { while (!waited) delay(50) }
        delay(400)
        ctl("gateway.pair_decide", buildJsonObject {
            put("allow", true)
            put("scopes", JsonArray(scopes.map { JsonPrimitive(it) }))
        })
        val unit = (pairing.await() as PairOutcome.Paired).unit
        return Phone(session, capture, Fingerprint.hex(id.fingerprint())) to unit
    }

    @Test
    fun thePacketSharkRoomAgainstTheRealGatewayAndShell() = runBlocking {
        assumeTrue("no gateway harness named", ctlAddr.isNotEmpty())
        val withShell = try {
            helpers() == 0L
        } catch (_: AssertionError) {
            false
        }
        assumeTrue("the harness runs no real shell (DOORS_SHELL_BIN)", withShell)
        val scope = CoroutineScope(Dispatchers.Default + Job())
        val all = listOf("status", "capture_view", "capture_control")

        // ---- a clean bench: nobody paired, the unit on its home screen ----
        ctl("gateway.pair_cancel")
        for (c in ctl("gateway.clients").obj("result")!!.array("clients").filterIsInstance<JsonObject>()) {
            ctl("gateway.revoke", buildJsonObject { put("client", c.string("fingerprint")!!) })
        }
        unit("shell.home")
        delay(800)
        if (unit("shell.info").string("current") != "home") {
            tap("LEAVE")
            delay(800)
        }
        assertEquals("home", unit("shell.info").string("current"))

        // ---- one phone, reading: PacketShark is closed ----
        val (one, unitOne) = pairPhone(scope, "Capture phone one", all)
        val loopOne = scope.launch { one.session.run(unitOne) }
        one.capture.watch(true)
        until("PacketShark is closed on the unit, and said so") { one.view.access == CaptureAccess.CLOSED }
        assertTrue("the phone may ask", one.view.canControl)

        // ---- a request: PacketShark opens with its sheet, and nothing starts ----
        one.capture.requestStart("synth0", 300)
        until("the request is shown: ${one.view.notice}") { one.view.request == StartRequest.SHOWN }
        until("PacketShark was opened by it") { unit("shell.info").string("current") == "packetshark" }
        assertEquals("shown", unitStatus().string("request"))
        assertEquals("idle", unitStatus().string("state"))
        assertEquals("nothing was started", 0L, helpers())
        until("the phone reads setup") { one.view.access == CaptureAccess.OPEN && one.view.status?.state == CaptureState.IDLE }
        assertTrue("REFUSE, tapped on the panel", tap("REFUSE"))
        until("the phone is told: refused") { one.view.request == StartRequest.REFUSED }
        assertEquals(0L, helpers())
        // Its own 30 seconds: nothing is sent, and it says why.
        one.capture.requestStart("synth0", 300)
        assertTrue(one.view.notice!!.contains("30 seconds"))

        // ---- a second phone asks, and a finger taps START ----
        val (two, unitTwo) = pairPhone(scope, "Capture phone two", all)
        val loopTwo = scope.launch { two.session.run(unitTwo) }
        two.capture.watch(true)
        until("the second phone reads") { two.view.access == CaptureAccess.OPEN }
        two.capture.requestStart("synth0", 300)
        until("its request is shown: ${two.view.notice}") { two.view.request == StartRequest.SHOWN }
        delay(1_800) // START on the sheet takes no tap in its first moment
        assertEquals("asking started nothing", 0L, helpers())
        assertTrue("START, tapped on the panel", tap("START"))
        until("the phone that asked is told it started") { two.view.request == StartRequest.STARTED }
        until("and reads a running capture") { two.view.status?.state == CaptureState.CAPTURING }
        until("the other phone reads it too") { one.view.status?.state == CaptureState.CAPTURING }
        val s = two.view.status!!
        assertEquals("synth0", s.iface)
        assertEquals(300L, s.limitS)
        assertFalse("HEADERS", s.fullBytes)
        until("it is marked as asked for by a companion") { two.view.status?.requestedByCompanion == true }
        assertEquals("one helper, the app's own", 1L, helpers())
        until("the graph has its seconds", 15_000) { two.view.history.size >= 3 }
        val first = two.view.status!!.seen
        until("the figures move") { two.view.status!!.seen > first && two.view.status!!.seen > 100 }
        assertEquals("every frame has a class", two.view.status!!.seen, two.view.status!!.classes.sum())

        // ---- hold, resume, stop: the buttons' own behaviour ----
        two.capture.hold(true)
        until("held on the unit") { unitStatus().string("state") == "hold" }
        until("and read as held by both") { two.view.status?.state == CaptureState.HOLD && one.view.status?.state == CaptureState.HOLD }
        two.capture.hold(false)
        until("capturing again") { unitStatus().string("state") == "capturing" && one.view.status?.state == CaptureState.CAPTURING }
        two.capture.stop()
        until("stopped on the unit, by the user") { unitStatus().string("state") == "stopped" && unitStatus().string("reason") == "user" }
        until("and read so") { two.view.status?.state == CaptureState.STOPPED && two.view.status?.reason == "user" }
        assertEquals("a stopped capture keeps its packets on the unit", 1L, helpers())
        two.capture.stop()
        assertEquals("Nothing is capturing.", two.view.notice)

        // ---- nobody watches: nothing is read ----
        one.capture.watch(false)
        two.capture.watch(false)
        delay(2_500)
        val calls = unit("shell.info").obj("app_ipc")!!.long("calls")!!
        delay(3_000)
        assertEquals("with no screen showing the figures the unit is asked nothing", calls, unit("shell.info").obj("app_ipc")!!.long("calls"))

        // ---- the phone that asked is revoked; the unit is left as it was found ----
        ctl("gateway.revoke", buildJsonObject { put("client", two.fingerprint) })
        ctl("gateway.revoke", buildJsonObject { put("client", one.fingerprint) })
        loopOne.cancelAndJoin()
        loopTwo.cancelAndJoin()
        unit("shell.home")
        delay(800)
        if (unit("shell.info").string("current") != "home") {
            tap("LEAVE")
            delay(1_000)
        }
        until("no helper is left") { helpers() == 0L }
    }
}
