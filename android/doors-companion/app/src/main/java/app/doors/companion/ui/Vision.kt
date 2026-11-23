// Copyright (c) 2026 Doors authors.
// SPDX-License-Identifier: Apache-2.0
package app.doors.companion.ui

import android.app.Activity
import android.content.Context
import android.content.ContextWrapper
import androidx.activity.compose.BackHandler
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.gestures.detectTapGestures
import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxScope
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.aspectRatio
import androidx.compose.foundation.layout.defaultMinSize
import androidx.compose.foundation.layout.fillMaxHeight
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.navigationBarsPadding
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.statusBarsPadding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.alpha
import androidx.compose.ui.draw.clip
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.Path
import androidx.compose.ui.graphics.StrokeCap
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.platform.LocalView
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.semantics.selected
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp
import androidx.core.view.WindowCompat
import androidx.core.view.WindowInsetsCompat
import androidx.core.view.WindowInsetsControllerCompat
import androidx.lifecycle.compose.LifecycleStartEffect
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import app.doors.companion.Link
import app.doors.companion.core.CameraOwner
import app.doors.companion.core.LinkState
import app.doors.companion.core.PreviewState
import app.doors.companion.core.VisionAccess
import app.doors.companion.core.VisionLimits
import app.doors.companion.core.VisionMode
import app.doors.companion.core.VisionSetting
import app.doors.companion.core.VisionSource
import app.doors.companion.core.VisionView
import kotlinx.coroutines.launch

/*
 * Vision, from the phone: the picture the Vision app on the unit already
 * makes for its own screen, with what it found in it, and its modes.
 *
 * The camera has one owner, the Vision app on the unit (ADR-006), and this
 * screen never changes that. The phone is authorized once on the unit; after
 * that the picture comes by itself while Vision is in front there and this
 * screen is in front here, and stops when this screen is left. The whole
 * of the contract behind this is PROPOSED (docs/design/companion/
 * VISION_PREVIEW.md); until a unit serves it the only pictures here are the
 * simulation's, under its band.
 */

/** Vision's own colour among the rooms: the teal of the unit's identity hues. */
val VisionHue = Color(0xFF74D1C4)

private fun Context.activity(): Activity? {
    var c: Context? = this
    while (c is ContextWrapper) {
        if (c is Activity) return c
        c = c.baseContext
    }
    return null
}

private class PreviewLook(val word: String, val color: Color, val pulse: Boolean)

private fun previewLook(v: VisionView): PreviewLook = when {
    v.access != VisionAccess.OPEN -> PreviewLook(
        when (v.access) {
            VisionAccess.OFFLINE -> "Away"
            VisionAccess.NOT_SERVED -> "Not served"
            VisionAccess.NOT_GRANTED -> "Not granted"
            VisionAccess.WAITING -> "Asking"
            else -> "Not open"
        },
        Doors.Muted, false,
    )
    v.preview == PreviewState.LIVE -> PreviewLook("Sharing", Doors.Ok, true)
    v.preview == PreviewState.STALLED -> PreviewLook("Stalled", Doors.Warn, true)
    v.preview == PreviewState.STOPPED -> PreviewLook("Stopped on unit", Doors.Warn, false)
    v.preview == PreviewState.BUSY -> PreviewLook("Other viewer", Doors.Warn, false)
    else -> PreviewLook("Not shared", Doors.Secondary, false)
}

