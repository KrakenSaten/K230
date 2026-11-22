// Copyright (c) 2026 Doors authors.
// SPDX-License-Identifier: Apache-2.0
package app.doors.companion.ui

import androidx.compose.foundation.Canvas
import androidx.compose.foundation.gestures.detectHorizontalDragGestures
import androidx.compose.foundation.gestures.detectTapGestures
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.rememberUpdatedState
import androidx.compose.ui.Modifier
import androidx.compose.ui.geometry.CornerRadius
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.PathEffect
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.drawText
import androidx.compose.ui.text.rememberTextMeasurer
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp
import app.doors.companion.core.CaptureFeed
import app.doors.companion.core.CaptureSample
import app.doors.companion.core.TrafficClass
import kotlin.math.ceil
import kotlin.math.log10
import kotlin.math.pow

/**
 * The class hues of PacketShark on the unit (DS section 59.2, the identity
 * hues of pos_theme.c): a class has the same colour on the phone as on the
 * panel. Indexed by [TrafficClass.ordinal].
 */
val ClassHue: List<Color> = listOf(
    Color(0xFFB7A8F2), // TCP
    Color(0xFF86BFF0), // UDP
    Color(0xFF74D1C4), // DNS
    Color(0xFFE8AC6A), // DHCP
    Color(0xFFD9CB6E), // ARP
    Color(0xFFE89DD2), // ICMP
    Color(0xFF9FD08A), // IPv6
    Color(0xFFA8A399), // other
)

enum class GraphMode { PACKETS, BITS }

/** The scale of a graph: whole decades from [lo] up to the first one at or above the peak. */
class LogScale(peak: Long, val lo: Long) {
    val decades: Int = ceil(log10(peak.coerceAtLeast(lo * 100).toDouble()) - log10(lo.toDouble())).toInt().coerceAtLeast(2)
    val hi: Long = lo * 10.0.pow(decades).toLong()

    /** Where a value stands between the floor (0) and the top (1). Anything at all is at least visible. */
    fun at(v: Long): Float = when {
        v <= 0 -> 0f
        v <= lo -> 0.012f
        else -> ((log10(v.toDouble()) - log10(lo.toDouble())) / decades).toFloat().coerceIn(0.012f, 1f)
    }

    /** The values of the lines between floor and top, lowest first. */
    fun lines(): List<Long> = (1..decades).map { lo * 10.0.pow(it).toLong() }
}

/**
 * Traffic over the last two minutes, a bar a second, newest at the right, on
 * a logarithmic scale as on the unit: a quiet link and a busy one are both
 * readable. In packets a bar is divided by class; in bits it is one colour,
 * because the unit reports bytes for the whole capture and not per class.
 * A tap or a drag along it picks a second to read.
 */
