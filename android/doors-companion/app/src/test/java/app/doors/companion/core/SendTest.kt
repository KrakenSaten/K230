// Copyright (c) 2026 Doors authors.
// SPDX-License-Identifier: Apache-2.0
package app.doors.companion.core

import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.cancelAndJoin
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.launch
import kotlinx.coroutines.runBlocking
import kotlinx.serialization.json.Json
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.buildJsonObject
import kotlinx.serialization.json.put
import org.junit.After
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test
import java.io.DataInputStream
import java.io.IOException
import java.net.InetAddress
import java.net.ServerSocket
import java.net.Socket
import java.util.concurrent.CopyOnWriteArrayList

/** SendStore in memory, with the rules the Room store keeps. [sent] stands for the synced history. */
class MemorySendStore(private val history: MemoryMeshStore? = null) : SendStore {
    class Sent(val conversation: String, val text: String, val runId: String, val id: Long, val ord: Long, val attempts: Int? = null)

    val rows = LinkedHashMap<String, Outgoing>()
    val sent = ArrayList<Sent>()
    val drafts = HashMap<String, String>()
    private var next = 1L

    override suspend fun add(unit: String, o: Outgoing, clearDraft: Boolean, replaces: String?): Outgoing = synchronized(this) {
        val stored = o.copy(ord = next++, afterOrd = rows[replaces]?.afterOrd ?: history?.size?.toLong() ?: sent.maxOfOrNull { it.ord } ?: 0)
        if (replaces != null) rows.remove(replaces)
        rows[o.clientMsgId] = stored
        if (clearDraft) drafts.remove(o.conversation)
        stored
    }

    override suspend fun get(unit: String, clientMsgId: String) = synchronized(this) { rows[clientMsgId] }

    override suspend fun put(unit: String, o: Outgoing) = synchronized(this) {
        if (o.clientMsgId in rows) rows[o.clientMsgId] = o
    }

    override suspend fun remove(unit: String, clientMsgId: String) = synchronized(this) {
        rows.remove(clientMsgId)
        Unit
    }

    override suspend fun open(unit: String) = synchronized(this) { rows.values.filter { it.phase == OutPhase.SENDING || it.phase == OutPhase.UNCERTAIN } }

    override suspend fun newerThan(unit: String, ord: Long) = synchronized(this) { (next - 1 - ord).toInt() }

    override suspend fun findSent(unit: String, o: Outgoing): Pair<String, Long>? = synchronized(this) {
        if (history != null) {
            // The synced history itself, in the order it was learned.
            val learned = synchronized(history) { history.messages.values.toList() }
            return@synchronized learned.withIndex().firstOrNull { (i, m) ->
                i + 1 > o.afterOrd && m.outgoing && m.conversation == o.conversation && MeshMessage.body(m.text, m.senderName) == o.text &&
                    rows.values.none { !it.resend && it.runId == m.runId && it.messageId == m.id }
            }?.let { it.value.runId to it.value.id }
        }
        sent.firstOrNull { s ->
            s.conversation == o.conversation && s.text == o.text && s.ord > o.afterOrd &&
                rows.values.none { !it.resend && it.runId == s.runId && it.messageId == s.id }
        }?.let { it.runId to it.id }
    }

    override suspend fun attempts(unit: String, runId: String, messageId: Long) = synchronized(this) {
        if (history != null) synchronized(history) { history.messages[runId to messageId]?.attempts } else sent.firstOrNull { it.runId == runId && it.id == messageId }?.attempts
    }

    @Synchronized
    fun all() = rows.values.toList()

    @Synchronized
    fun only() = rows.values.single()
}

/** A gateway on loopback that says what the test tells it to: a result, an error, or nothing. */
class ScriptedGateway(private val answer: (method: String, params: JsonObject) -> Any?) {
    class Err(val code: Int, val message: String = "error")

    private val server = ServerSocket(0, 1, InetAddress.getLoopbackAddress())
    private var peer: Socket? = null
    val requests = CopyOnWriteArrayList<Pair<String, JsonObject>>()