@Composable
fun VisionScreen(link: Link, onBack: () -> Unit) {
    var demo by rememberSaveable { mutableStateOf(false) }
    val source: VisionSource = if (demo) link.visionDemo else link.vision
    // Read, and shared, only while this screen is in front.
    LifecycleStartEffect(source) {
        source.watch(true)
        onStopOrDispose { source.watch(false) }
    }
    val view by source.view.collectAsStateWithLifecycle()
    val shown by rememberPreview(source)
    val state by link.session.state.collectAsStateWithLifecycle()
    val scopes by link.session.scopes.collectAsStateWithLifecycle()
    val scope = rememberCoroutineScope()
    var full by rememberSaveable { mutableStateOf(false) }
    var fill by rememberSaveable { mutableStateOf(false) }
    var labels by rememberSaveable { mutableStateOf(true) }
    val live = shown != null

    // The phone stays awake while a picture is on it, as the unit does while Vision is open.
    val host = LocalView.current
    DisposableEffect(live) {
        host.keepScreenOn = live
        onDispose { host.keepScreenOn = false }
    }
    // Full screen needs a picture: when it ends, so does full screen.
    LaunchedEffect(live) { if (!live) full = false }
    BackHandler(full) { full = false }

    if (full && shown != null) {
        FullScreen(view, shown!!, fill, labels, onFill = { fill = !fill }, onLabels = { labels = !labels }, onClose = { full = false })
        return
    }

    val start: () -> Unit = { scope.launch { source.startPreview() } }
    val stop: () -> Unit = { scope.launch { source.stopPreview() } }
    val look = previewLook(view)
    val open = view.access == VisionAccess.OPEN

    BoxWithConstraints(Modifier.fillMaxSize().statusBarsPadding().navigationBarsPadding()) {
        val wide = maxWidth >= 600.dp
        val finderMax = maxHeight * 0.46f
        Column(Modifier.fillMaxSize().padding(horizontal = if (wide) 24.dp else 16.dp)) {
            InstrumentBar("Vision", onBack) {
                if (wide && open) {
                    ShareKey(view, compact = true, start, stop)
                    Spacer(Modifier.width(12.dp))
                }
                Pill(look.word, look.color, look.pulse)
            }
            if (view.simulated) {
                SimulatedBand("A drawn street. No camera is open, here or on your unit.", compact = wide) { demo = false }
                Spacer(Modifier.height(10.dp))
            }
            val finder: @Composable (Modifier) -> Unit = { m ->
                Viewfinder(view, shown, fill, labels, m, onFull = { full = true }, onFill = { fill = !fill }, onLabels = { labels = !labels })
            }
            val panels: @Composable () -> Unit = {
                if (open) {
                    view.notice?.let { Notice(it, source::dismissNotice) }
                    ShareNotes(view)
                    ModeBezel(view) { m -> scope.launch { source.setMode(m) } }
                    SettingsBezel(view, onSet = { s, value -> scope.launch { source.set(s, value) } }, onReset = { scope.launch { source.resetCounts() } })
                    TelemetryBezel(view)
                    StreamBezel(view, shown?.skipped ?: 0)
                    CameraBezel(view)
                } else {
                    VisionGate(view, state)
                    Bezel("On this unit") {
                        val hello = (state as? LinkState.Online)?.hello
                        KeyValue("Gateway serves it", if (hello == null) "—" else if ("vision" in hello.capabilities) "Yes" else "Not yet", mono = false)
                        KeyValue("Vision", if (hello == null) "—" else if ("vision" in scopes) "Granted" else "Not granted", mono = false)
                    }
                    if (!view.simulated) {
                        GhostButton("Open the simulated viewfinder") { demo = true }
                        Text("A drawn scene, marked as simulated on every screen. No camera is opened and nothing is sent to the unit.", style = Type.Body, color = Doors.Muted)
                    }
                }
                Spacer(Modifier.height(8.dp))
            }
            if (wide) {
                Row(Modifier.weight(1f).fillMaxWidth(), horizontalArrangement = Arrangement.spacedBy(16.dp)) {
                    if (open) finder(Modifier.weight(1.25f).fillMaxHeight().padding(bottom = 12.dp))
                    Column(Modifier.weight(1f).verticalScroll(rememberScrollState()), verticalArrangement = Arrangement.spacedBy(12.dp)) { panels() }
                }
            } else {
                if (open) {
                    // The picture keeps its place while the panels scroll under it.
                    val ratio = shown?.let { it.w.toFloat() / it.h } ?: (16f / 9f)
                    finder(Modifier.fillMaxWidth().heightIn(max = finderMax).aspectRatio(ratio.coerceIn(0.5f, 2.2f), matchHeightConstraintsFirst = ratio < 1f))
                    Spacer(Modifier.height(10.dp))
                    ShareKey(view, compact = false, start, stop)
                    Spacer(Modifier.height(10.dp))
                }
                Column(Modifier.weight(1f).verticalScroll(rememberScrollState()), verticalArrangement = Arrangement.spacedBy(12.dp)) { panels() }
            }
        }
    }
}

