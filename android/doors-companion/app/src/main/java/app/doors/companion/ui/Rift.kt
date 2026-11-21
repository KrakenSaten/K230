// Copyright (c) 2026 Doors authors.
// SPDX-License-Identifier: Apache-2.0
package app.doors.companion.ui

import androidx.activity.compose.BackHandler
import androidx.compose.animation.AnimatedContent
import androidx.compose.animation.animateColorAsState
import androidx.compose.animation.core.tween
import androidx.compose.animation.fadeIn
import androidx.compose.animation.fadeOut
import androidx.compose.animation.slideInHorizontally
import androidx.compose.animation.slideOutHorizontally
import androidx.compose.animation.togetherWith
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
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
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.LazyListScope
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.State
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.produceState
import androidx.compose.runtime.remember
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.runtime.withFrameMillis
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.geometry.CornerRadius
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.Path
import androidx.compose.ui.graphics.PathEffect
import androidx.compose.ui.graphics.StrokeCap
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import app.doors.companion.Link
import app.doors.companion.core.ActivityBin
import app.doors.companion.core.FeedItem
import app.doors.companion.core.FeedKind
import app.doors.companion.core.MarkKind
import app.doors.companion.core.MeshLink
import app.doors.companion.core.MeshMessage
import app.doors.companion.core.MeshPhase
import app.doors.companion.core.PairedUnit
import app.doors.companion.data.ChannelRow
import app.doors.companion.data.ConversationRow
import app.doors.companion.data.MarkRow
import app.doors.companion.data.MessageRow
import app.doors.companion.data.NodeRow
import app.doors.companion.data.UnitRow
import app.doors.companion.data.markKind
import kotlinx.coroutines.delay
import kotlin.math.abs
import kotlin.math.ln

// RIFT, read-only (M4.1): the mesh as the unit hears it, and as this phone
// has stored it. The room's colour is the radio's receive blue; amber is what
// the unit itself put on the air. Nothing on these screens sends anything.

internal val RiftInk = Doors.Rx
private val Violet = Color(0xFFB7A3E8)
private val Teal = Color(0xFF7FD1C7)
private val Hues = listOf(Doors.Rx, Doors.Accent, Teal, Violet, Doors.Ok, Doors.Tx)

/** A colour for a name or a key: the same one every time, and nothing more than a way to tell rows apart. */
internal fun hue(seed: String): Color = Hues[abs(seed.hashCode() % Hues.size)]

internal fun shortKey(key: String?): String = key.orEmpty().take(8).chunked(4).joinToString(" ")

internal fun ago(atMs: Long?, now: Long): String {
    if (atMs == null || atMs <= 0) return "—"
    val s = ((now - atMs) / 1000).coerceAtLeast(0)
    return when {
        s < 5 -> "now"
        s < 60 -> "$s s"
        s < 3600 -> "${s / 60} min"
        s < 86400 -> "${s / 3600} h"
        else -> "${s / 86400} d"
    }
}

@Composable
internal fun rememberClock(periodMs: Long = 1000): Long {
    val now by produceState(System.currentTimeMillis()) {
        while (true) {
            delay(periodMs)
            value = System.currentTimeMillis()
        }
    }
    return now
}

internal fun threadTitle(m: MessageRow, nodes: Map<String, NodeRow> = emptyMap()): String =
    if (m.channelKind) m.channelName?.ifBlank { null } ?: "Channel ${m.channel}"
    else nodes[m.peerKey]?.name?.ifBlank { null } ?: m.peerName?.ifBlank { null } ?: shortKey(m.peerKey)

internal fun markTitle(kind: MarkKind?): String = when (kind) {
    MarkKind.GAP -> "Messages are missing here"
    else -> "Continuity unknown"
}

internal fun markText(kind: MarkKind?): String = when (kind) {
    MarkKind.GAP -> "The unit's journal had moved on before this phone asked. What passed in between was not received, and cannot be fetched."
    MarkKind.MESH_RESTARTED -> "The mesh service on the unit restarted. Messages may have passed before this point that this phone never saw."
    MarkKind.GATEWAY_RESTARTED -> "The gateway on the unit restarted and rebuilt its journal. Messages may have passed before this point that this phone never saw."
    MarkKind.UNIT_RESTARTED -> "The unit restarted. Messages may have passed before this point that this phone never saw."
    null -> "The history is not known to be whole here."
}