    init {
        Thread {
            try {
                val s = server.accept()
                peer = s
                val input = DataInputStream(s.getInputStream())
                while (true) {
                    val buf = ByteArray(input.readInt())
                    input.readFully(buf)
                    val req = Json.parseToJsonElement(String(buf)) as JsonObject
                    val method = req.string("method")!!
                    val params = req.obj("params") ?: JsonObject(emptyMap())
                    requests += method to params
                    val out = when (val a = answer(method, params)) {
                        null -> continue
                        is Err -> buildJsonObject {
                            put("id", req.long("id")!!)
                            put("error", buildJsonObject { put("code", a.code); put("message", a.message) })
                        }
                        else -> buildJsonObject { put("id", req.long("id")!!); put("result", a as JsonObject) }
                    }.toString().toByteArray()
                    synchronized(this) {
                        s.getOutputStream().write(byteArrayOf((out.size ushr 24).toByte(), (out.size ushr 16).toByte(), (out.size ushr 8).toByte(), out.size.toByte()) + out)
                        s.getOutputStream().flush()
                    }
                }
            } catch (_: IOException) {
            }
        }.apply { isDaemon = true }.start()
    }

    fun connect() = GatewayConnection(Socket(InetAddress.getLoopbackAddress(), server.localPort))

    fun drop() {
        peer?.close()
        server.close()
    }
}

class SendTest {
    private val unit = PairedUnit("ab".repeat(32), "bench", "127.0.0.1", 1, 0)
    private val id = unit.fingerprint
    private val key = "c1".repeat(32)
    private val conv = "d:$key"
    private val store = MemorySendStore()
    private val link = MutableStateFlow(MeshLink(MeshPhase.LIVE, runId = "run1", journalId = "j1"))
    private val scopes = MutableStateFlow(setOf("status", "mesh_read", "mesh_send"))
    private val scope = CoroutineScope(Dispatchers.Default + Job())
    private val gateways = ArrayList<ScriptedGateway>()

    @After
    fun done() {
        gateways.forEach { it.drop() }
    }

    private fun hello(capabilities: Set<String> = setOf("status", "mesh", "mesh.send"), perMin: Int? = 20) =
        Hello(1, id, "bench", true, null, null, null, scopes.value, capabilities, true, null, null, perMin)

    private fun sender(callMs: Long = 400, settleMs: Long = 100, windowMs: Long = 60_000) = MeshSend(store, link, callMs = callMs, settleMs = settleMs, autoWindowMs = windowMs)

    private fun accepted(messageId: Long = 7, duplicate: Boolean = false) = buildJsonObject {
        put("accepted", true); put("message_id", messageId); put("run_id", "run1"); put("route", "flood")
        put("ack_expected", true); put("ack_timeout_ms", 9000); put("duplicate", duplicate)
    }

    private fun session(send: MeshSend, hello: Hello = hello(), answer: (String, JsonObject) -> Any?): Pair<ScriptedGateway, Job> {
        val gw = ScriptedGateway(answer).also { gateways += it }
        val conn = gw.connect()
        val job = scope.launch {
            try {
                send.run(unit, conn, hello, scopes)
            } finally {
                conn.close()
            }
        }
        return gw to job
    }

    private suspend fun until(what: String, ms: Long = 5_000, cond: () -> Boolean) {
        val end = System.currentTimeMillis() + ms
        while (!cond()) {
            if (System.currentTimeMillis() > end) throw AssertionError("never: $what; rows ${store.all()}")
            delay(10)
        }
    }

    private suspend fun online(send: MeshSend) = until("the session") { send.state.value.session }

    @Test
    fun textIsCountedInBytesAndKeptClean() {
        assertEquals(1, SendText.bytes("a"))
        assertEquals(2, SendText.bytes("æ"))
        assertEquals(3, SendText.bytes("€"))
        assertEquals(4, SendText.bytes("😀"))
        assertNull(SendText.check("a".repeat(160), 160))
        assertEquals(SendText.Fault.TOO_LONG, SendText.check("a".repeat(161), 160))
        assertNull(SendText.check("æ".repeat(80), 160))
        assertEquals("81 two-byte letters are 162 bytes", SendText.Fault.TOO_LONG, SendText.check("æ".repeat(81), 160))
        assertNull(SendText.check("😀".repeat(35), 140))
        assertEquals("a channel's own limit", SendText.Fault.TOO_LONG, SendText.check("😀".repeat(36), 140))
        assertEquals(SendText.Fault.EMPTY, SendText.check("", 160))
        assertEquals(SendText.Fault.EMPTY, SendText.check(" \n\t", 160))
        assertNull("newline and tab are text", SendText.check("a\nb\tc", 160))
        for (bad in listOf("a\u0000b", "a\u0007", "a\rb", "a\u001b[0m", "a\u007f", "a\u0085b", "a\u009f")) {
            assertEquals(bad.map { it.code }.toString(), SendText.Fault.CONTROL, SendText.check(bad, 160))
        }
        assertEquals(SendText.Fault.MALFORMED, SendText.check("a\ud83d", 160))
        assertEquals(SendText.Fault.MALFORMED, SendText.check("\ude00a", 160))
        assertEquals("hello\nthere", SendText.clean("  hello\r\nthere \n"))
        assertEquals(160, SendText.limit(null))
        assertEquals(140, SendText.limit(140))
        assertEquals("never above the direct limit", 160, SendText.limit(400))
    }

