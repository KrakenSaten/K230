// Copyright (c) 2026 Doors authors.
// SPDX-License-Identifier: Apache-2.0
package app.doors.companion.ui

import androidx.compose.animation.animateColorAsState
import androidx.compose.animation.core.animateFloatAsState
import androidx.compose.animation.core.spring
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.interaction.MutableInteractionSource
import androidx.compose.foundation.interaction.collectIsFocusedAsState
import androidx.compose.foundation.interaction.collectIsPressedAsState
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.text.BasicTextField
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.rememberUpdatedState
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.draw.scale
import androidx.compose.ui.geometry.CornerRadius
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.Path
import androidx.compose.ui.graphics.SolidColor
import androidx.compose.ui.graphics.StrokeCap
import androidx.compose.ui.graphics.StrokeJoin
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.input.KeyboardCapitalization
import androidx.compose.ui.unit.dp
import androidx.compose.ui.window.Dialog
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import app.doors.companion.Link
import app.doors.companion.core.MeshSend
import app.doors.companion.core.OutPhase
import app.doors.companion.core.SendBlock
import app.doors.companion.core.SendText
import app.doors.companion.core.Why
import app.doors.companion.data.OutboxRow
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch

/** What a thread sends to. Null fields: the destination is not on the unit (any more). */
internal class Destination(val peerKey: String?, val channel: Int?, val limit: Int, val gone: Boolean)

internal fun why(o: OutboxRow, perMin: Int): String = when (o.outWhy) {
    Why.NONE -> ""
    Why.NO_ANSWER -> "The unit did not answer."
    Why.SERVICE_AWAY -> "The unit's mesh service did not answer."
    Why.REFUSED -> if (o.channel != null) "The unit refused it: too long for this channel, or the channel is no longer there." else "The unit refused it: it does not know this node, or does not take this text."
    Why.BUSY -> "The unit had no room: eight messages already wait for an acknowledgement, or its radio is not available."
    Why.NOT_PERMITTED -> "The unit does not let this phone send."
    Why.RATE -> "The unit takes $perMin messages a minute from this phone."
    Why.OUTCOME_UNKNOWN -> "The unit cannot tell whether it went out."
    Why.STALE_RUN -> "It belongs to an earlier run of the mesh service."
    Why.GATEWAY_RESTARTED -> "The gateway restarted and no longer knows it, and it is not among the messages the unit still holds."
    Why.RECORD_GONE -> "Too much was sent since for the unit to remember it, and it is not among the messages the unit still holds."
    Why.UNIT_ERROR -> "The unit answered with an error."
    Why.NOT_CONNECTED -> "The connection ended before it was handed over."
}

/** What the composer says when nothing can be sent now. The draft is kept in every case. */
internal fun blockText(b: SendBlock, perMin: Int, waitS: Long): String = when (b) {
    SendBlock.OFFLINE -> "Not connected. Your draft is kept on this phone. Nothing is sent until you tap Send while the unit is reached."
    SendBlock.SYNCING -> "Catching up with the unit…"
    SendBlock.SERVICE_AWAY -> "The unit's mesh service is away. Your draft is kept."
    SendBlock.RATE -> "The unit takes $perMin messages a minute from this phone. Sending opens again in $waitS s."
    SendBlock.NOT_SERVED -> "This unit's gateway does not offer sending."
    SendBlock.NOT_PERMITTED -> "This phone may read the mesh but not send. Sending is granted on the unit, on its Companion page."
    else -> ""
}

/** A small text control under a bubble. */
@Composable
internal fun Act(text: String, color: Color, onClick: () -> Unit) {
    Text(
        text.uppercase(), style = Type.Label, color = color,
        modifier = Modifier.clip(RoundedCornerShape(8.dp)).border(1.dp, color.copy(alpha = 0.45f), RoundedCornerShape(8.dp))
            .clickable(role = Role.Button, onClick = onClick).padding(horizontal = 10.dp, vertical = 7.dp),
    )
}

/** A question that is asked before anything is transmitted on a person's second thought. */
@Composable
internal fun Confirm(title: String, text: String, yes: String, onYes: () -> Unit, onNo: () -> Unit) {
    Dialog(onDismissRequest = onNo) {
        Panel(Modifier.fillMaxWidth(), tint = Doors.Raised) {
            Text(title, style = Type.Title)
            Text(text, style = Type.Body, modifier = Modifier.padding(top = 8.dp, bottom = 18.dp))
            PrimaryButton(yes, onClick = onYes)
            Spacer(Modifier.height(10.dp))
            GhostButton("Not now", onClick = onNo)
        }
    }
}

