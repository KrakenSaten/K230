// Copyright (c) 2026 Doors authors.
// SPDX-License-Identifier: Apache-2.0
package app.doors.companion.core

import kotlinx.coroutines.CoroutineStart
import kotlinx.coroutines.TimeoutCancellationException
import kotlinx.coroutines.awaitCancellation
import kotlinx.coroutines.channels.Channel
import kotlinx.coroutines.coroutineScope
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.collectLatest
import kotlinx.coroutines.flow.distinctUntilChanged
import kotlinx.coroutines.flow.map
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.launch
import kotlinx.coroutines.withTimeoutOrNull
import kotlinx.serialization.json.JsonArray
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.JsonPrimitive
import kotlinx.serialization.json.buildJsonObject
import kotlinx.serialization.json.put
import java.io.IOException

enum class MeshPhase {
    /** No session with the unit. What is shown is what the phone stored. */
    OFFLINE,

    /** This gateway build does not serve the mesh. */
    NOT_SERVED,

    /** The unit has not granted this phone `mesh_read`. */
    NOT_PERMITTED,
    SYNCING,
    LIVE,

    /** The gateway is there; meshcored behind it is not. */
    SERVICE_AWAY,
}

data class MeshLink(
    val phase: MeshPhase = MeshPhase.OFFLINE,
    /** meshcored's own state word, when it has said one in this session. */
    val meshState: String? = null,
    val runId: String? = null,
    val lastSyncMs: Long = 0,
)

enum class FeedKind { MESSAGE, NODE, STATE, CONTINUITY }

data class FeedItem(val atMs: Long, val kind: FeedKind, val text: String)

/**
 * Reading the mesh over a live session (gateway.md section 7): subscribe,
 * bring the stored history up to the gateway's journal, then follow events.
 *
 * The cursor is `(journal_id, run_id, seq)`. It is only ever advanced
 * together with the messages it stands for (MeshStore.commit), and only along
 * an unbroken run of `seq`: an event that is not the next one is not applied,
 * it starts a sync from the cursor, and the gateway's answer decides whether
 * anything was lost. Three things break continuity, and each leaves a mark in
 * the stored history that is never removed:
 *
 *  - `gap: true`: the journal no longer holds what came after the cursor;
 *  - another `run_id`: meshcored started again and its journal began anew;
 *  - another `journal_id`: the gateway started again and rebuilt its journal
 *    from the little meshcored still held.
 *
 * In the last two the phone cannot know whether anything passed unseen, and
 * says exactly that. Nothing here ever reports missing messages as recovered.
 *
 * It lives inside the session's own scope: no session, no reading.
 */
