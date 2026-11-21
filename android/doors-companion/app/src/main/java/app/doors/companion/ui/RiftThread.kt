// Copyright (c) 2026 Doors authors.
// SPDX-License-Identifier: Apache-2.0
package app.doors.companion.ui

import androidx.compose.foundation.Canvas
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.imePadding
import androidx.compose.foundation.layout.navigationBarsPadding
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.statusBarsPadding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.lazy.rememberLazyListState
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.runtime.snapshotFlow
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.PathEffect
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import app.doors.companion.Link
import app.doors.companion.core.MeshMessage
import app.doors.companion.core.MeshPhase
import app.doors.companion.core.MeshSend
import app.doors.companion.core.OutPhase
import app.doors.companion.core.PairedUnit
import app.doors.companion.core.SendText
import app.doors.companion.data.MarkRow
import app.doors.companion.data.MessageRow
import app.doors.companion.data.OutboxRow
import app.doors.companion.data.markKind
import kotlinx.coroutines.launch
import java.time.Instant
import java.time.LocalDate
import java.time.ZoneId
import java.time.format.DateTimeFormatter
import java.util.Locale

private const val PAGE = 60

private val CLOCK = DateTimeFormatter.ofPattern("HH:mm", Locale.ROOT)
private val DATE = DateTimeFormatter.ofPattern("EEEE d MMMM", Locale.ENGLISH)

internal fun clock(ms: Long): String = Instant.ofEpochMilli(ms).atZone(ZoneId.systemDefault()).format(CLOCK)

/** Something a person is asked before it is done. */
private sealed interface Ask {
    class Check(val o: OutboxRow) : Ask
    class AsNew(val o: OutboxRow) : Ask
    class Resend(val m: MessageRow, val old: OutboxRow?) : Ask
}

