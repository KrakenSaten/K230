// Copyright (c) 2026 Doors authors.
// SPDX-License-Identifier: Apache-2.0
package app.doors.companion.core

import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.NonCancellable
import kotlinx.coroutines.TimeoutCancellationException
import kotlinx.coroutines.channels.Channel
import kotlinx.coroutines.coroutineScope
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.collectLatest
import kotlinx.coroutines.flow.combine
import kotlinx.coroutines.flow.distinctUntilChanged
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.launch
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import kotlinx.coroutines.withContext
import kotlinx.serialization.json.buildJsonObject
import kotlinx.serialization.json.put
import java.io.IOException
import java.util.UUID

/**
 * What a person may type into a message (gateway.md section 8): 1 to 160
 * bytes of well-formed UTF-8, with no control character but newline and tab,
 * and less on a channel, where the unit's name travels in front of the text.
 * The gateway checks the same; the phone checks first so that nothing it
 * knows to be refused is ever asked for.
 */
object SendText {
    const val DIRECT_MAX = 160

    enum class Fault { EMPTY, TOO_LONG, CONTROL, MALFORMED }

    /** What is sent of what was typed: the space around it carries nothing. */
    fun clean(text: String): String = text.replace("\r\n", "\n").trim()

    /** The length in UTF-8 bytes, which is what the limit counts. A lone surrogate counts as the three bytes it cannot be. */
    fun bytes(text: String): Int {
        var n = 0
        var i = 0
        while (i < text.length) {
            val c = text[i]
            when {
                c.code < 0x80 -> n += 1
                c.code < 0x800 -> n += 2
                Character.isHighSurrogate(c) && i + 1 < text.length && Character.isLowSurrogate(text[i + 1]) -> {
                    n += 4
                    i++
                }
                else -> n += 3
            }
            i++
        }
        return n
    }

    /** The limit for a destination: a channel's own `text_limit` when the unit has said one, never above the direct limit. */
    fun limit(channelTextLimit: Int?): Int = channelTextLimit?.takeIf { it in 1..DIRECT_MAX } ?: DIRECT_MAX

    /** Null when [text] (already [clean]) may be sent within [limit] bytes. */
    fun check(text: String, limit: Int): Fault? {
        if (text.isBlank()) return Fault.EMPTY
        var i = 0
        while (i < text.length) {
            val c = text[i]
            if (Character.isHighSurrogate(c)) {
                if (i + 1 >= text.length || !Character.isLowSurrogate(text[i + 1])) return Fault.MALFORMED
                i += 2
                continue
            }
            if (Character.isLowSurrogate(c)) return Fault.MALFORMED
            val u = c.code
            if ((u < 0x20 && c != '\n' && c != '\t') || u == 0x7f || u in 0x80..0x9f) return Fault.CONTROL
            i++
        }
        return if (bytes(text) > limit) Fault.TOO_LONG else null
    }
}

/**
 * Where a message this phone asked to have sent stands. The delivery itself
 * (on the air, acknowledged, not acknowledged) is never kept here: it is the
 * state of the mesh message the send became, and arrives through the journal
 * like every other message.
 */
enum class OutPhase {
    /** Stored on the phone and handed to the connection; no answer yet. */
    SENDING,

    /** The unit took it: it is the message `(runId, messageId)`. */
    ACCEPTED,

    /** No answer came. Whether the unit took it is not known yet; the unit can still be asked under the same id. */
    UNCERTAIN,

    /** It cannot be found out. Never sent again under this id; only a person may send the words as a new message. */
    UNKNOWN,

    /** Refused: nothing was transmitted. */
    FAILED,
}

/** Why a send is where it is. */
enum class Why {
    NONE,

    /** No answer in time, or the connection ended first. */
    NO_ANSWER,

    /** Code 9: meshcored was away, or did not answer the gateway. */
    SERVICE_AWAY,

