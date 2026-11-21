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
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertTrue
import org.junit.Assume.assumeTrue
import org.junit.Test
import java.io.BufferedReader
import java.io.InputStreamReader
import java.net.InetSocketAddress
import java.net.Socket

/** MeshStore in memory, with the same rules the Room store keeps: one row per (run, id), marks placed before the batch they came with. */
class MemoryMeshStore : MeshStore {
    val messages = LinkedHashMap<Pair<String, Long>, MeshMessage>()
    val heardAt = HashMap<Pair<String, Long>, Long>()

    /** The stored history in the order it would be drawn: message keys and marks. */
    val timeline = ArrayList<Any>()
    val nodes = LinkedHashMap<String, MeshNode>()
    val channels = java.util.TreeMap<Int, MeshChannel>()
    var identity: MeshIdentity? = null
    var cursor: MeshCursor? = null
    val marks: List<MarkKind> get() = synchronized(this) { timeline.filterIsInstance<MeshMark>().map { it.kind } }
    val size: Int get() = synchronized(this) { messages.size }

    override suspend fun cursor(unit: String) = synchronized(this) { cursor }

    override suspend fun commit(unit: String, batch: MeshBatch) = synchronized(this) {
        timeline.addAll(batch.marks)
        for (h in batch.messages) {
            val k = h.message.runId to h.message.id
            if (k !in messages) {
                timeline += k
                heardAt[k] = h.atMs
            }
            messages[k] = h.message
        }
        cursor = batch.cursor
    }

    override suspend fun putNodes(unit: String, nodes: List<Pair<MeshNode, Long?>>, whole: Boolean) = synchronized(this) {
        if (whole) this.nodes.clear()
        for ((n, _) in nodes) this.nodes[n.publicKey] = n
    }

    override suspend fun removeNode(unit: String, publicKey: String) = synchronized(this) { nodes.remove(publicKey); Unit }
    override suspend fun putChannels(unit: String, channels: List<MeshChannel>) = synchronized(this) {
        this.channels.clear()
        for (c in channels) this.channels[c.slot] = c
    }

    override suspend fun putChannel(unit: String, channel: MeshChannel) = synchronized(this) { channels[channel.slot] = channel }
    override suspend fun removeChannel(unit: String, slot: Int) = synchronized(this) { channels.remove(slot); Unit }
    override suspend fun putIdentity(unit: String, identity: MeshIdentity) = synchronized(this) { this.identity = identity }
}

/**
 * MeshSync against the real gatewayd and the harness's fake meshcored
 * (harness/run-gateway.sh): the first sync, live events, a state change of a
 * known message, lists, activity, a cursor resumed over several pages, a
 * journal that moved on (gap), meshcored restarting, meshcored away and back,
 * the gateway restarting, and the scope taken away and given back.
 *
 * What it holds to throughout: no message is stored twice, messages of an
 * earlier run stay what they were, and every break in continuity leaves
 * exactly one mark, placed where it happened.
 *
 * Skipped when no harness was named:
 *   gradle :app:testDebugUnitTest -Ddoors.gateway.ctl=127.0.0.1:47232
 */
class MeshIntegrationTest {
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

    private suspend fun until(what: String, ms: Long = 20_000, cond: () -> Boolean) {
        val end = System.currentTimeMillis() + ms
        while (!cond()) {
            if (System.currentTimeMillis() > end) throw AssertionError("never: $what")
            delay(50)
        }
    }