    @Test
    fun aMessageIsStoredBeforeItIsTransmittedAndBecomesTheMessageTheUnitNames() = runBlocking {
        val send = sender()
        val seen = ArrayList<OutPhase?>()
        val (gw, job) = session(send) { _, p ->
            // What the phone holds at the moment the request is on the unit's side.
            seen += store.rows[p.string("client_msg_id")]?.phase
            accepted(41)
        }
        online(send)
        store.drafts[conv] = "kept until taken"
        assertNull(send.send(id, conv, key, null, "  hello there ", 160))
        until("accepted") { store.only().phase == OutPhase.ACCEPTED }
        val o = store.only()
        assertEquals(listOf<OutPhase?>(OutPhase.SENDING), seen)
        assertEquals("run1" to 41L, o.runId to o.messageId)
        assertEquals("j1", o.journalId)
        assertNull("the draft went with the message", store.drafts[conv])
        val (method, params) = gw.requests.single()
        assertEquals("mesh.send", method)
        assertEquals(setOf("to", "text", "client_msg_id"), params.keys)
        assertEquals(key, params.string("to"))
        assertEquals("hello there", params.string("text"))
        assertEquals(o.clientMsgId, params.string("client_msg_id"))
        assertTrue("a UUID fits the gateway's 16 to 36 characters", o.clientMsgId.length in 16..36 && o.clientMsgId.all { it.isLetterOrDigit() || it == '-' })

        assertNull(send.send(id, "c:1:a4", null, 1, "to everyone", 140))
        until("the channel message") { gw.requests.size == 2 }
        assertEquals(setOf("channel", "text", "client_msg_id"), gw.requests[1].second.keys)
        assertEquals(1L, gw.requests[1].second.long("channel"))
        job.cancelAndJoin()
    }

    @Test
    fun aRefusalOfTheFirstRequestSaysNothingWasSentAndIsNeverRepeated() = runBlocking {
        val cases = listOf(
            2 to (OutPhase.FAILED to Why.REFUSED), 5 to (OutPhase.FAILED to Why.BUSY), 7 to (OutPhase.FAILED to Why.NOT_PERMITTED),
            10 to (OutPhase.FAILED to Why.RATE), 4 to (OutPhase.FAILED to Why.UNIT_ERROR), 3 to (OutPhase.UNKNOWN to Why.OUTCOME_UNKNOWN),
        )
        for ((code, want) in cases) {
            store.rows.clear()
            val send = sender()
            val (gw, job) = session(send) { _, _ -> ScriptedGateway.Err(code, if (code == 3) "outcome unknown" else "x") }
            online(send)
            assertNull(send.send(id, conv, key, null, "code $code", 160))
            until("code $code settles") { store.only().phase != OutPhase.SENDING }
            delay(400)
            assertEquals("code $code", want, store.only().phase to store.only().why)
            assertEquals("code $code is asked once", 1, gw.requests.size)
            job.cancelAndJoin()
        }
    }

    @Test
    fun noAnswerIsUncertainAndTheSameIdIsAskedOnceMore() = runBlocking {
        val send = sender()
        var n = 0
        val (gw, job) = session(send) { _, _ -> if (n++ == 0) null else accepted(9, duplicate = true) }
        online(send)
        assertNull(send.send(id, conv, key, null, "lost answer", 160))
        until("uncertain") { store.only().phase == OutPhase.UNCERTAIN }
        assertEquals(Why.NO_ANSWER, store.only().why)
        until("found by the repeat") { store.only().phase == OutPhase.ACCEPTED }
        delay(400)
        assertEquals(2, gw.requests.size)
        assertEquals("the repeat is the same request", gw.requests[0], gw.requests[1])
        assertEquals(9L, store.only().messageId)
        assertEquals(1, store.only().checks)
        job.cancelAndJoin()
    }

