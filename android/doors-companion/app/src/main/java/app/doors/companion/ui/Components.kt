// Copyright (c) 2026 Doors authors.
// SPDX-License-Identifier: Apache-2.0
package app.doors.companion.ui

import androidx.compose.animation.core.FastOutSlowInEasing
import androidx.compose.animation.core.RepeatMode
import androidx.compose.animation.core.animateFloat
import androidx.compose.animation.core.animateFloatAsState
import androidx.compose.animation.core.infiniteRepeatable
import androidx.compose.animation.core.rememberInfiniteTransition
import androidx.compose.animation.core.spring
import androidx.compose.animation.core.tween
import androidx.compose.animation.animateColorAsState
import androidx.compose.foundation.BorderStroke
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.interaction.MutableInteractionSource
import androidx.compose.foundation.interaction.collectIsPressedAsState
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ColumnScope
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.remember
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.draw.scale
import androidx.compose.ui.geometry.CornerRadius
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.Path
import androidx.compose.ui.graphics.StrokeCap
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp

val PanelShape = RoundedCornerShape(22.dp)

@Composable
fun Panel(
    modifier: Modifier = Modifier,
    onClick: (() -> Unit)? = null,
    tint: Color = Doors.Surface,
    content: @Composable ColumnScope.() -> Unit,
) {
    val source = remember { MutableInteractionSource() }
    val pressed by source.collectIsPressedAsState()
    val scale by animateFloatAsState(if (pressed) 0.975f else 1f, spring(stiffness = 600f), label = "press")
    var m = modifier.scale(scale).clip(PanelShape).background(tint).border(1.dp, Doors.Line.copy(alpha = 0.7f), PanelShape)
    if (onClick != null) m = m.clickable(interactionSource = source, indication = null, onClick = onClick)
    Column(m.padding(18.dp), content = content)
}

@Composable
fun Label(text: String, modifier: Modifier = Modifier, color: Color = Doors.Muted) {
    Text(text.uppercase(), modifier = modifier, style = Type.Label, color = color, maxLines = 1, overflow = TextOverflow.Ellipsis)
}

@Composable
fun Dot(color: Color, size: Dp = 8.dp, pulse: Boolean = false) {
    val alpha = if (pulse) {
        val t = rememberInfiniteTransition(label = "dot")
        t.animateFloat(1f, 0.25f, infiniteRepeatable(tween(900, easing = FastOutSlowInEasing), RepeatMode.Reverse), label = "a").value
    } else 1f
    Box(Modifier.size(size).clip(CircleShape).background(color.copy(alpha = alpha)))
}

