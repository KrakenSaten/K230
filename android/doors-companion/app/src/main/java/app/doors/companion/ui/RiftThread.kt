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
import androidx.compose.runtime.remember
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.runtime.snapshotFlow
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.geometry.CornerRadius
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.PathEffect
import androidx.compose.ui.graphics.StrokeCap
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import app.doors.companion.Link
import app.doors.companion.core.MeshMessage
import app.doors.companion.core.PairedUnit
import app.doors.companion.data.MarkRow
import app.doors.companion.data.MessageRow
import app.doors.companion.data.markKind
import java.time.Instant
import java.time.LocalDate
import java.time.ZoneId
import java.time.format.DateTimeFormatter
import java.util.Locale

private const val PAGE = 60

private sealed interface Entry {
    val key: String
}

private class Said(val m: MessageRow, val first: Boolean) : Entry {
    override val key get() = "m" + m.ord
}

private class Day(val date: LocalDate) : Entry {
    override val key get() = "d$date"
}

private class Break(val mark: MarkRow) : Entry {
    override val key get() = "k" + mark.id
}

private fun day(ms: Long): LocalDate = Instant.ofEpochMilli(ms).atZone(ZoneId.systemDefault()).toLocalDate()

private val CLOCK = DateTimeFormatter.ofPattern("HH:mm", Locale.ROOT)
private val DATE = DateTimeFormatter.ofPattern("EEEE d MMMM", Locale.ENGLISH)

private fun clock(ms: Long): String = Instant.ofEpochMilli(ms).atZone(ZoneId.systemDefault()).format(CLOCK)

/**
 * The thread as it is drawn, oldest first: the loaded messages in the order
 * this phone learned of them, a line where the day changes, and every break in
 * continuity that lies inside the loaded span. A break is global (the journal
 * is one journal), so it is drawn in every thread that was already there.
 */
private fun timeline(rows: List<MessageRow>, marks: List<MarkRow>): List<Entry> {
    val asc = rows.asReversed()
    val oldest = asc.firstOrNull()?.ord ?: return emptyList()
    val pending = ArrayDeque(marks.filter { it.afterOrd >= oldest }.sortedWith(compareBy({ it.afterOrd }, { it.id })))
    val out = ArrayList<Entry>(asc.size + 8)
    var last: MessageRow? = null
    var shown: LocalDate? = null
    for (m in asc) {
        while (pending.isNotEmpty() && pending.first().afterOrd < m.ord) {
            out += Break(pending.removeFirst())
            last = null
        }
        val d = day(m.heardAtMs)
        if (d != shown) {
            out += Day(d)
            shown = d
            last = null
        }
        val sameVoice = last != null && last.outgoing == m.outgoing && last.senderName == m.senderName && m.heardAtMs - last.heardAtMs < 5 * 60_000
        out += Said(m, !sameVoice)
        last = m
    }
    while (pending.isNotEmpty()) out += Break(pending.removeFirst())
    return out
}