    /** Code 2: the unit will not send this (too long for the channel, an unknown node or slot, text it does not take). */
    REFUSED,

    /** Code 5: the unit has no room (8 messages waiting for an ACK, or its radio is not available). */
    BUSY,

    /** Code 7: the unit does not let this phone send. */
    NOT_PERMITTED,

    /** Code 10. */
    RATE,

    /** Code 3 "outcome unknown": the gateway itself cannot find out. */
    OUTCOME_UNKNOWN,

    /** The message belongs to an earlier run of the mesh service and cannot be resent. */
    STALE_RUN,

    /** The gateway started again and no longer knows this id; the message was not found in what it holds. */
    GATEWAY_RESTARTED,

    /** Too many sends since: the gateway may have forgotten this id, so it is not asked. */
    RECORD_GONE,

    /** Any other error of the unit's: nothing was sent. */
    UNIT_ERROR,

    /** The session ended before the message was handed to it: nothing was sent. */
    NOT_CONNECTED,
}

/** One send, or one resend, as the phone keeps it. Written before anything is transmitted. */
data class Outgoing(
    /** The order of sends on this phone; 0 until stored. */
    val ord: Long = 0,
    val clientMsgId: String,
    val conversation: String,
    val peerKey: String?,
    val channel: Int?,
    val text: String,
    /** A resend: the message asked to be sent again, and how often it had been sent when asked. */
    val resendRunId: String? = null,
    val resendMsgId: Long? = null,
    val attemptsBefore: Int? = null,
    val phase: OutPhase = OutPhase.SENDING,
    val why: Why = Why.NONE,
    /** The gateway journal that was current when the id was first given to a gateway: its records die with it. */
    val journalId: String? = null,
    /** What it became, once accepted. */
    val runId: String? = null,
    val messageId: Long? = null,
    /** The newest stored message when this was written: where it stands in the thread until it is a message itself. */
    val afterOrd: Long = 0,
    val createdAtMs: Long,
    val updatedAtMs: Long = createdAtMs,
    /** How often the unit was asked again about this id. */
    val checks: Int = 0,
) {
    val resend: Boolean get() = resendMsgId != null
}

/** Where sends are kept. On the phone: the same database as the history, so a message and its draft change hands in one transaction. */
interface SendStore {
    /**
     * Atomic: stores [o] (giving it its `ord` and `afterOrd`), removes the
     * draft of its thread when [clearDraft], and removes the send [replaces]
     * names, which [o] takes the place of.
     */
    suspend fun add(unit: String, o: Outgoing, clearDraft: Boolean, replaces: String? = null): Outgoing
    suspend fun get(unit: String, clientMsgId: String): Outgoing?
    suspend fun put(unit: String, o: Outgoing)
    suspend fun remove(unit: String, clientMsgId: String)

    /** Every send still [OutPhase.SENDING] or [OutPhase.UNCERTAIN], oldest first. */
    suspend fun open(unit: String): List<Outgoing>

    /** How many sends were stored after the one with [ord], whether or not they are still kept. */
    suspend fun newerThan(unit: String, ord: Long): Int

    /**
     * Looks in the stored history for the message [o] may have become: an
     * outgoing message of the same thread with the same words, learned after
     * [o] was written, that no other send already is. Null when there is none.
     */
    suspend fun findSent(unit: String, o: Outgoing): Pair<String, Long>?

    /** A stored message's `attempts`, when it has any. */
    suspend fun attempts(unit: String, runId: String, messageId: Long): Int?
}

/** What the session allows right now. */
data class SendState(
    /** A session with the unit is open. */
    val session: Boolean = false,
    /** The gateway names the `mesh.send` capability. */
    val served: Boolean = false,
    /** The unit grants this phone `mesh_send` (with `mesh_read`). */
    val permitted: Boolean = false,
    val perMin: Int = MeshSend.PER_MIN,
    /** Until when the rate limit holds sends back; 0 when it does not. */
    val rateOpenAtMs: Long = 0,
)

