// Copyright (c) 2026 Doors authors.
// SPDX-License-Identifier: Apache-2.0
package app.doors.companion.ui

import androidx.activity.compose.BackHandler
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.interaction.MutableInteractionSource
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ColumnScope
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
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.produceState
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.alpha
import androidx.compose.ui.draw.clip
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.PathEffect
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.lifecycle.compose.LifecycleStartEffect
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import app.doors.companion.Link
import app.doors.companion.core.CaptureAccess
import app.doors.companion.core.CaptureCommand
import app.doors.companion.core.CaptureDiag
import app.doors.companion.core.CaptureLimits
import app.doors.companion.core.CaptureSample
import app.doors.companion.core.CaptureSignal
import app.doors.companion.core.CaptureSource
import app.doors.companion.core.CaptureState
import app.doors.companion.core.CaptureStatus
import app.doors.companion.core.CaptureView
import app.doors.companion.core.GatewayCapture
import app.doors.companion.core.LinkState
import app.doors.companion.core.StartRequest
import app.doors.companion.core.Telemetry
import app.doors.companion.core.TrafficClass
import app.doors.companion.core.array
import app.doors.companion.core.bool
import app.doors.companion.core.double
import app.doors.companion.core.obj
import app.doors.companion.core.string
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch
import kotlinx.serialization.json.JsonObject

/*
 * PacketShark, from the phone: the state of the capture that the PacketShark
 * app on the unit owns, in counts and rates. No packet, address, name or port
 * is ever here (ADR-016 rule 11). Three things can be asked of the unit: hold,
 * stop, and to show its own start sheet. A capture starts with a tap there.
 */

/** A network interface of the unit, as its system service reports it (no hardware address). */
private data class UnitIface(val name: String, val up: Boolean, val carrier: Boolean, val ipv4: String?)

private fun interfaces(t: Telemetry): List<UnitIface> =
    t.system.data?.array("network").orEmpty().filterIsInstance<JsonObject>().mapNotNull { n ->
        val name = n.string("name") ?: return@mapNotNull null
        UnitIface(name, n.string("operstate") == "up", n.bool("carrier") != false, n.string("ipv4"))
    }.filter { CaptureLimits.ifaceOk(it.name) }

private class StateLook(val word: String, val color: Color, val pulse: Boolean)

private fun stateLook(s: CaptureStatus?): StateLook = when (s?.state) {
    CaptureState.CAPTURING -> StateLook("Capturing", Doors.Ok, true)
    CaptureState.HOLD -> StateLook("Hold", Doors.Warn, false)
    CaptureState.SAVING -> StateLook("Saving", Doors.Accent, true)
    CaptureState.STOPPED -> StateLook("Stopped", Doors.Secondary, false)
    CaptureState.ENDED -> StateLook("Ended", if (s.reason in setOf("helper_crashed", "helper_hung", "error")) Doors.Error else Doors.Warn, false)
    CaptureState.IDLE -> StateLook("Idle", Doors.Muted, false)
    null -> StateLook("No reading", Doors.Muted, false)
}

private fun reasonLine(s: CaptureStatus): String? = when (s.state) {
    CaptureState.HOLD -> "The view is held on the unit. The capture goes on behind it."
    CaptureState.SAVING -> "The unit is writing the capture to its storage."
    CaptureState.STOPPED -> "Stopped. The packets stay on the unit until they are saved or discarded there."
    CaptureState.ENDED -> when (s.reason) {
        "limit" -> "The time limit was reached."
        "link" -> "The interface lost its link."
        "left" -> "PacketShark was left on the unit, which ends a capture."
        "helper_crashed" -> "The capture helper stopped unexpectedly."
        "helper_hung" -> "The capture helper stopped answering and was ended."
        "refused" -> "The start was refused on the unit."
        "timeout" -> "Nobody answered the start sheet on the unit."
        else -> "The capture ended."
    }
    CaptureState.IDLE -> "Nothing is being captured."
    CaptureState.CAPTURING -> null
}

@Composable
private fun rememberClock(): Long {
    val now by produceState(System.currentTimeMillis()) {
        while (true) {
            delay(1000)
            value = System.currentTimeMillis()
        }
    }
    return now
}