    @Test
    fun aRepeatThatLearnsNothingLeavesItUncertainAndOnlyAPersonAsksAgain() = runBlocking {
        val send = sender()
        var n = 0
        val (gw, job) = session(send) { _, _ ->
            when (n++) {
                0 -> ScriptedGateway.Err(9, "upstream unavailable")
                1 -> ScriptedGateway.Err(5, "busy")
                else -> accepted(3, duplicate = true)
            }
        }
        online(send)
        assertNull(send.send(id, conv, key, null, "still waiting", 160))
        until("the repeat was made") { gw.requests.size == 2 && store.only().phase == OutPhase.UNCERTAIN }
        delay(600)
        assertEquals("the app asks once by itself", 2, gw.requests.size)
        assertEquals(OutPhase.UNCERTAIN to Why.BUSY, store.only().phase to store.only().why)
        assertNull(send.check(id, store.only().clientMsgId))
        until("the person's question is answered") { store.only().phase == OutPhase.ACCEPTED }
        assertEquals(3, gw.requests.size)
        assertEquals(1, gw.requests.map { it.second.string("client_msg_id") }.toSet().size)
        job.cancelAndJoin()
    }

    @Test
    fun outcomeUnknownIsFinalAndSendingAgainIsANewMessage() = runBlocking {
        val send = sender()
        var n = 0
        val (gw, job) = session(send) { _, _ ->
            when (n++) {
                0 -> ScriptedGateway.Err(9, "upstream unavailable")
                1 -> ScriptedGateway.Err(3, "outcome unknown")
                else -> accepted(12)
            }
        }
        online(send)
        assertNull(send.send(id, conv, key, null, "did it go?", 160))
        until("unknown") { store.only().phase == OutPhase.UNKNOWN }
        val first = store.only().clientMsgId
        assertEquals(SendBlock.NOT_FOUND, send.check(id, first))
        delay(400)
        assertEquals("an unknown id is never asked again", 2, gw.requests.size)
        store.drafts[conv] = "other words, still being typed"
        assertEquals("not to another place than it was written for", SendBlock.NOT_FOUND, send.sendAgain(id, first, null, 3, 160))
        assertNull(send.sendAgain(id, first, key, null, 160))
        assertEquals("the old entry went in the step that stored the new one", 1, store.all().size)
        assertEquals("the draft of the thread is not this message", "other words, still being typed", store.drafts[conv])
        until("the new message") { store.all().size == 1 && store.only().phase == OutPhase.ACCEPTED }
        assertNotEquals(first, store.only().clientMsgId)
        assertEquals("did it go?", gw.requests[2].second.string("text"))
        assertNotEquals(first, gw.requests[2].second.string("client_msg_id"))
        job.cancelAndJoin()
    }

    @Test
    fun whatAnEarlierProcessLeftOnItsWayIsUncertainAndARepeatThatIsRefusedStaysSo() = runBlocking {
        val send = sender()
        store.add(id, Outgoing(clientMsgId = "0123456789abcdef-left", conversation = conv, peerKey = key, channel = null, text = "the app was closed", journalId = "j1", createdAtMs = System.currentTimeMillis()), false)
        send.recover(id)
        assertEquals(OutPhase.UNCERTAIN to Why.NO_ANSWER, store.only().phase to store.only().why)
        // While its first request may still be waiting, "not permitted" says nothing about it.
        val (gw, job) = session(send) { _, _ -> ScriptedGateway.Err(7, "not permitted") }
        until("asked once") { gw.requests.size == 1 && store.only().phase == OutPhase.UNCERTAIN && store.only().checks == 1 }
        delay(500)
        assertEquals(1, gw.requests.size)
        assertEquals("0123456789abcdef-left", gw.requests.single().second.string("client_msg_id"))
        assertEquals(Why.NOT_PERMITTED, store.only().why)
        // A person may let it go; a send on its way may not be let go.
        send.discard(id, store.only().clientMsgId)
        assertEquals(0, store.all().size)
        job.cancelAndJoin()
    }