@Composable
fun ThreadScreen(link: Link, unit: PairedUnit, conv: String, onBack: () -> Unit) {
    val dao = link.meshDb.dao()
    val id = unit.fingerprint
    val scope = rememberCoroutineScope()
    var limit by rememberSaveable(conv) { mutableIntStateOf(PAGE) }
    val rows by remember(id, conv, limit) { dao.thread(id, conv, limit) }.collectAsStateWithLifecycle(emptyList())
    val total by remember(id, conv) { dao.threadSize(id, conv) }.collectAsStateWithLifecycle(0)
    val marks by remember(id) { dao.marks(id) }.collectAsStateWithLifecycle(emptyList())
    val nodes by remember(id) { dao.nodes(id) }.collectAsStateWithLifecycle(emptyList())
    val listed by remember(id) { dao.channels(id) }.collectAsStateWithLifecycle(null)
    val outbox by remember(id, conv) { dao.outbox(id, conv) }.collectAsStateWithLifecycle(emptyList())
    val unitRow by remember(id) { dao.unit(id) }.collectAsStateWithLifecycle(null)
    val mesh by link.mesh.link.collectAsStateWithLifecycle()
    val sendState by link.send.state.collectAsStateWithLifecycle()
    val now = rememberClock()
    val channels = listed.orEmpty()
    val newest = rows.firstOrNull()
    val channel = conv.startsWith("c:")
    val peer = if (channel) null else conv.substring(2)
    val slot = channels.firstOrNull { it.conv == conv }
    val title = when {
        newest != null -> threadTitle(newest, nodes.associateBy { it.publicKey })
        channel -> slot?.name?.ifBlank { null } ?: "Channel"
        else -> nodes.firstOrNull { it.publicKey == peer }?.name?.ifBlank { null } ?: shortKey(peer)
    }
    val color = if (channel) RiftInk else hue(peer.orEmpty())
    val whole = rows.size >= total
    val entries = remember(rows, marks, outbox, whole) { timeline(rows, marks, outbox, whole).asReversed() }
    val state = rememberLazyListState()

    val block = MeshSend.blocked(sendState, mesh, now)
    // The run the unit's mesh service is in, as far as this phone has synced. A message of another run can no longer change.
    val storedRun = unitRow?.runId
    val liveRun = mesh.runId.takeIf { mesh.phase == MeshPhase.LIVE }
    val to = if (channel) Destination(null, slot?.slot, SendText.limit(slot?.textLimit), gone = listed != null && slot == null)
    else Destination(peer, null, SendText.DIRECT_MAX, gone = !MeshMessage.isKey(peer))
    val sending = outbox.any { !it.isResend && it.outPhase == OutPhase.SENDING }
    var ask by remember { mutableStateOf<Ask?>(null) }

    // Reading it is reaching its end: the newest message on screen is read.
    LaunchedEffect(newest?.ord, outbox.lastOrNull()?.ord) {
        newest?.let { dao.markRead(id, conv, it.ord) }
        // Someone reading the end stays at the end; someone reading back is left where they are.
        if (state.firstVisibleItemIndex <= 2) state.animateScrollToItem(0)
    }
    // Older pages are taken from the phone's store as the list is scrolled back.
    LaunchedEffect(state, total) {
        snapshotFlow { (state.layoutInfo.visibleItemsInfo.lastOrNull()?.index ?: 0) >= state.layoutInfo.totalItemsCount - 8 }
            .collect { nearTop -> if (nearTop && limit < total) limit += PAGE }
    }

    Column(Modifier.fillMaxSize().statusBarsPadding().navigationBarsPadding().imePadding()) {
        Row(Modifier.fillMaxWidth().padding(start = 20.dp, end = 20.dp, top = 20.dp, bottom = 12.dp), verticalAlignment = Alignment.CenterVertically) {
            BackButton(onBack)
            Spacer(Modifier.width(14.dp))
            Badge(color, 40.dp, glyph = if (channel) GLYPH_CHANNEL else GLYPH_NONE, initials = initials(title))
            Spacer(Modifier.width(12.dp))
            Column(Modifier.weight(1f)) {
                Text(title, style = Type.Title, maxLines = 1, overflow = TextOverflow.Ellipsis)
                Text(
                    if (channel) listOfNotNull("channel", slot?.let { "slot ${it.slot}" }, newest?.channelHash?.let { "hash $it" } ?: slot?.let { "hash ${it.hash}" }).joinToString("  ·  ")
                    else shortKey(peer) + "  ·  direct",
                    style = Type.Mono, color = Doors.Muted, maxLines = 1,
                )
            }
        }
        Hairline()
        LazyColumn(
            Modifier.weight(1f).fillMaxWidth(), state = state, reverseLayout = true,
            contentPadding = PaddingValues(horizontal = 16.dp, vertical = 14.dp),
        ) {
            items(entries, key = { it.key }) { e ->
                when (e) {
                    is Said -> {
                        val again = e.resend?.outPhase
                        val canResend = block == null && e.m.outgoing && !e.m.channelKind && e.m.state == "no_ack" && liveRun != null && e.m.runId == liveRun &&
                            (again == null || again == OutPhase.FAILED || again == OutPhase.UNKNOWN)
                        Bubble(
                            e.m, e.first, channel, e.mine, e.resend, storedRun, sendState.perMin,
                            onResend = if (canResend) ({ ask = Ask.Resend(e.m, e.resend) }) else null,
                            onCheck = e.resend?.takeIf { block == null && again == OutPhase.UNCERTAIN }?.let { r -> { ask = Ask.Check(r) } },
                            onDismiss = e.resend?.takeIf { again == OutPhase.FAILED || again == OutPhase.UNKNOWN }?.let { r -> { scope.launch { link.send.discard(id, r.clientMsgId) } } },
                        )
                    }
                    is Pending -> PendingBubble(
                        e.o, e.first, sendState.perMin, storedRun, canAsk = block == null && !sending,
                        onCheck = { ask = Ask.Check(e.o) },
                        // Refused: nothing went out, so trying again is a plain send. Unknown: it may have, so it is asked.
                        onAgain = { if (e.o.outPhase == OutPhase.UNKNOWN) ask = Ask.AsNew(e.o) else scope.launch { link.send.sendAgain(id, e.o.clientMsgId, to.limit) } },
                        onDiscard = { scope.launch { link.send.discard(id, e.o.clientMsgId) } },
                    )
                    is Day -> DayLine(e.date)
                    is Break -> BreakLine(e.mark)
                }
            }
            if (rows.isNotEmpty() && whole) {
                item(key = "start") {
                    Column(Modifier.fillMaxWidth().padding(vertical = 18.dp), horizontalAlignment = Alignment.CenterHorizontally) {
                        Label("Where this phone's copy begins", color = Doors.Faint)
                        Text("The unit keeps no history. Anything earlier was never given to this phone.", style = Type.Body, color = Doors.Muted, modifier = Modifier.padding(top = 4.dp, start = 24.dp, end = 24.dp))
                    }
                }
            }
            if (entries.isEmpty()) {
                item(key = "none") {
                    Column(Modifier.fillMaxWidth().padding(vertical = 80.dp), horizontalAlignment = Alignment.CenterHorizontally) {
                        Text("Nothing stored here yet", style = Type.Title)
                        Text("Messages appear as the unit hears them.", style = Type.Body, modifier = Modifier.padding(top = 6.dp))
                    }
                }
            }
        }
        Hairline()
        Composer(link, id, conv, title, channel, to, block, sendState.perMin, rateWait(link.send, now), sending)
    }

    when (val a = ask) {
        is Ask.Check -> Confirm(
            "Ask the unit?",
            "The unit is asked about this message under the id it was first given. If the unit has no record of it, it sends it now. It cannot be sent twice this way.",
            "Ask the unit", onYes = { ask = null; scope.launch { link.send.check(id, a.o.clientMsgId) } }, onNo = { ask = null },
        )
        is Ask.AsNew -> Confirm(
            "Send it as a new message?",
            "Nobody can tell whether the first one went out. If it did, ${if (channel) "the channel" else title} gets these words twice.",
            "Send as new", onYes = { ask = null; scope.launch { link.send.sendAgain(id, a.o.clientMsgId, to.limit) } }, onNo = { ask = null },
        )
        is Ask.Resend -> Confirm(
            "Send it again?",
            "No acknowledgement came back. The unit transmits the same message once more. If the first did arrive and only its acknowledgement was lost, $title gets it twice.",
            "Send again",
            onYes = {
                ask = null
                scope.launch {
                    a.old?.let { link.send.discard(id, it.clientMsgId) }
                    link.send.resend(id, conv, a.m.runId, a.m.msgId, a.m.attempts)
                }
            },
            onNo = { ask = null },
        )
        null -> Unit
    }
}