@Composable
fun PacketSharkScreen(link: Link, onBack: () -> Unit) {
    var demo by rememberSaveable { mutableStateOf(false) }
    val source: CaptureSource = if (demo) link.captureDemo else link.capture
    // Read only while this screen is in front (ADR-016 rule 20).
    LifecycleStartEffect(source) {
        source.watch(true)
        onStopOrDispose { source.watch(false) }
    }
    val view by source.view.collectAsStateWithLifecycle()
    val state by link.session.state.collectAsStateWithLifecycle()
    val scopes by link.session.scopes.collectAsStateWithLifecycle()
    val telemetry by link.session.telemetry.collectAsStateWithLifecycle()
    val scope = rememberCoroutineScope()
    var sheet by rememberSaveable { mutableStateOf(false) }
    BackHandler(sheet) { sheet = false }

    val ifaces = interfaces(telemetry).ifEmpty {
        if (view.simulated) listOf(UnitIface("eth0", true, true, null), UnitIface("wlan0", true, true, null)) else emptyList()
    }
    val status = view.status
    val open = view.access == CaptureAccess.OPEN && status != null
    val look = stateLook(if (open) status else null)

    Box(Modifier.fillMaxSize()) {
        BoxWithConstraints(Modifier.fillMaxSize().statusBarsPadding().navigationBarsPadding()) {
            val wide = maxWidth >= 600.dp
            Column(Modifier.fillMaxSize().padding(horizontal = if (wide) 24.dp else 16.dp)) {
                val hold: (Boolean) -> Unit = { on -> scope.launch { source.hold(on) } }
                val stop: () -> Unit = { scope.launch { source.stop() } }
                val ask = { sheet = true }
                val offered = open || view.access == CaptureAccess.CLOSED
                InstrumentBar("PacketShark", onBack) {
                    // A wide screen is short: its keys live on the bar.
                    if (wide && offered) {
                        ControlKeys(view, status.takeIf { open }, compact = true, hold, stop, ask)
                        Spacer(Modifier.width(12.dp))
                    }
                    if (open) Pill(look.word, look.color, look.pulse) else Pill(gateWord(view.access), Doors.Muted)
                }
                if (view.simulated) {
                    SimulatedBand("Made-up traffic. Only the unit's interfaces and load, where shown, are its own.", compact = wide) { demo = false }
                    Spacer(Modifier.height(10.dp))
                }
                val notes: @Composable () -> Unit = { if (offered) ControlNotes(view, source::dismissNotice) }
                if (open && status != null) {
                    Instrument(view, status, telemetry, ifaces, wide, Modifier.weight(1f), notes.takeIf { wide })
                } else {
                    Column(Modifier.weight(1f).verticalScroll(rememberScrollState()), verticalArrangement = Arrangement.spacedBy(12.dp)) {
                        if (wide) notes()
                        Gate(view, state)
                        Bezel("On this unit") {
                            val hello = (state as? LinkState.Online)?.hello
                            KeyValue("Gateway serves it", if (hello == null) "—" else if (GatewayCapture.CAPABILITY in hello.capabilities) "Yes" else "Not yet", mono = false)
                            KeyValue("Capture view", if (hello == null) "—" else if (GatewayCapture.SCOPE_VIEW in scopes) "Granted" else "Not granted", mono = false)
                            KeyValue("Capture control", if (hello == null) "—" else if (GatewayCapture.SCOPE_CONTROL in scopes) "Granted" else "Not granted", mono = false)
                        }
                        InterfacesBezel(ifaces, status?.iface)
                        if (!view.simulated) {
                            GhostButton("Open the simulated instrument") { demo = true }
                            Text("Made-up figures, marked as such on every screen. Nothing is sent to the unit and nothing is read from it.", style = Type.Body, color = Doors.Muted)
                        }
                        Spacer(Modifier.height(8.dp))
                    }
                }
                if (!wide && offered) {
                    Column(Modifier.fillMaxWidth().padding(vertical = 10.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                        notes()
                        ControlKeys(view, status.takeIf { open }, compact = false, hold, stop, ask)
                    }
                }
            }
        }
        if (sheet) {
            StartSheet(ifaces, view.simulated, onCancel = { sheet = false }) { iface, limit ->
                sheet = false
                scope.launch { source.requestStart(iface, limit) }
            }
        }
    }
}

private fun gateWord(a: CaptureAccess) = when (a) {
    CaptureAccess.OFFLINE -> "Away"
    CaptureAccess.NOT_SERVED -> "Not served"
    CaptureAccess.NOT_GRANTED -> "Not granted"
    CaptureAccess.WAITING -> "Asking"
    CaptureAccess.CLOSED -> "Not open"
    CaptureAccess.OPEN -> "No reading"
}

/** A back key and a name on one compact line, with the room's state at the right. */
@Composable
fun InstrumentBar(title: String, onBack: () -> Unit, trailing: @Composable () -> Unit) {
    Row(Modifier.fillMaxWidth().padding(top = 8.dp, bottom = 12.dp), verticalAlignment = Alignment.CenterVertically) {
        Box(
            Modifier.size(44.dp).clip(RoundedCornerShape(12.dp)).background(Doors.Surface).border(1.dp, Doors.Line.copy(alpha = 0.7f), RoundedCornerShape(12.dp)).clickable(onClickLabel = "Back", onClick = onBack),
            contentAlignment = Alignment.Center,
        ) { Chevron(back = true, color = Doors.Accent) }
        Spacer(Modifier.width(14.dp))
        Text(title, style = Type.Title, modifier = Modifier.weight(1f), maxLines = 1, overflow = TextOverflow.Ellipsis)
        trailing()
    }
}

// ---- when there is nothing to read ------------------------------------------

@Composable
private fun Gate(view: CaptureView, link: LinkState) {
    val (title, body) = when (view.access) {
        CaptureAccess.OFFLINE -> "The unit is away" to
            "Capture figures are read live and never stored on the phone, so there is nothing to show until the session is back." +
            if (link is LinkState.NoWifi) " The unit is reached over Wi-Fi only." else ""
        CaptureAccess.NOT_SERVED -> "Not served by this unit" to
            "This unit's gateway does not serve capture. Reading PacketShark from a phone is a proposed change to the capture rules (ADR-016, amendment 1) and is in no released Doors yet."
        CaptureAccess.NOT_GRANTED -> "Not granted to this phone" to
            "Capture view is given to a phone on the unit. Its Settings has no switch for it yet: it is given from the unit's command line (gateway.scope). Hold, stop and start requests need Capture control as well."
        CaptureAccess.WAITING -> "Asking the unit" to "Waiting for PacketShark's state."
        CaptureAccess.CLOSED -> "PacketShark is not open on the unit" to
            "Its figures can be read only while PacketShark is the app in front there." +
            if (view.canControl) " Open it on the unit, or ask for a capture from here: the request opens it." else " Open it on the unit."
        CaptureAccess.OPEN -> "No reading yet" to "Waiting for the first figures."
    }
    Bezel("Capture") {
        // An empty graph: the frame of what will be here, with nothing in it.
        Canvas(Modifier.fillMaxWidth().height(96.dp)) {
            val dash = PathEffect.dashPathEffect(floatArrayOf(2.dp.toPx(), 5.dp.toPx()))
            for (i in 0..3) {
                val y = size.height * i / 3f
                drawLine(Doors.Line.copy(alpha = 0.6f), Offset(0f, y), Offset(size.width, y), 1f, pathEffect = if (i == 3) null else dash)
            }
            drawLine(Doors.Tx.copy(alpha = 0.5f), Offset(0f, size.height - 2.dp.toPx()), Offset(size.width, size.height - 2.dp.toPx()), 2.dp.toPx())
        }
        Spacer(Modifier.height(14.dp))
        Text(title, style = Type.Title)
        Text(body, style = Type.Body, modifier = Modifier.padding(top = 6.dp))
    }
}

// ---- the instrument ----------------------------------------------------------

@Composable
private fun Instrument(view: CaptureView, s: CaptureStatus, telemetry: Telemetry, ifaces: List<UnitIface>, wide: Boolean, modifier: Modifier, notes: (@Composable () -> Unit)?) {
    var mode by rememberSaveable { mutableStateOf(GraphMode.PACKETS) }
    var selected by remember { mutableStateOf<Long?>(null) }
    // A second that scrolled off the graph is no longer selected.
    LaunchedEffect(view.history.firstOrNull()?.elapsedMs) {
        if (selected != null && view.history.none { it.elapsedMs == selected }) selected = null
    }
    val now = rememberClock()
    val stale = s.state.running && view.readAtMs > 0 && now - view.readAtMs > STALE_MS

    val head: @Composable ColumnScope.() -> Unit = {
        notes?.invoke()
        SessionLine(s, stale, (now - view.readAtMs) / 1000)
        TrafficBezel(s, view.history, mode, { mode = it }, selected, { selected = it }, if (wide) 104.dp else 168.dp)
    }
    val rest: @Composable ColumnScope.() -> Unit = {
        ProtocolsBezel(s, view.history.lastOrNull())
        SessionBezel(s)
        SignalsBezel(s)
        DiagnosisBezel(s.diag)
        ResourcesBezel(s, telemetry, view.simulated)
        InterfacesBezel(ifaces, s.iface.takeIf { !view.simulated })
        Spacer(Modifier.height(4.dp))
    }
    if (wide) {
        Row(modifier.fillMaxWidth(), horizontalArrangement = Arrangement.spacedBy(16.dp)) {
            Column(Modifier.weight(1.1f).verticalScroll(rememberScrollState()), verticalArrangement = Arrangement.spacedBy(12.dp), content = head)
            Column(Modifier.weight(1f).verticalScroll(rememberScrollState()), verticalArrangement = Arrangement.spacedBy(12.dp), content = rest)
        }
    } else {
        Column(modifier.fillMaxWidth().verticalScroll(rememberScrollState()), verticalArrangement = Arrangement.spacedBy(12.dp)) {
            head()
            rest()
        }
    }
}

/** Where, what is kept, how long, and how long it may still run. */
@Composable
private fun SessionLine(s: CaptureStatus, stale: Boolean, ageS: Long) {
    Column(Modifier.fillMaxWidth()) {
        Row(verticalAlignment = Alignment.Bottom) {
            Column(Modifier.weight(1f)) {
                Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    Text(s.iface.ifEmpty { "—" }, style = Type.Reading)
                    Tag(if (s.fullBytes) "Full packets" else "Headers", if (s.fullBytes) Doors.Warn else Doors.Muted)
                    if (s.requestedByCompanion) Tag("Asked from a phone", Doors.Accent)
                }
                Spacer(Modifier.height(4.dp))
                Text(
                    if (stale) "Last read $ageS s ago" else reasonLine(s) ?: "Time limit ${Fmt.limit(s.limitS)}",
                    style = Type.Body.copy(fontSize = Type.Mono.fontSize, lineHeight = Type.Mono.lineHeight),
                    color = if (stale) Doors.Warn else Doors.Secondary,
                )
            }
            Text(Fmt.clock(s.elapsedMs), style = Type.Figure, color = if (s.state.running) Doors.Text else Doors.Secondary)
        }
        if (s.limitS > 0) {
            Spacer(Modifier.height(10.dp))
            Meter((s.elapsedMs / (s.limitS * 1000f)).coerceIn(0f, 1f), if (s.state.running) Doors.Tx else Doors.Faint)
        }
    }
}

