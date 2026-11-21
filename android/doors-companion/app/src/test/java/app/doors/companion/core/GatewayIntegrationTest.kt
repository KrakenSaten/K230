// Copyright (c) 2026 Doors authors.
// SPDX-License-Identifier: Apache-2.0
package app.doors.companion.core

import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.async
import kotlinx.coroutines.cancelAndJoin
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.first
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
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Assume.assumeTrue
import org.junit.Test
import java.io.BufferedReader
import java.io.InputStreamReader
import java.net.InetSocketAddress
import java.net.Socket

/**
 * The app's session code against the real gatewayd (harness/run-gateway.sh):
 * pairing, an authenticated session, real status reads, a scope reduction,
 * the unit going away and coming back, a wrong key, an unknown phone,
 * revocation and forgetting. One test, in the order a phone lives it.
 *
 * The phone's key is in the JVM here (TestIdentity); everything else is the
 * code the app runs. Skipped when no harness was named:
 *   gradle :app:testDebugUnitTest -Ddoors.gateway.ctl=127.0.0.1:47232
 */
class GatewayIntegrationTest {
    private val ctlAddr = System.getProperty("doors.gateway.ctl").orEmpty()

    private val transport = object : Transport {
        override suspend fun connect(host: String, port: Int, timeoutMs: Int): Socket = withContext(Dispatchers.IO) {
            // The harness runs in WSL: Windows forwards a loopback port a moment
            // after gatewayd binds it, and gatewayd binds only once a window is
            // open or a phone is paired. The bench's delay, not the unit's.
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
            s.soTimeout = 15_000
            s.getOutputStream().write((buildJsonObject {
                put("method", method)
                if (params != null) put("params", params)
            }.toString() + "\n").toByteArray())
            val line = BufferedReader(InputStreamReader(s.getInputStream())).readLine()
            return Json.parseToJsonElement(line) as JsonObject
        }
    }

    private fun clients(): List<JsonObject> =
        ctl("gateway.clients").obj("result")!!.array("clients").filterIsInstance<JsonObject>()

    private suspend fun GatewaySession.await(what: String, ms: Long = 20_000, p: (LinkState) -> Boolean): LinkState =
        try {
            withTimeout(ms) { state.first(p) }
        } catch (e: Exception) {
            throw AssertionError("$what: still ${state.value}")
        }