/**
 * A send of this phone that is not a message on the unit: on its way,
 * accepted and not yet heard of, without an answer, unknown, or refused.
 * Every state says what is known and offers only what is safe.
 */
@Composable
internal fun PendingBubble(o: OutboxRow, first: Boolean, perMin: Int, currentRun: String?, canAsk: Boolean, onCheck: () -> Unit, onAgain: () -> Unit, onDiscard: () -> Unit) {
    val phase = o.outPhase
    val color = when (phase) {
        OutPhase.SENDING, OutPhase.ACCEPTED -> Doors.Tx
        OutPhase.UNCERTAIN, OutPhase.UNKNOWN -> Doors.Warn
        OutPhase.FAILED -> Doors.Error
    }
    val shape = RoundedCornerShape(topStart = 18.dp, topEnd = if (first) 6.dp else 18.dp, bottomStart = 18.dp, bottomEnd = 18.dp)
    val quiet = phase == OutPhase.SENDING || phase == OutPhase.FAILED
    Column(Modifier.fillMaxWidth().padding(top = if (first) 10.dp else 3.dp), horizontalAlignment = Alignment.End) {
        if (first) Text("From this phone", style = Type.Label.copy(letterSpacing = Type.Body.letterSpacing), color = Doors.Tx, modifier = Modifier.padding(horizontal = 6.dp, vertical = 3.dp))
        Column(
            Modifier.widthIn(max = 300.dp).clip(shape).background(color.copy(alpha = if (quiet) 0.05f else 0.10f))
                .border(1.dp, color.copy(alpha = if (phase == OutPhase.SENDING) 0.2f else 0.4f), shape).padding(horizontal = 14.dp, vertical = 10.dp),
        ) {
            Text(o.body, style = Type.Body.copy(color = if (quiet) Doors.Secondary else Doors.Text))
            Spacer(Modifier.height(6.dp))
            Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                Text(clock(o.createdAtMs), style = Type.Mono.copy(fontSize = Type.Label.fontSize), color = Doors.Muted)
                Dot(color, 5.dp, pulse = phase == OutPhase.SENDING)
                Text(
                    when (phase) {
                        OutPhase.SENDING -> if (o.checks > 0) "Asking the unit…" else "Sending…"
                        OutPhase.ACCEPTED -> "Accepted by the unit"
                        OutPhase.UNCERTAIN -> "No answer"
                        OutPhase.UNKNOWN -> "Outcome unknown"
                        OutPhase.FAILED -> "Not sent"
                    },
                    style = Type.Label.copy(letterSpacing = Type.Body.letterSpacing), color = color,
                )
            }
        }
        val note = when (phase) {
            OutPhase.SENDING -> null
            OutPhase.ACCEPTED -> if (currentRun != null && o.runId != currentRun) "The mesh service restarted since. What became of it is not known." else null
            OutPhase.UNCERTAIN -> why(o, perMin) + " Whether it was sent is not known. It is not sent again unless you ask."
            OutPhase.UNKNOWN -> why(o, perMin) + " It may have been sent. It will never be sent again by itself."
            OutPhase.FAILED -> why(o, perMin) + " Nothing was transmitted."
        }
        if (note != null) Text(note, style = Type.Body.copy(fontSize = Type.Mono.fontSize, lineHeight = Type.Mono.lineHeight), color = Doors.Muted, modifier = Modifier.widthIn(max = 300.dp).padding(start = 6.dp, end = 6.dp, top = 5.dp))
        Row(Modifier.padding(top = if (phase == OutPhase.SENDING || phase == OutPhase.ACCEPTED) 0.dp else 8.dp, bottom = if (phase == OutPhase.SENDING || phase == OutPhase.ACCEPTED) 0.dp else 8.dp, end = 2.dp), horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            when (phase) {
                OutPhase.UNCERTAIN -> if (canAsk) Act("Ask the unit", Doors.Warn, onCheck)
                OutPhase.UNKNOWN -> {
                    Act("Discard", Doors.Muted, onDiscard)
                    if (canAsk) Act("Send as new", Doors.Warn, onAgain)
                }
                OutPhase.FAILED -> {
                    Act("Discard", Doors.Muted, onDiscard)
                    if (canAsk) Act("Try again", Doors.Accent, onAgain)
                }
                else -> Unit
            }
        }
    }
}