// ---- the viewfinder -----------------------------------------------------------

/** The frame the picture lives in: the picture when there is one, and otherwise why there is none. */
@Composable
private fun Viewfinder(
    view: VisionView, shown: Shown?, fill: Boolean, labels: Boolean, modifier: Modifier,
    onFull: () -> Unit, onFill: () -> Unit, onLabels: () -> Unit,
) {
    val shape = RoundedCornerShape(14.dp)
    Box(modifier.clip(shape).background(Color(0xFF050505)).border(1.dp, if (shown != null) VisionHue.copy(alpha = 0.45f) else Doors.Line, shape)) {
        if (shown != null) {
            PreviewImage(shown, view.mode, fill, labels, Modifier.fillMaxSize().pointerInput(Unit) { detectTapGestures(onDoubleTap = { onFill() }) })
            Hud(view, shown)
            Row(Modifier.align(Alignment.BottomEnd).padding(8.dp), horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                GlassKey(if (labels) "Labels" else "Boxes", onLabels)
                GlassKey(if (fill) "Fill" else "Fit", onFill)
                GlassKey("Full", onFull)
            }
        } else {
            Reticle(Modifier.fillMaxSize(), Doors.Line)
            Column(Modifier.align(Alignment.Center).padding(24.dp), horizontalAlignment = Alignment.CenterHorizontally) {
                val (title, body) = when (view.preview) {
                    PreviewState.STOPPED -> "Stopped on the unit" to "Someone tapped STOP there. The picture comes back when it is resumed on the unit or Vision is opened again."
                    PreviewState.BUSY -> "Another phone has the picture" to "The unit shows its picture to one phone at a time."
                    PreviewState.STALLED, PreviewState.LIVE -> "Waiting for a picture" to "The unit is sharing; no picture has arrived yet."
                    PreviewState.OFF -> "No picture" to when {
                        view.owner != CameraOwner.VISION -> "Vision does not have the camera right now."
                        view.mode?.remote == false -> "This mode's picture stays on the unit."
                        else -> "The picture is paused on this phone."
                    }
                }
                Text(title, style = Type.Title, color = Doors.Text)
                Spacer(Modifier.height(6.dp))
                Text(body, style = Type.Body, modifier = Modifier.fillMaxWidth(0.86f))
            }
        }
    }
}

/** What the picture is, said on the picture: mode and model at the top left, the stream at the top right. */
@Composable
private fun BoxScope.Hud(view: VisionView, shown: Shown) {
    Row(Modifier.align(Alignment.TopStart).padding(8.dp), horizontalArrangement = Arrangement.spacedBy(6.dp)) {
        // A picture that stopped arriving is not called live.
        if (view.preview == PreviewState.STALLED) GlassTag("Stalled", Doors.Warn)
        else GlassTag(if (view.simulated) "Simulated" else "Live", if (view.simulated) Doors.Warn else Doors.Ok, dot = true)
        view.mode?.let { GlassTag(it.word, VisionHue) }
        if (shown.scene.boxes.isNotEmpty()) GlassTag("${shown.scene.boxes.size}", Doors.Text)
    }
    Row(Modifier.align(Alignment.TopEnd).padding(8.dp), horizontalArrangement = Arrangement.spacedBy(6.dp)) {
        GlassTag("%.1f fps".format(view.stream.fpsX10 / 10f), Doors.Text)
        view.stream.ageMs?.let { GlassTag("$it ms", if (it > 400) Doors.Warn else Doors.Text) }
    }
}