    @Test
    fun aPhonesLifeAgainstTheRealGateway() = runBlocking {
        assumeTrue("no gateway harness named", ctlAddr.isNotEmpty())
        val scope = CoroutineScope(Dispatchers.Default + Job())
        val phone = TestIdentity.create()
        val session = GatewaySession(transport, phone, "test")

        // ---- pairing: the code from the unit, the tap on the unit ----
        ctl("gateway.pair_cancel") // a window an earlier, broken run left open
        for (c in clients()) ctl("gateway.revoke", buildJsonObject { put("client", c.string("fingerprint")!!) })
        val first = ctl("gateway.pair_begin")
        val begun = first.obj("result") ?: throw AssertionError("pair_begin failed: $first")
        val code = PairingUri.parse(begun.string("uri")!!)
        var waited = false
        val pairing = scope.async { session.pair(code, "Test phone") { waited = true } }
        withTimeout(10_000) { while (!waited) { if (pairing.isCompleted) throw AssertionError("pairing ended early: " + pairing.await()); delay(50) } }
        delay(400) // the candidate's token reaches the unit
        assertTrue("nobody is paired before the tap", clients().isEmpty())
        ctl("gateway.pair_decide", buildJsonObject { put("allow", true) })
        val outcome = pairing.await()
        assertTrue("paired: $outcome", outcome is PairOutcome.Paired)
        val unit = (outcome as PairOutcome.Paired).unit
        assertEquals(Fingerprint.hex(code.fingerprint), unit.fingerprint)
        assertEquals(setOf("status", "mesh_read", "mesh_send"), outcome.scopes)
        assertEquals("the unit knows this phone by the fingerprint the app shows", Fingerprint.hex(phone.fingerprint()), clients().single().string("fingerprint"))

        // ---- a spent token opens nothing ----
        val again = GatewaySession(transport, TestIdentity.create(), "test").pair(code, "Second phone") {}
        assertFalse("a used code pairs nobody: $again", again is PairOutcome.Paired)

        // ---- the session: hello, then real status reads ----
        var loop = scope.launch { session.run(unit) }
        val online = session.await("online") { it is LinkState.Online } as LinkState.Online
        assertTrue(online.hello.paired)
        assertEquals(1, online.hello.proto)
        assertEquals(unit.fingerprint, online.hello.deviceId)
        assertTrue("status" in online.hello.capabilities)
        val t = withTimeout(10_000) { session.telemetry.first { it.mesh.data != null && it.system.data != null && it.info.data != null } }
        assertEquals("0.4.0", t.info.data!!.string("version"))
        assertNotNull(t.system.data!!.long("uptime_s"))
        assertEquals("workshop", t.wifi.data!!.string("ssid"))
        assertEquals("idle", t.radio.data!!.string("state"))
        assertEquals("ready", t.mesh.data!!.string("state"))
        // What the gateway filters stays filtered on the way to the phone.
        assertNull(t.wifi.data!!["bssid"])
        assertTrue(t.system.data!!.array("network").filterIsInstance<JsonObject>().none { "mac" in it })

        // ---- upstream away: the session stays, the reading is marked ----
        ctl("harness.upstream", buildJsonObject { put("service", "netd"); put("mode", "error") })
        withTimeout(10_000) { session.telemetry.first { it.wifi.error != null } }
        assertEquals("the last value stays", "workshop", session.telemetry.value.wifi.data!!.string("ssid"))
        assertTrue(session.state.value is LinkState.Online)
        ctl("harness.upstream", buildJsonObject { put("service", "netd"); put("mode", "ok") })
        withTimeout(10_000) { session.telemetry.first { it.wifi.error == null } }

        // ---- scopes reduced on the unit: told at once, held to it ----
        ctl("gateway.set_scopes", buildJsonObject {
            put("client", Fingerprint.hex(phone.fingerprint()))
            put("scopes", JsonArray(listOf(JsonPrimitive("mesh_read"))))
        })
        withTimeout(10_000) { session.scopes.first { it == setOf("mesh_read") } }
        withTimeout(10_000) { session.telemetry.first { it.system.error == GatewayError.NOT_PERMITTED } }
        ctl("gateway.set_scopes", buildJsonObject {
            put("client", Fingerprint.hex(phone.fingerprint()))
            put("scopes", JsonArray(listOf(JsonPrimitive("status"))))
        })
        withTimeout(10_000) { session.telemetry.first { it.system.error == null } }

        // ---- the unit goes away and comes back: the loop finds it again ----
        ctl("harness.stop")
        session.await("away") { it is LinkState.Waiting || it is LinkState.Connecting }
        delay(1_500)
        ctl("harness.start")
        session.kick()
        session.await("online again", 30_000) { it is LinkState.Online }
        assertEquals("still one paired client after the restart", 1, clients().size)

        // ---- another key at the unit's address is never accepted ----
        val impostor = GatewaySession(transport, phone, "test")
        val wrong = unit.copy(fingerprint = Fingerprint.hex(TestIdentity.create().fingerprint()))
        val wrongLoop = scope.launch { impostor.run(wrong) }
        assertEquals(LinkState.Stopped(Stop.WRONG_UNIT), impostor.await("wrong unit") { it is LinkState.Stopped })
        wrongLoop.cancelAndJoin()
        val wrongCode = GatewaySession(transport, TestIdentity.create(), "test")
            .pair(PairingUri.parse("doors-pair:1?h=${unit.host}&p=${unit.port}&fp=${b64(TestIdentity.create().fingerprint())}&t=${b64(ByteArray(16))}&n=x"), "x") {}
        assertEquals("a code promising another key sends nothing", PairOutcome.WrongUnit, wrongCode)

        // ---- a phone the unit does not know is turned away, and the loop stops ----
        val stranger = GatewaySession(transport, TestIdentity.create(), "test")
        val strangerLoop = scope.launch { stranger.run(unit) }
        assertEquals(LinkState.Stopped(Stop.REVOKED), stranger.await("stranger", 40_000) { it is LinkState.Stopped })
        strangerLoop.cancelAndJoin()
        assertTrue("the paired session was not disturbed", session.state.value is LinkState.Online)

        // ---- revoked on the unit: told, closed, and no reconnect loop ----
        ctl("gateway.revoke", buildJsonObject { put("client", Fingerprint.hex(phone.fingerprint())) })
        assertEquals(LinkState.Stopped(Stop.REVOKED), session.await("revoked") { it is LinkState.Stopped })
        loop.join()
        assertTrue(clients().isEmpty())

        // ---- pair again, then forget from the phone ----
        val code2 = PairingUri.parse(ctl("gateway.pair_begin").obj("result")!!.string("uri")!!)
        waited = false
        val pairing2 = scope.async { session.pair(code2, "Test phone") { waited = true } }
        withTimeout(10_000) { while (!waited) { if (pairing2.isCompleted) throw AssertionError("pairing ended early: " + pairing2.await()); delay(50) } }
        delay(400)
        ctl("gateway.pair_decide", buildJsonObject { put("allow", true) })
        assertTrue(pairing2.await() is PairOutcome.Paired)
        loop = scope.launch { session.run(unit) }
        session.await("online after pairing again") { it is LinkState.Online }
        assertTrue("the unit was told", session.unpair())
        delay(500)
        assertTrue("the unit forgot the phone", clients().isEmpty())
        loop.cancelAndJoin()

        // ---- refused on the unit ----
        val code3 = PairingUri.parse(ctl("gateway.pair_begin").obj("result")!!.string("uri")!!)
        waited = false
        val pairing3 = scope.async { session.pair(code3, "Test phone") { waited = true } }
        withTimeout(10_000) { while (!waited) { if (pairing3.isCompleted) throw AssertionError("pairing ended early: " + pairing3.await()); delay(50) } }
        delay(400)
        ctl("gateway.pair_decide", buildJsonObject { put("allow", false) })
        assertEquals(PairOutcome.Refused, pairing3.await())
        assertTrue(clients().isEmpty())
    }

    private fun b64(b: ByteArray) = java.util.Base64.getUrlEncoder().withoutPadding().encodeToString(b)
}