    @Test
    fun afterTheWindowTheAppDoesNotAskByItself() = runBlocking {
        val send = sender(windowMs = 0)
        val (gw, job) = session(send) { _, _ -> null }
        online(send)
        assertNull(send.send(id, conv, key, null, "too late to ask", 160))
        until("uncertain") { store.only().phase == OutPhase.UNCERTAIN }
        delay(600)
        assertEquals(1, gw.requests.size)
        assertEquals(OutPhase.UNCERTAIN, store.only().phase)
        job.cancelAndJoin()
    }

    @Test
    fun aLostConnectionLeavesItUncertainAndTheNextSessionAsksUnderTheSameId() = runBlocking {
        val send = sender(callMs = 5_000)
        val (gw1, job1) = session(send) { _, _ -> null }
        online(send)
        assertNull(send.send(id, conv, key, null, "phone went away", 160))
        until("the request left") { gw1.requests.size == 1 }
        gw1.drop()
        link.value = link.value.copy(phase = MeshPhase.OFFLINE)
        job1.cancelAndJoin()
        assertEquals(OutPhase.UNCERTAIN, store.only().phase)
        assertEquals(SendBlock.OFFLINE, send.send(id, conv, key, null, "nothing is taken offline", 160))
        assertEquals(1, store.all().size)

        val (gw2, job2) = session(send) { _, _ -> accepted(5, duplicate = true) }
        delay(300)
        assertEquals("nothing is asked before the mesh is synced on the new connection", 0, gw2.requests.size)
        link.value = link.value.copy(phase = MeshPhase.LIVE)
        until("answered from the gateway's record") { store.only().phase == OutPhase.ACCEPTED }
        assertEquals(gw1.requests.single(), gw2.requests.single())
        job2.cancelAndJoin()
    }

    @Test
    fun afterAGatewayRestartNothingIsRepeatedAndTheHistoryDecides() = runBlocking {
        val send = sender(callMs = 5_000)
        val (gw1, job1) = session(send) { _, _ -> null }
        online(send)
        assertNull(send.send(id, conv, key, null, "found again", 160))
        until("the first request left") { gw1.requests.size == 1 }
        gw1.drop()
        job1.cancelAndJoin()
        // It was stored and handed over; then the gateway died with its records.
        store.put(id, store.only().copy(phase = OutPhase.UNCERTAIN))
        val lost = store.add(id, Outgoing(clientMsgId = "0123456789abcdef-lost", conversation = conv, peerKey = key, channel = null, text = "never seen", phase = OutPhase.UNCERTAIN, journalId = "j1", createdAtMs = System.currentTimeMillis()), false)
        // The new gateway's journal, synced: it holds the first message and not the second.
        store.sent += MemorySendStore.Sent(conv, "found again", "run1", 77, ord = 5)
        link.value = MeshLink(MeshPhase.LIVE, runId = "run1", journalId = "j2")

        val (gw2, job2) = session(send) { _, _ -> accepted(99) }
        until("both are settled") { store.all().none { it.phase == OutPhase.UNCERTAIN } }
        delay(300)
        assertEquals("a gateway that never heard of an id is not asked about it", 0, gw2.requests.size)
        val found = store.all().first { it.text == "found again" }
        assertEquals(OutPhase.ACCEPTED, found.phase)
        assertEquals("run1" to 77L, found.runId to found.messageId)
        assertEquals(OutPhase.UNKNOWN to Why.GATEWAY_RESTARTED, store.rows[lost.clientMsgId]!!.let { it.phase to it.why })
        job2.cancelAndJoin()
    }

    @Test
    fun aSecondTapWhileTheFirstIsOnItsWayIsNotASecondMessage() = runBlocking {
        val send = sender(callMs = 5_000)
        val (gw, job) = session(send) { _, _ -> null }
        online(send)
        assertNull(send.send(id, conv, key, null, "once", 160))
        assertEquals(SendBlock.IN_FLIGHT, send.send(id, conv, key, null, "once", 160))
        assertEquals(SendBlock.IN_FLIGHT, send.send(id, conv, key, null, "once", 160))
        until("the one request") { gw.requests.size == 1 }
        delay(200)
        assertEquals(1, gw.requests.size)
        assertEquals(1, store.all().size)
        job.cancelAndJoin()
    }