@Composable
fun ThreadScreen(link: Link, unit: PairedUnit, conv: String, onBack: () -> Unit) {
    val dao = link.meshDb.dao()
    val id = unit.fingerprint
    var limit by rememberSaveable(conv) { mutableIntStateOf(PAGE) }
    val rows by remember(id, conv, limit) { dao.thread(id, conv, limit) }.collectAsStateWithLifecycle(emptyList())
    val total by remember(id, conv) { dao.threadSize(id, conv) }.collectAsStateWithLifecycle(0)
    val marks by remember(id) { dao.marks(id) }.collectAsStateWithLifecycle(emptyList())
    val nodes by remember(id) { dao.nodes(id) }.collectAsStateWithLifecycle(emptyList())
    val channels by remember(id) { dao.channels(id) }.collectAsStateWithLifecycle(emptyList())
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
    val entries = remember(rows, marks) { timeline(rows, marks).asReversed() }
    val state = rememberLazyListState()

    // Reading it is reaching its end: the newest message on screen is read.
    LaunchedEffect(newest?.ord) {
        newest?.let { dao.markRead(id, conv, it.ord) }
        // Someone reading the end stays at the end; someone reading back is left where they are.
        if (state.firstVisibleItemIndex <= 2) state.animateScrollToItem(0)
    }
    // Older pages are taken from the phone's store as the list is scrolled back.
    LaunchedEffect(state, total) {
        snapshotFlow { (state.layoutInfo.visibleItemsInfo.lastOrNull()?.index ?: 0) >= state.layoutInfo.totalItemsCount - 8 }
            .collect { nearTop -> if (nearTop && limit < total) limit += PAGE }
    }

    Column(Modifier.fillMaxSize().statusBarsPadding().navigationBarsPadding()) {
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
                    is Said -> Bubble(e.m, e.first, channel)
                    is Day -> DayLine(e.date)
                    is Break -> BreakLine(e.mark)
                }
            }
            if (rows.isNotEmpty() && rows.size >= total) {
                item(key = "start") {
                    Column(Modifier.fillMaxWidth().padding(vertical = 18.dp), horizontalAlignment = Alignment.CenterHorizontally) {
                        Label("Where this phone's copy begins", color = Doors.Faint)
                        Text("The unit keeps no history. Anything earlier was never given to this phone.", style = Type.Body, color = Doors.Muted, modifier = Modifier.padding(top = 4.dp, start = 24.dp, end = 24.dp))
                    }
                }
            }
            if (rows.isEmpty()) {
                item(key = "none") {
                    Column(Modifier.fillMaxWidth().padding(vertical = 80.dp), horizontalAlignment = Alignment.CenterHorizontally) {
                        Text("Nothing stored here yet", style = Type.Title)
                        Text("Messages appear as the unit hears them.", style = Type.Body, modifier = Modifier.padding(top = 6.dp))
                    }
                }
            }
        }
        Hairline()
        // Where a composer will be. It is a statement, not a control.
        Row(Modifier.fillMaxWidth().padding(horizontal = 20.dp, vertical = 14.dp), verticalAlignment = Alignment.CenterVertically) {
            Canvas(Modifier.size(14.dp)) {
                val w = size.width
                drawRoundRect(Doors.Muted, Offset(w * 0.12f, w * 0.44f), Size(w * 0.76f, w * 0.52f), CornerRadius(w * 0.12f), style = Stroke(1.4.dp.toPx()))
                drawArc(Doors.Muted, 180f, 180f, false, Offset(w * 0.28f, w * 0.06f), Size(w * 0.44f, w * 0.7f), style = Stroke(1.4.dp.toPx(), cap = StrokeCap.Round))
            }
            Spacer(Modifier.width(12.dp))
            Text("Read only. This release of the Companion sends nothing to the mesh.", style = Type.Body, color = Doors.Muted)
        }
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

private class Delivery(val text: String, val color: Color)

/**
 * What may be said about a message the unit sent. Only what MeshCore can
 * know: a channel message is never "delivered", and "sent" means accepted by
 * the service, not transmitted (docs/api/mesh.md).
 */
private fun delivery(m: MessageRow): Delivery = when {
    m.channelKind -> when {
        (m.heardBack ?: 0) > 0 -> Delivery("Heard back ×${m.heardBack}", Doors.Ok)
        m.transmitted == true -> Delivery("On the air", Doors.Tx)
        m.transmitted == false -> Delivery("Not transmitted", Doors.Error)
        else -> Delivery("Queued", Doors.Muted)
    }
    m.state == "acked" -> Delivery("Acknowledged", Doors.Ok)
    m.state == "no_ack" -> Delivery("No acknowledgement", Doors.Warn)
    m.state == "failed" -> Delivery("Failed", Doors.Error)
    m.state == "sent_direct" -> Delivery(if (m.ackExpected) "Sent direct  ·  awaiting ACK" else "Sent direct", Doors.Muted)
    m.state == "sent_flood" -> Delivery(if (m.ackExpected) "Sent flood  ·  awaiting ACK" else "Sent flood", Doors.Muted)
    else -> Delivery(m.state.replace('_', ' '), Doors.Muted)
}

@Composable
private fun Bubble(m: MessageRow, first: Boolean, channel: Boolean) {
    val out = m.outgoing
    val shape = RoundedCornerShape(
        topStart = if (!out && first) 6.dp else 18.dp, topEnd = if (out && first) 6.dp else 18.dp, bottomStart = 18.dp, bottomEnd = 18.dp,
    )
    val tint = if (out) Doors.Tx else RiftInk
    Column(Modifier.fillMaxWidth().padding(top = if (first) 10.dp else 3.dp), horizontalAlignment = if (out) Alignment.End else Alignment.Start) {
        if (first) {
            val who = if (out) "This unit" else if (channel) m.senderName?.ifBlank { null } else null
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
                    val d = delivery(m)
                    Dot(d.color, 5.dp)
                    Text(d.text, style = Type.Label.copy(letterSpacing = Type.Body.letterSpacing), color = d.color)
                } else if (m.rssiDbm != null || m.snrDb != null) {
                    Text(
                        listOfNotNull(m.rssiDbm?.let { "%.0f dBm".format(it) }, m.snrDb?.let { "%.1f dB".format(it) }).joinToString("  "),
                        style = Type.Mono.copy(fontSize = Type.Label.fontSize), color = Doors.Faint,
                    )
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
        }
        Spacer(Modifier.height(12.dp))
    }
}
