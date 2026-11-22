// Copyright (c) 2026 Doors authors.
// SPDX-License-Identifier: Apache-2.0
package app.doors.companion.ui

import androidx.compose.foundation.Canvas
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ColumnScope
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.defaultMinSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.geometry.CornerRadius
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.role
import androidx.compose.ui.semantics.selected
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp

/*
 * The instrument rooms (PacketShark, Vision) share a second register of the
 * Doors look: where the house pages are soft panels and a serif, an
 * instrument is a bezel with a hairline, a tracked label on its top edge and
 * monospace figures. Same tokens, tighter radius, nothing that moves unless a
 * figure changed.
 */

val BezelShape = RoundedCornerShape(14.dp)

/** A framed group of readings with its name on the top edge. */
@Composable
fun Bezel(
    title: String,
    modifier: Modifier = Modifier,
    trailing: (@Composable () -> Unit)? = null,
    content: @Composable ColumnScope.() -> Unit,
) {
    Column(
        modifier.clip(BezelShape).background(Doors.Surface.copy(alpha = 0.72f)).border(1.dp, Doors.Line.copy(alpha = 0.8f), BezelShape)
            .padding(start = 16.dp, end = 16.dp, top = 12.dp, bottom = 16.dp),
    ) {
        Row(Modifier.fillMaxWidth().defaultMinSize(minHeight = 22.dp), verticalAlignment = Alignment.CenterVertically) {
            Label(title, Modifier.weight(1f))
            trailing?.invoke()
        }
        Spacer(Modifier.height(12.dp))
        content()
    }
}

/** A small reading: its name above, the figure below. */
@Composable
fun Readout(label: String, value: String, modifier: Modifier = Modifier, unit: String = "", color: Color = Doors.Text) {
    Column(modifier) {
        Label(label)
        Spacer(Modifier.height(5.dp))
        Row(verticalAlignment = Alignment.Bottom) {
            Text(value, style = Type.Reading, color = color, maxLines = 1, overflow = TextOverflow.Ellipsis)
            if (unit.isNotEmpty()) Text(" $unit", style = Type.MonoSmall, modifier = Modifier.padding(bottom = 3.dp), maxLines = 1)
        }
    }
}

