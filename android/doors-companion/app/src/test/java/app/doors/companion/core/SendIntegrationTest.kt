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
import org.junit.Assert.assertNotEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Assume.assumeTrue
import org.junit.Test
import java.io.BufferedReader
import java.io.InputStreamReader
import java.net.InetSocketAddress
import java.net.Socket

/**
 * MeshSend with MeshSync against the real gatewayd (M4.2) and the harness's
 * fake meshcored (harness/run-gateway.sh), which counts what it was made to
 * "transmit". What is held to throughout is that count: one tap is at most
 * one transmission, whatever became of the answer, the connection, the mesh
 * service or the gateway, and a message the app cannot account for is said to
 * be unknown, never sent again by itself.
 *
 * Covered: the scope on its own; a direct and a channel message with their
 * delivery states; a channel's own text limit; a second tap; an answer lost
 * with the message taken, not taken, and with the line gone quiet; a refusal;
 * a resend; the phone's connection lost with a send in flight; the gateway
 * restarting under two unanswered sends; the mesh service restarting; the
 * gateway's rate limit; the scope taken away, and the phone revoked.
 *
 * Skipped when no harness was named:
 *   gradle :app:testDebugUnitTest -Ddoors.gateway.ctl=127.0.0.1:47232
 */
class SendIntegrationTest {
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

    private fun mesh(op: String, build: kotlinx.serialization.json.JsonObjectBuilder.() -> Unit = {}): JsonObject =
        ctl("harness.mesh", buildJsonObject { put("op", op); build() }).obj("result")!!

    private fun mode(m: String) = mesh("send_mode") { put("mode", m) }

    /** What the fake meshcored was asked to send, and what it was made to transmit. */
    private fun asked() = mesh("sends").long("asked")!!.toInt()
    private fun taken() = mesh("sends").array("taken").filterIsInstance<JsonObject>()
    private fun takenTexts() = taken().mapNotNull { it.string("text") }

    private fun key(i: Int) = "%02x".format(0xa0 + i) + "%062x".format(0x5eed0000 + i * 7919)

    private fun scopes(fp: String, vararg s: String) =
        ctl("gateway.set_scopes", buildJsonObject { put("client", fp); put("scopes", JsonArray(s.map { JsonPrimitive(it) })) })