/** Why a message cannot be sent now. Each is a reason to keep the draft. */
enum class SendBlock { OFFLINE, NOT_SERVED, NOT_PERMITTED, SYNCING, SERVICE_AWAY, RATE, TEXT, IN_FLIGHT, STALE_RUN, NOT_FOUND }

/**
 * Sending through the gateway (gateway.md section 8), for a person who tapped
 * Send. Rules it holds to:
 *
 *  - **Stored first.** A message and its `client_msg_id` are written to the
 *    phone before a byte of it goes to the connection.
 *  - **One id, one message.** An id is only ever repeated with the same
 *    request. The gateway answers a repeat from its record, or looks before it
 *    sends, so a repeat cannot make a second transmission while that gateway
 *    runs.
 *  - **No answer is not "failed".** A send without an answer is UNCERTAIN. The
 *    unit is asked again under the same id once by the app itself, and only
 *    within [autoWindowMs] of the tap; after that only a person asks. When the gateway has started again its records are gone:
 *    then nothing is repeated at all, the phone looks in what it synced, and
 *    what it does not find is UNKNOWN.
 *  - **UNKNOWN is final.** That id is never sent again. Sending the words as a
 *    new message is a person's decision, taken with the risk in front of them.
 *  - **Nothing waits to be sent.** A message is only taken while the session
 *    is open, the gateway serves sending, the unit grants it and the mesh is
 *    live; otherwise the words stay a draft. Nothing here sends on reconnect
 *    what was not already handed to a connection.
 *  - **Delivery is not decided here.** `mesh.message` events and `mesh.sync`
 *    carry it under `(run_id, message_id)`, also when they arrive before the
 *    answer to the send; this class only records which message a send became.
 *
 * It lives inside the session's own scope, like MeshSync: no session, no send.
 */