@Composable
private fun DayLine(date: LocalDate) {
    val today = LocalDate.now()
    val text = when (date) {
        today -> "Today"
        today.minusDays(1) -> "Yesterday"
        else -> date.format(DATE)
    }
    Box(Modifier.fillMaxWidth().padding(vertical = 14.dp), contentAlignment = Alignment.Center) { Label(text, color = Doors.Faint) }
}

/** A break in continuity, drawn as the line it is: torn, and labelled with what is and is not known. */
@Composable
private fun BreakLine(mark: MarkRow) {
    Column(Modifier.fillMaxWidth().padding(vertical = 16.dp), horizontalAlignment = Alignment.CenterHorizontally) {
        Row(verticalAlignment = Alignment.CenterVertically) {
            Dashes(Modifier.weight(1f))
            Rupture(Doors.Warn, Modifier.padding(horizontal = 10.dp).size(width = 26.dp, height = 16.dp))
            Dashes(Modifier.weight(1f))
        }
        Spacer(Modifier.height(8.dp))
        Label(markTitle(mark.markKind()), color = Doors.Warn)
        Text(markText(mark.markKind()), style = Type.Body, color = Doors.Muted, modifier = Modifier.padding(top = 4.dp, start = 20.dp, end = 20.dp))
        Text("Found ${clock(mark.atMs)}", style = Type.Mono, color = Doors.Faint, modifier = Modifier.padding(top = 4.dp))
    }
}

@Composable
private fun Dashes(modifier: Modifier) {
    Canvas(modifier.height(1.dp)) {
        drawLine(Doors.Warn.copy(alpha = 0.5f), Offset(0f, 0f), Offset(size.width, 0f), 1.dp.toPx(), pathEffect = PathEffect.dashPathEffect(floatArrayOf(6f, 6f)))
    }
}

internal class Delivery(val text: String, val color: Color)

/**
 * What may be said about a message the unit sent. Only what MeshCore can
 * know: "accepted" is the unit having taken it, not a transmission
 * (docs/api/mesh.md); "delivered" is the recipient's own acknowledgement and
 * nothing less; a channel message has no such thing and is never called
 * delivered. A message of an earlier run that still waited for its
 * acknowledgement when the mesh service started again can never get one:
 * what became of it is not known ([currentRun]: the run this phone last
 * synced with).
 */
internal fun delivery(m: MessageRow, currentRun: String?): Delivery = when {
    m.channelKind -> when {
        (m.heardBack ?: 0) > 0 -> Delivery("Heard back ×${m.heardBack}", Doors.Ok)
        m.transmitted == true -> Delivery("On the air", Doors.Tx)
        m.transmitted == false -> Delivery("Not transmitted", Doors.Error)
        else -> Delivery("Accepted", Doors.Muted)
    }
    m.state == "acked" -> Delivery("Delivered", Doors.Ok)
    m.state == "no_ack" -> Delivery("No acknowledgement", Doors.Warn)
    m.state == "failed" -> Delivery("Failed", Doors.Error)
    m.state == "sent_direct" || m.state == "sent_flood" -> when {
        !m.ackExpected -> Delivery("Accepted", Doors.Muted)
        currentRun != null && m.runId != currentRun -> Delivery("Outcome unknown", Doors.Warn)
        else -> Delivery("Accepted  ·  awaiting acknowledgement", Doors.Muted)
    }
    else -> Delivery(m.state.replace('_', ' '), Doors.Muted)
}