@Composable
private fun TrafficBezel(
    s: CaptureStatus, history: List<CaptureSample>, mode: GraphMode, onMode: (GraphMode) -> Unit,
    selected: Long?, onSelect: (Long?) -> Unit, graphHeight: androidx.compose.ui.unit.Dp,
) {
    Bezel("Traffic  ·  last ${history.size.coerceAtLeast(1)} s", trailing = {
        Segmented(listOf(GraphMode.PACKETS to "Pkt", GraphMode.BITS to "Bit"), mode, color = Doors.Tx, onSelect = onMode)
    }) {
        val (rate, unit) = Fmt.bitrate(s.bps)
        Row(verticalAlignment = Alignment.Bottom) {
            Text(Fmt.count(s.pps), style = Type.Hero, maxLines = 1)
            Text("  PKT/S", style = Type.Label, modifier = Modifier.padding(bottom = 9.dp).weight(1f))
            Column(horizontalAlignment = Alignment.End) {
                Text("$rate $unit", style = Type.Reading)
                Label("Throughput")
            }
        }
        Spacer(Modifier.height(12.dp))
        TrafficGraph(history, mode, selected, onSelect, height = graphHeight)
        Spacer(Modifier.height(8.dp))
        Hairline()
        Spacer(Modifier.height(8.dp))
        val pickedSample = history.firstOrNull { it.elapsedMs == selected }
        val line = if (pickedSample != null) {
            val (r, u) = Fmt.bitrate(pickedSample.bps)
            listOfNotNull(Fmt.clock(pickedSample.elapsedMs), "${Fmt.count(pickedSample.total())} pkt/s", "$r $u", pickedSample.lead(), if (pickedSample.drops > 0) "${pickedSample.drops} lost" else null).joinToString("  ·  ")
        } else if (history.isEmpty()) {
            "The graph fills as readings arrive, a bar a second."
        } else {
            val peak = history.maxOf { it.total() }
            val mean = history.sumOf { it.total() } / history.size
            "Peak ${Fmt.count(peak)}  ·  mean ${Fmt.count(mean)} pkt/s  ·  touch a bar"
        }
        Text(line, style = Type.MonoSmall, color = if (pickedSample != null) Doors.Accent else Doors.Muted, maxLines = 2)
    }
}