/**
 * The composer. Typing is always possible while sending is offered at all,
 * and what is typed is the thread's draft: kept on the phone through lost
 * connections, other screens and a restart of the app, until it is sent or
 * cleared by hand. Send is a tap and nothing else: the keyboard's return key
 * makes a line, and a message leaves only while its going can be watched.
 */
@Composable
internal fun Composer(link: Link, unit: String, conv: String, title: String, channel: Boolean, to: Destination, block: SendBlock?, perMin: Int, waitS: Long, sending: Boolean) {
    val scope = rememberCoroutineScope()
    val dao = link.meshDb.dao()
    var text by remember(unit, conv) { mutableStateOf("") }
    var loaded by remember(unit, conv) { mutableStateOf(false) }
    var busy by remember(unit, conv) { mutableStateOf(false) }
    val latest by rememberUpdatedState(text)

    LaunchedEffect(unit, conv) {
        text = dao.draft(unit, conv).orEmpty()
        loaded = true
    }
    // The draft follows the typing, a moment behind; never while a send is taking it.
    LaunchedEffect(text, busy, loaded) {
        if (!loaded || busy) return@LaunchedEffect
        delay(300)
        link.keepDraft(unit, conv, text)
    }
    DisposableEffect(unit, conv) {
        onDispose { if (loaded && !busy) link.keepDraft(unit, conv, latest) }
    }

    if (block == SendBlock.NOT_SERVED || block == SendBlock.NOT_PERMITTED) {
        // Not a control: sending is not this phone's to do here.
        Row(Modifier.fillMaxWidth().padding(horizontal = 20.dp, vertical = 14.dp), verticalAlignment = Alignment.CenterVertically) {
            Canvas(Modifier.size(14.dp)) {
                val w = size.width
                drawRoundRect(Doors.Muted, Offset(w * 0.12f, w * 0.44f), Size(w * 0.76f, w * 0.52f), CornerRadius(w * 0.12f), style = Stroke(1.4.dp.toPx()))
                drawArc(Doors.Muted, 180f, 180f, false, Offset(w * 0.28f, w * 0.06f), Size(w * 0.44f, w * 0.7f), style = Stroke(1.4.dp.toPx(), cap = StrokeCap.Round))
            }
            Spacer(Modifier.width(12.dp))
            Text(blockText(block, perMin, waitS), style = Type.Body, color = Doors.Muted)
        }
        return
    }

    val clean = SendText.clean(text)
    val bytes = SendText.bytes(clean)
    val fault = if (clean.isEmpty()) null else SendText.check(clean, to.limit)
    val ready = loaded && !busy && !sending && block == null && !to.gone && clean.isNotEmpty() && fault == null
    val notice: Pair<String, Color>? = when {
        to.gone -> (if (channel) "This channel is no longer on the unit." else "This node cannot be written to.") to Doors.Warn
        fault == SendText.Fault.TOO_LONG -> "Too long by ${bytes - to.limit} bytes. ${if (channel) "This channel carries" else "A direct message carries"} ${to.limit}." to Doors.Error
        fault == SendText.Fault.CONTROL || fault == SendText.Fault.MALFORMED -> "It holds a character the mesh does not carry." to Doors.Error
        block != null -> blockText(block, perMin, waitS) to (if (block == SendBlock.SYNCING) Doors.Muted else Doors.Warn)
        else -> null
    }

    Column(Modifier.fillMaxWidth().padding(start = 16.dp, end = 16.dp, top = 10.dp, bottom = 12.dp)) {
        if (channel) {
            Text(
                "A channel message is broadcast. Nobody acknowledges it, so its delivery is never confirmed.",
                style = Type.Body.copy(fontSize = Type.Mono.fontSize, lineHeight = Type.Mono.lineHeight), color = Doors.Muted, modifier = Modifier.padding(start = 4.dp, end = 4.dp, bottom = 8.dp),
            )
        }
        if (notice != null) {
            Text(notice.first, style = Type.Body.copy(fontSize = Type.Mono.fontSize, lineHeight = Type.Mono.lineHeight), color = notice.second, modifier = Modifier.padding(start = 4.dp, end = 4.dp, bottom = 8.dp))
        }
        Row(verticalAlignment = Alignment.Bottom) {
            val source = remember { MutableInteractionSource() }
            val focused by source.collectIsFocusedAsState()
            val edge by animateColorAsState(if (fault != null) Doors.Error.copy(alpha = 0.7f) else if (focused) Doors.Accent.copy(alpha = 0.6f) else Doors.Line, label = "edge")
            val box = RoundedCornerShape(22.dp)
            BasicTextField(
                value = text,
                onValueChange = { if (!busy) text = it.replace("\r\n", "\n").replace('\r', '\n') },
                modifier = Modifier.weight(1f).heightIn(min = 46.dp).clip(box).background(Doors.Surface).border(1.dp, edge, box).padding(horizontal = 16.dp, vertical = 12.dp),
                textStyle = Type.Body.copy(color = Doors.Text),
                cursorBrush = SolidColor(Doors.Accent),
                maxLines = 5,
                keyboardOptions = KeyboardOptions(capitalization = KeyboardCapitalization.Sentences),
                interactionSource = source,
                decorationBox = { inner ->
                    Box(contentAlignment = Alignment.CenterStart) {
                        if (text.isEmpty()) Text(if (channel) "Message $title" else "Message to $title", style = Type.Body, color = Doors.Faint, maxLines = 1)
                        inner()
                    }
                },
            )
            Spacer(Modifier.width(10.dp))
            SendButton(ready, busy || sending) {
                // One tap is one message: the button is spent before anything else happens.
                if (busy) return@SendButton
                busy = true
                val words = text
                scope.launch {
                    try {
                        val refused = link.send.send(unit, conv, to.peerKey, to.channel, words, to.limit)
                        if (refused == null) {
                            text = ""
                            link.keepDraft(unit, conv, "")
                        }
                    } finally {
                        busy = false
                    }
                }
            }
        }
        if (clean.isNotEmpty()) {
            val near = bytes > to.limit * 9 / 10
            Text(
                "$bytes / ${to.limit} bytes", style = Type.Mono.copy(fontSize = Type.Label.fontSize),
                color = if (bytes > to.limit) Doors.Error else if (near) Doors.Warn else Doors.Faint,
                modifier = Modifier.align(Alignment.End).padding(top = 6.dp, end = 60.dp),
            )
        }
    }
}