@Composable
private fun Bubble(
    m: MessageRow, first: Boolean, channel: Boolean, mine: Boolean, resend: OutboxRow?, currentRun: String?, perMin: Int,
    onResend: (() -> Unit)?, onCheck: (() -> Unit)?, onDismiss: (() -> Unit)?,
) {
    val out = m.outgoing
    val shape = RoundedCornerShape(
        topStart = if (!out && first) 6.dp else 18.dp, topEnd = if (out && first) 6.dp else 18.dp, bottomStart = 18.dp, bottomEnd = 18.dp,
    )
    val tint = if (out) Doors.Tx else RiftInk
    Column(Modifier.fillMaxWidth().padding(top = if (first) 10.dp else 3.dp), horizontalAlignment = if (out) Alignment.End else Alignment.Start) {
        if (first) {
            val who = if (out) (if (mine) "From this phone" else "This unit") else if (channel) m.senderName?.ifBlank { null } else null
            if (who != null) {
                Row(Modifier.padding(horizontal = 6.dp, vertical = 3.dp), verticalAlignment = Alignment.CenterVertically) {
                    Text(who, style = Type.Label.copy(letterSpacing = Type.Body.letterSpacing), color = if (out) Doors.Tx else hue(who), maxLines = 1)
                    // A channel frame is signed by nobody: the name is what the sender wrote.
                    if (!out && channel) Text("  claimed", style = Type.Label.copy(letterSpacing = Type.Body.letterSpacing), color = Doors.Faint)
                }
            }
        }
        Column(
            Modifier.widthIn(max = 300.dp).clip(shape).background(if (out) tint.copy(alpha = 0.10f) else Doors.Surface)
                .border(1.dp, if (out) tint.copy(alpha = 0.35f) else Doors.Line.copy(alpha = 0.7f), shape).padding(horizontal = 14.dp, vertical = 10.dp),
        ) {
            Text(MeshMessage.body(m.body, m.senderName), style = Type.Body.copy(color = Doors.Text))
            Spacer(Modifier.height(6.dp))
            Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                Text(clock(m.heardAtMs), style = Type.Mono.copy(fontSize = Type.Label.fontSize), color = Doors.Muted)
                if (out) {
                    val d = delivery(m, currentRun)
                    Dot(d.color, 5.dp)
                    Text(d.text, style = Type.Label.copy(letterSpacing = Type.Body.letterSpacing), color = d.color)
                    if ((m.attempts ?: 1) > 1) Text("sent ×${m.attempts}", style = Type.Mono.copy(fontSize = Type.Label.fontSize), color = Doors.Faint)
                } else if (m.rssiDbm != null || m.snrDb != null) {
                    Text(
                        listOfNotNull(m.rssiDbm?.let { "%.0f dBm".format(it) }, m.snrDb?.let { "%.1f dB".format(it) }).joinToString("  "),
                        style = Type.Mono.copy(fontSize = Type.Label.fontSize), color = Doors.Faint,
                    )
                }
            }
        }
        if (out) {
            val note = when (resend?.outPhase) {
                OutPhase.SENDING -> "Asking the unit to send it again…"
                OutPhase.UNCERTAIN -> "Sending it again was asked for and got no answer. Whether it went out again is not known."
                OutPhase.UNKNOWN -> "Whether it went out again is not known. " + why(resend, perMin)
                OutPhase.FAILED -> "It was not sent again. " + why(resend, perMin)
                else -> if (m.state == "sent_direct" || m.state == "sent_flood") {
                    if (m.ackExpected && currentRun != null && m.runId != currentRun) "The mesh service restarted while it waited for its acknowledgement." else null
                } else null
            }
            if (note != null) Text(note, style = Type.Body.copy(fontSize = Type.Mono.fontSize, lineHeight = Type.Mono.lineHeight), color = Doors.Muted, modifier = Modifier.widthIn(max = 300.dp).padding(start = 6.dp, end = 6.dp, top = 5.dp))
            if (onResend != null || onCheck != null || onDismiss != null) {
                Row(Modifier.padding(top = 8.dp, bottom = 8.dp, end = 2.dp), horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    if (onDismiss != null) Act("Dismiss", Doors.Muted, onDismiss)
                    if (onCheck != null) Act("Ask the unit", Doors.Warn, onCheck)
                    if (onResend != null) Act("Send again", Doors.Accent, onResend)
                }
            }
        }
    }
}