private class PhaseLook(val pill: String, val color: Color, val pulse: Boolean, val line: String?)

private fun phaseLook(l: MeshLink): PhaseLook = when (l.phase) {
    MeshPhase.LIVE -> PhaseLook("Live", RiftInk, true, null)
    MeshPhase.SYNCING -> PhaseLook("Syncing", Doors.Accent, true, null)
    MeshPhase.SERVICE_AWAY -> PhaseLook("Mesh away", Doors.Warn, false, "The unit answers, but its mesh service does not. What is shown is what this phone stored.")
    MeshPhase.NOT_PERMITTED -> PhaseLook("Not granted", Doors.Warn, false, "This phone has not been granted mesh reading on the unit. Its Companion page decides that.")
    MeshPhase.NOT_SERVED -> PhaseLook("Not served", Doors.Muted, false, "This unit's gateway does not serve the mesh yet.")
    MeshPhase.OFFLINE -> PhaseLook("Stored", Doors.Muted, false, "Not connected. What is shown is what this phone stored; nothing new arrives until the unit is reached.")
}

private enum class Tab(val title: String) { Threads("Threads"), Channels("Channels"), Nodes("Nodes"), Activity("Activity") }

@Composable
fun RiftScreen(link: Link, unit: PairedUnit, onBack: () -> Unit) {
    // null: the room. "t:<conversation>": a thread. "n:<public key>": a node.
    var open by rememberSaveable { mutableStateOf<String?>(null) }
    var tab by rememberSaveable { mutableStateOf(Tab.Threads) }
    BackHandler(enabled = open != null) { open = null }
    AnimatedContent(
        open, label = "rift",
        transitionSpec = {
            val dir = if (targetState != null) 1 else -1
            (fadeIn(tween(220)) + slideInHorizontally(tween(280)) { dir * it / 8 }) togetherWith
                (fadeOut(tween(120)) + slideOutHorizontally(tween(280)) { -dir * it / 8 })
        },
    ) { o ->
        when {
            o == null -> RiftRoom(link, unit, tab, { tab = it }, { open = it }, onBack)
            o.startsWith("t:") -> ThreadScreen(link, unit, o.substring(2)) { open = null }
            else -> NodeScreen(link, unit, o.substring(2), { open = it }) { open = null }
        }
    }
}

@Composable
private fun RiftRoom(link: Link, unit: PairedUnit, tab: Tab, onTab: (Tab) -> Unit, open: (String) -> Unit, onBack: () -> Unit) {
    val dao = link.meshDb.dao()
    val id = unit.fingerprint
    val mesh by link.mesh.link.collectAsStateWithLifecycle()
    val bins by link.mesh.activity.collectAsStateWithLifecycle()
    val feed by link.mesh.feed.collectAsStateWithLifecycle()
    val threads by remember(id) { dao.conversations(id) }.collectAsStateWithLifecycle(emptyList())
    val channels by remember(id) { dao.channels(id) }.collectAsStateWithLifecycle(emptyList())
    val nodes by remember(id) { dao.nodes(id) }.collectAsStateWithLifecycle(emptyList())
    val marks by remember(id) { dao.marks(id) }.collectAsStateWithLifecycle(emptyList())
    val stored by remember(id) { dao.stored(id) }.collectAsStateWithLifecycle(0)
    val row by remember(id) { dao.unit(id) }.collectAsStateWithLifecycle(null)
    val now = rememberClock()
    val look = phaseLook(mesh)
    val byKey = remember(nodes) { nodes.associateBy { it.publicKey } }

    LazyColumn(
        Modifier.fillMaxSize().statusBarsPadding().navigationBarsPadding(),
        contentPadding = PaddingValues(start = 20.dp, end = 20.dp, top = 12.dp, bottom = 28.dp),
        verticalArrangement = Arrangement.spacedBy(10.dp),
    ) {
        item(key = "bar") {
            Row(Modifier.fillMaxWidth().padding(top = 8.dp, bottom = 6.dp), verticalAlignment = Alignment.CenterVertically) {
                BackButton(onBack)
                Spacer(Modifier.width(16.dp))
                Column(Modifier.weight(1f)) {
                    Text("RIFT", style = Type.Title)
                    Label("Mesh  ·  read only", color = RiftInk)
                }
                Pill(look.pill, look.color, look.pulse)
            }
        }
        item(key = "scope") { ScopePanel(mesh, bins, row, nodes.size, stored, now) }
        look.line?.let { line -> item(key = "phase") { Text(line, style = Type.Body, modifier = Modifier.padding(horizontal = 4.dp)) } }
        marks.lastOrNull()?.let { m ->
            item(key = "mark") {
                Panel(Modifier.fillMaxWidth(), onClick = { onTab(Tab.Activity) }, tint = Doors.Warn.copy(alpha = 0.07f)) {
                    Row(verticalAlignment = Alignment.CenterVertically) {
                        Rupture(Doors.Warn, Modifier.size(width = 22.dp, height = 16.dp))
                        Spacer(Modifier.width(12.dp))
                        Column(Modifier.weight(1f)) {
                            Text(if (marks.size == 1) "The stored history has a break" else "The stored history has ${marks.size} breaks", style = Type.BodyStrong)
                            Text("Latest ${ago(m.atMs, now)} ago. Nothing here is shown as recovered.", style = Type.Body)
                        }
                        Chevron()
                    }
                }
            }
        }
        item(key = "tabs") { Tabs(tab, threads.sumOf { it.unread }, onTab) }
        when (tab) {
            Tab.Threads -> threads(threads, byKey, now, open)
            Tab.Channels -> channels(channels, threads, open)
            Tab.Nodes -> nodes(nodes, now, open)
            Tab.Activity -> activity(bins, feed, marks, row, mesh, stored, now)
        }
    }
}