@Composable
private fun ProtocolsBezel(s: CaptureStatus, newest: CaptureSample?) {
    Bezel("Protocols  ·  session") {
        val total = s.classes.sum()
        ShareBar(TrafficClass.entries.map { s.classes[it.ordinal] to ClassHue[it.ordinal] })
        Spacer(Modifier.height(14.dp))
        TrafficClass.entries.chunked(2).forEach { pair ->
            Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.spacedBy(16.dp)) {
                pair.forEach { c ->
                    val n = s.classes[c.ordinal]
                    Column(Modifier.weight(1f).padding(bottom = 12.dp).alpha(if (n == 0L) 0.5f else 1f)) {
                        Row(verticalAlignment = Alignment.CenterVertically) {
                            Box(Modifier.size(8.dp).background(ClassHue[c.ordinal]))
                            Spacer(Modifier.width(8.dp))
                            Text(c.word.uppercase(), style = Type.Label, color = ClassHue[c.ordinal], modifier = Modifier.width(48.dp))
                            Text("${Fmt.short(newest?.classes?.getOrNull(c.ordinal) ?: 0)}/s", style = Type.MonoSmall, modifier = Modifier.weight(1f), maxLines = 1)
                            Text(Fmt.share(n, total), style = Type.Mono, maxLines = 1)
                        }
                        Spacer(Modifier.height(6.dp))
                        Meter(if (total > 0) n.toFloat() / total else null, ClassHue[c.ordinal], Modifier.height(2.dp))
                    }
                }
            }
        }
    }
}