@Composable
private fun GlassTag(text: String, color: Color, dot: Boolean = false) {
    Row(
        Modifier.clip(RoundedCornerShape(6.dp)).background(Color.Black.copy(alpha = 0.58f)).padding(horizontal = 8.dp, vertical = 4.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        if (dot) {
            Dot(color, 6.dp, pulse = true)
            Spacer(Modifier.width(6.dp))
        }
        Text(text.uppercase(), style = Type.MonoSmall, color = color, maxLines = 1)
    }
}

/** A key that sits on the picture. Small to the eye, a full target to the finger. */
@Composable
private fun GlassKey(text: String, onClick: () -> Unit) {
    Box(Modifier.defaultMinSize(minWidth = 48.dp, minHeight = 48.dp).clickable(role = Role.Button, onClick = onClick), contentAlignment = Alignment.Center) {
        Text(
            text.uppercase(), style = Type.Label, color = Doors.Text,
            modifier = Modifier.clip(RoundedCornerShape(8.dp)).background(Color.Black.copy(alpha = 0.58f)).border(1.dp, Color.White.copy(alpha = 0.18f), RoundedCornerShape(8.dp)).padding(horizontal = 10.dp, vertical = 7.dp),
        )
    }
}

/** An empty viewfinder: corner marks and a centre cross, nothing that pretends to be a picture. */
@Composable
private fun Reticle(modifier: Modifier, color: Color) {
    Canvas(modifier) {
        val m = 14.dp.toPx()
        val c = 22.dp.toPx()
        val s = Stroke(1.5.dp.toPx(), cap = StrokeCap.Round)
        val p = Path().apply {
            moveTo(m, m + c); lineTo(m, m); lineTo(m + c, m)
            moveTo(size.width - m - c, m); lineTo(size.width - m, m); lineTo(size.width - m, m + c)
            moveTo(size.width - m, size.height - m - c); lineTo(size.width - m, size.height - m); lineTo(size.width - m - c, size.height - m)
            moveTo(m + c, size.height - m); lineTo(m, size.height - m); lineTo(m, size.height - m - c)
        }
        drawPath(p, color, style = s)
        val cx = size.width / 2
        val top = m + 6.dp.toPx()
        drawLine(color.copy(alpha = 0.5f), Offset(cx - 8.dp.toPx(), top), Offset(cx + 8.dp.toPx(), top), 1.dp.toPx())
        drawLine(color.copy(alpha = 0.5f), Offset(cx, top - 4.dp.toPx()), Offset(cx, top + 4.dp.toPx()), 1.dp.toPx())
    }
}

/** The picture and nothing else, edge to edge, in either orientation. A tap shows or hides what lies over it. */
@Composable
private fun FullScreen(view: VisionView, shown: Shown, fill: Boolean, labels: Boolean, onFill: () -> Unit, onLabels: () -> Unit, onClose: () -> Unit) {
    var chrome by rememberSaveable { mutableStateOf(true) }
    val host = LocalView.current
    DisposableEffect(Unit) {
        val window = host.context.activity()?.window
        val bars = window?.let { WindowCompat.getInsetsController(it, host) }
        bars?.systemBarsBehavior = WindowInsetsControllerCompat.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE
        bars?.hide(WindowInsetsCompat.Type.systemBars())
        onDispose { bars?.show(WindowInsetsCompat.Type.systemBars()) }
    }
    Box(Modifier.fillMaxSize().background(Color.Black)) {
        PreviewImage(
            shown, view.mode, fill, labels,
            Modifier.fillMaxSize().pointerInput(Unit) { detectTapGestures(onTap = { chrome = !chrome }, onDoubleTap = { onFill() }) },
        )
        // The simulation's mark stays whatever else is hidden.
        if (view.simulated) Box(Modifier.align(Alignment.BottomStart).padding(12.dp)) { GlassTag("Simulated  ·  not a camera", Doors.Warn, dot = true) }
        if (chrome) {
            Hud(view, shown)
            Row(Modifier.align(Alignment.BottomEnd).padding(12.dp), horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                GlassKey(if (labels) "Labels" else "Boxes", onLabels)
                GlassKey(if (fill) "Fill" else "Fit", onFill)
                GlassKey("Close", onClose)
            }
        }
    }
}

// ---- asking for the picture ----------------------------------------------------

/** Start and stop. One key, and it always says what a tap will do. */
@Composable
private fun ShareKey(view: VisionView, compact: Boolean, onStart: () -> Unit, onStop: () -> Unit) {
    val m = if (compact) Modifier.width(190.dp).height(44.dp) else Modifier.fillMaxWidth()
    val shareable = view.owner == CameraOwner.VISION && view.mode?.remote != false
    when (view.preview) {
        PreviewState.LIVE, PreviewState.STALLED -> InstrumentKey(if (compact) "Pause" else "Pause the picture", m, onClick = onStop)
        PreviewState.STOPPED, PreviewState.BUSY -> InstrumentKey(if (compact) "Show picture" else "Show the unit's picture", m, color = VisionHue, filled = true, enabled = false, onClick = onStart)
        else -> InstrumentKey(if (view.busy) "Starting…" else if (compact) "Show picture" else "Show the unit's picture", m, color = VisionHue, filled = true, enabled = !view.busy && shareable, onClick = onStart)
    }
}

@Composable
private fun ShareNotes(view: VisionView) {
    when {
        view.owner == CameraOwner.CAMERA -> Statement("The Camera app has the camera on the unit. Vision, and its picture, are there again when Vision is opened.", color = Doors.Warn)
        view.mode?.remote == false -> Statement("${view.mode.word} is running on the unit. Its picture is never shared, and it is chosen on the unit only.", color = Doors.Warn)
        view.preview == PreviewState.STALLED -> Statement("No picture for a few seconds. The unit may be busy; the session is kept.", color = Doors.Warn)
    }
}

@Composable
private fun Notice(text: String, onDismiss: () -> Unit) {
    Row(
        Modifier.fillMaxWidth().clip(RoundedCornerShape(10.dp)).background(Doors.Warn.copy(alpha = 0.08f)).clickable(onClickLabel = "Dismiss", onClick = onDismiss).padding(horizontal = 12.dp, vertical = 10.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Text(text, style = Type.Body, color = Doors.Text, modifier = Modifier.weight(1f))
        Text("OK", style = Type.Label, color = Doors.Warn, modifier = Modifier.padding(start = 12.dp))
    }
}

// ---- modes and their settings -----------------------------------------------------

@Composable
private fun ModeBezel(view: VisionView, onMode: (VisionMode) -> Unit) {
    Bezel("Mode", trailing = { view.model?.let { Text(it, style = Type.MonoSmall, maxLines = 1, overflow = TextOverflow.Ellipsis) } }) {
        Row(Modifier.fillMaxWidth().horizontalScroll(rememberScrollState()), horizontalArrangement = Arrangement.spacedBy(14.dp)) {
            VisionMode.Group.entries.forEach { group ->
                Column {
                    Text(group.word.uppercase(), style = Type.MonoSmall)
                    Spacer(Modifier.height(6.dp))
                    Row(horizontalArrangement = Arrangement.spacedBy(6.dp)) {
                        VisionMode.entries.filter { it.group == group }.forEach { m ->
                            ModeChip(m, on = m == view.mode, can = m in view.modes && m.remote && !view.busy, onClick = { onMode(m) })
                        }
                    }
                }
            }
        }
        val why = "People and text modes are chosen on the unit and are never shared. A dimmed mode has no model on this unit."
        Spacer(Modifier.height(10.dp))
        Text(why, style = Type.MonoSmall)
    }
}

@Composable
private fun ModeChip(m: VisionMode, on: Boolean, can: Boolean, onClick: () -> Unit) {
    val shape = RoundedCornerShape(10.dp)
    Box(
        Modifier.defaultMinSize(minWidth = 64.dp, minHeight = 44.dp).clip(shape)
            .background(if (on) VisionHue.copy(alpha = 0.16f) else Color.Transparent)
            .border(1.dp, if (on) VisionHue.copy(alpha = 0.7f) else Doors.Line, shape)
            .alpha(if (can || on) 1f else 0.42f)
            .clickable(enabled = can && !on, role = Role.Tab, onClick = onClick)
            .semantics { selected = on }
            .padding(horizontal = 12.dp),
        contentAlignment = Alignment.Center,
    ) { Text(m.word.uppercase(), style = Type.Label, color = if (on) VisionHue else Doors.Text, maxLines = 1) }
}

/** What the mode in use lets a phone change, and what it counts. Only what Vision's own sheets offer. */
@Composable
private fun SettingsBezel(view: VisionView, onSet: (VisionSetting, String) -> Unit, onReset: () -> Unit) {
    val mode = view.mode ?: return
    val settings = VisionSetting.entries.filter { it.shownIn(mode) }
    val counts = mode == VisionMode.TRACK || mode == VisionMode.TRAFFIC
    if (settings.isEmpty() && !counts) return
    Bezel(mode.word) {
        if (counts) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Readout("Left to right", Fmt.count(view.countAb), Modifier.weight(1f), color = VisionHue)
                Readout("Right to left", Fmt.count(view.countBa), Modifier.weight(1f), color = VisionHue)
                InstrumentKey("Reset", Modifier.width(96.dp).height(44.dp), enabled = !view.busy && view.countAb + view.countBa > 0, onClick = onReset)
            }
            if (settings.isNotEmpty()) {
                Spacer(Modifier.height(12.dp))
                Hairline()
                Spacer(Modifier.height(4.dp))
            }
        }
        settings.forEach { s ->
            Row(Modifier.fillMaxWidth().padding(top = 8.dp), verticalAlignment = Alignment.CenterVertically) {
                Text(s.word, style = Type.Body, color = Doors.Text, modifier = Modifier.weight(1f), maxLines = 1, overflow = TextOverflow.Ellipsis)
                val value = view.settings[s]
                Segmented(s.values, value ?: "", color = VisionHue) { if (!view.busy && it != value) onSet(s, it) }
            }
        }
    }
}

