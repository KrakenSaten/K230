// Copyright (c) 2026 Doors authors.
// SPDX-License-Identifier: Apache-2.0
package app.doors.companion.ui

import android.graphics.Bitmap
import android.graphics.BitmapFactory
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.Image
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.runtime.Composable
import androidx.compose.runtime.State
import androidx.compose.runtime.produceState
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clipToBounds
import androidx.compose.ui.geometry.CornerRadius
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.FilterQuality
import androidx.compose.ui.graphics.ImageBitmap
import androidx.compose.ui.graphics.Path
import androidx.compose.ui.graphics.StrokeCap
import androidx.compose.ui.graphics.asImageBitmap
import androidx.compose.ui.graphics.drawscope.DrawScope
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.TextMeasurer
import androidx.compose.ui.text.drawText
import androidx.compose.ui.text.rememberTextMeasurer
import androidx.compose.ui.unit.dp
import androidx.lifecycle.Lifecycle
import androidx.lifecycle.compose.LocalLifecycleOwner
import androidx.lifecycle.repeatOnLifecycle
import app.doors.companion.core.FramePixels
import app.doors.companion.core.FrameSlot
import app.doors.companion.core.SimulatedVision
import app.doors.companion.core.VisionBox
import app.doors.companion.core.VisionFrame
import app.doors.companion.core.VisionLimits
import app.doors.companion.core.VisionMode
import app.doors.companion.core.VisionScene
import app.doors.companion.core.VisionSource
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.channels.Channel
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import kotlin.math.max
import kotlin.math.min

/*
 * From a picture that arrived to a picture on the glass. Three parts, each
 * replaceable without the others knowing:
 *
 *   VisionSource.frame  ->  FrameSlot (one picture, newest wins)
 *                       ->  FrameRenderer (decode off the main thread, two bitmaps reused)
 *                       ->  PreviewImage + BoxOverlay (one transform for pixels and boxes)
 *
 * Nothing here knows how a picture travelled; a real transport changes none of it.
 */

/** How a picture of [w] by [h] sits in a space: the one transform used for the pixels and for the boxes over them. */
class FitTransform(val scale: Float, val dx: Float, val dy: Float) {
    fun x(px: Float): Float = dx + px * scale
    fun y(py: Float): Float = dy + py * scale

    companion object {
        /** Whole picture visible ([fill] false, bars where the shapes differ) or the space covered ([fill] true, edges cut). */
        fun of(w: Int, h: Int, spaceW: Float, spaceH: Float, fill: Boolean): FitTransform {
            if (w <= 0 || h <= 0 || spaceW <= 0f || spaceH <= 0f) return FitTransform(0f, 0f, 0f)
            val sx = spaceW / w
            val sy = spaceH / h
            val s = if (fill) max(sx, sy) else min(sx, sy)
            return FitTransform(s, (spaceW - w * s) / 2f, (spaceH - h * s) / 2f)
        }
    }
}

/** A picture ready to draw, with the scene that belongs to it. */
class Shown(val image: ImageBitmap, val w: Int, val h: Int, val seq: Long, val scene: VisionScene, val skipped: Long)

/**
 * Makes bitmaps of frames. Two bitmaps of the picture's size are kept and
 * used in turn, so the one on screen is never written while it is drawn and
 * nothing is allocated per frame. A picture larger than the preview's bounds
 * is refused before it is decoded.
 */
class FrameRenderer {
    private val buffers = arrayOfNulls<Bitmap>(2)
    private var next = 0