    @Test
    fun readingTheMeshThroughTheRealGateway() = runBlocking {
        assumeTrue("no gateway harness named", ctlAddr.isNotEmpty())
        val scope = CoroutineScope(Dispatchers.Default + Job())
        val phone = TestIdentity.create()
        val fp = Fingerprint.hex(phone.fingerprint())
        val store = MemoryMeshStore()
        val sync = MeshSync(store, retryMs = 500)
        val session = GatewaySession(transport, phone, "test", features = listOf(sync), backoff = { 300 })
        fun phase() = sync.link.value.phase

        // ---- a clean bench: nobody paired, a new run of meshcored, the demo off ----
        ctl("gateway.pair_cancel")
        for (c in ctl("gateway.clients").obj("result")!!.array("clients").filterIsInstance<JsonObject>()) {
            ctl("gateway.revoke", buildJsonObject { put("client", c.string("fingerprint")!!) })
        }
        mesh("demo") { put("on", false) }
        val runA = mesh("restart").string("run_id")!!
        // What the unit heard before any phone could read: it is in meshcored's rings.
        repeat(3) { i -> mesh("message") { put("peer", 0); put("text", "before $i") } }
        repeat(2) { i -> mesh("message") { put("kind", "channel"); put("peer", 1); put("text", "public $i") } }

        // ---- pair ----
        val code = PairingUri.parse(ctl("gateway.pair_begin").obj("result")!!.string("uri")!!)
        var waited = false
        val pairing = scope.async { session.pair(code, "Mesh test phone") { waited = true } }
        withTimeout(10_000) { while (!waited) delay(50) }
        delay(400)
        ctl("gateway.pair_decide", buildJsonObject { put("allow", true) })
        val unit = (pairing.await() as PairOutcome.Paired).unit

        // ---- the first sync: the journal from its start, the lists, and no break ----
        var loop = scope.launch { session.run(unit) }
        until("live") { phase() == MeshPhase.LIVE }
        until("the five messages from before") { store.size == 5 }
        assertEquals(listOf(1L, 2L, 3L, 4L, 5L), store.messages.values.map { it.id })
        assertTrue(store.messages.values.all { it.runId == runA })
        assertEquals(runA, store.cursor!!.runId)
        assertEquals("nothing is marked on a first sync", emptyList<MarkKind>(), store.marks)
        val direct = store.messages[runA to 1L]!!
        assertEquals("before 0", direct.text)
        assertEquals("Ola Nordmann", direct.peerName)
        assertEquals("d:" + direct.peerKey, direct.conversation)
        val pub = store.messages[runA to 4L]!!
        assertEquals("c:0:11", pub.conversation)
        assertEquals("Kari Fjell", pub.senderName)
        assertEquals("public 0", MeshMessage.body(pub.text, pub.senderName))
        until("the lists") { store.identity != null && store.nodes.size == 7 && store.channels.size == 2 }
        assertEquals("Doors bench", store.identity!!.name)
        assertEquals("public", store.channels[0]!!.wellKnown)
        assertEquals(2, store.nodes.values.first { it.name == "Ridge repeater" }.type)
        assertTrue("the phone's time for a message is at or before now", store.heardAt.values.all { it <= System.currentTimeMillis() })

        // ---- live: an event is the next entry, and a state change is the same message ----
        mesh("message") { put("peer", 1); put("text", "live one") }
        until("the live message") { store.size == 6 }
        val sent = mesh("message") { put("peer", 1); put("direction", "out"); put("text", "an answer") }.long("id")!!
        until("the outgoing message") { store.messages[runA to sent]?.state == "sent_direct" }
        mesh("state") { put("id", sent); put("state", "acked") }
        until("its acknowledgement") { store.messages[runA to sent]?.state == "acked" }
        assertEquals("a state change is not a second message", 7, store.size)
        assertEquals(7, store.timeline.size)

        // ---- activity as counts, a node heard ----
        mesh("activity") { put("rx", 3); put("kind", "advert") }
        until("an activity bin") { sync.activity.value.any { it.rxFrames == 3 && it.byType["advert"] == 3 } }
        mesh("node") { put("peer", 2) }
        until("the node in the feed") { sync.feed.value.any { it.kind == FeedKind.NODE && it.text.contains("Ridge repeater") } }

        // ---- away, and back over several pages: every entry once, in order, and no break ----
        loop.cancelAndJoin()
        assertEquals(MeshPhase.OFFLINE, phase())
        mesh("bulk") { put("count", 250); put("text", "paged") }
        delay(1_500) // the gateway journals what meshcored told it
        loop = scope.launch { session.run(unit) }
        until("all 250 after the cursor") { store.size == 257 }
        assertEquals(emptyList<MarkKind>(), store.marks)
        val ids = store.timeline.map { (it as Pair<*, *>).second as Long }
        assertEquals("stored in the journal's order", ids.sorted(), ids)

        // ---- away while more passed than the journal holds: a gap, said once, and nothing invented ----
        loop.cancelAndJoin()
        mesh("bulk") { put("count", 600); put("text", "flood") }
        delay(2_500)
        loop = scope.launch { session.run(unit) }
        until("the gap is marked") { store.marks == listOf(MarkKind.GAP) }
        until("what the journal still held") { phase() == MeshPhase.LIVE && store.size == 257 + 512 }
        val gapAt = store.timeline.indexOfFirst { it is MeshMark }
        assertEquals("the mark stands between what was had and what came after", 257, gapAt)
        val firstAfter = store.messages[store.timeline[gapAt + 1]]!!
        assertEquals("the 88 that fell out are not there", "flood 88", MeshMessage.body(firstAfter.text, firstAfter.senderName))
        delay(1_000)
        assertEquals("a gap is marked once", listOf(MarkKind.GAP), store.marks)

        // ---- meshcored restarts: a new run, the old one kept as it was, continuity marked unknown ----
        val before = store.size
        val runB = mesh("restart").string("run_id")!!
        until("the restart is marked") { store.marks == listOf(MarkKind.GAP, MarkKind.MESH_RESTARTED) }
        mesh("message") { put("peer", 0); put("text", "first of the new run") }
        until("the new run's first message") { store.messages[runB to 1L] != null }
        assertEquals("first of the new run", store.messages[runB to 1L]!!.text)
        assertEquals("the old run's message 1 is still itself", "before 0", store.messages[runA to 1L]!!.text)
        assertEquals(before + 1, store.size)
        assertEquals(runB, store.cursor!!.runId)

        // ---- meshcored away and back in the same run: said, and no break claimed or invented ----
        mesh("down")
        // The gateway notices on its next read or request; a list call finds out too.
        until("the service is seen to be away") { phase() == MeshPhase.SERVICE_AWAY }
        mesh("up")
        until("live again") { phase() == MeshPhase.LIVE }
        assertEquals(listOf(MarkKind.GAP, MarkKind.MESH_RESTARTED), store.marks)
        assertEquals(before + 1, store.size)

        // ---- the gateway restarts: another journal over the same run; nothing twice, continuity marked unknown ----
        ctl("harness.stop")
        until("the session is gone") { phase() == MeshPhase.OFFLINE }
        ctl("harness.start")
        session.kick()
        until("the new journal is marked", 40_000) { store.marks == listOf(MarkKind.GAP, MarkKind.MESH_RESTARTED, MarkKind.GATEWAY_RESTARTED) }
        until("live on the new journal") { phase() == MeshPhase.LIVE }
        assertEquals("the same messages again are the same messages", before + 1, store.size)
        mesh("message") { put("peer", 0); put("text", "after the gateway came back") }
        until("events on the new journal") { store.size == before + 2 }

        // ---- the scope taken away and given back: not readable, then a new subscription ----
        ctl("gateway.set_scopes", buildJsonObject { put("client", fp); put("scopes", JsonArray(listOf(JsonPrimitive("status")))) })
        until("not permitted") { phase() == MeshPhase.NOT_PERMITTED }
        ctl("gateway.set_scopes", buildJsonObject {
            put("client", fp)
            put("scopes", JsonArray(listOf(JsonPrimitive("status"), JsonPrimitive("mesh_read"))))
        })
        until("live with the scope back") { phase() == MeshPhase.LIVE }
        // The gateway read nothing while nobody could: it says gap, and so does the phone.
        assertEquals(MarkKind.GAP, store.marks.last())
        assertEquals(4, store.marks.size)
        mesh("message") { put("peer", 0); put("text", "readable again") }
        until("events after the scope returned") { store.messages.values.last().text == "readable again" }
        assertNotNull(store.cursor)
        assertEquals("every stored message is one row", store.messages.size, store.timeline.count { it !is MeshMark })

        loop.cancelAndJoin()
        ctl("gateway.revoke", buildJsonObject { put("client", fp) })
        Unit
    }
}