@Composable
internal fun BackButton(onBack: () -> Unit) {
    Box(
        Modifier.size(44.dp).clip(RoundedCornerShape(14.dp)).background(Doors.Surface).border(1.dp, Doors.Line.copy(alpha = 0.7f), RoundedCornerShape(14.dp)).clickable(onClick = onBack),
        contentAlignment = Alignment.Center,
    ) { Chevron(back = true, color = RiftInk) }
}

// ---- the scope: sixty seconds of the radio ---------------------------------

/**
 * The room's image: a fissure across the panel, the last minute running along
 * it from left to right. What the radio received stands above the line in
 * blue, what the unit transmitted hangs below in amber (red when it did not go
 * out). Counts only; the gateway passes on no frame.
 */
@Composable
private fun ScopePanel(mesh: MeshLink, bins: List<ActivityBin>, row: UnitRow?, nodes: Int, stored: Int, now: Long) {
    val live = mesh.phase == MeshPhase.LIVE
    val clock: State<Long> = produceState(System.currentTimeMillis(), live) {
        while (live) withFrameMillis { value = System.currentTimeMillis() }
    }
    val ink by animateColorAsState(if (live) RiftInk else Doors.Faint, tween(600), label = "ink")
    val recent = bins.filter { now - it.atMs < SCOPE_MS }
    Panel(Modifier.fillMaxWidth(), tint = Doors.Bg) {
        Row(verticalAlignment = Alignment.CenterVertically) {
            Column(Modifier.weight(1f)) {
                Label("This node")
                Spacer(Modifier.height(4.dp))
                Text(row?.nodeName?.ifBlank { null } ?: "—", style = Type.BodyStrong, maxLines = 1, overflow = TextOverflow.Ellipsis)
            }
            Text(shortKey(row?.nodeKey).ifEmpty { "····" }, style = Type.Mono, color = Doors.Secondary)
        }
        Spacer(Modifier.height(10.dp))
        Canvas(Modifier.fillMaxWidth().height(118.dp)) {
            val t = if (live) clock.value else now
            val mid = size.height * 0.56f
            val up = mid - 6.dp.toPx()
            val down = size.height - mid - 6.dp.toPx()
            val slot = size.width / SCOPE_SLOTS
            val bar = (slot * 0.56f).coerceAtLeast(1.5f)
            // Ten-second ticks, as on a chart recorder's paper.
            for (i in 1 until 6) {
                val x = size.width * i / 6f
                drawLine(Doors.Line.copy(alpha = 0.35f), Offset(x, 0f), Offset(x, size.height), 1f, pathEffect = PathEffect.dashPathEffect(floatArrayOf(3f, 7f)))
            }
            for (b in bins) {
                val age = (t - b.atMs).toFloat()
                if (age < 0 || age > SCOPE_MS) continue
                val x = size.width * (1f - age / SCOPE_MS) - bar / 2
                val fade = 0.35f + 0.65f * (1f - age / SCOPE_MS)
                if (b.rxFrames > 0) {
                    val h = up * level(b.rxFrames)
                    drawRoundRect(
                        Brush.verticalGradient(listOf(RiftInk.copy(alpha = fade), RiftInk.copy(alpha = fade * 0.25f)), startY = mid - h, endY = mid),
                        Offset(x, mid - h - 2.dp.toPx()), Size(bar, h), CornerRadius(bar / 2),
                    )
                }
                val tx = b.txOk + b.txFailed + b.txUnknown
                if (tx > 0) {
                    val h = down * level(tx)
                    val c = if (b.txFailed > 0) Doors.Error else if (b.txUnknown > 0 && b.txOk == 0) Doors.Warn else Doors.Tx
                    drawRoundRect(c.copy(alpha = fade), Offset(x, mid + 2.dp.toPx()), Size(bar, h), CornerRadius(bar / 2))
                }
            }
            // The fissure itself: a line that is never quite straight.
            val rift = Path().apply {
                moveTo(0f, mid)
                var x = 0f
                var i = 0
                while (x < size.width) {
                    x += size.width / 24f
                    lineTo(x, mid + JAG[i++ % JAG.size] * 1.6.dp.toPx())
                }
            }
            drawPath(rift, ink.copy(alpha = 0.22f), style = Stroke(5.dp.toPx(), cap = StrokeCap.Round))
            drawPath(rift, ink, style = Stroke(1.2.dp.toPx(), cap = StrokeCap.Round))
            if (live) drawCircle(ink, 3.dp.toPx(), Offset(size.width - 3.dp.toPx(), mid + JAG[23 % JAG.size] * 1.6.dp.toPx()))
        }
        Row(Modifier.fillMaxWidth().padding(top = 2.dp), horizontalArrangement = Arrangement.SpaceBetween) {
            Label("60 s ago", color = Doors.Faint)
            Label(if (live) "now" else "not live", color = Doors.Faint)
        }
        Spacer(Modifier.height(12.dp))
        Hairline()
        Row(Modifier.fillMaxWidth().padding(top = 12.dp)) {
            Figure("Heard", if (live) recent.sumOf { it.rxFrames }.toString() else "—", "frames / min", RiftInk, Modifier.weight(1f))
            Figure("Sent", if (live) recent.sumOf { it.txOk }.toString() else "—", "frames / min", Doors.Tx, Modifier.weight(1f))
            Figure("Nodes", nodes.toString(), "known", Doors.Text, Modifier.weight(1f))
            Figure("Stored", stored.toString(), "messages", Doors.Text, Modifier.weight(1f))
        }
    }
}