@Composable
fun TrafficGraph(
    history: List<CaptureSample>,
    mode: GraphMode,
    selected: Long?,
    onSelect: (Long?) -> Unit,
    modifier: Modifier = Modifier,
    height: Dp = 168.dp,
) {
    val measurer = rememberTextMeasurer()
    val current by rememberUpdatedState(history)
    val chosen by rememberUpdatedState(selected)
    val pick by rememberUpdatedState(onSelect)
    val gutter = 40.dp
    val slots = CaptureFeed.KEPT
    val peak = history.maxOfOrNull { if (mode == GraphMode.PACKETS) it.total() else it.bps } ?: 0
    val scale = LogScale(peak, if (mode == GraphMode.PACKETS) 1 else 1_000)

    fun sampleAt(x: Float, width: Float, gutterPx: Float): CaptureSample? {
        val slotW = (width - gutterPx) / slots
        val i = (x / slotW).toInt() - (slots - current.size)
        return current.getOrNull(i)
    }

    Column(modifier) {
        Canvas(
            Modifier.fillMaxWidth().height(height)
                .semantics {
                    contentDescription = if (history.isEmpty()) "Traffic graph, no readings yet"
                    else "Traffic graph, last ${history.size} seconds, peak ${Fmt.short(peak)} ${if (mode == GraphMode.PACKETS) "packets" else "bits"} per second"
                }
                .pointerInput(Unit) {
                    detectTapGestures { p ->
                        val s = sampleAt(p.x, size.width.toFloat(), gutter.toPx())
                        pick(if (s == null || s.elapsedMs == chosen) null else s.elapsedMs)
                    }
                }
                .pointerInput(Unit) {
                    detectHorizontalDragGestures { change, _ ->
                        sampleAt(change.position.x, size.width.toFloat(), gutter.toPx())?.let { pick(it.elapsedMs) }
                    }
                },
        ) {
            val plotW = size.width - gutter.toPx()
            val h = size.height
            val slotW = plotW / slots
            val barW = if (slotW >= 3f) slotW - 1f else slotW
            val dash = PathEffect.dashPathEffect(floatArrayOf(2.dp.toPx(), 4.dp.toPx()))

            // The decades, and their names in the gutter.
            scale.lines().forEach { v ->
                val y = h * (1f - scale.at(v))
                drawLine(Doors.Line.copy(alpha = 0.55f), Offset(0f, y), Offset(plotW, y), 1f, pathEffect = if (v == scale.hi) null else dash)
                val text = measurer.measure(Fmt.short(v), Type.MonoSmall)
                drawText(text, topLeft = Offset(size.width - text.size.width, (y - text.size.height / 2f).coerceIn(0f, h - text.size.height)))
            }
            drawLine(Doors.Line, Offset(0f, h - 0.5f), Offset(plotW, h - 0.5f), 1f)

            history.forEachIndexed { i, s ->
                val x = (slots - history.size + i) * slotW
                val total = if (mode == GraphMode.PACKETS) s.total() else s.bps
                val barH = h * scale.at(total)
                if (barH > 0f) {
                    val classed = s.classes.sum()
                    if (mode == GraphMode.BITS || classed <= 0) {
                        drawRect(if (mode == GraphMode.BITS) Doors.Tx else ClassHue.last(), Offset(x, h - barH), Size(barW, barH))
                    } else {
                        var y = h
                        s.classes.forEachIndexed { c, n ->
                            if (n > 0) {
                                val part = barH * n / classed
                                y -= part
                                drawRect(ClassHue[c], Offset(x, y), Size(barW, part))
                            }
                        }
                    }
                }
                // A second in which frames were lost is marked where it cannot be missed.
                if (s.drops > 0) drawRect(Doors.Error, Offset(x, 0f), Size(barW.coerceAtLeast(2f), 3.dp.toPx()))
                if (s.elapsedMs == selected) {
                    val top = (h - barH - 4.dp.toPx()).coerceAtLeast(0f)
                    drawRoundRect(
                        Doors.Accent, Offset(x - 2.dp.toPx(), top), Size(barW + 4.dp.toPx(), h - top),
                        CornerRadius(2.dp.toPx()), style = Stroke(1.5.dp.toPx()),
                    )
                }
            }
        }
        Spacer(Modifier.height(6.dp))
        Row(Modifier.fillMaxWidth().padding(end = gutter)) {
            Text("-${slots} S", style = Type.MonoSmall, modifier = Modifier.weight(1f))
            Text("NOW", style = Type.MonoSmall)
        }
    }
}

/** Frames in the second: by class when the classes are known, else the unit's own rate. */
fun CaptureSample.total(): Long = classes.sum().takeIf { it > 0 } ?: pps

/** The class that led a second, and its share, as "TCP 71%"; null for an empty second. */
fun CaptureSample.lead(): String? {
    val sum = classes.sum()
    if (sum <= 0) return null
    val i = classes.indices.maxBy { classes[it] }
    return "${TrafficClass.entries[i].word} ${classes[i] * 100 / sum}%"
}