class MeshSync(
    private val store: MeshStore,
    private val now: () -> Long = System::currentTimeMillis,
    private val retryMs: Long = RETRY_MS,
) : SessionFeature {
    private val _link = MutableStateFlow(MeshLink())
    val link: StateFlow<MeshLink> = _link

    private val _activity = MutableStateFlow<List<ActivityBin>>(emptyList())

    /** The last [ACTIVITY_KEPT] seconds in which the radio did something, oldest first. Memory only. */
    val activity: StateFlow<List<ActivityBin>> = _activity

    private val _feed = MutableStateFlow<List<FeedItem>>(emptyList())

    /** What happened on the mesh while this session watched, newest first. Memory only. */
    val feed: StateFlow<List<FeedItem>> = _feed

    override suspend fun run(unit: PairedUnit, conn: GatewayConnection, hello: Hello, scopes: StateFlow<Set<String>>) {
        try {
            if (CAPABILITY !in hello.capabilities) {
                phase(MeshPhase.NOT_SERVED)
                awaitCancellation()
            }
            // A topic is lost with its scope and does not return with it
            // (gateway.md section 5): every time the scope is there again,
            // the session starts over with a new subscription.
            scopes.map { SCOPE in it }.distinctUntilChanged().collectLatest { granted ->
                if (!granted) phase(MeshPhase.NOT_PERMITTED) else Live(unit.fingerprint, conn).run()
            }
        } catch (_: IOException) {
            // The connection ended. The session loop reports that; this only stops.
        } finally {
            _link.update { it.copy(phase = MeshPhase.OFFLINE, meshState = null) }
            _activity.value = emptyList()
        }
    }

    private fun phase(p: MeshPhase) = _link.update { it.copy(phase = p) }

    private fun say(kind: FeedKind, text: String) = _feed.update { (listOf(FeedItem(now(), kind, text)) + it).take(FEED_KEPT) }

    /** One subscription's life on one connection. */
    private inner class Live(private val unit: String, private val conn: GatewayConnection) {
        private var cursor: MeshCursor? = null
        private var subscribed = false
        private var needSync = true
        private var needLists = true
        private var away = false

        suspend fun run() = coroutineScope {
            // Listening starts before the subscription is asked for, so that
            // nothing the gateway sends after it can be missed.
            val inbox = Channel<GatewayEvent>(Channel.UNLIMITED)
            val pump = launch(start = CoroutineStart.UNDISPATCHED) { conn.events.collect { inbox.trySend(it) } }
            try {
                phase(MeshPhase.SYNCING)
                cursor = store.cursor(unit)
                while (true) {
                    try {
                        if (!subscribed) subscribe()
                        if (needSync) {
                            sync()
                            needSync = false
                        }
                        if (needLists) {
                            lists()
                            needLists = false
                            away = false
                        }
                        phase(if (away) MeshPhase.SERVICE_AWAY else MeshPhase.LIVE)
                    } catch (e: GatewayError) {
                        when (e.code) {
                            GatewayError.NOT_PERMITTED -> {
                                phase(MeshPhase.NOT_PERMITTED)
                                awaitCancellation() // until the scopes change, or the session ends
                            }
                            GatewayError.UPSTREAM -> {
                                away = true
                                phase(MeshPhase.SERVICE_AWAY)
                            }
                            else -> Unit // no answer, busy, rate limited: asked again below
                        }
                    }
                    val pending = !subscribed || needSync || needLists
                    val first = if (pending) withTimeoutOrNull(retryMs) { inbox.receive() } else inbox.receive()
                    var e = first
                    while (e != null) {
                        handle(e)
                        e = inbox.tryReceive().getOrNull()
                    }
                }
            } finally {
                pump.cancel()
            }
        }

        private suspend fun ask(method: String, params: JsonObject? = null): Reply = try {
            conn.call(method, params, CALL_MS)
        } catch (e: TimeoutCancellationException) {
            throw GatewayError(NO_ANSWER, "no answer")
        }

        private suspend fun subscribe() {
            val r = ask("gateway.subscribe", buildJsonObject {
                put("topics", JsonArray(listOf(JsonPrimitive(TOPIC), JsonPrimitive(TOPIC_ACTIVITY))))
            }).result
            if (TOPIC !in r.strings("topics")) throw GatewayError(GatewayError.NOT_PERMITTED, "topic not held")
            away = r.obj("upstream")?.bool("meshcored") == false
            subscribed = true
        }

        /** The journal from the cursor to its end, a page at a time. */
        private suspend fun sync() {
            var c = cursor ?: start(ask("mesh.sync", buildJsonObject { put("limit", 1) }).result)
            val marks = ArrayList<MeshMark>()
            var restarts = 0
            while (true) {
                val reply = ask("mesh.sync", buildJsonObject {
                    put("after_seq", c.seq)
                    put("limit", PAGE)
                })
                val r = reply.result
                val journal = r.string("journal_id").orEmpty()
                val run = r.string("run_id").orEmpty()
                if (journal.isEmpty() || run.isEmpty()) throw GatewayError(NO_ANSWER, "an answer without a journal")
                if (journal != c.journalId || run != c.runId) {
                    // The cursor belonged to another journal or run, so this
                    // answer was not an answer to it. Whether anything passed
                    // in between cannot be known from here, and is marked so.
                    if (++restarts > 3) throw GatewayError(NO_ANSWER, "the journal keeps changing")
                    marks += MeshMark(
                        when {
                            journal != c.journalId && run != c.runId -> MarkKind.UNIT_RESTARTED
                            journal != c.journalId -> MarkKind.GATEWAY_RESTARTED
                            else -> MarkKind.MESH_RESTARTED
                        },
                        now(),
                    )
                    c = start(r)
                    continue
                }
                if (r.bool("gap") == true) marks += MeshMark(MarkKind.GAP, now())
                val at = now()
                val entries = r.array("messages").filterIsInstance<JsonObject>()
                val heard = entries.mapNotNull { e ->
                    val seq = e.long("seq") ?: return@mapNotNull null
                    val m = MeshMessage.parse(run, seq, e.obj("message") ?: return@mapNotNull null) ?: return@mapNotNull null
                    Heard(m, heardAt(at, reply.monoMs, m.monoMs))
                }
                // Where the answer ended; an empty one ends at the newest held.
                val end = entries.mapNotNull { it.long("seq") }.maxOrNull() ?: r.long("seq") ?: c.seq
                c = c.copy(seq = end)
                store.commit(unit, MeshBatch(marks.toList(), heard, c))
                cursor = c
                for (m in marks) say(FeedKind.CONTINUITY, describe(m.kind))
                marks.clear()
                if (r.bool("more") != true) break
            }
            _link.update { it.copy(lastSyncMs = now(), runId = c.runId) }
        }

        /** A cursor at the oldest entry the journal holds: everything it has is then asked for, in order. */
        private fun start(r: JsonObject): MeshCursor {
            val journal = r.string("journal_id").orEmpty()
            val run = r.string("run_id").orEmpty()
            val first = r.long("first_seq")
            if (journal.isEmpty() || run.isEmpty() || first == null || first < 1) throw GatewayError(NO_ANSWER, "an answer without a journal")
            return MeshCursor(journal, run, first - 1)
        }

        /** The unit's node, its channels and the nodes it knows. Needs meshcored; the journal does not. */
        private suspend fun lists() {
            val id = ask("mesh.identity").result
            id.string("public_key")?.takeIf { MeshMessage.isKey(it) }?.let { store.putIdentity(unit, MeshIdentity(it.lowercase(), id.string("name").orEmpty())) }

            store.putChannels(unit, ask("mesh.channels").result.array("channels").filterIsInstance<JsonObject>().mapNotNull { MeshChannel.parse(it) })

            val nodes = ArrayList<Pair<MeshNode, Long?>>()
            var offset = 0
            while (offset < NODES_MAX) {
                val reply = ask("mesh.nodes", buildJsonObject {
                    put("offset", offset)
                    put("limit", PAGE)
                })
                val at = now()
                val page = reply.result.array("nodes").filterIsInstance<JsonObject>()
                for (o in page) MeshNode.parse(o)?.let { n -> nodes += n to n.lastHeardMonoMs?.let { heardAt(at, reply.monoMs, it) } }
                offset += page.size
                if (page.isEmpty() || offset >= (reply.result.long("total") ?: 0)) break
            }
            store.putNodes(unit, nodes, whole = true)
        }

        private suspend fun handle(e: GatewayEvent) {
            val d = e.data
            when (e.name) {
                "mesh.message" -> message(e)
                "mesh.node" -> {
                    val n = MeshNode.parse(d.obj("node") ?: return) ?: return
                    val name = n.name.ifEmpty { n.publicKey.take(8) }
                    when (d.string("reason")) {
                        "removed" -> {
                            store.removeNode(unit, n.publicKey)
                            say(FeedKind.NODE, "$name was removed on the unit")
                        }
                        else -> {
                            store.putNodes(unit, listOf(n to n.lastHeardMonoMs?.let { heardAt(now(), e.monoMs, it) }), whole = false)
                            say(FeedKind.NODE, if (d.string("reason") == "path") "A route to $name was learned" else "Heard $name")
                        }
                    }
                }
                "mesh.channel" -> {
                    val c = MeshChannel.parse(d.obj("channel") ?: return) ?: return
                    if (d.string("reason") == "removed") store.removeChannel(unit, c.slot) else store.putChannel(unit, c)
                }
                "mesh.state" -> {
                    val s = d.string("state") ?: return
                    _link.update { it.copy(meshState = s) }
                    say(FeedKind.STATE, "Mesh service: $s")
                }
                "mesh.activity_bin" -> _activity.update { (it + ActivityBin.parse(d, now())).takeLast(ACTIVITY_KEPT) }
                "gateway.upstream" -> {
                    if (d.string("service") != "meshcored") return
                    if (d.bool("connected") == true) {
                        // "The moment to call mesh.sync" (gateway.md section 5).
                        needSync = true
                        needLists = true
                        say(FeedKind.STATE, "The mesh service is back")
                    } else {
                        away = true
                        phase(MeshPhase.SERVICE_AWAY)
                        say(FeedKind.STATE, "The mesh service went away")
                    }
                }
            }
        }

        private suspend fun message(e: GatewayEvent) {
            val d = e.data
            val seq = d.long("seq") ?: return
            val c = cursor
            if (c == null || d.string("journal_id") != c.journalId || d.string("run_id") != c.runId || seq > c.seq + 1) {
                // Not the next entry of the journal this phone is in: an
                // event was missed, or the journal is another one. It is not
                // applied; the sync from the cursor finds out what is true.
                needSync = true
                return
            }
            if (seq <= c.seq) return // already had it from a sync
            val next = c.copy(seq = seq)
            val m = d.obj("message")?.let { MeshMessage.parse(c.runId, seq, it) }
            store.commit(unit, MeshBatch(emptyList(), listOfNotNull(m?.let { Heard(it, heardAt(now(), e.monoMs, it.monoMs)) }), next))
            cursor = next
            if (m != null && !m.outgoing && m.state == "received") {
                say(FeedKind.MESSAGE, if (m.channelKind) "Message on ${m.channelName ?: "channel ${m.channel}"}" else "Message from ${m.peerName ?: m.peerKey?.take(8)}")
            }
        }
    }

    companion object {
        const val CAPABILITY = "mesh"
        const val SCOPE = "mesh_read"
        const val TOPIC = "mesh"
        const val TOPIC_ACTIVITY = "mesh.activity"
        const val PAGE = 100
        const val NODES_MAX = 1000
        const val CALL_MS = 8_000L
        const val RETRY_MS = 4_000L
        const val ACTIVITY_KEPT = 90
        const val FEED_KEPT = 60

        /** Not a gateway code: the phone's own "this request had no usable answer". */
        const val NO_ANSWER = -1

        /**
         * The phone's estimate of when the unit saw something: the unit's
         * clock is not relied on (gateway.md section 1), so the unit's
         * monotonic age of the thing is taken off the phone's own time.
         */
        fun heardAt(nowMs: Long, envelopeMonoMs: Long?, monoMs: Long?): Long =
            if (envelopeMonoMs != null && monoMs != null && envelopeMonoMs >= monoMs) nowMs - (envelopeMonoMs - monoMs) else nowMs

        fun describe(kind: MarkKind): String = when (kind) {
            MarkKind.GAP -> "Messages passed that this phone did not receive"
            MarkKind.MESH_RESTARTED -> "The mesh service restarted; earlier messages may be missing"
            MarkKind.GATEWAY_RESTARTED -> "The gateway restarted; earlier messages may be missing"
            MarkKind.UNIT_RESTARTED -> "The unit restarted; earlier messages may be missing"
        }
    }
}