@Composable
private fun SessionBezel(s: CaptureStatus) {
    Bezel("Session") {
        ReadoutRow(
            Modifier,
            { m -> Readout("Packets", Fmt.short(s.seen), m) },
            { m -> Readout("Bytes", Fmt.bytes(s.bytes), m) },
            { m -> Readout("Lost", Fmt.short(s.drops), m, color = if (s.drops == 0L) Doors.Ok else Doors.Error) },
            { m -> Readout("Store", Fmt.permille(s.fillPermille), m, color = if (s.fillPermille >= 900) Doors.Warn else Doors.Text) },
        )
    }
}

@Composable
private fun SignalsBezel(s: CaptureStatus) {
    Bezel("Signals  ·  totals") {
        CaptureSignal.entries.chunked(3).forEachIndexed { row, signals ->
            if (row > 0) Spacer(Modifier.height(8.dp))
            Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                signals.forEach { sig ->
                    val n = s.findings[sig.ordinal]
                    val shape = RoundedCornerShape(10.dp)
                    Column(Modifier.weight(1f).clip(shape).background(Doors.Raised.copy(alpha = if (n > 0) 0.9f else 0.4f)).padding(horizontal = 10.dp, vertical = 9.dp)) {
                        Text(sig.word.uppercase(), style = Type.Label.copy(fontSize = Type.MonoSmall.fontSize, letterSpacing = Type.MonoSmall.letterSpacing), maxLines = 1, overflow = TextOverflow.Ellipsis)
                        Spacer(Modifier.height(4.dp))
                        Text(Fmt.short(n), style = Type.Reading, color = if (n > 0) Doors.Warn else Doors.Faint)
                    }
                }
            }
        }
        Spacer(Modifier.height(10.dp))
        Text("Counts only. Which packets and which hosts stays on the unit.", style = Type.MonoSmall)
    }
}

private class Verdict(val word: String, val color: Color)

private fun dnsVerdict(d: CaptureDiag): Verdict = when {
    d.dnsQueries == 0L -> Verdict("No queries seen", Doors.Muted)
    d.dnsFailures == 0L -> Verdict("Answering", Doors.Ok)
    d.dnsFailures * 20 >= d.dnsQueries -> Verdict("${Fmt.share(d.dnsFailures, d.dnsQueries)} failing", Doors.Error)
    else -> Verdict("${d.dnsFailures} failed", Doors.Warn)
}

private fun tcpVerdict(d: CaptureDiag): Verdict = when {
    d.tcpAttempts == 0L -> Verdict("No new connections seen", Doors.Muted)
    d.tcpFailures == 0L -> Verdict("Connecting", Doors.Ok)
    d.tcpFailures * 20 >= d.tcpAttempts -> Verdict("${Fmt.share(d.tcpFailures, d.tcpAttempts)} failing", Doors.Error)
    else -> Verdict("${d.tcpFailures} failed", Doors.Warn)
}