// ---- figures ---------------------------------------------------------------------

@Composable
private fun TelemetryBezel(view: VisionView) {
    val s = view.stats
    Bezel("Detector  ·  on the unit") {
        ReadoutRow(
            Modifier,
            { m -> Readout("Frames", s?.let { "%.1f".format(it.fpsX10 / 10f) } ?: "—", m, unit = "/s") },
            { m -> Readout("Inference", s?.inferMs?.takeIf { it > 0 }?.toString() ?: "—", m, unit = "ms") },
            { m -> Readout("Processor", s?.let { "${it.cpuPct}" } ?: "—", m, unit = "%", color = if ((s?.cpuPct ?: 0) > 85) Doors.Warn else Doors.Text) },
            { m -> Readout("Memory", s?.let { Fmt.bytes(it.rssKb * 1024) } ?: "—", m) },
        )
        Spacer(Modifier.height(14.dp))
        Row(verticalAlignment = Alignment.CenterVertically) {
            Label("Frame rate, 1 min", Modifier.width(132.dp))
            Sparkline(view.fpsHistory.map { it / 10f }, VisionHue, Modifier.weight(1f).height(28.dp), slots = VisionLimits.HISTORY)
        }
        if (s != null && s.preMs + s.inferMs + s.postMs > 0) {
            Spacer(Modifier.height(12.dp))
            // Where a frame's time goes: before the net, in it, after it.
            ShareBar(listOf(s.preMs.toLong() to Doors.Rx, s.inferMs.toLong() to VisionHue, s.postMs.toLong() to Doors.Tx))
            Spacer(Modifier.height(6.dp))
            Row(horizontalArrangement = Arrangement.spacedBy(14.dp)) {
                Legend("Prepare ${s.preMs} ms", Doors.Rx)
                Legend("Infer ${s.inferMs} ms", VisionHue)
                Legend("Track ${s.postMs} ms", Doors.Tx)
            }
        }
        if ((s?.dropped ?: 0) > 0) {
            Spacer(Modifier.height(8.dp))
            Text("${Fmt.count(s!!.dropped)} detections dropped for want of room", style = Type.MonoSmall, color = Doors.Warn)
        }
    }
}