// ---- a node ----------------------------------------------------------------

@Composable
fun NodeScreen(link: Link, unit: PairedUnit, key: String, open: (String) -> Unit, onBack: () -> Unit) {
    val dao = link.meshDb.dao()
    val id = unit.fingerprint
    val nodes by remember(id) { dao.nodes(id) }.collectAsStateWithLifecycle(emptyList())
    val threads by remember(id) { dao.conversations(id) }.collectAsStateWithLifecycle(emptyList())
    val now = rememberClock()
    val n = nodes.firstOrNull { it.publicKey == key }
    val thread = threads.firstOrNull { it.last.conv == "d:$key" }
    val color = hue(key)
    Column(
        Modifier.fillMaxSize().verticalScroll(rememberScrollState()).statusBarsPadding().navigationBarsPadding().padding(horizontal = 20.dp, vertical = 12.dp),
        verticalArrangement = Arrangement.spacedBy(12.dp),
    ) {
        Row(Modifier.fillMaxWidth().padding(top = 8.dp, bottom = 8.dp), verticalAlignment = Alignment.CenterVertically) {
            BackButton(onBack)
            Spacer(Modifier.width(16.dp))
            Text("Node", style = Type.Title)
        }
        Panel(Modifier.fillMaxWidth(), tint = Doors.Bg) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Badge(color, 60.dp, glyph = nodeGlyph(n?.type ?: 0), initials = initials(n?.name.orEmpty()))
                Spacer(Modifier.width(16.dp))
                Column(Modifier.weight(1f)) {
                    Text(n?.name?.ifBlank { null } ?: shortKey(key), style = Type.Display.copy(fontSize = Type.Title.fontSize * 1.2f, lineHeight = Type.Title.lineHeight * 1.2f), maxLines = 2, overflow = TextOverflow.Ellipsis)
                    Spacer(Modifier.height(4.dp))
                    Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                        Tag(nodeKind(n?.type ?: 0), color)
                        if (n == null) Tag("No longer on the unit", Doors.Warn)
                    }
                }
            }
            Spacer(Modifier.height(16.dp))
            Label("Public key")
            Spacer(Modifier.height(6.dp))
            Text(key.chunked(8).chunked(4).joinToString("\n") { it.joinToString(" ") }, style = Type.Mono, color = Doors.Secondary)
        }
        if (n != null) {
            Section("Heard") {
                Panel(Modifier.fillMaxWidth()) {
                    KeyValue("Last heard", if (n.heardAtMs == null) "Not since the mesh service started" else ago(n.heardAtMs, now).let { if (it == "now") "just now" else "$it ago" }, mono = n.heardAtMs != null)
                    KeyValue("Signal", n.rssiDbm?.let { "%.0f dBm".format(it) } ?: "—")
                    KeyValue("SNR", n.snrDb?.let { "%.1f dB".format(it) } ?: "—")
                    KeyValue("Advert came", n.advertHops?.let { if (it == 0) "straight from the node" else "through $it relay" + if (it == 1) "" else "s" } ?: "—", mono = false)
                }
            }
            Section("Route back") {
                Panel(Modifier.fillMaxWidth()) {
                    KeyValue("Route", route(n).replaceFirstChar { it.uppercase() }, mono = false, valueColor = if (n.pathKnown) Doors.Ok else Doors.Secondary)
                    Text("How an advert arrived is not the route back: that is learned only when a message to the node is answered.", style = Type.Body, color = Doors.Muted, modifier = Modifier.padding(top = 6.dp))
                }
            }
            if (n.lat != null && n.lon != null) {
                Section("Position") {
                    Panel(Modifier.fillMaxWidth()) {
                        KeyValue("Latitude", "%.6f".format(Locale.ROOT, n.lat))
                        KeyValue("Longitude", "%.6f".format(Locale.ROOT, n.lon))
                        Text("As the node claims it in its adverts. Nothing measured it.", style = Type.Body, color = Doors.Muted, modifier = Modifier.padding(top = 6.dp))
                    }
                }
            }
        }
        if (thread != null) {
            Spacer(Modifier.height(4.dp))
            GhostButton("Open the conversation  ·  ${thread.total}", color = RiftInk) { open("t:d:$key") }
        } else if (n != null && (n.type == 1 || n.type == 3)) {
            // A chat node or a room: a conversation can begin here.
            Spacer(Modifier.height(4.dp))
            GhostButton("Write a message", color = RiftInk) { open("t:d:$key") }
        }
        Spacer(Modifier.height(12.dp))
    }
}