@Composable
private fun DiagnosisBezel(d: CaptureDiag) {
    Bezel("Diagnosis") {
        DiagBlock(
            "DNS", dnsVerdict(d), d.dnsAnswered + d.dnsLate, d.dnsQueries, ClassHue[TrafficClass.DNS.ordinal],
            listOf(
                "Queries" to Fmt.count(d.dnsQueries), "Answered" to Fmt.share(d.dnsAnswered + d.dnsLate, d.dnsQueries),
                "No reply" to Fmt.count(d.dnsNoReply), "Asked again" to Fmt.count(d.dnsRetries),
                "No such name" to Fmt.count(d.dnsNxdomain), "Server failed" to Fmt.count(d.dnsServfail + d.dnsRefused),
                "Response, mean" to Fmt.latency(d.dnsRttAvgUs), "Response, slowest" to Fmt.latency(d.dnsRttMaxUs.takeIf { it > 0 }),
            ),
        )
        Spacer(Modifier.height(14.dp))
        Hairline()
        Spacer(Modifier.height(14.dp))
        DiagBlock(
            "TCP", tcpVerdict(d), d.tcpConnected, d.tcpAttempts, ClassHue[TrafficClass.TCP.ordinal],
            listOf(
                "Attempts" to Fmt.count(d.tcpAttempts), "Connected" to Fmt.share(d.tcpConnected, d.tcpAttempts),
                "Refused" to Fmt.count(d.tcpRefused), "Reset while open" to Fmt.count(d.tcpAborted),
                "Handshake, mean" to Fmt.latency(d.tcpHsAvgUs), "Handshake, slowest" to Fmt.latency(d.tcpHsMaxUs.takeIf { it > 0 }),
            ),
        )
    }
}

@Composable
private fun DiagBlock(name: String, verdict: Verdict, good: Long, all: Long, hue: Color, rows: List<Pair<String, String>>) {
    Row(verticalAlignment = Alignment.CenterVertically) {
        Text(name, style = Type.Label, color = hue, modifier = Modifier.width(44.dp))
        Dot(verdict.color)
        Spacer(Modifier.width(8.dp))
        Text(verdict.word, style = Type.BodyStrong, color = verdict.color, modifier = Modifier.weight(1f), maxLines = 1, overflow = TextOverflow.Ellipsis)
    }
    Spacer(Modifier.height(8.dp))
    Meter(if (all > 0) good.toFloat() / all else null, hue, Modifier.height(3.dp))
    Spacer(Modifier.height(8.dp))
    rows.chunked(2).forEach { pair ->
        Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.spacedBy(16.dp)) {
            pair.forEach { (k, v) ->
                Row(Modifier.weight(1f).padding(vertical = 4.dp), verticalAlignment = Alignment.CenterVertically) {
                    Text(k, style = Type.Body.copy(fontSize = Type.Mono.fontSize), modifier = Modifier.weight(1f), maxLines = 1, overflow = TextOverflow.Ellipsis)
                    Text(v, style = Type.Mono, maxLines = 1)
                }
            }
            if (pair.size == 1) Spacer(Modifier.weight(1f))
        }
    }
}

@Composable
private fun ResourcesBezel(s: CaptureStatus, t: Telemetry, simulated: Boolean) {
    Bezel("Loss and load") {
        ReadoutRow(
            Modifier,
            { m -> Readout("Kernel", Fmt.short(s.kernelDrops), m, color = if (s.kernelDrops == 0L) Doors.Text else Doors.Error) },
            { m -> Readout("Interface", Fmt.short(s.ifaceDrops), m, color = if (s.ifaceDrops == 0L) Doors.Text else Doors.Error) },
            { m -> Readout("Shed", Fmt.short(s.shed), m, color = if (s.shed == 0L) Doors.Text else Doors.Warn) },
            { m -> Readout("Held", Fmt.short(s.held), m) },
        )
        Spacer(Modifier.height(12.dp))
        Row(verticalAlignment = Alignment.CenterVertically) {
            Label("Packet store", Modifier.width(104.dp))
            Meter(s.fillPermille / 1000f, if (s.fillPermille >= 900) Doors.Warn else Doors.Tx, Modifier.weight(1f))
            Text(Fmt.permille(s.fillPermille), style = Type.MonoSmall, color = Doors.Text, modifier = Modifier.width(40.dp).padding(start = 8.dp))
        }
        // The unit's own load comes from its system service, and is the unit's also beside simulated traffic.
        val sys = t.system.data
        val cpu = sys?.double("cpu_percent")
        val mem = sys?.obj("memory")
        val total = mem?.double("total_kb")
        val avail = mem?.double("available_kb")
        val used = if (total != null && avail != null && total > 0) ((total - avail) / total).toFloat() else null
        if (cpu != null || used != null) {
            Spacer(Modifier.height(8.dp))
            Row(verticalAlignment = Alignment.CenterVertically) {
                Label("Unit processor", Modifier.width(104.dp))
                Meter(cpu?.div(100)?.toFloat(), Doors.Accent, Modifier.weight(1f))
                Text(cpu?.let { "%.0f%%".format(it) } ?: "—", style = Type.MonoSmall, color = Doors.Text, modifier = Modifier.width(40.dp).padding(start = 8.dp))
            }
            Spacer(Modifier.height(8.dp))
            Row(verticalAlignment = Alignment.CenterVertically) {
                Label("Unit memory", Modifier.width(104.dp))
                Meter(used, Doors.Rx, Modifier.weight(1f))
                Text(used?.let { "%.0f%%".format(it * 100) } ?: "—", style = Type.MonoSmall, color = Doors.Text, modifier = Modifier.width(40.dp).padding(start = 8.dp))
            }
            if (simulated) {
                Spacer(Modifier.height(8.dp))
                Text("Processor and memory are your unit's own, read now. Everything else on this screen is simulated.", style = Type.MonoSmall)
            }
        }
    }
}