    @Test
    fun sendingThroughTheRealGateway() = runBlocking {
        assumeTrue("no gateway harness named", ctlAddr.isNotEmpty())
        val scope = CoroutineScope(Dispatchers.Default + Job())
        val phone = TestIdentity.create()
        val fp = Fingerprint.hex(phone.fingerprint())
        val history = MemoryMeshStore()
        val outbox = MemorySendStore(history)
        val sync = MeshSync(history, retryMs = 500)
        val send = MeshSend(outbox, sync.link, settleMs = 500)
        // The gateway's own limit is what is tested, at the end; until then the phone's count stays out of the way.
        send.guardRate = false
        val session = GatewaySession(transport, phone, "test", features = listOf(sync, send), backoff = { 300 })
        fun phase() = sync.link.value.phase
        fun row(text: String) = outbox.all().last { it.text == text }
        fun heard(o: Outgoing) = synchronized(history) { history.messages[o.runId to o.messageId] }

        suspend fun until(what: String, ms: Long = 20_000, cond: () -> Boolean) {
            val end = System.currentTimeMillis() + ms
            while (!cond()) {
                if (System.currentTimeMillis() > end) throw AssertionError("never: $what; mesh ${sync.link.value}; send ${send.state.value}; rows ${outbox.all().takeLast(3)}")
                delay(50)
            }
        }
        suspend fun ready() = until("live and allowed to send") { send.blocked() == null }
        val ola = key(0)
        val kari = key(1)
        val toOla = "d:$ola"
        var uid = "" // the unit, as the stores name it
        suspend fun say(text: String, conv: String = toOla, peer: String? = ola, channel: Int? = null, limit: Int = 160) = send.send(uid, conv, peer, channel, text, limit)

        // ---- a clean bench: nobody paired, a new run of meshcored, nothing automatic ----
        ctl("gateway.pair_cancel")
        for (c in ctl("gateway.clients").obj("result")!!.array("clients").filterIsInstance<JsonObject>()) {
            ctl("gateway.revoke", buildJsonObject { put("client", c.string("fingerprint")!!) })
        }
        mesh("demo") { put("on", false) }
        mesh("auto") { put("on", false) }
        mode("ok")
        val runA = mesh("restart").string("run_id")!!
        mesh("sends") { put("reset", true) }

        val code = PairingUri.parse(ctl("gateway.pair_begin").obj("result")!!.string("uri")!!)
        var waited = false
        val pairing = scope.async { session.pair(code, "Send test phone") { waited = true } }
        withTimeout(10_000) { while (!waited) delay(50) }
        delay(400)
        ctl("gateway.pair_decide", buildJsonObject { put("allow", true) })
        val unit = (pairing.await() as PairOutcome.Paired).unit
        uid = unit.fingerprint

        // ---- reading alone does not send ----
        scopes(fp, "status", "mesh_read")
        var loop = scope.launch { session.run(unit) }
        until("live, reading") { phase() == MeshPhase.LIVE && send.state.value.session }
        assertTrue("the gateway names the capability", send.state.value.served)
        assertEquals(SendBlock.NOT_PERMITTED, say("not allowed"))
        assertEquals("nothing left the phone", 0, outbox.all().size)
        scopes(fp, "status", "mesh_read", "mesh_send")
        ready()
        assertEquals("the gateway's own limit is the phone's", 20, send.state.value.perMin)

        // ---- a direct message: accepted, then delivered, as the journal says ----
        assertNull(say("hello Ola"))
        until("accepted") { row("hello Ola").phase == OutPhase.ACCEPTED }
        val first = row("hello Ola")
        assertEquals(runA, first.runId)
        until("its message in the history") { heard(first) != null }
        heard(first)!!.let {
            assertTrue(it.outgoing && it.ackExpected)
            assertEquals("hello Ola", it.text)
            assertEquals(toOla, it.conversation)
            assertEquals("sent_flood", it.state)
        }
        mesh("state") { put("id", first.messageId!!); put("state", "acked") }
        until("delivered") { heard(first)?.state == "acked" }
        assertEquals(listOf("hello Ola"), takenTexts())

        // ---- a channel message: no acknowledgement exists; on the air and heard back is all there is ----
        assertNull(say("evening all", "c:1:a4", null, 1, 140))
        until("the channel message") { row("evening all").phase == OutPhase.ACCEPTED && heard(row("evening all")) != null }
        heard(row("evening all"))!!.let {
            assertTrue(it.channelKind && !it.ackExpected)
            assertEquals("c:1:a4", it.conversation)
            assertEquals("evening all", MeshMessage.body(it.text, it.senderName))
        }
        mesh("state") { put("id", row("evening all").messageId!!); put("more", buildJsonObject { put("transmitted", true); put("heard_back", 2) }) }
        until("on the air and heard back") { heard(row("evening all"))?.let { it.transmitted == true && it.heardBack == 2 } == true }
        // A channel carries less than a node; the phone knows the unit's number and does not ask.
        assertEquals(140, history.channels[1]!!.textLimit)
        assertEquals(SendBlock.TEXT, say("æ".repeat(71), "c:1:a4", null, 1, SendText.limit(history.channels[1]!!.textLimit)))
        // And if it did ask, the unit refuses, and says nothing was sent.
        assertNull(say("x".repeat(150), "c:1:a4", null, 1, 160))
        until("refused by the unit") { row("x".repeat(150)).phase == OutPhase.FAILED }
        assertEquals(Why.REFUSED, row("x".repeat(150)).why)
        assertEquals(listOf("hello Ola", "evening all"), takenTexts())

        // ---- two taps, one message ----
        mode("hold")
        assertNull(say("once"))
        assertEquals(SendBlock.IN_FLIGHT, say("once"))
        until("the one request") { asked() == 4 }
        mesh("release")
        mode("ok")
        until("accepted once") { row("once").phase == OutPhase.ACCEPTED }
        assertEquals(1, takenTexts().count { it == "once" })
        assertEquals(1, outbox.all().count { it.text == "once" })

        // ---- the answer is lost and the message was taken: found, not sent again ----
        mode("swallow")
        assertNull(say("taken, answer lost"))
        until("no answer") { row("taken, answer lost").phase == OutPhase.UNCERTAIN }
        mode("ok")
        until("found by asking under the same id") { row("taken, answer lost").phase == OutPhase.ACCEPTED }
        assertEquals(1, takenTexts().count { it == "taken, answer lost" })
        until("and it is that message") { heard(row("taken, answer lost"))?.text == "taken, answer lost" }

        // ---- the answer is lost and the message was not taken: the gateway proves it and sends it, once ----
        mode("ignore")
        assertNull(say("never read"))
        until("no answer") { row("never read").phase == OutPhase.UNCERTAIN }
        assertEquals(0, takenTexts().count { it == "never read" })
        mode("ok")
        until("sent by the repeat") { row("never read").phase == OutPhase.ACCEPTED }
        assertEquals(1, takenTexts().count { it == "never read" })

        // ---- the gateway restarts under two unanswered sends: nothing is repeated, the history decides ----
        ready()
        val all = "c:0:11"
        mode("ignore")
        val askedFirst = asked()
        assertNull(say("lost with the gateway", all, null, 0, 140))
        until("the first reached the mesh service") { asked() == askedFirst + 1 }
        mode("swallow")
        assertNull(say("kept by the mesh service"))
        until("the second was taken") { takenTexts().contains("kept by the mesh service") }
        val askedBefore = asked()
        ctl("harness.stop")
        until("the session is gone") { !send.state.value.session }
        assertEquals(OutPhase.UNCERTAIN, row("lost with the gateway").phase)
        assertEquals(OutPhase.UNCERTAIN, row("kept by the mesh service").phase)
        mode("ok")
        ctl("harness.start")
        session.kick()
        until("both are settled", 40_000) { row("lost with the gateway").phase != OutPhase.UNCERTAIN && row("kept by the mesh service").phase != OutPhase.UNCERTAIN }
        assertEquals("found in what the new gateway holds", OutPhase.ACCEPTED, row("kept by the mesh service").phase)
        assertEquals("kept by the mesh service", heard(row("kept by the mesh service"))!!.text)
        assertEquals(OutPhase.UNKNOWN to Why.GATEWAY_RESTARTED, row("lost with the gateway").let { it.phase to it.why })
        delay(1_500)
        assertEquals("a gateway that never heard of an id is not asked about it", askedBefore, asked())
        assertEquals(0, takenTexts().count { it == "lost with the gateway" })
        assertEquals(1, takenTexts().count { it == "kept by the mesh service" })

        // ---- the line goes quiet: nothing can be proven, and it is never sent again by itself ----
        mode("drop")
        assertNull(say("the line went quiet"))
        until("no answer") { row("the line went quiet").phase == OutPhase.UNCERTAIN }
        mode("ok")
        // The gateway gave its connection to the mesh service up; the app asks once by itself, a person after that.
        delay(1_500)
        ready()
        until("the app has asked, or is done") { row("the line went quiet").let { it.phase == OutPhase.UNKNOWN || (it.phase == OutPhase.UNCERTAIN && it.checks >= 1) } }
        if (row("the line went quiet").phase == OutPhase.UNCERTAIN) {
            ready()
            assertNull(send.check(uid, row("the line went quiet").clientMsgId))
        }
        until("outcome unknown", 40_000) { row("the line went quiet").phase == OutPhase.UNKNOWN }
        val quiet = row("the line went quiet")
        assertEquals(Why.OUTCOME_UNKNOWN, quiet.why)
        assertEquals(SendBlock.NOT_FOUND, send.check(uid, quiet.clientMsgId))
        delay(1_500)
        assertEquals("nothing sends it", 0, takenTexts().count { it == "the line went quiet" })
        // A person decides to send the words as a new message.
        ready()
        assertNull(send.sendAgain(uid, quiet.clientMsgId, ola, null, 160))
        until("the new message") { row("the line went quiet").phase == OutPhase.ACCEPTED }
        assertNotEquals(quiet.clientMsgId, row("the line went quiet").clientMsgId)
        assertEquals(1, takenTexts().count { it == "the line went quiet" })
        assertEquals("the unknown one is gone, not kept beside it", 1, outbox.all().count { it.text == "the line went quiet" })

        // ---- the unit has no room: refused, nothing sent, and trying again is a new send ----
        mode("refuse")
        assertNull(say("when there is room"))
        until("refused") { row("when there is room").phase == OutPhase.FAILED }
        assertEquals(Why.BUSY, row("when there is room").why)
        assertEquals(0, takenTexts().count { it == "when there is room" })
        mode("ok")
        assertNull(send.sendAgain(uid, row("when there is room").clientMsgId, ola, null, 160))
        until("sent on the second try") { row("when there is room").phase == OutPhase.ACCEPTED }
        assertEquals(1, takenTexts().count { it == "when there is room" })

        // ---- no acknowledgement, and a person asks for it again ----
        assertNull(say("are you there", "d:$kari", kari))
        until("accepted") { row("are you there").phase == OutPhase.ACCEPTED && heard(row("are you there")) != null }
        val unanswered = row("are you there")
        mesh("state") { put("id", unanswered.messageId!!); put("state", "no_ack") }
        until("no acknowledgement") { heard(unanswered)?.state == "no_ack" }
        assertNull(send.resend(uid, "d:$kari", unanswered.runId!!, unanswered.messageId!!, heard(unanswered)!!.attempts))
        until("resent") { outbox.all().any { it.resend && it.phase == OutPhase.ACCEPTED } }
        until("the same message, sent twice") { heard(unanswered)?.let { it.attempts == 2 && it.state == "sent_direct" } == true }
        assertEquals(unanswered.messageId, taken().last().long("resend"))
        assertEquals(1, takenTexts().count { it == "are you there" })

        // ---- the phone's connection is lost with a send in flight ----
        mode("hold")
        val before = asked()
        assertNull(say("phone went away"))
        until("it reached the mesh service") { asked() == before + 1 }
        loop.cancelAndJoin()
        assertEquals(OutPhase.UNCERTAIN, row("phone went away").phase)
        assertEquals("nothing is taken while away", SendBlock.OFFLINE, say("not while away"))
        mesh("release") // the unit sent it; nobody was there to hear the answer
        mode("ok")
        loop = scope.launch { session.run(unit) }
        until("answered from the gateway's record") { row("phone went away").phase == OutPhase.ACCEPTED }
        assertEquals(1, takenTexts().count { it == "phone went away" })

        // ---- the mesh service restarts: an older run's message cannot be resent, and sending goes on ----
        ready()
        assertNull(say("before the restart", "d:$kari", kari))
        until("accepted") { row("before the restart").phase == OutPhase.ACCEPTED && heard(row("before the restart")) != null }
        val old = row("before the restart")
        val runB = mesh("restart").string("run_id")!!
        mesh("sends") { put("reset", true) }
        until("the new run") { sync.link.value.runId == runB && send.blocked() == null }
        assertEquals(SendBlock.STALE_RUN, send.resend(uid, "d:$kari", old.runId!!, old.messageId!!, 1))
        assertEquals("it stays what it was: waiting, in a run that is over", "sent_flood", heard(old)!!.state)
        assertNull(say("after the restart"))
        until("sent in the new run") { row("after the restart").phase == OutPhase.ACCEPTED }
        assertEquals(runB, row("after the restart").runId)
        assertEquals(0, asked() - taken().size)

        // ---- the gateway's rate limit: said, and nothing sent beyond it ----
        var limited: Outgoing? = null
        var tries = 0
        while (limited == null && tries < 30) {
            val text = "rate ${tries++}"
            assertNull(say(text))
            until("rate $text settles") { row(text).phase != OutPhase.SENDING }
            if (row(text).phase == OutPhase.FAILED) limited = row(text) else assertEquals(OutPhase.ACCEPTED, row(text).phase)
        }
        assertEquals(Why.RATE, limited!!.why)
        assertTrue("the limit came within the gateway's 20 a minute: $tries", tries in 2..21)
        assertEquals("what was limited was not sent", tries - 1, takenTexts().count { it.startsWith("rate ") })
        send.guardRate = true
        val atLimit = asked()
        assertEquals("and the phone now waits by itself", SendBlock.RATE, say("one too many"))
        assertEquals(atLimit, asked())
        assertTrue(send.state.value.rateOpenAtMs > System.currentTimeMillis())

        // ---- the scope is taken away, then the phone is revoked ----
        scopes(fp, "status", "mesh_read")
        until("sending is no longer granted") { !send.state.value.permitted }
        assertEquals(SendBlock.NOT_PERMITTED, send.blocked())
        assertEquals(SendBlock.NOT_PERMITTED, say("revoked"))
        ctl("gateway.revoke", buildJsonObject { put("client", fp) })
        until("revoked") { session.state.value == LinkState.Stopped(Stop.REVOKED) }
        until("no session") { !send.state.value.session }
        assertEquals(SendBlock.OFFLINE, say("after the revocation"))
        assertEquals(atLimit, asked())

        // ---- at most once, over everything above ----
        val words = outbox.all().filter { !it.resend }.map { it.text }
        assertEquals("every send is one entry", words.size, words.toSet().size)
        loop.cancelAndJoin()
        Unit
    }
}
