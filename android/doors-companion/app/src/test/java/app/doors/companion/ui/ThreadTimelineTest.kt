// Copyright (c) 2026 Doors authors.
// SPDX-License-Identifier: Apache-2.0
package app.doors.companion.ui

import app.doors.companion.core.OutPhase
import app.doors.companion.data.MarkRow
import app.doors.companion.data.MessageRow
import app.doors.companion.data.OutboxRow
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * A send and the message it became are drawn as one, whichever of the answer
 * and the event the phone gets first, and what is said about delivery is read
 * from the message alone.
 */
class ThreadTimelineTest {
    private val conv = "d:" + "c1".repeat(32)
    private val t0 = 1_700_000_000_000L

    private fun msg(ord: Long, id: Long, body: String, outgoing: Boolean = true, state: String = "sent_flood", run: String = "run1", sender: String? = null) = MessageRow(
        ord = ord, unit = "u", runId = run, msgId = id, seq = ord, conv = conv, outgoing = outgoing, channelKind = false, peerKey = "c1".repeat(32),
        peerName = "Ola", channel = null, channelName = null, channelHash = null, senderName = sender, body = body, sentAt = null, state = state,
        ackExpected = outgoing, attempts = if (outgoing) 1 else null, route = null, pathHops = null, transmitted = null, heardBack = null,
        heardBackHops = null, snrDb = null, rssiDbm = null, heardAtMs = t0 + ord * 1000,
    )

    private fun out(ord: Long, body: String, phase: OutPhase, afterOrd: Long, run: String? = null, id: Long? = null, resendOf: Long? = null) = OutboxRow(
        ord = ord, unit = "u", clientMsgId = "id-$ord", conv = conv, peerKey = "c1".repeat(32), channel = null, body = body,
        resendRunId = resendOf?.let { "run1" }, resendMsgId = resendOf, attemptsBefore = resendOf?.let { 1 }, phase = phase.name, why = "NONE",
        journalId = "j1", runId = run, msgId = id, afterOrd = afterOrd, createdAtMs = t0 + afterOrd * 1000 + 500, updatedAtMs = t0, checks = 0,
    )

    private fun draw(rows: List<MessageRow>, outbox: List<OutboxRow>, marks: List<MarkRow> = emptyList(), whole: Boolean = true) =
        timeline(rows.sortedByDescending { it.ord }, marks, outbox, whole).filter { it !is Day }

    @Test
    fun theAnswerFirstThenTheMessage() {
        val had = msg(1, 1, "hello?", outgoing = false, state = "received")
        // Tapped: on its way.
        var t = draw(listOf(had), listOf(out(1, "yes", OutPhase.SENDING, afterOrd = 1)))
        assertEquals(listOf("m1", "o1"), t.map { it.key })
        // Answered before the event came: accepted, drawn alone.
        t = draw(listOf(had), listOf(out(1, "yes", OutPhase.ACCEPTED, 1, "run1", 2)))
        assertEquals(listOf("m1", "o1"), t.map { it.key })
        // The event: one bubble, the message's, marked as this phone's.
        t = draw(listOf(had, msg(2, 2, "yes")), listOf(out(1, "yes", OutPhase.ACCEPTED, 1, "run1", 2)))
        assertEquals(listOf("m1", "m2"), t.map { it.key })
        assertTrue((t[1] as Said).mine)
        assertFalse((t[0] as Said).mine)
    }

    @Test
    fun theMessageFirstThenTheAnswer() {
        val had = msg(1, 1, "hello?", outgoing = false, state = "received")
        // The event passed the answer, and already says acknowledged.
        val arrived = msg(2, 2, "yes", state = "acked")
        var t = draw(listOf(had, arrived), listOf(out(1, "yes", OutPhase.SENDING, afterOrd = 1)))
        assertEquals("never two bubbles for one message", listOf("m1", "m2"), t.map { it.key })
        assertEquals("Delivered", delivery((t[1] as Said).m, "run1").text)
        // The answer names it: still one bubble, and the answer took nothing back.
        t = draw(listOf(had, arrived), listOf(out(1, "yes", OutPhase.ACCEPTED, 1, "run1", 2)))
        assertEquals(listOf("m1", "m2"), t.map { it.key })
        assertTrue((t[1] as Said).mine)
        assertEquals("Delivered", delivery((t[1] as Said).m, "run1").text)
    }