@Composable
fun Pill(text: String, color: Color, pulse: Boolean = false, modifier: Modifier = Modifier) {
    Row(
        modifier.clip(CircleShape).background(color.copy(alpha = 0.10f)).border(1.dp, color.copy(alpha = 0.35f), CircleShape)
            .padding(horizontal = 12.dp, vertical = 7.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Dot(color, pulse = pulse)
        Spacer(Modifier.width(8.dp))
        Text(text.uppercase(), style = Type.Label, color = color)
    }
}

@Composable
fun Tag(text: String, color: Color = Doors.Muted) {
    Text(
        text.uppercase(), style = Type.Label, color = color,
        modifier = Modifier.clip(RoundedCornerShape(6.dp)).border(1.dp, color.copy(alpha = 0.4f), RoundedCornerShape(6.dp)).padding(horizontal = 7.dp, vertical = 3.dp),
    )
}

@Composable
fun PrimaryButton(text: String, modifier: Modifier = Modifier, enabled: Boolean = true, onClick: () -> Unit) {
    val bg by animateColorAsState(if (enabled) Doors.Accent else Doors.Raised, label = "btn")
    Box(
        modifier.fillMaxWidth().height(54.dp).clip(RoundedCornerShape(16.dp)).background(bg).clickable(enabled = enabled, onClick = onClick),
        contentAlignment = Alignment.Center,
    ) { Text(text.uppercase(), style = Type.Button, color = if (enabled) Doors.OnAccent else Doors.Faint) }
}

@Composable
fun GhostButton(text: String, modifier: Modifier = Modifier, color: Color = Doors.Text, onClick: () -> Unit) {
    Box(
        modifier.fillMaxWidth().height(54.dp).clip(RoundedCornerShape(16.dp))
            .border(BorderStroke(1.dp, Doors.Line), RoundedCornerShape(16.dp)).clickable(onClick = onClick),
        contentAlignment = Alignment.Center,
    ) { Text(text.uppercase(), style = Type.Button, color = color) }
}

@Composable
fun KeyValue(key: String, value: String, mono: Boolean = true, valueColor: Color = Doors.Text) {
    Row(Modifier.fillMaxWidth().padding(vertical = 7.dp), verticalAlignment = Alignment.Top) {
        Text(key, style = Type.Body, modifier = Modifier.width(128.dp))
        Text(value, style = if (mono) Type.Mono else Type.BodyStrong, color = valueColor, modifier = Modifier.weight(1f))
    }
}

@Composable
fun Hairline() {
    Box(Modifier.fillMaxWidth().height(1.dp).background(Doors.Line.copy(alpha = 0.6f)))
}

/** A thin bar for a share between 0 and 1; null draws the empty track. */
@Composable
fun Meter(fraction: Float?, color: Color, modifier: Modifier = Modifier) {
    val f by animateFloatAsState((fraction ?: 0f).coerceIn(0f, 1f), tween(700, easing = FastOutSlowInEasing), label = "meter")
    Canvas(modifier.fillMaxWidth().height(4.dp)) {
        val r = CornerRadius(size.height / 2)
        drawRoundRect(Doors.Line.copy(alpha = 0.7f), size = size, cornerRadius = r)
        if (f > 0f) drawRoundRect(color, size = Size(size.width * f, size.height), cornerRadius = r)
    }
}

@Composable
fun SignalBars(bars: Int?, color: Color = Doors.Rx) {
    Canvas(Modifier.size(width = 26.dp, height = 18.dp)) {
        val w = size.width / 7f
        for (i in 0 until 4) {
            val h = size.height * (0.35f + 0.65f * i / 3f)
            drawRoundRect(
                if (bars != null && i < bars) color else Doors.Line,
                topLeft = Offset(i * 2 * w, size.height - h), size = Size(w, h), cornerRadius = CornerRadius(w / 2),
            )
        }
    }
}

/**
 * The threshold: the app's one image. An arched doorway whose leaf stands
 * open by [open] (0 shut, 1 wide), with the room's light falling across the
 * sill. Shut and dark while the unit is away; ajar and breathing while the
 * app is knocking; open and lit when the unit has let it in.
 */
@Composable
fun Threshold(open: Float, lit: Color, modifier: Modifier = Modifier, searching: Boolean = false) {
    val target by animateFloatAsState(open, spring(dampingRatio = 0.72f, stiffness = 60f), label = "door")
    val breath = if (searching) {
        val t = rememberInfiniteTransition(label = "knock")
        t.animateFloat(0f, 0.16f, infiniteRepeatable(tween(1400, easing = FastOutSlowInEasing), RepeatMode.Reverse), label = "b").value
    } else 0f
    val light by animateColorAsState(lit, tween(900), label = "light")
    Canvas(modifier) {
        val o = (target + breath).coerceIn(0f, 1f)
        val h = size.height
        val doorH = h * 0.78f
        val doorW = doorH * 0.52f
        val left = (size.width - doorW) / 2f
        val right = left + doorW
        val top = h * 0.06f
        val sill = top + doorH
        val arch = doorW / 2f

        fun frame(inset: Float) = Path().apply {
            moveTo(left + inset, sill)
            lineTo(left + inset, top + arch)
            arcTo(androidx.compose.ui.geometry.Rect(left + inset, top + inset, right - inset, top + doorW - inset), 180f, 180f, false)
            lineTo(right - inset, sill)
        }

        // The room beyond: light gathers at the opening's hinge side and fades.
        val opening = frame(0f).apply { close() }
        drawPath(opening, Doors.Surface)
        if (o > 0.01f) {
            drawPath(
                opening,
                Brush.horizontalGradient(
                    0f to light.copy(alpha = 0.05f * o), 0.55f to light.copy(alpha = 0.38f * o), 1f to light.copy(alpha = 0.85f * o),
                    startX = left, endX = right,
                ),
            )
            // Light across the sill, wider the further the leaf stands open.
            val spill = Path().apply {
                moveTo(left + doorW * (1f - o), sill)
                lineTo(right, sill)
                lineTo(right + doorW * 0.9f * o, h)
                lineTo(left - doorW * 0.5f * o, h)
                close()
            }
            drawPath(spill, Brush.verticalGradient(0f to light.copy(alpha = 0.30f * o), 1f to Color.Transparent, startY = sill, endY = h))
        }
        // The leaf: hinged on the left, foreshortening as it swings in.
        val leafW = doorW * (1f - 0.86f * o)
        val leaf = Path().apply {
            moveTo(left, sill)
            lineTo(left, top + arch)
            arcTo(androidx.compose.ui.geometry.Rect(left, top, left + leafW * 2f, top + doorW), 180f, 90f, false)
            lineTo(left + leafW, sill + doorH * 0.035f * o)
            close()
        }
        drawPath(leaf, Brush.horizontalGradient(0f to Doors.Raised, 1f to Doors.Surface, startX = left, endX = left + leafW.coerceAtLeast(1f)))
        drawPath(leaf, Doors.Line, style = Stroke(1.dp.toPx()))
        // The handle.
        drawCircle(light.copy(alpha = 0.5f + 0.5f * o), 2.6.dp.toPx(), Offset(left + leafW * 0.84f, top + doorH * 0.58f))
        // The frame and the floor line.
        drawPath(frame(0f), Doors.Accent.copy(alpha = 0.55f + 0.45f * o), style = Stroke(1.6.dp.toPx(), cap = StrokeCap.Round))
        drawLine(Doors.Line, Offset(left - doorW * 0.9f, sill), Offset(right + doorW * 0.9f, sill), 1.dp.toPx(), StrokeCap.Round)
    }
}

/** A drawn chevron; the app ships no icon font. */
@Composable
fun Chevron(back: Boolean = false, color: Color = Doors.Muted, size: Dp = 14.dp) {
    Canvas(Modifier.size(size)) {
        val w = this.size.width
        val x0 = if (back) w * 0.65f else w * 0.35f
        val x1 = if (back) w * 0.3f else w * 0.7f
        val p = Path().apply { moveTo(x0, w * 0.15f); lineTo(x1, w * 0.5f); lineTo(x0, w * 0.85f) }
        drawPath(p, color, style = Stroke(1.8.dp.toPx(), cap = StrokeCap.Round))
    }
}

@Composable
fun TopBar(title: String, onBack: () -> Unit) {
    Row(Modifier.fillMaxWidth().padding(top = 8.dp, bottom = 20.dp), verticalAlignment = Alignment.CenterVertically) {
        Box(
            Modifier.size(44.dp).clip(RoundedCornerShape(14.dp)).background(Doors.Surface).border(1.dp, Doors.Line.copy(alpha = 0.7f), RoundedCornerShape(14.dp)).clickable(onClick = onBack),
            contentAlignment = Alignment.Center,
        ) { Chevron(back = true, color = Doors.Accent) }
        Spacer(Modifier.width(16.dp))
        Text(title, style = Type.Title)
    }
}

@Composable
fun Section(title: String, content: @Composable ColumnScope.() -> Unit) {
    Column(Modifier.fillMaxWidth(), verticalArrangement = Arrangement.spacedBy(10.dp)) {
        Label(title, Modifier.padding(start = 4.dp, top = 14.dp))
        content()
    }
}