    @Test
    fun nothingIsTakenWithoutSessionCapabilityScopeOrMesh() = runBlocking {
        val send = sender()
        assertEquals(SendBlock.OFFLINE, send.send(id, conv, key, null, "no session", 160))

        scopes.value = setOf("status", "mesh_read")
        val (gw, job) = session(send) { _, _ -> accepted() }
        online(send)
        assertEquals(SendBlock.NOT_PERMITTED, send.send(id, conv, key, null, "no scope", 160))
        scopes.value = setOf("status", "mesh_read", "mesh_send")
        until("the scope is granted") { send.state.value.permitted }
        link.value = link.value.copy(phase = MeshPhase.SERVICE_AWAY)
        assertEquals(SendBlock.SERVICE_AWAY, send.send(id, conv, key, null, "no mesh service", 160))
        link.value = link.value.copy(phase = MeshPhase.SYNCING)
        assertEquals(SendBlock.SYNCING, send.send(id, conv, key, null, "not synced", 160))
        link.value = link.value.copy(phase = MeshPhase.LIVE)
        assertEquals(SendBlock.TEXT, send.send(id, conv, key, null, "a".repeat(161), 160))
        assertEquals(SendBlock.TEXT, send.send(id, "c:0:11", null, 0, "a".repeat(141), 140))
        assertEquals(SendBlock.TEXT, send.send(id, conv, key, null, "bell\u0007", 160))
        assertEquals(SendBlock.NOT_FOUND, send.send(id, conv, "abc", null, "a prefix is not a key", 160))
        // Taken away while the session is open: the gateway says so, and the phone stops at once.
        scopes.value = setOf("status", "mesh_read")
        until("the scope is gone") { !send.state.value.permitted }
        assertEquals(SendBlock.NOT_PERMITTED, send.send(id, conv, key, null, "revoked", 160))
        job.cancelAndJoin()
        assertEquals(0, gw.requests.size)
        assertEquals(0, store.all().size)

        val (gw2, job2) = session(send, hello(capabilities = setOf("status", "mesh"))) { _, _ -> accepted() }
        scopes.value = setOf("status", "mesh_read", "mesh_send")
        online(send)
        assertEquals(SendBlock.NOT_SERVED, send.send(id, conv, key, null, "no capability", 160))
        job2.cancelAndJoin()
        assertEquals(0, gw2.requests.size)
        assertEquals(0, store.all().size)
    }

    @Test
    fun thePhoneKeepsToTheGatewaysRate() = runBlocking {
        val send = sender()
        var n = 0L
        val (gw, job) = session(send, hello(perMin = 3)) { _, _ -> accepted(++n) }
        online(send)
        repeat(3) { i ->
            assertNull(send.send(id, conv, key, null, "message $i", 160))
            until("message $i") { store.all().size == i + 1 && store.all().all { it.phase == OutPhase.ACCEPTED } }
        }
        assertEquals(SendBlock.RATE, send.send(id, conv, key, null, "one too many", 160))
        assertEquals(3, gw.requests.size)
        assertEquals(3, store.all().size)
        val wait = send.state.value.rateOpenAtMs - System.currentTimeMillis()
        assertTrue("it says when it opens again: $wait", wait in 50_000..60_000)
        job.cancelAndJoin()
    }

    @Test
    fun aResendNamesTheMessageAndTheRun() = runBlocking {
        val send = sender()
        var n = 0
        val (gw, job) = session(send) { _, _ -> if (n++ == 0) accepted(4) else ScriptedGateway.Err(3, "not a message of this run") }
        online(send)
        assertEquals(SendBlock.STALE_RUN, send.resend(id, conv, "an-older-run", 4, 1))
        assertEquals(0, gw.requests.size)
        assertNull(send.resend(id, conv, "run1", 4, 1))
        until("resent") { store.only().phase == OutPhase.ACCEPTED }
        val (method, params) = gw.requests.single()
        assertEquals("mesh.resend", method)
        assertEquals(setOf("message_id", "run_id", "client_msg_id"), params.keys)
        assertEquals(4L, params.long("message_id"))
        assertEquals("run1", params.string("run_id"))
        send.discard(id, store.only().clientMsgId)
        assertEquals(0, store.all().size)

        assertNull(send.resend(id, conv, "run1", 4, 2))
        until("refused") { store.only().phase == OutPhase.FAILED }
        assertEquals(Why.STALE_RUN, store.only().why)
        job.cancelAndJoin()
    }
}