private const val SCOPE_MS = 60_000f
private const val SCOPE_SLOTS = 60
private val JAG = floatArrayOf(0f, 0.6f, -0.4f, 0.9f, -0.8f, 0.2f, -0.3f, 1f, -0.6f, 0.4f, -1f, 0.5f, 0.1f, -0.7f, 0.8f, -0.2f, 0.3f, -0.9f, 0.7f, -0.5f, 0.2f, -0.1f, 0.6f, 0f)

private fun level(n: Int): Float = (ln(1f + n) / ln(17f)).coerceIn(0.12f, 1f)

@Composable
private fun Figure(label: String, value: String, unit: String, color: Color, modifier: Modifier) {
    Column(modifier) {
        Label(label)
        Text(value, style = Type.Figure.copy(fontSize = Type.Title.fontSize), color = color, maxLines = 1)
        Text(unit, style = Type.Label.copy(letterSpacing = Type.Body.letterSpacing), color = Doors.Faint, maxLines = 1)
    }
}

/** Two halves of a line that do not meet: the mark for a break in the history. */
@Composable
internal fun Rupture(color: Color, modifier: Modifier = Modifier) {
    Canvas(modifier) {
        val y = size.height / 2
        val w = size.width
        val s = Stroke(1.6.dp.toPx(), cap = StrokeCap.Round)
        drawPath(Path().apply { moveTo(0f, y); lineTo(w * 0.30f, y); lineTo(w * 0.42f, y - size.height * 0.4f) }, color, style = s)
        drawPath(Path().apply { moveTo(w * 0.58f, y + size.height * 0.4f); lineTo(w * 0.70f, y); lineTo(w, y) }, color, style = s)
    }
}