class MeshSend(
    private val store: SendStore,
    private val mesh: StateFlow<MeshLink>,
    private val now: () -> Long = System::currentTimeMillis,
    private val newId: () -> String = { UUID.randomUUID().toString() },
    private val callMs: Long = CALL_MS,
    private val settleMs: Long = SETTLE_MS,
    private val autoWindowMs: Long = AUTO_WINDOW_MS,
) : SessionFeature {
    private val _state = MutableStateFlow(SendState())
    val state: StateFlow<SendState> = _state

    private class Live(val unit: String, val conn: GatewayConnection, val scope: CoroutineScope)

    @Volatile
    private var live: Live? = null

    /** Guards every change of a stored send, and the decision to take a new one. Never held across a request. */
    private val lock = Mutex()
    private val wake = Channel<Unit>(Channel.CONFLATED)
    private val asked = ArrayDeque<Long>()

    /** The phone's own count of the gateway's rate window. Off only in tests that want the gateway's own answer. */
    @Volatile
    var guardRate = true

    override suspend fun run(unit: PairedUnit, conn: GatewayConnection, hello: Hello, scopes: StateFlow<Set<String>>) {
        val id = unit.fingerprint
        val served = CAPABILITY in hello.capabilities
        try {
            // What a session before this one (or a process before this one)
            // left in flight has no answer coming any more.
            orphan(id)
            coroutineScope {
                live = Live(id, conn, this)
                launch {
                    scopes.collect { s ->
                        _state.update {
                            it.copy(session = true, served = served, permitted = SCOPE in s && MeshSync.SCOPE in s, perMin = hello.sendPerMin ?: PER_MIN)
                        }
                    }
                }
                if (!served) return@coroutineScope
                combine(mesh, scopes) { l, s -> l.phase == MeshPhase.LIVE && SCOPE in s }.distinctUntilChanged().collectLatest { ready ->
                    if (!ready) return@collectLatest
                    while (true) {
                        checkOpen(id)
                        wake.receive()
                    }
                }
            }
        } finally {
            live = null
            // What was in flight is marked before anyone is told the session is over.
            withContext(NonCancellable) { orphan(id) }
            _state.value = SendState()
        }
    }

    /**
     * At the start of the app: what an earlier process left in flight has no
     * answer coming. Does nothing while a session is open.
     */
    suspend fun recover(unit: String) {
        if (live == null) orphan(unit)
    }

    private suspend fun orphan(unit: String) = lock.withLock {
        for (o in store.open(unit)) {
            if (o.phase == OutPhase.SENDING) store.put(unit, o.copy(phase = OutPhase.UNCERTAIN, why = Why.NO_ANSWER, updatedAtMs = now()))
        }
    }

    /** Why nothing can be sent now, or null. Text and destination are the caller's to check. */
    fun blocked(): SendBlock? = blocked(_state.value, mesh.value, now())

    /**
     * Takes a message for a node ([peerKey]) or a channel slot ([channel]).
     * Null when it was stored and handed to the connection; otherwise why it
     * was not taken, and then nothing was stored and the draft is untouched.
     */
    suspend fun send(unit: String, conversation: String, peerKey: String?, channel: Int?, text: String, limit: Int): SendBlock? =
        send(unit, conversation, peerKey, channel, text, limit, clearDraft = true, replaces = null)

    private suspend fun send(unit: String, conversation: String, peerKey: String?, channel: Int?, text: String, limit: Int, clearDraft: Boolean, replaces: String?): SendBlock? {
        val body = SendText.clean(text)
        if ((peerKey == null) == (channel == null) || (peerKey != null && !MeshMessage.isKey(peerKey))) return SendBlock.NOT_FOUND
        if (SendText.check(body, limit) != null) return SendBlock.TEXT
        return take(unit) { journal ->
            // One tap, one message: a second tap while the first is on its way is not a second message.
            if (store.open(unit).any { it.phase == OutPhase.SENDING && !it.resend && it.conversation == conversation }) return@take null
            store.add(unit, Outgoing(clientMsgId = newId(), conversation = conversation, peerKey = peerKey?.lowercase(), channel = channel, text = body, journalId = journal, createdAtMs = now()), clearDraft, replaces)
        }
    }

    /**
     * Asks for an unacknowledged direct message of the current run to be sent
     * again (`mesh.resend`). The caller has asked the person.
     */
    suspend fun resend(unit: String, conversation: String, runId: String, messageId: Long, attempts: Int?): SendBlock? {
        if (mesh.value.runId != runId) return SendBlock.STALE_RUN
        return take(unit) { journal ->
            val mine = store.open(unit).filter { it.resendRunId == runId && it.resendMsgId == messageId }
            if (mine.isNotEmpty()) return@take null
            store.add(unit, Outgoing(clientMsgId = newId(), conversation = conversation, peerKey = null, channel = null, text = "", resendRunId = runId, resendMsgId = messageId, attemptsBefore = attempts, journalId = journal, createdAtMs = now()), clearDraft = false)
        }
    }

    /** The shared gate of [send] and [resend]: session, capability, scope, mesh, rate; then [make] stores the send, or returns null when one is already on its way. */
    private suspend fun take(unit: String, make: suspend (journal: String?) -> Outgoing?): SendBlock? {
        val l = live?.takeIf { it.unit == unit } ?: return SendBlock.OFFLINE
        // Stored and handed over as one step, whatever becomes of the caller:
        // a screen that closes cannot leave a message stored and not sent.
        return withContext(NonCancellable) {
            val o = lock.withLock {
                blocked()?.let { return@withContext it }
                rateOpenAt()?.let { at ->
                    _state.update { it.copy(rateOpenAtMs = at) }
                    return@withContext SendBlock.RATE
                }
                make(mesh.value.journalId) ?: return@withContext SendBlock.IN_FLIGHT
            }
            if (!launchIn(l) { transmit(l, o, first = true) }) {
                // The session ended between the check and here: it was stored and
                // never handed to a connection. The words are kept, as not sent.
                lock.withLock { store.put(unit, o.copy(phase = OutPhase.FAILED, why = Why.NOT_CONNECTED, updatedAtMs = now())) }
            }
            null
        }
    }

    private fun launchIn(l: Live, block: suspend () -> Unit): Boolean {
        if (live !== l) return false
        return l.scope.launch { block() }.let { !it.isCancelled }
    }

    /**
     * A person asks the unit about a send that got no answer. The same id is
     * repeated; if the unit has no record of it, it sends it now.
     */
    suspend fun check(unit: String, clientMsgId: String): SendBlock? {
        val l = live?.takeIf { it.unit == unit } ?: return SendBlock.OFFLINE
        blocked()?.let { return it }
        val o = lock.withLock { store.get(unit, clientMsgId)?.takeIf { it.phase == OutPhase.UNCERTAIN } } ?: return SendBlock.NOT_FOUND
        return if (launchIn(l) { resolve(l, o) }) null else SendBlock.OFFLINE
    }

    /**
     * A person decides to send the words of a FAILED or UNKNOWN send as a new
     * message, under a new id. For UNKNOWN the caller has said that it may
     * arrive twice. The old entry goes in the same step in which the new one
     * is stored, or not at all. [peerKey] and [channel] are where the thread
     * sends now: a channel slot that has since become another channel's is
     * not written to.
     */
    suspend fun sendAgain(unit: String, clientMsgId: String, peerKey: String?, channel: Int?, limit: Int): SendBlock? {
        val old = lock.withLock { store.get(unit, clientMsgId) } ?: return SendBlock.NOT_FOUND
        if (old.resend || (old.phase != OutPhase.FAILED && old.phase != OutPhase.UNKNOWN)) return SendBlock.NOT_FOUND
        if (old.peerKey != peerKey?.lowercase() || old.channel != channel) return SendBlock.NOT_FOUND
        return send(unit, old.conversation, old.peerKey, old.channel, old.text, limit, clearDraft = false, replaces = clientMsgId)
    }

    /**
     * A person removes a send that is not a message: FAILED, UNKNOWN, a
     * finished resend, or one without an answer that they no longer want
     * asked about. Never one that is on its way.
     */
    suspend fun discard(unit: String, clientMsgId: String) = lock.withLock {
        val o = store.get(unit, clientMsgId) ?: return@withLock
        if (o.phase != OutPhase.SENDING && (o.phase != OutPhase.ACCEPTED || o.resend)) store.remove(unit, clientMsgId)
    }

    // ---- the request ----

    private sealed interface Answer {
        class Accepted(val runId: String, val messageId: Long) : Answer
        class Error(val code: Int, val message: String) : Answer
        data object None : Answer
        data object Unreadable : Answer
    }

    private suspend fun transmit(l: Live, o: Outgoing, first: Boolean) {
        val params = buildJsonObject {
            if (o.resend) {
                put("message_id", o.resendMsgId)
                put("run_id", o.resendRunId)
            } else {
                if (o.peerKey != null) put("to", o.peerKey) else put("channel", o.channel)
                put("text", o.text)
            }
            put("client_msg_id", o.clientMsgId)
        }
        lock.withLock {
            noteAsked()
            rateOpenAt()?.let { at -> _state.update { it.copy(rateOpenAtMs = at) } }
        }
        val answer = try {
            val r = l.conn.call(if (o.resend) "mesh.resend" else "mesh.send", params, callMs).result
            val run = r.string("run_id")
            val id = r.long("message_id")
            if (r.bool("accepted") == true && !run.isNullOrEmpty() && id != null && id >= 1) Answer.Accepted(run, id) else Answer.Unreadable
        } catch (e: GatewayError) {
            Answer.Error(e.code, e.message.orEmpty())
        } catch (_: TimeoutCancellationException) {
            Answer.None
        } catch (_: IOException) {
            Answer.None
        }
        val next = settle(o, answer, first)
        // An answer that came is written down, also when the session ends at this moment.
        withContext(NonCancellable) {
            lock.withLock {
                // Only a send still in flight is settled: nothing overwrites what was decided since.
                if (store.get(l.unit, o.clientMsgId)?.phase == OutPhase.SENDING) store.put(l.unit, next)
                if (answer is Answer.Error && answer.code == GatewayError.RATE) _state.update { it.copy(rateOpenAtMs = rateOpenAt() ?: (now() + RATE_HOLD_MS)) }
            }
        }
        if (next.phase == OutPhase.UNCERTAIN) {
            l.scope.launch {
                delay(settleMs)
                wake.trySend(Unit)
            }
        }
    }

    /**
     * What an answer means. On the first request every refusal says that
     * nothing was sent. On a repeat, a refusal that the gateway also gives
     * while the first request is still on its way (busy, not permitted, rate,
     * away) says nothing about the first, and the send stays uncertain.
     */
    private fun settle(o: Outgoing, a: Answer, first: Boolean): Outgoing {
        fun to(phase: OutPhase, why: Why) = o.copy(phase = phase, why = why, updatedAtMs = now())
        return when (a) {
            is Answer.Accepted -> o.copy(phase = OutPhase.ACCEPTED, why = Why.NONE, runId = a.runId, messageId = a.messageId, updatedAtMs = now())
            Answer.None -> to(OutPhase.UNCERTAIN, Why.NO_ANSWER)
            Answer.Unreadable -> to(OutPhase.UNKNOWN, Why.OUTCOME_UNKNOWN)
            is Answer.Error -> when (a.code) {
                GatewayError.UPSTREAM -> to(OutPhase.UNCERTAIN, Why.SERVICE_AWAY)
                GatewayError.POLICY -> if (o.resend && a.message == STALE_RUN_MESSAGE) to(OutPhase.FAILED, Why.STALE_RUN) else to(OutPhase.UNKNOWN, Why.OUTCOME_UNKNOWN)
                GatewayError.INVALID -> to(OutPhase.FAILED, Why.REFUSED)
                GatewayError.UNKNOWN_METHOD, GatewayError.BACKEND, GatewayError.NOT_SUPPORTED -> to(OutPhase.FAILED, Why.UNIT_ERROR)
                GatewayError.BUSY -> if (first) to(OutPhase.FAILED, Why.BUSY) else to(OutPhase.UNCERTAIN, Why.BUSY)
                GatewayError.NOT_PERMITTED -> if (first) to(OutPhase.FAILED, Why.NOT_PERMITTED) else to(OutPhase.UNCERTAIN, Why.NOT_PERMITTED)
                GatewayError.RATE -> if (first) to(OutPhase.FAILED, Why.RATE) else to(OutPhase.UNCERTAIN, Why.RATE)
                // A code this app does not know says nothing it can rely on.
                else -> to(OutPhase.UNCERTAIN, Why.NO_ANSWER)
            }
        }
    }

    // ---- sends without an answer ----

    /** The app's own asking: once per send, soon after the tap, never later. */
    private suspend fun checkOpen(unit: String) {
        val l = live?.takeIf { it.unit == unit } ?: return
        for (o in lock.withLock { store.open(unit) }) {
            if (o.phase != OutPhase.UNCERTAIN || o.checks >= AUTO_CHECKS || now() - o.createdAtMs > autoWindowMs) continue
            val since = now() - o.updatedAtMs
            if (since < settleMs) {
                l.scope.launch {
                    delay(settleMs - since)
                    wake.trySend(Unit)
                }
                continue
            }
            // In the session's scope, not this loop's: a request once made is waited for.
            l.scope.launch { resolve(l, o) }
        }
    }

    private suspend fun resolve(l: Live, seen: Outgoing) {
        val o = lock.withLock {
            val o = store.get(l.unit, seen.clientMsgId)?.takeIf { it.phase == OutPhase.UNCERTAIN } ?: return
            val journal = mesh.value.journalId
            val gone = when {
                // Another gateway process: it never heard of this id, and a repeat would be a new message.
                o.journalId == null || journal == null || journal != o.journalId -> Why.GATEWAY_RESTARTED
                // The gateway keeps 64 ids per phone and replaces settled ones first.
                store.newerThan(l.unit, o.ord) >= RECORDS_SAFE -> Why.RECORD_GONE
                else -> null
            }
            if (gone != null) {
                store.put(l.unit, looked(l.unit, o, gone))
                return
            }
            o.copy(phase = OutPhase.SENDING, checks = o.checks + 1, updatedAtMs = now()).also { store.put(l.unit, it) }
        }
        transmit(l, o, first = false)
    }

    /** Without a gateway record: what the synced history says, and nothing more. */
    private suspend fun looked(unit: String, o: Outgoing, why: Why): Outgoing {
        if (o.resend) {
            val attempts = store.attempts(unit, o.resendRunId.orEmpty(), o.resendMsgId ?: 0)
            return if (attempts != null && o.attemptsBefore != null && attempts > o.attemptsBefore) {
                o.copy(phase = OutPhase.ACCEPTED, why = Why.NONE, runId = o.resendRunId, messageId = o.resendMsgId, updatedAtMs = now())
            } else o.copy(phase = OutPhase.UNKNOWN, why = why, updatedAtMs = now())
        }
        val found = store.findSent(unit, o) ?: return o.copy(phase = OutPhase.UNKNOWN, why = why, updatedAtMs = now())
        return o.copy(phase = OutPhase.ACCEPTED, why = Why.NONE, runId = found.first, messageId = found.second, updatedAtMs = now())
    }

    // ---- the rate window, as the phone can count it ----

    private fun noteAsked() {
        val t = now()
        asked.addLast(t)
        while (asked.isNotEmpty() && (t - asked.first() >= RATE_WINDOW_MS || asked.size > 64)) asked.removeFirst()
    }

    /** When the next request fits the window, or null when one fits now. */
    private fun rateOpenAt(): Long? {
        if (!guardRate) return null
        val t = now()
        val recent = asked.filter { t - it < RATE_WINDOW_MS }
        val perMin = _state.value.perMin.coerceAtLeast(1)
        return if (recent.size >= perMin) recent[recent.size - perMin] + RATE_WINDOW_MS else null
    }

    companion object {
        const val CAPABILITY = "mesh.send"
        const val SCOPE = "mesh_send"
        const val PER_MIN = 20
        const val RATE_WINDOW_MS = 60_000L
        const val RATE_HOLD_MS = 15_000L

        /** Longer than the gateway's own 2 s for meshcored, and its lookup after that. */
        const val CALL_MS = 10_000L

        /** The gateway gives meshcored 2 s before it calls a send unanswered; asking sooner would only hear "busy". */
        const val SETTLE_MS = 3_000L
        const val AUTO_CHECKS = 1
        const val AUTO_WINDOW_MS = 120_000L
        const val RECORDS_SAFE = 48
        const val STALE_RUN_MESSAGE = "not a message of this run"

        fun blocked(s: SendState, l: MeshLink, nowMs: Long): SendBlock? = when {
            !s.session -> SendBlock.OFFLINE
            !s.served -> SendBlock.NOT_SERVED
            !s.permitted -> SendBlock.NOT_PERMITTED
            l.phase == MeshPhase.SERVICE_AWAY -> SendBlock.SERVICE_AWAY
            l.phase == MeshPhase.NOT_PERMITTED -> SendBlock.NOT_PERMITTED
            l.phase != MeshPhase.LIVE || l.journalId == null -> SendBlock.SYNCING
            s.rateOpenAtMs > nowMs -> SendBlock.RATE
            else -> null
        }
    }
}