    fun render(f: VisionFrame): Bitmap? {
        if (f.w !in 1..VisionLimits.MAX_SIDE || f.h !in 1..VisionLimits.MAX_SIDE) return null
        val target = buffer(f.w, f.h)
        return when (val p = f.pixels) {
            is FramePixels.Argb -> {
                if (p.pixels.size < f.w * f.h) return null
                target.setPixels(p.pixels, 0, f.w, 0, 0, f.w, f.h)
                target
            }
            is FramePixels.Jpeg -> {
                if (p.bytes.size > VisionLimits.MAX_FRAME_BYTES) return null
                val bounds = BitmapFactory.Options().apply { inJustDecodeBounds = true }
                BitmapFactory.decodeByteArray(p.bytes, 0, p.bytes.size, bounds)
                // The picture must be what its frame said it is; a file that claims another size is not shown.
                if (bounds.outWidth != f.w || bounds.outHeight != f.h) return null
                val into = BitmapFactory.Options().apply {
                    inMutable = true
                    inBitmap = target
                    inSampleSize = 1
                }
                try {
                    BitmapFactory.decodeByteArray(p.bytes, 0, p.bytes.size, into)
                } catch (_: IllegalArgumentException) {
                    null
                }
            }
        }
    }

    private fun buffer(w: Int, h: Int): Bitmap {
        next = (next + 1) % buffers.size
        val have = buffers[next]
        if (have != null && have.width == w && have.height == h && !have.isRecycled) return have
        have?.recycle()
        return Bitmap.createBitmap(w, h, Bitmap.Config.ARGB_8888).also { buffers[next] = it }
    }

    fun release() {
        for (i in buffers.indices) {
            buffers[i]?.recycle()
            buffers[i] = null
        }
    }
}

/**
 * The preview's controller: follows [source] only while the screen is at
 * least started, holds one pending picture, decodes on a background thread,
 * and gives the newest finished one. When pictures stop, or the screen goes,
 * what was shown is let go: no last frame lingers as if it were live.
 */
@Composable
fun rememberPreview(source: VisionSource): State<Shown?> {
    val owner = LocalLifecycleOwner.current
    return produceState<Shown?>(null, source, owner) {
        val renderer = FrameRenderer()
        try {
            owner.lifecycle.repeatOnLifecycle(Lifecycle.State.STARTED) {
                val slot = FrameSlot()
                val wake = Channel<Unit>(Channel.CONFLATED)
                launch {
                    source.frame.collect { f ->
                        if (f == null) {
                            slot.clear()
                            value = null
                        } else {
                            slot.put(f)
                            wake.trySend(Unit)
                        }
                    }
                }
                for (unused in wake) {
                    val f = slot.take() ?: continue
                    val bitmap = withContext(Dispatchers.Default) { renderer.render(f) } ?: continue
                    // Pictures may have ended while this one was being made.
                    if (source.frame.value != null) value = Shown(bitmap.asImageBitmap(), f.w, f.h, f.seq, f.scene, slot.skipped)
                }
            }
        } finally {
            value = null
            renderer.release()
        }
    }
}

/** The picture and its boxes, placed by one transform. */
@Composable
fun PreviewImage(shown: Shown, mode: VisionMode?, fill: Boolean, labels: Boolean, modifier: Modifier = Modifier) {
    val measurer = rememberTextMeasurer()
    Box(modifier.clipToBounds().semantics { contentDescription = "Preview picture, ${shown.scene.boxes.size} objects marked" }, contentAlignment = Alignment.Center) {
        Image(
            shown.image, null, Modifier.fillMaxSize(),
            contentScale = if (fill) ContentScale.Crop else ContentScale.Fit,
            // The picture is small and shown large: smoothing reads better than blocks.
            filterQuality = FilterQuality.Medium,
        )
        Canvas(Modifier.fillMaxSize()) {
            val scene = shown.scene
            // Boxes are in the pixels of the picture they came with; when that is another size, in proportion.
            val t = FitTransform.of(shown.w, shown.h, size.width, size.height, fill)
            val kx = if (scene.w > 0) shown.w.toFloat() / scene.w else 1f
            val ky = if (scene.h > 0) shown.h.toFloat() / scene.h else 1f
            scene.boxes.forEach { b -> drawBox(b, t, kx, ky, mode, labels, measurer) }
        }
    }
}