@Composable
private fun Tabs(tab: Tab, unread: Int, onTab: (Tab) -> Unit) {
    Row(
        Modifier.fillMaxWidth().padding(top = 6.dp).clip(RoundedCornerShape(16.dp)).background(Doors.Surface)
            .border(1.dp, Doors.Line.copy(alpha = 0.7f), RoundedCornerShape(16.dp)).padding(4.dp),
    ) {
        for (t in Tab.entries) {
            val on = t == tab
            val bg by animateColorAsState(if (on) Doors.Raised else Color.Transparent, label = "tab")
            Row(
                Modifier.weight(1f).height(40.dp).clip(RoundedCornerShape(12.dp)).background(bg).clickable { onTab(t) },
                horizontalArrangement = Arrangement.Center, verticalAlignment = Alignment.CenterVertically,
            ) {
                Text(t.title.uppercase(), style = Type.Label, color = if (on) RiftInk else Doors.Muted, maxLines = 1)
                if (t == Tab.Threads && unread > 0) {
                    Spacer(Modifier.width(6.dp))
                    Dot(RiftInk, 6.dp)
                }
            }
        }
    }
}

// ---- glyphs ----------------------------------------------------------------

/** A thread's or a node's badge. The app ships no icon font: every glyph is drawn. */
@Composable
internal fun Badge(color: Color, size: Dp = 44.dp, glyph: Int = GLYPH_NONE, initials: String = "") {
    val shape = if (glyph == GLYPH_CHANNEL) RoundedCornerShape(size * 0.3f) else CircleShape
    Box(Modifier.size(size).clip(shape).background(color.copy(alpha = 0.12f)).border(1.dp, color.copy(alpha = 0.45f), shape), contentAlignment = Alignment.Center) {
        if (glyph == GLYPH_NONE) {
            Text(initials, style = Type.BodyStrong, color = color, maxLines = 1)
        } else {
            Canvas(Modifier.size(size * 0.42f)) {
                val w = this.size.width
                val s = Stroke(1.7.dp.toPx(), cap = StrokeCap.Round)
                when (glyph) {
                    GLYPH_CHANNEL -> { // a hash
                        drawLine(color, Offset(w * 0.36f, w * 0.08f), Offset(w * 0.24f, w * 0.92f), s.width, StrokeCap.Round)
                        drawLine(color, Offset(w * 0.76f, w * 0.08f), Offset(w * 0.64f, w * 0.92f), s.width, StrokeCap.Round)
                        drawLine(color, Offset(w * 0.08f, w * 0.34f), Offset(w * 0.96f, w * 0.34f), s.width, StrokeCap.Round)
                        drawLine(color, Offset(w * 0.04f, w * 0.66f), Offset(w * 0.92f, w * 0.66f), s.width, StrokeCap.Round)
                    }
                    GLYPH_REPEATER -> { // a mast, and what it passes on
                        drawLine(color, Offset(w * 0.5f, w * 0.42f), Offset(w * 0.5f, w), s.width, StrokeCap.Round)
                        drawCircle(color, w * 0.09f, Offset(w * 0.5f, w * 0.34f))
                        drawArc(color, 200f, 140f, false, Offset(w * 0.2f, w * 0.04f), Size(w * 0.6f, w * 0.6f), style = s)
                        drawArc(color, 215f, 110f, false, Offset(0f, -w * 0.16f), Size(w, w), style = s)
                    }
                    GLYPH_ROOM -> drawRoundRect(color, Offset(w * 0.1f, w * 0.18f), Size(w * 0.8f, w * 0.64f), CornerRadius(w * 0.14f), style = s)
                    else -> drawPath(Path().apply { moveTo(w * 0.5f, 0f); lineTo(w, w * 0.5f); lineTo(w * 0.5f, w); lineTo(0f, w * 0.5f); close() }, color, style = s)
                }
            }
        }
    }
}