@Composable
private fun Legend(text: String, color: Color) {
    Row(verticalAlignment = Alignment.CenterVertically) {
        Box(Modifier.size(7.dp).background(color))
        Spacer(Modifier.width(6.dp))
        Text(text, style = Type.MonoSmall, maxLines = 1)
    }
}

/** How good the stream is, as four bars: frames arriving, and how old they are when they do. */
@Composable
private fun Quality(stream: app.doors.companion.core.StreamStats, live: Boolean, width: Dp = 30.dp) {
    val bars = when {
        !live || stream.frames == 0L -> 0
        stream.fpsX10 >= 60 && (stream.ageMs ?: 0) < 250 -> 4
        stream.fpsX10 >= 40 && (stream.ageMs ?: 0) < 500 -> 3
        stream.fpsX10 >= 20 -> 2
        else -> 1
    }
    Canvas(Modifier.size(width = width, height = 18.dp).semantics { }) {
        val w = size.width / 7f
        for (i in 0 until 4) {
            val h = size.height * (0.35f + 0.65f * i / 3f)
            drawRoundRect(
                if (i < bars) (if (bars <= 1) Doors.Warn else VisionHue) else Doors.Line,
                topLeft = Offset(i * 2 * w, size.height - h), size = androidx.compose.ui.geometry.Size(w, h), cornerRadius = androidx.compose.ui.geometry.CornerRadius(w / 2),
            )
        }
    }
}