@Composable
private fun SendButton(enabled: Boolean, working: Boolean, onClick: () -> Unit) {
    val source = remember { MutableInteractionSource() }
    val pressed by source.collectIsPressedAsState()
    val scale by animateFloatAsState(if (pressed && enabled) 0.9f else 1f, spring(stiffness = 700f), label = "send")
    val bg by animateColorAsState(if (enabled) Doors.Accent else Doors.Raised, label = "sendbg")
    Box(
        Modifier.size(46.dp).scale(scale).clip(CircleShape).background(bg)
            .clickable(interactionSource = source, indication = null, enabled = enabled, role = Role.Button, onClick = onClick)
            .semantics { contentDescription = if (working) "Sending" else "Send" },
        contentAlignment = Alignment.Center,
    ) {
        if (working) {
            Dot(Doors.Tx, 8.dp, pulse = true)
        } else {
            val ink = if (enabled) Doors.OnAccent else Doors.Faint
            Canvas(Modifier.size(18.dp)) {
                val w = size.width
                val p = Path().apply {
                    moveTo(w * 0.5f, w * 0.9f); lineTo(w * 0.5f, w * 0.12f)
                    moveTo(w * 0.18f, w * 0.44f); lineTo(w * 0.5f, w * 0.12f); lineTo(w * 0.82f, w * 0.44f)
                }
                drawPath(p, ink, style = Stroke(2.dp.toPx(), cap = StrokeCap.Round, join = StrokeJoin.Round))
            }
        }
    }
}

/** How long the rate limit still holds, in whole seconds, for the composer's notice. */
@Composable
internal fun rateWait(send: MeshSend, now: Long): Long {
    val s by send.state.collectAsStateWithLifecycle()
    return ((s.rateOpenAtMs - now + 999) / 1000).coerceAtLeast(0)
}