@Composable
private fun InterfacesBezel(ifaces: List<UnitIface>, capturing: String?) {
    Bezel("Interfaces on the unit") {
        if (ifaces.isEmpty()) Text("No reading. Interfaces come with the unit's status.", style = Type.Body)
        ifaces.forEach { i ->
            val linked = i.up && i.carrier
            Row(Modifier.fillMaxWidth().padding(vertical = 7.dp), verticalAlignment = Alignment.CenterVertically) {
                Dot(if (linked) Doors.Ok else Doors.Faint)
                Spacer(Modifier.width(12.dp))
                Text(i.name, style = Type.Mono, modifier = Modifier.width(64.dp))
                Text(if (i.name.startsWith("wlan")) "Wi-Fi" else "Ethernet", style = Type.Body, modifier = Modifier.weight(1f))
                if (i.name == capturing) {
                    Tag("Capture", Doors.Tx)
                    Spacer(Modifier.width(8.dp))
                }
                Text(if (linked) i.ipv4 ?: "link up" else "no link", style = Type.MonoSmall, color = if (linked) Doors.Text else Doors.Muted)
            }
        }
    }
}

// ---- what can be asked of the unit --------------------------------------------

/** What the unit said about the last thing asked of it, and a request that waits for a tap there. */
@Composable
private fun ControlNotes(view: CaptureView, onDismiss: () -> Unit) {
    val now = rememberClock()
    view.notice?.let { text ->
        Row(
            Modifier.fillMaxWidth().clip(RoundedCornerShape(10.dp)).background(Doors.Warn.copy(alpha = 0.08f))
                .clickable(onClickLabel = "Dismiss", onClick = onDismiss).padding(horizontal = 12.dp, vertical = 10.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            Text(text, style = Type.Body, color = Doors.Text, modifier = Modifier.weight(1f))
            Text("OK", style = Type.Label, color = Doors.Warn, modifier = Modifier.padding(start = 12.dp))
        }
    }
    when {
        view.request == StartRequest.SHOWN -> {
            val left = ((CaptureLimits.SHEET_MS - (now - view.requestAtMs)) / 1000).coerceIn(0, 60)
            Column(Modifier.fillMaxWidth().clip(RoundedCornerShape(12.dp)).border(1.dp, Doors.Accent.copy(alpha = 0.45f), RoundedCornerShape(12.dp)).padding(14.dp)) {
                Row(verticalAlignment = Alignment.CenterVertically) {
                    Dot(Doors.Accent, pulse = true)
                    Spacer(Modifier.width(10.dp))
                    Text(if (view.simulated) "Waiting for a tap (simulated)" else "Waiting for a tap on the unit", style = Type.BodyStrong, modifier = Modifier.weight(1f))
                    Text("$left s", style = Type.Mono, color = Doors.Accent)
                }
                Spacer(Modifier.height(10.dp))
                Meter(left / 60f, Doors.Accent)
                Spacer(Modifier.height(8.dp))
                Text("The unit shows a sheet that names this phone. Nothing is captured unless someone taps START there.", style = Type.MonoSmall)
            }
        }
        !view.canControl -> Statement("View only. The unit has not granted this phone capture control, so hold, stop and start requests are not offered.")
        view.request == StartRequest.REFUSED -> Statement("The start was refused on the unit.", color = Doors.Warn)
        view.request == StartRequest.TIMED_OUT -> Statement("Nobody answered the start sheet on the unit.", color = Doors.Warn)
    }
}

/**
 * Hold and stop while a capture runs, the start request while none does.
 * Nothing at all without capture control, or while a request waits for its tap.
 * [compact] is the top bar's form on a wide screen.
 */
@Composable
private fun ControlKeys(view: CaptureView, s: CaptureStatus?, compact: Boolean, onHold: (Boolean) -> Unit, onStop: () -> Unit, onAsk: () -> Unit) {
    if (!view.canControl || view.request == StartRequest.SHOWN) return
    var confirmStop by remember { mutableStateOf(false) }
    // A confirmation that is not used goes away by itself.
    LaunchedEffect(confirmStop) {
        if (confirmStop) {
            delay(4000)
            confirmStop = false
        }
    }
    val running = s?.state?.running == true
    LaunchedEffect(running) { if (!running) confirmStop = false }
    val key = if (compact) Modifier.width(132.dp).height(44.dp) else null
    Row(if (compact) Modifier else Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.spacedBy(10.dp)) {
        if (running) {
            val holding = s?.state == CaptureState.HOLD
            InstrumentKey(
                if (view.busy == CaptureCommand.HOLD || view.busy == CaptureCommand.RESUME) "…" else if (holding) "Resume" else "Hold",
                key ?: Modifier.weight(1f), color = if (holding) Doors.Ok else Doors.Text, enabled = view.busy == null,
            ) { onHold(!holding) }
            InstrumentKey(
                if (view.busy == CaptureCommand.STOP) "…" else if (confirmStop) (if (compact) "Stop it?" else "Stop the capture?") else "Stop",
                key ?: Modifier.weight(1f), color = Doors.Error, filled = confirmStop, enabled = view.busy == null,
            ) {
                if (confirmStop) {
                    confirmStop = false
                    onStop()
                } else confirmStop = true
            }
        } else {
            InstrumentKey(
                if (view.busy == CaptureCommand.REQUEST_START) "Asking…" else if (compact) "Ask to start" else "Ask the unit to start",
                if (compact) Modifier.width(180.dp).height(44.dp) else Modifier.weight(1f),
                color = Doors.Tx, filled = true, enabled = view.busy == null && s?.state != CaptureState.SAVING, onClick = onAsk,
            )
        }
    }
}

/** What a request may ask for, and what it cannot: chosen here, confirmed on the unit. */
@Composable
private fun StartSheet(ifaces: List<UnitIface>, simulated: Boolean, onCancel: () -> Unit, onSend: (String, Int) -> Unit) {
    val usable = ifaces.filter { it.up && it.carrier }
    var iface by rememberSaveable { mutableStateOf(usable.firstOrNull()?.name ?: "") }
    var limit by rememberSaveable { mutableStateOf(900) }
    Box(
        Modifier.fillMaxSize().background(Color.Black.copy(alpha = 0.62f))
            .clickable(interactionSource = remember { MutableInteractionSource() }, indication = null, onClickLabel = "Close", onClick = onCancel),
        contentAlignment = Alignment.BottomCenter,
    ) {
        val shape = RoundedCornerShape(topStart = 22.dp, topEnd = 22.dp)
        Column(
            Modifier.fillMaxWidth().clip(shape).background(Doors.Surface).border(1.dp, Doors.Line, shape)
                // Taps on the sheet itself are not taps on the scrim behind it.
                .clickable(interactionSource = remember { MutableInteractionSource() }, indication = null, onClick = {})
                .navigationBarsPadding().verticalScroll(rememberScrollState()).padding(20.dp),
            verticalArrangement = Arrangement.spacedBy(14.dp),
        ) {
            Text("Ask the unit to start", style = Type.Title)
            Text(
                "The unit opens PacketShark and shows a sheet naming this phone, the interface and the limit. The capture starts only if someone taps START there within 60 seconds." +
                    if (simulated) " In this simulation the tap is played for you." else "",
                style = Type.Body,
            )
            Label("Interface")
            if (usable.isEmpty()) {
                Statement("No interface of the unit has a link right now.", color = Doors.Warn)
            } else {
                Segmented(usable.map { it.name to it.name }, iface, color = Doors.Tx) { iface = it }
            }
            Label("Stops by itself after")
            Segmented(CaptureLimits.LIMITS_S.map { it to Fmt.limit(it.toLong()) }, limit, color = Doors.Tx) { limit = it }
            Hairline()
            KeyValue("Kept", "Headers only", mono = false)
            KeyValue("Other devices' traffic", "Not captured", mono = false)
            Text("Full packets and a capture without a limit can only be chosen on the unit.", style = Type.MonoSmall)
            Row(horizontalArrangement = Arrangement.spacedBy(10.dp)) {
                InstrumentKey("Cancel", Modifier.weight(1f), onClick = onCancel)
                InstrumentKey("Send request", Modifier.weight(1f), color = Doors.Tx, filled = true, enabled = iface.isNotEmpty() && usable.any { it.name == iface }) { onSend(iface, limit) }
            }
        }
    }
}

private const val STALE_MS = 6_000L