@Composable
private fun StreamBezel(view: VisionView, skipped: Long) {
    val live = view.preview == PreviewState.LIVE
    val st = view.stream
    Bezel("Preview  ·  to this phone", trailing = { Quality(st, live) }) {
        if (!live) {
            Text("No picture is leaving the unit.", style = Type.Body)
            return@Bezel
        }
        ReadoutRow(
            Modifier,
            { m -> Readout("Pictures", "%.1f".format(st.fpsX10 / 10f), m, unit = "/s") },
            { m -> Readout("Rate", if (view.simulated) "—" else Fmt.bitrate(st.kbps * 1000L).first, m, unit = if (view.simulated) "" else Fmt.bitrate(st.kbps * 1000L).second) },
            { m -> Readout("Age", st.ageMs?.toString() ?: "—", m, unit = "ms", color = if ((st.ageMs ?: 0) > 400) Doors.Warn else Doors.Text) },
            { m -> Readout("Skipped", Fmt.short(skipped + st.skipped), m) },
        )
        Spacer(Modifier.height(10.dp))
        Text(
            if (view.simulated) "Drawn on the phone: no bit rate, because nothing travels." else "One picture is held at a time. A newer one replaces an unseen one rather than queueing behind it.",
            style = Type.MonoSmall,
        )
    }
}

@Composable
private fun CameraBezel(view: VisionView) {
    Bezel("Camera on the unit") {
        KeyValue(
            "Held by",
            when (view.owner) {
                CameraOwner.VISION -> "Vision"
                CameraOwner.CAMERA -> "Camera"
                CameraOwner.OTHER -> "Another app"
                CameraOwner.NONE -> "Nobody"
            },
            mono = false,
        )
        KeyValue(
            "Picture",
            when (view.preview) {
                PreviewState.LIVE, PreviewState.STALLED -> "Shared with this phone"
                PreviewState.STOPPED -> "Stopped on the unit"
                PreviewState.BUSY -> "Shared with another phone"
                else -> "Stays on the unit"
            },
            mono = false, valueColor = if (view.preview == PreviewState.LIVE) Doors.Ok else Doors.Text,
        )
        Text("The camera has one owner at a time, and it is never this phone. While a picture is shared the unit shows a mark and a STOP on its screen, and sharing ends when this screen is left.", style = Type.MonoSmall, modifier = Modifier.padding(top = 6.dp))
    }
}

// ---- when Vision cannot be read -----------------------------------------------------

@Composable
private fun VisionGate(view: VisionView, link: LinkState) {
    val (title, body) = when (view.access) {
        VisionAccess.OFFLINE -> "The unit is away" to
            "Nothing of Vision is stored on the phone, so there is nothing to show until the session is back." +
            if (link is LinkState.NoWifi) " The unit is reached over Wi-Fi only." else ""
        VisionAccess.NOT_SERVED -> "Not served by this unit" to
            "This unit's gateway does not serve Vision. Sharing a picture with a paired phone is decided but not built yet, and is in no released Doors."
        VisionAccess.NOT_GRANTED -> "Not granted to this phone" to
            "On the unit, open Settings, Companion, this phone, and allow Vision. That is done once; after it the picture comes by itself whenever Vision is open on the unit."
        VisionAccess.WAITING -> "Asking the unit" to "Waiting for Vision's state."
        VisionAccess.CLOSED -> "Vision is not open on the unit" to
            "The camera runs only while Vision is the app in front there, and a phone cannot open it."
        VisionAccess.OPEN -> "No reading yet" to "Waiting for Vision's state."
    }
    Bezel("Vision") {
        Reticle(Modifier.fillMaxWidth().height(120.dp), Doors.Line)
        Spacer(Modifier.height(12.dp))
        Text(title, style = Type.Title)
        Text(body, style = Type.Body, modifier = Modifier.padding(top = 6.dp))
    }
}
