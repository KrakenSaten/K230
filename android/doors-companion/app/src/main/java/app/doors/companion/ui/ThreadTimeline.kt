// Copyright (c) 2026 Doors authors.
// SPDX-License-Identifier: Apache-2.0
package app.doors.companion.ui

import app.doors.companion.core.MeshMessage
import app.doors.companion.core.OutPhase
import app.doors.companion.data.MarkRow
import app.doors.companion.data.MessageRow
import app.doors.companion.data.OutboxRow
import java.time.Instant
import java.time.LocalDate
import java.time.ZoneId

internal sealed interface Entry {
    val key: String
}

/**
 * A message the unit holds. [mine]: this phone asked for it to be sent.
 * [resend]: a resend this phone asked for that is not simply done.
 */
internal class Said(val m: MessageRow, val first: Boolean, val mine: Boolean, val resend: OutboxRow?) : Entry {
    override val key get() = "m" + m.ord
}

/** A send of this phone that is not (yet) a message the unit holds. */
internal class Pending(val o: OutboxRow, val first: Boolean) : Entry {
    override val key get() = "o" + o.ord
}

/** [before]: the entry it stands above. The phone's clock can go back, so a date alone may come twice. */
internal class Day(val date: LocalDate, val before: String) : Entry {
    override val key get() = "d$before"
}

internal class Break(val mark: MarkRow) : Entry {
    override val key get() = "k" + mark.id
}

internal fun day(ms: Long): LocalDate = Instant.ofEpochMilli(ms).atZone(ZoneId.systemDefault()).toLocalDate()

private fun sameWords(m: MessageRow, o: OutboxRow) = m.body == o.body || MeshMessage.body(m.body, m.senderName) == o.body

/**
 * The thread as it is drawn, oldest first: the loaded messages in the order
 * this phone learned of them, a line where the day changes, every break in
 * continuity that lies inside the loaded span, and this phone's own sends
 * where they were made.
 *
 * A send and the message it became are one bubble, whichever the phone hears
 * of first:
 *  - the answer first: the send is drawn alone until its message arrives;
 *  - the message first (its event can pass the answer): the message is drawn,
 *    and a send still on its way with the same words steps back for it;
 *  - once the answer names the message, the message carries the send's mark.
 * How the message fared is always read from the message, never from the send.
 *
 * [rows] newest first, as the store pages them; [whole]: nothing older is
 * left to load.
 */
internal fun timeline(rows: List<MessageRow>, marks: List<MarkRow>, outbox: List<OutboxRow>, whole: Boolean): List<Entry> {
    val asc = rows.asReversed()
    val oldest = asc.firstOrNull()?.ord
    val sends = outbox.filter { !it.isResend }
    val linked = sends.filter { it.runId != null && it.msgId != null }.associateBy { it.runId!! to it.msgId!! }
    val resends = outbox.filter { it.isResend && it.outPhase != OutPhase.ACCEPTED }.groupBy { it.resendRunId to it.resendMsgId }.mapValues { it.value.maxBy { r -> r.ord } }
    val shown = asc.mapTo(HashSet()) { it.runId to it.msgId }

    // A send still on its way steps back for an unclaimed message with its words.
    val absorbed = HashSet<Long>()
    val taken = HashSet<Long>()
    for (o in sends) {
        if (o.outPhase != OutPhase.SENDING) continue
        val hit = asc.firstOrNull { m -> m.outgoing && m.ord > o.afterOrd && m.ord !in taken && (m.runId to m.msgId) !in linked && sameWords(m, o) } ?: continue
        taken += hit.ord
        absorbed += o.ord
    }
    val pending = ArrayDeque(
        sends.filter { o ->
            o.ord !in absorbed && !(o.runId != null && o.msgId != null && (o.runId to o.msgId) in shown) &&
                (whole || oldest == null || o.afterOrd >= oldest)
        }.sortedBy { it.ord },
    )
    val breaks = ArrayDeque(marks.filter { oldest != null && it.afterOrd >= oldest }.sortedWith(compareBy({ it.afterOrd }, { it.id })))

    val out = ArrayList<Entry>(asc.size + pending.size + 8)
    var shownDay: LocalDate? = null
    var lastOut: Boolean? = null
    var lastName: String? = null
    var lastAt = 0L

    fun voice(outgoing: Boolean, name: String?, atMs: Long, key: String): Boolean {
        val d = day(atMs)
        if (d != shownDay) {
            out += Day(d, key)
            shownDay = d
            lastOut = null
        }
        val same = lastOut == outgoing && lastName == name && atMs - lastAt < 5 * 60_000
        lastOut = outgoing
        lastName = name
        lastAt = atMs
        return !same
    }

    fun before(ord: Long) {
        while (breaks.isNotEmpty() && breaks.first().afterOrd < ord) {
            out += Break(breaks.removeFirst())
            lastOut = null
        }
        while (pending.isNotEmpty() && pending.first().afterOrd < ord) {
            val o = pending.removeFirst()
            out += Pending(o, voice(true, null, o.createdAtMs, "o" + o.ord))
        }
    }

    for (m in asc) {
        before(m.ord)
        val first = voice(m.outgoing, m.senderName, m.heardAtMs, "m" + m.ord)
        out += Said(m, first, mine = (m.runId to m.msgId) in linked, resend = resends[m.runId to m.msgId])
    }
    before(Long.MAX_VALUE)
    return out
}