/** A row of readouts with hairlines between them, as on the unit's SESSION strip. */
@Composable
fun ReadoutRow(modifier: Modifier = Modifier, vararg cells: @Composable (Modifier) -> Unit) {
    Row(modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {
        cells.forEachIndexed { i, cell ->
            if (i > 0) {
                Box(Modifier.width(1.dp).height(34.dp).background(Doors.Line.copy(alpha = 0.6f)))
                Spacer(Modifier.width(12.dp))
            }
            cell(Modifier.weight(1f))
        }
    }
}

/** Two or three choices of which one is always taken. Each is a full touch target. */
@Composable
fun <T> Segmented(options: List<Pair<T, String>>, selected: T, modifier: Modifier = Modifier, color: Color = Doors.Accent, onSelect: (T) -> Unit) {
    val shape = RoundedCornerShape(10.dp)
    Row(modifier.clip(shape).border(1.dp, Doors.Line, shape)) {
        options.forEach { (value, text) ->
            val on = value == selected
            Box(
                Modifier.defaultMinSize(minWidth = 52.dp, minHeight = 44.dp)
                    .background(if (on) color.copy(alpha = 0.14f) else Color.Transparent)
                    .clickable(role = Role.Tab) { onSelect(value) }
                    .semantics { this.selected = on }
                    .padding(horizontal = 12.dp),
                contentAlignment = Alignment.Center,
            ) { Text(text.uppercase(), style = Type.Label, color = if (on) color else Doors.Muted) }
        }
    }
}

/** An instrument's key: outlined, or filled when it is the one thing to do next. */
@Composable
fun InstrumentKey(
    text: String,
    modifier: Modifier = Modifier,
    color: Color = Doors.Text,
    filled: Boolean = false,
    enabled: Boolean = true,
    onClick: () -> Unit,
) {
    val shape = RoundedCornerShape(12.dp)
    val ink = if (!enabled) Doors.Faint else if (filled) Doors.OnAccent else color
    Box(
        modifier.height(54.dp).clip(shape)
            .background(if (filled && enabled) color else Doors.Surface)
            .border(1.dp, if (enabled && !filled) color.copy(alpha = 0.45f) else Doors.Line, shape)
            .clickable(enabled = enabled, role = Role.Button, onClick = onClick),
        contentAlignment = Alignment.Center,
    ) { Text(text.uppercase(), style = Type.Button, color = ink, maxLines = 1, overflow = TextOverflow.Ellipsis) }
}

/**
 * The mark of figures that were made on the phone. It is drawn across the
 * whole width, hatched, and is never a small tag: a simulated screen must not
 * be mistaken for a unit's at a glance or in a screenshot.
 */
@Composable
fun SimulatedBand(what: String, modifier: Modifier = Modifier, compact: Boolean = false, onLeave: (() -> Unit)? = null) {
    val shape = RoundedCornerShape(12.dp)
    Row(
        modifier.fillMaxWidth().clip(shape).background(Doors.Warn.copy(alpha = 0.08f)).border(1.dp, Doors.Warn.copy(alpha = 0.55f), shape)
            .semantics { contentDescription = "Simulated. $what" },
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Canvas(Modifier.width(26.dp).height(if (compact) 44.dp else 56.dp)) {
            // Hazard hatching: the same in every simulated room.
            val step = 9.dp.toPx()
            var y = -size.width
            while (y < size.height + size.width) {
                drawLine(Doors.Warn.copy(alpha = 0.7f), Offset(0f, y + size.width), Offset(size.width, y), 3.dp.toPx())
                y += step
            }
        }
        val words = Type.Body.copy(fontSize = Type.Mono.fontSize, lineHeight = Type.Mono.lineHeight)
        if (compact) {
            // One line, for a screen that has width and no height.
            Row(Modifier.weight(1f).padding(horizontal = 12.dp), verticalAlignment = Alignment.CenterVertically) {
                Text("SIMULATED", style = Type.Label, color = Doors.Warn)
                Spacer(Modifier.width(12.dp))
                Text(what, style = words, color = Doors.Text, maxLines = 1, overflow = TextOverflow.Ellipsis)
            }
        } else {
            Column(Modifier.weight(1f).padding(horizontal = 12.dp, vertical = 8.dp)) {
                Text("SIMULATED", style = Type.Label, color = Doors.Warn)
                Text(what, style = words, color = Doors.Text)
            }
        }
        if (onLeave != null) {
            Box(Modifier.defaultMinSize(minWidth = 64.dp, minHeight = if (compact) 44.dp else 56.dp).clickable(role = Role.Button, onClick = onLeave), contentAlignment = Alignment.Center) {
                Text("LEAVE", style = Type.Label, color = Doors.Warn)
            }
        }
    }
}

/** Parts of a whole on one line, in the order given. Parts too small to see still get a sliver. */
@Composable
fun ShareBar(parts: List<Pair<Long, Color>>, modifier: Modifier = Modifier, height: Dp = 8.dp) {
    Canvas(modifier.fillMaxWidth().height(height)) {
        val total = parts.sumOf { it.first }
        val r = CornerRadius(size.height / 2)
        drawRoundRect(Doors.Line.copy(alpha = 0.7f), size = size, cornerRadius = r)
        if (total <= 0) return@Canvas
        val gap = 1.5.dp.toPx()
        val live = parts.filter { it.first > 0 }
        val room = size.width - gap * (live.size - 1)
        var x = 0f
        live.forEach { (n, c) ->
            val w = (room * n / total).coerceAtLeast(2.dp.toPx())
            drawRect(c, topLeft = Offset(x, 0f), size = Size(w.coerceAtMost(size.width - x).coerceAtLeast(0f), size.height))
            x += w + gap
        }
    }
}

/** A line of small values over time, newest at the right; for a rate that only needs its shape. */
@Composable
fun Sparkline(values: List<Float>, color: Color, modifier: Modifier = Modifier, slots: Int = 60) {
    Canvas(modifier) {
        if (values.size < 2) {
            drawLine(Doors.Line, Offset(0f, size.height - 1f), Offset(size.width, size.height - 1f), 1.dp.toPx())
            return@Canvas
        }
        val top = (values.max()).coerceAtLeast(1e-6f)
        val step = size.width / (slots - 1).coerceAtLeast(1)
        val shown = values.takeLast(slots)
        val x0 = size.width - step * (shown.size - 1)
        val path = androidx.compose.ui.graphics.Path()
        shown.forEachIndexed { i, v ->
            val x = x0 + i * step
            val y = size.height - (v / top).coerceIn(0f, 1f) * (size.height - 2f) - 1f
            if (i == 0) path.moveTo(x, y) else path.lineTo(x, y)
        }
        drawPath(path, color, style = androidx.compose.ui.graphics.drawscope.Stroke(1.6.dp.toPx(), cap = androidx.compose.ui.graphics.StrokeCap.Round, join = androidx.compose.ui.graphics.StrokeJoin.Round))
    }
}

/** A statement in place of a control, or under one: why something is not there. */
@Composable
fun Statement(text: String, modifier: Modifier = Modifier, color: Color = Doors.Muted) {
    Row(modifier.fillMaxWidth(), verticalAlignment = Alignment.Top, horizontalArrangement = Arrangement.spacedBy(10.dp)) {
        Box(Modifier.padding(top = 7.dp)) { Dot(color, 6.dp) }
        Text(text, style = Type.Body, color = Doors.Secondary, modifier = Modifier.weight(1f))
    }
}