    @Test
    fun aSendOnItsWayStepsBackOnlyForItsOwnWords() {
        val older = msg(1, 1, "yes") // the same words, sent before the tap
        val other = msg(2, 2, "something else") // sent after it, by the unit
        val t = draw(listOf(older, other), listOf(out(1, "yes", OutPhase.SENDING, afterOrd = 1)))
        assertEquals(listOf("m1", "o1", "m2"), t.map { it.key })
        // Two sends of the same words: the one message that came hides only the one on its way.
        val two = draw(listOf(older, msg(3, 3, "yes")), listOf(out(1, "yes", OutPhase.SENDING, 1), out(2, "yes", OutPhase.UNCERTAIN, 1)))
        assertEquals(listOf("m1", "o2", "m3"), two.map { it.key })
    }

    @Test
    fun whatIsNotAMessageStaysWhereItWasSent() {
        val rows = listOf(msg(1, 1, "a", outgoing = false, state = "received"), msg(2, 2, "b", outgoing = false, state = "received"))
        val t = draw(rows, listOf(out(1, "refused", OutPhase.FAILED, afterOrd = 1), out(2, "who knows", OutPhase.UNKNOWN, afterOrd = 2), out(3, "no answer", OutPhase.UNCERTAIN, afterOrd = 2)))
        assertEquals(listOf("m1", "o1", "m2", "o2", "o3"), t.map { it.key })
        // With nothing stored at all, a first message to a node is still drawn.
        assertEquals(listOf("o1"), draw(emptyList(), listOf(out(1, "first words", OutPhase.SENDING, 0))).map { it.key })
        // Not everything is loaded: a send from before the loaded span waits for its page.
        assertEquals(listOf("m2", "o2", "o3"), draw(rows.takeLast(1), listOf(out(1, "refused", OutPhase.FAILED, 1), out(2, "x", OutPhase.UNKNOWN, 2), out(3, "y", OutPhase.UNCERTAIN, 2)), whole = false).map { it.key })
    }

    @Test
    fun aResendBelongsToItsMessageAndDeliveryKnowsItsLimits() {
        val waiting = msg(1, 1, "are you there", state = "no_ack")
        val t = draw(listOf(waiting), listOf(out(1, "are you there", OutPhase.ACCEPTED, 0, "run1", 1), out(2, "", OutPhase.UNCERTAIN, 1, resendOf = 1)))
        assertEquals(listOf("m1"), t.map { it.key })
        assertEquals(OutPhase.UNCERTAIN, (t[0] as Said).resend!!.outPhase)
        assertEquals("a finished resend leaves no mark", null, (draw(listOf(waiting), listOf(out(2, "", OutPhase.ACCEPTED, 1, "run1", 1, resendOf = 1)))[0] as Said).resend)

        assertEquals("No acknowledgement", delivery(waiting, "run1").text)
        assertEquals("Accepted  ·  awaiting acknowledgement", delivery(msg(2, 2, "x"), "run1").text)
        assertEquals("a message of an earlier run can get no acknowledgement any more", "Outcome unknown", delivery(msg(2, 2, "x"), "run2").text)
        assertEquals("what it already had, it keeps", "Delivered", delivery(msg(2, 2, "x", state = "acked"), "run2").text)
        val channel = msg(3, 3, "Doors bench: evening", sender = "Doors bench").copy(channelKind = true, ackExpected = false)
        assertEquals("Accepted", delivery(channel, "run1").text)
        assertEquals("On the air", delivery(channel.copy(transmitted = true), "run1").text)
        assertEquals("Heard back ×2", delivery(channel.copy(transmitted = true, heardBack = 2), "run1").text)
        assertEquals("a channel message is never called delivered", "On the air", delivery(channel.copy(transmitted = true, state = "acked"), "run1").text)
    }
}