internal const val GLYPH_NONE = 0
internal const val GLYPH_CHANNEL = 1
internal const val GLYPH_REPEATER = 2
internal const val GLYPH_ROOM = 3
internal const val GLYPH_SENSOR = 4

internal fun initials(name: String): String =
    name.split(' ', '-', '_').filter { it.isNotEmpty() }.take(2).joinToString("") { it.take(1) }.uppercase().ifEmpty { "?" }

internal fun nodeGlyph(type: Int) = when (type) {
    2 -> GLYPH_REPEATER
    3 -> GLYPH_ROOM
    4 -> GLYPH_SENSOR
    else -> GLYPH_NONE
}

internal fun nodeKind(type: Int) = when (type) {
    1 -> "Chat"
    2 -> "Repeater"
    3 -> "Room"
    4 -> "Sensor"
    else -> "Node"
}

@Composable
private fun Empty(title: String, text: String) {
    Panel(Modifier.fillMaxWidth(), tint = Doors.Bg) {
        Text(title, style = Type.Title)
        Text(text, style = Type.Body, modifier = Modifier.padding(top = 6.dp))
    }
}

// ---- threads ---------------------------------------------------------------

private fun LazyListScope.threads(list: List<ConversationRow>, nodes: Map<String, NodeRow>, now: Long, open: (String) -> Unit) {
    if (list.isEmpty()) {
        item { Empty("No messages stored", "The unit keeps no messages itself. What its gateway still holds is read when this phone connects, and what arrives while it is connected is kept here.") }
    }
    items(list, key = { "t" + it.last.conv }) { c ->
        val m = c.last
        val title = threadTitle(m, nodes)
        val color = if (m.channelKind) RiftInk else hue(m.peerKey.orEmpty())
        Panel(Modifier.fillMaxWidth(), onClick = { open("t:" + m.conv) }) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Badge(color, glyph = if (m.channelKind) GLYPH_CHANNEL else GLYPH_NONE, initials = initials(title))
                Spacer(Modifier.width(14.dp))
                Column(Modifier.weight(1f)) {
                    Row(verticalAlignment = Alignment.CenterVertically) {
                        Text(title, style = Type.BodyStrong, maxLines = 1, overflow = TextOverflow.Ellipsis, modifier = Modifier.weight(1f))
                        Text(ago(m.heardAtMs, now), style = Type.Mono, color = if (c.unread > 0) RiftInk else Doors.Muted)
                    }
                    Spacer(Modifier.height(2.dp))
                    Row(verticalAlignment = Alignment.CenterVertically) {
                        val who = if (m.outgoing) "Unit: " else if (m.channelKind && !m.senderName.isNullOrEmpty()) "${m.senderName}: " else ""
                        Text(
                            who + MeshMessage.body(m.body, m.senderName).replace('\n', ' '), style = Type.Body,
                            color = if (c.unread > 0) Doors.Text else Doors.Secondary, maxLines = 1, overflow = TextOverflow.Ellipsis, modifier = Modifier.weight(1f),
                        )
                        if (c.unread > 0) {
                            Spacer(Modifier.width(10.dp))
                            Box(Modifier.clip(CircleShape).background(RiftInk).padding(horizontal = 8.dp, vertical = 2.dp)) {
                                Text(if (c.unread > 99) "99+" else c.unread.toString(), style = Type.Label.copy(letterSpacing = Type.Body.letterSpacing), color = Doors.OnAccent)
                            }
                        }
                    }
                }
            }
        }
    }
}

// ---- channels --------------------------------------------------------------