/** A track keeps its colour; a box without an id takes its class's. */
fun boxColor(b: VisionBox): Color = Color(SimulatedVision.TRACK_HUE[(if (b.id > 0) b.id else b.cls + 3).mod(SimulatedVision.TRACK_HUE.size)])

private fun DrawScope.drawBox(b: VisionBox, t: FitTransform, kx: Float, ky: Float, mode: VisionMode?, labels: Boolean, measurer: TextMeasurer) {
    val left = t.x(b.x * kx)
    val top = t.y(b.y * ky)
    val w = b.w * kx * t.scale
    val h = b.h * ky * t.scale
    if (w < 2f || h < 2f) return
    val color = boxColor(b)
    val thin = 1.dp.toPx()
    val thick = 2.5.dp.toPx()
    // A light outline, and heavy corners that stay readable over any picture.
    drawRect(Color.Black.copy(alpha = 0.35f), Offset(left - thin, top - thin), Size(w + 2 * thin, h + 2 * thin), style = Stroke(thin))
    drawRect(color.copy(alpha = 0.55f), Offset(left, top), Size(w, h), style = Stroke(thin))
    val c = min(min(w, h) * 0.3f, 14.dp.toPx())
    val corners = Path().apply {
        moveTo(left, top + c); lineTo(left, top); lineTo(left + c, top)
        moveTo(left + w - c, top); lineTo(left + w, top); lineTo(left + w, top + c)
        moveTo(left + w, top + h - c); lineTo(left + w, top + h); lineTo(left + w - c, top + h)
        moveTo(left + c, top + h); lineTo(left, top + h); lineTo(left, top + h - c)
    }
    drawPath(corners, color, style = Stroke(thick, cap = StrokeCap.Square))
    if (!labels) return

    val words = buildList {
        if (b.id > 0 && mode?.ids != false) add("#${b.id}")
        add(b.label.ifEmpty { "class ${b.cls}" })
        add("${(b.confidence + 5) / 10}%")
        if (b.kmh10 > 0) add("${(b.kmh10 + 5) / 10} km/h")
    }.joinToString("  ")
    val text = measurer.measure(words, Type.MonoSmall.copy(color = Doors.OnAccent), maxLines = 1)
    val padX = 5.dp.toPx()
    val tagW = text.size.width + 2 * padX
    val tagH = text.size.height + 3.dp.toPx()
    // Above the box; inside its top when there is no room above; never off the side.
    val tagTop = if (top - tagH >= 0f) top - tagH else top
    val tagLeft = left.coerceIn(0f, max(0f, size.width - tagW))
    drawRoundRect(color, Offset(tagLeft, tagTop), Size(tagW, tagH), CornerRadius(3.dp.toPx()))
    drawText(text, topLeft = Offset(tagLeft + padX, tagTop + 1.5.dp.toPx()))

    if (b.dir in 1..4) {
        // Which way it is going, as a small arrowhead on that side of the box.
        val cx = left + w / 2
        val cy = top + h / 2
        val a = 6.dp.toPx()
        val tip = when (b.dir) {
            1 -> Offset(left - a * 1.6f, cy)
            2 -> Offset(left + w + a * 1.6f, cy)
            3 -> Offset(cx, top - a * 1.6f)
            else -> Offset(cx, top + h + a * 1.6f)
        }
        val head = Path().apply {
            when (b.dir) {
                1 -> { moveTo(tip.x, tip.y); lineTo(tip.x + a, tip.y - a); lineTo(tip.x + a, tip.y + a) }
                2 -> { moveTo(tip.x, tip.y); lineTo(tip.x - a, tip.y - a); lineTo(tip.x - a, tip.y + a) }
                3 -> { moveTo(tip.x, tip.y); lineTo(tip.x - a, tip.y + a); lineTo(tip.x + a, tip.y + a) }
                else -> { moveTo(tip.x, tip.y); lineTo(tip.x - a, tip.y - a); lineTo(tip.x + a, tip.y - a) }
            }
            close()
        }
        drawPath(head, color)
    }
}