private fun LazyListScope.channels(list: List<ChannelRow>, threads: List<ConversationRow>, open: (String) -> Unit) {
    if (list.isEmpty()) {
        item { Empty("No channels read yet", "The unit's channels are listed when its mesh service answers. A channel's key never leaves the unit, so none is shown here.") }
    }
    items(list, key = { "c" + it.slot }) { c ->
        val t = threads.firstOrNull { it.last.conv == c.conv }
        Panel(Modifier.fillMaxWidth(), onClick = { open("t:" + c.conv) }) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Badge(RiftInk, glyph = GLYPH_CHANNEL)
                Spacer(Modifier.width(14.dp))
                Column(Modifier.weight(1f)) {
                    Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(10.dp)) {
                        Text(c.name.ifBlank { "Channel ${c.slot}" }, style = Type.BodyStrong, maxLines = 1, overflow = TextOverflow.Ellipsis, modifier = Modifier.weight(1f, fill = false))
                        if (c.wellKnown == "public") Tag("Public", Doors.Accent)
                    }
                    Text(
                        listOfNotNull("slot ${c.slot}", "hash ${c.hash}", c.keyBits?.let { "$it-bit" }).joinToString("  ·  "),
                        style = Type.Mono, color = Doors.Secondary, maxLines = 1,
                    )
                }
                Column(horizontalAlignment = Alignment.End) {
                    Text((t?.total ?: 0).toString(), style = Type.Mono, color = if ((t?.unread ?: 0) > 0) RiftInk else Doors.Text)
                    Label("stored", color = Doors.Faint)
                }
            }
        }
    }
    if (list.isNotEmpty()) {
        item { Text("A channel's name is the unit's own and is never on the air; the one-byte hash is. Two channels can share a hash.", style = Type.Body, color = Doors.Muted, modifier = Modifier.padding(horizontal = 4.dp, vertical = 4.dp)) }
    }
}

// ---- nodes -----------------------------------------------------------------

internal fun route(n: NodeRow): String = when {
    !n.pathKnown -> "no route"
    n.direct == true || n.hops == 0 -> "direct"
    n.hops != null -> "${n.hops} hop" + if (n.hops == 1) "" else "s"
    else -> "route known"
}

private fun LazyListScope.nodes(list: List<NodeRow>, now: Long, open: (String) -> Unit) {
    if (list.isEmpty()) {
        item { Empty("No nodes read yet", "The nodes the unit has heard are listed when its mesh service answers.") }
    }
    items(list, key = { "n" + it.publicKey }) { n ->
        val color = hue(n.publicKey)
        val fresh = n.heardAtMs != null && now - n.heardAtMs < 15 * 60_000
        Panel(Modifier.fillMaxWidth(), onClick = { open("n:" + n.publicKey) }) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Badge(color, glyph = nodeGlyph(n.type), initials = initials(n.name))
                Spacer(Modifier.width(14.dp))
                Column(Modifier.weight(1f)) {
                    Text(n.name.ifBlank { shortKey(n.publicKey) }, style = Type.BodyStrong, maxLines = 1, overflow = TextOverflow.Ellipsis)
                    Text("${nodeKind(n.type)}  ·  ${route(n)}", style = Type.Body, maxLines = 1)
                }
                Column(horizontalAlignment = Alignment.End) {
                    Row(verticalAlignment = Alignment.CenterVertically) {
                        if (fresh) {
                            Dot(Doors.Ok, 6.dp)
                            Spacer(Modifier.width(6.dp))
                        }
                        Text(if (n.heardAtMs == null) "not heard" else ago(n.heardAtMs, now), style = Type.Mono, color = if (fresh) Doors.Text else Doors.Muted)
                    }
                    Text(n.rssiDbm?.let { "%.0f dBm".format(it) } ?: shortKey(n.publicKey).take(4), style = Type.Mono, color = Doors.Faint)
                }
            }
        }
    }
}

// ---- activity --------------------------------------------------------------

private fun LazyListScope.activity(bins: List<ActivityBin>, feed: List<FeedItem>, marks: List<MarkRow>, row: UnitRow?, mesh: MeshLink, stored: Int, now: Long) {
    item {
        Section("On the air  ·  last minute") {
            Panel(Modifier.fillMaxWidth()) {
                val recent = bins.filter { now - it.atMs < SCOPE_MS }
                val types = recent.flatMap { it.byType.entries }.groupBy({ it.key }, { it.value }).mapValues { it.value.sum() }.entries.sortedByDescending { it.value }
                val most = types.firstOrNull()?.value ?: 0
                if (types.isEmpty()) {
                    Text(if (mesh.phase == MeshPhase.LIVE) "Quiet. Nothing was received in the last minute." else "Shown while the room is live.", style = Type.Body)
                }
                types.take(8).forEach { (name, n) ->
                    Row(Modifier.fillMaxWidth().padding(vertical = 6.dp), verticalAlignment = Alignment.CenterVertically) {
                        Text(name.replace('_', ' '), style = Type.Body, color = Doors.Text, modifier = Modifier.width(104.dp), maxLines = 1)
                        Meter(n.toFloat() / most, RiftInk, Modifier.weight(1f))
                        Text(n.toString(), style = Type.Mono, modifier = Modifier.width(44.dp).padding(start = 10.dp))
                    }
                }
                val ok = recent.sumOf { it.txOk }
                val failed = recent.sumOf { it.txFailed }
                val unknown = recent.sumOf { it.txUnknown }
                if (ok + failed + unknown > 0) {
                    Spacer(Modifier.height(8.dp))
                    Hairline()
                    KeyValue("Transmitted", ok.toString(), valueColor = Doors.Tx)
                    if (failed > 0) KeyValue("Not transmitted", failed.toString(), valueColor = Doors.Error)
                    if (unknown > 0) KeyValue("Outcome unknown", unknown.toString(), valueColor = Doors.Warn)
                }
            }
        }
    }
    item {
        Section("Heard while watching") {
            Panel(Modifier.fillMaxWidth()) {
                if (feed.isEmpty()) Text("Nothing yet in this session. This list is not stored.", style = Type.Body)
                feed.take(14).forEach { f ->
                    Row(Modifier.fillMaxWidth().padding(vertical = 6.dp), verticalAlignment = Alignment.CenterVertically) {
                        Dot(
                            when (f.kind) {
                                FeedKind.MESSAGE -> RiftInk
                                FeedKind.NODE -> Doors.Ok
                                FeedKind.STATE -> Doors.Accent
                                FeedKind.CONTINUITY -> Doors.Warn
                            },
                            6.dp,
                        )
                        Spacer(Modifier.width(12.dp))
                        Text(f.text, style = Type.Body, color = Doors.Text, modifier = Modifier.weight(1f), maxLines = 2, overflow = TextOverflow.Ellipsis)
                        Text(ago(f.atMs, now), style = Type.Mono, color = Doors.Muted, modifier = Modifier.padding(start = 10.dp))
                    }
                }
            }
        }
    }
    item {
        Section("Breaks in the stored history") {
            Panel(Modifier.fillMaxWidth(), tint = if (marks.isEmpty()) Doors.Surface else Doors.Warn.copy(alpha = 0.06f)) {
                if (marks.isEmpty()) {
                    Text("None recorded. Every message the gateway's journal gave this phone followed the one before it.", style = Type.Body)
                }
                marks.takeLast(6).reversed().forEachIndexed { i, m ->
                    if (i > 0) Hairline()
                    Row(Modifier.padding(vertical = 10.dp), verticalAlignment = Alignment.Top) {
                        Rupture(Doors.Warn, Modifier.padding(top = 4.dp).size(width = 22.dp, height = 14.dp))
                        Spacer(Modifier.width(12.dp))
                        Column(Modifier.weight(1f)) {
                            Row {
                                Text(markTitle(m.markKind()), style = Type.BodyStrong, modifier = Modifier.weight(1f))
                                Text(ago(m.atMs, now), style = Type.Mono, color = Doors.Muted)
                            }
                            Text(markText(m.markKind()), style = Type.Body)
                        }
                    }
                }
                if (marks.isNotEmpty()) {
                    Hairline()
                    Text("A break is never removed. No later sync can show that what was missed arrived.", style = Type.Body, color = Doors.Muted, modifier = Modifier.padding(top = 10.dp))
                }
            }
        }
    }
    item {
        Section("Sync") {
            Panel(Modifier.fillMaxWidth()) {
                KeyValue("Last sync", if (row?.syncedAtMs == null) "never" else ago(row.syncedAtMs, now).let { if (it == "now") it else "$it ago" })
                KeyValue("Journal", row?.journalId ?: "—")
                KeyValue("Mesh run", row?.runId ?: "—")
                KeyValue("Position", row?.seq?.toString() ?: "—")
                KeyValue("Stored here", "$stored messages")
                mesh.meshState?.let { KeyValue("Mesh service", it, mono = false) }
                Text("The unit holds its last 512 message events in memory and nothing on storage. This phone keeps what it was given.", style = Type.Body, color = Doors.Muted, modifier = Modifier.padding(top = 8.dp))
            }
        }
    }
}
