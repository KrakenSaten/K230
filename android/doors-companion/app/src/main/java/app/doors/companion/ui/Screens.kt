// Copyright (c) 2026 Doors authors.
// SPDX-License-Identifier: Apache-2.0
package app.doors.companion.ui

import androidx.compose.foundation.background
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.ui.draw.clip
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ColumnScope
import androidx.compose.foundation.layout.ExperimentalLayoutApi
import androidx.compose.foundation.layout.FlowRow
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.navigationBarsPadding
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.statusBarsPadding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.produceState
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.alpha
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import app.doors.companion.Link
import app.doors.companion.core.Fingerprint
import app.doors.companion.core.GatewayError
import app.doors.companion.core.LinkState
import app.doors.companion.core.MeshPhase
import app.doors.companion.core.PairedUnit
import app.doors.companion.core.Reading
import app.doors.companion.core.Stop
import app.doors.companion.core.Trouble
import app.doors.companion.core.array
import app.doors.companion.core.bool
import app.doors.companion.core.double
import app.doors.companion.core.long
import app.doors.companion.core.obj
import app.doors.companion.core.string
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import kotlinx.serialization.json.JsonArray
import kotlinx.serialization.json.JsonNull
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.JsonPrimitive

enum class Screen { Home, Telemetry, Connection, Rift, PacketShark }

private class Look(val pill: String, val color: Color, val pulse: Boolean, val open: Float, val line: String)

private fun look(state: LinkState, unit: PairedUnit): Look = when (state) {
    is LinkState.Online -> Look("Open", Doors.Ok, false, 1f, "${unit.host}  ·  TLS 1.3  ·  key pinned")
    is LinkState.Connecting -> Look("Knocking", Doors.Accent, true, 0.10f, "Reaching ${unit.host}")
    is LinkState.Waiting -> when (state.trouble) {
        Trouble.NOT_ADMITTED -> Look("Not let in", Doors.Warn, false, 0f, "The unit answered but did not let this phone in. Trying again")
        Trouble.UNREACHABLE -> Look("Away", Doors.Warn, false, 0f, "No answer on this network. Trying again shortly")
        Trouble.LOST -> Look("Away", Doors.Warn, false, 0f, "The connection was lost. Trying again shortly")
    }
    LinkState.NoWifi -> Look("No Wi-Fi", Doors.Muted, false, 0f, "The unit is reached over Wi-Fi only")
    LinkState.Idle -> Look("Idle", Doors.Muted, false, 0f, "Not connected")
    is LinkState.Stopped -> when (state.why) {
        Stop.REVOKED -> Look("Removed", Doors.Error, false, 0f, "The unit says this phone was removed")
        Stop.WRONG_UNIT -> Look("Not your unit", Doors.Error, false, 0f, "This is not the unit you paired")
        Stop.INCOMPATIBLE -> Look("Update needed", Doors.Error, false, 0f, "The unit and the app share no protocol version")
    }
}

@Composable
private fun rememberNow(): Long {
    val now by produceState(System.currentTimeMillis()) {
        while (true) {
            delay(1000)
            value = System.currentTimeMillis()
        }
    }
    return now
}

private fun age(atMs: Long, now: Long): String {
    if (atMs <= 0) return "no reading yet"
    val s = ((now - atMs) / 1000).coerceAtLeast(0)
    return when {
        s < 5 -> "live"
        s < 90 -> "$s s ago"
        s < 5400 -> "${s / 60} min ago"
        else -> "${s / 3600} h ago"
    }
}

private fun uptime(s: Long?): String {
    if (s == null) return "—"
    val d = s / 86400
    val h = (s % 86400) / 3600
    val m = (s % 3600) / 60
    return if (d > 0) "${d}d ${h}h" else "${h}h ${"%02d".format(m)}m"
}

private fun bytes(b: Double?): String {
    if (b == null) return "—"
    val units = listOf("B", "KB", "MB", "GB", "TB")
    var v = b
    var i = 0
    while (v >= 1024 && i < units.lastIndex) {
        v /= 1024
        i++
    }
    return if (i == 0) "${v.toLong()} B" else "%.1f %s".format(v, units[i])
}

private fun why(r: Reading): String? = when (r.error) {
    null -> null
    GatewayError.NOT_PERMITTED -> "Not permitted on the unit"
    GatewayError.UPSTREAM -> "Service is away"
    else -> "Unavailable"
}

@Composable
private fun Page(content: @Composable ColumnScope.() -> Unit) {
    Column(
        Modifier.fillMaxSize().verticalScroll(rememberScrollState()).statusBarsPadding().navigationBarsPadding()
            .padding(horizontal = 20.dp, vertical = 12.dp),
        verticalArrangement = Arrangement.spacedBy(12.dp),
        content = content,
    )
}

// ---- the dashboard ---------------------------------------------------------

@Composable
fun Home(link: Link, unit: PairedUnit, go: (Screen) -> Unit) {
    val state by link.session.state.collectAsStateWithLifecycle()
    val t by link.session.telemetry.collectAsStateWithLifecycle()
    val now = rememberNow()
    val l = look(state, unit)
    val mesh by link.mesh.link.collectAsStateWithLifecycle()
    val unread by remember(unit.fingerprint) { link.meshDb.dao().unread(unit.fingerprint) }.collectAsStateWithLifecycle(0)
    val online = state is LinkState.Online
    val dim = if (online) 1f else 0.45f

    Page {
        Row(Modifier.fillMaxWidth().padding(top = 8.dp), verticalAlignment = Alignment.CenterVertically) {
            Column(Modifier.weight(1f)) {
                Label("Doors  ·  Companion", color = Doors.Accent)
                Spacer(Modifier.height(6.dp))
                Text(unit.name, style = Type.Display, maxLines = 1, overflow = TextOverflow.Ellipsis)
            }
            Pill(l.pill, l.color, l.pulse)
        }

        Panel(onClick = { go(Screen.Connection) }, tint = Doors.Bg) {
            Threshold(l.open, if (online) Doors.Accent else l.color, Modifier.fillMaxWidth().height(190.dp), searching = l.pulse)
            Spacer(Modifier.height(6.dp))
            Row(verticalAlignment = Alignment.CenterVertically) {
                Text(l.line, style = Type.Body, modifier = Modifier.weight(1f))
                Chevron()
            }
        }

        if (state is LinkState.Stopped) {
            Panel(tint = Doors.Error.copy(alpha = 0.08f)) {
                Text(
                    when ((state as LinkState.Stopped).why) {
                        Stop.REVOKED -> "This phone was removed on the unit. Forget the unit here and pair again from its Settings."
                        Stop.WRONG_UNIT -> "The address answered with a different key. The app will never accept it. If the unit was reset, forget it and pair again."
                        Stop.INCOMPATIBLE -> "Update the app or the unit so that they share a protocol version."
                    },
                    style = Type.Body, color = Doors.Text,
                )
                Spacer(Modifier.height(14.dp))
                GhostButton("Connection details") { go(Screen.Connection) }
            }
        }

        if ((state as? LinkState.Waiting)?.trouble == Trouble.NOT_ADMITTED) {
            Panel(tint = Doors.Warn.copy(alpha = 0.07f)) {
                Text("It is the right unit: its key matched. It closed the door without saying why. It may be busy or restarting, or this phone may have been removed on it. The pairing is kept here; the unit's Companion page shows whether this phone is still listed.", style = Type.Body, color = Doors.Text)
            }
        }

        Section("Vitals  ·  ${age(t.system.atMs, now)}") {
            val s = t.system.data
            val mem = s?.obj("memory")
            val total = mem?.double("total_kb")
            val avail = mem?.double("available_kb")
            val used = if (total != null && avail != null && total > 0) ((total - avail) / total).toFloat() else null
            val cpu = s?.double("cpu_percent")
            val temp = s?.double("temperature_c")
            Row(horizontalArrangement = Arrangement.spacedBy(12.dp)) {
                Vital("Processor", cpu?.let { "%.0f".format(it) }, "%", cpu?.div(100)?.toFloat(), Doors.Accent, dim, Modifier.weight(1f)) { go(Screen.Telemetry) }
                Vital("Memory", used?.let { "%.0f".format(it * 100) }, "%", used, Doors.Rx, dim, Modifier.weight(1f)) { go(Screen.Telemetry) }
            }
            Row(horizontalArrangement = Arrangement.spacedBy(12.dp)) {
                Vital("Temperature", temp?.let { "%.0f".format(it) }, "°C", temp?.let { ((it - 20) / 65).toFloat() }, if ((temp ?: 0.0) > 70) Doors.Error else Doors.Tx, dim, Modifier.weight(1f)) { go(Screen.Telemetry) }
                Vital("Uptime", uptime(s?.long("uptime_s")).takeIf { s != null }, "", null, Doors.Ok, dim, Modifier.weight(1f)) { go(Screen.Telemetry) }
            }
            why(t.system)?.let { Text(it, style = Type.Body, color = Doors.Warn) }
        }

        Section("Links") {
            val w = t.wifi.data
            Panel(Modifier.fillMaxWidth().alpha(dim), onClick = { go(Screen.Telemetry) }) {
                Row(verticalAlignment = Alignment.CenterVertically) {
                    Column(Modifier.weight(1f)) {
                        Label("Wi-Fi")
                        Spacer(Modifier.height(6.dp))
                        Text(w?.string("ssid") ?: why(t.wifi) ?: w?.string("state") ?: "—", style = Type.BodyStrong, maxLines = 1, overflow = TextOverflow.Ellipsis)
                        Text(listOfNotNull(w?.string("state"), w?.string("ipv4"), w?.long("signal_dbm")?.let { "$it dBm" }).joinToString("  ·  ").ifEmpty { "No reading" }, style = Type.Mono, color = Doors.Secondary)
                    }
                    SignalBars(w?.long("bars")?.toInt())
                }
            }
            Row(horizontalArrangement = Arrangement.spacedBy(12.dp)) {
                Service("Radio", t.radio, Doors.Tx, dim, Modifier.weight(1f)) { go(Screen.Telemetry) }
                Service("Mesh", t.mesh, Doors.Rx, dim, Modifier.weight(1f)) { go(Screen.Telemetry) }
            }
        }

        Section("Rooms") {
            Room(
                "RIFT", "Mesh messages, nodes and channels", Doors.Rx,
                when {
                    unread > 0 -> if (unread > 99) "99+ new" else "$unread new"
                    mesh.phase == MeshPhase.LIVE -> "Live"
                    else -> "Stored"
                },
            ) { go(Screen.Rift) }
            Room("PacketShark", "Capture state and traffic figures", Doors.Tx, "Soon") { go(Screen.PacketShark) }
        }
        Spacer(Modifier.height(12.dp))
    }
}

@Composable
private fun Vital(label: String, value: String?, unit: String, share: Float?, color: Color, dim: Float, modifier: Modifier, onClick: () -> Unit) {
    Panel(modifier.alpha(dim), onClick = onClick) {
        Label(label)
        Spacer(Modifier.height(10.dp))
        Row(verticalAlignment = Alignment.Bottom) {
            Text(value ?: "—", style = Type.Figure, maxLines = 1)
            if (value != null && unit.isNotEmpty()) Text(" $unit", style = Type.Mono, color = Doors.Muted, modifier = Modifier.padding(bottom = 4.dp))
        }
        Spacer(Modifier.height(12.dp))
        Meter(share, color)
    }
}

@Composable
private fun Service(label: String, r: Reading, color: Color, dim: Float, modifier: Modifier, onClick: () -> Unit) {
    val state = r.data?.string("state")
    Panel(modifier.alpha(dim), onClick = onClick) {
        Row(verticalAlignment = Alignment.CenterVertically) {
            Dot(if (r.error != null) Doors.Warn else if (state != null) color else Doors.Faint)
            Spacer(Modifier.width(8.dp))
            Label(label)
        }
        Spacer(Modifier.height(8.dp))
        Text(why(r) ?: state?.replaceFirstChar { it.uppercase() } ?: "—", style = Type.BodyStrong, maxLines = 1, overflow = TextOverflow.Ellipsis)
    }
}

@Composable
private fun Room(name: String, what: String, color: Color, tag: String, onClick: () -> Unit) {
    Panel(Modifier.fillMaxWidth(), onClick = onClick) {
        Row(verticalAlignment = Alignment.CenterVertically) {
            Box(Modifier.width(3.dp).height(38.dp).clip(CircleShape).background(color))
            Spacer(Modifier.width(14.dp))
            Column(Modifier.weight(1f)) {
                Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(10.dp)) {
                    Text(name, style = Type.Title)
                    Tag(tag, color)
                }
                Text(what, style = Type.Body)
            }
            Chevron()
        }
    }
}

// ---- telemetry -------------------------------------------------------------

private fun human(key: String) = key.replace('_', ' ').replaceFirstChar { it.uppercase() }

/** Whatever a service reports, as rows: scalars as they are, one level of nesting, lists as a count. */
private fun rows(o: JsonObject, prefix: String = ""): List<Pair<String, String>> = o.entries.flatMap { (k, v) ->
    when (v) {
        is JsonNull -> listOf(human(prefix + k) to "—")
        is JsonPrimitive -> listOf(human(prefix + k) to v.content)
        is JsonObject -> if (prefix.isEmpty()) rows(v, "$k ") else emptyList()
        is JsonArray -> listOf(human(prefix + k) to "${v.size} listed")
    }
}.take(28)

@Composable
private fun Generic(title: String, r: Reading) {
    Section(title) {
        Panel(Modifier.fillMaxWidth()) {
            val list = r.data?.let { rows(it) }.orEmpty()
            why(r)?.let { Text(it, style = Type.Body, color = Doors.Warn) }
            if (list.isEmpty() && r.error == null) Text("No reading", style = Type.Body)
            list.forEach { (k, v) -> KeyValue(k, v) }
        }
    }
}

@Composable
fun TelemetryScreen(link: Link, onBack: () -> Unit) {
    val t by link.session.telemetry.collectAsStateWithLifecycle()
    val now = rememberNow()
    Page {
        TopBar("Telemetry", onBack)
        Label("Read ${age(t.updatedAtMs, now)}", Modifier.padding(start = 4.dp), color = Doors.Accent)
        Section("System") {
            Panel(Modifier.fillMaxWidth()) {
                val i = t.info.data
                why(t.info)?.let { Text(it, style = Type.Body, color = Doors.Warn) }
                KeyValue("Model", i?.string("model") ?: "—", mono = false)
                KeyValue("Doors", listOfNotNull(i?.string("version"), i?.string("build")).joinToString("  ·  ").ifEmpty { "—" })
                KeyValue("Kernel", i?.string("kernel") ?: "—")
                KeyValue("OS", i?.string("os") ?: "—", mono = false)
                KeyValue("Hostname", i?.string("hostname") ?: "—")
                KeyValue("Processors", i?.long("cpus")?.toString() ?: "—")
                val s = t.system.data
                Hairline()
                KeyValue("Uptime", uptime(s?.long("uptime_s")))
                KeyValue("Load", s?.array("load")?.joinToString("  ") { (it as? JsonPrimitive)?.content ?: "—" }?.ifEmpty { null } ?: "—")
                KeyValue("Clock", if (s?.bool("clock_set") == true) "Set" else "Not set (no time source yet)", mono = false)
            }
        }
        val s = t.system.data
        Section("Storage") {
            Panel(Modifier.fillMaxWidth()) {
                val mounts = s?.array("storage").orEmpty().filterIsInstance<JsonObject>()
                if (mounts.isEmpty()) Text("No reading", style = Type.Body)
                mounts.forEach { m ->
                    val total = m.double("total_bytes")
                    val avail = m.double("avail_bytes")
                    KeyValue(m.string("mount") ?: "?", "${bytes(avail)} free of ${bytes(total)}")
                    Meter(if (total != null && avail != null && total > 0) (1 - avail / total).toFloat() else null, Doors.Accent)
                    Spacer(Modifier.height(8.dp))
                }
            }
        }
        Section("Network") {
            Panel(Modifier.fillMaxWidth()) {
                val nets = s?.array("network").orEmpty().filterIsInstance<JsonObject>()
                if (nets.isEmpty()) Text("No reading", style = Type.Body)
                nets.forEach { n ->
                    val up = n.string("operstate") == "up"
                    KeyValue(n.string("name") ?: "?", listOfNotNull(n.string("operstate"), n.string("ipv4")).joinToString("  ·  "), valueColor = if (up) Doors.Ok else Doors.Secondary)
                }
            }
        }
        Section("Services") {
            Panel(Modifier.fillMaxWidth()) {
                val services = s?.array("services").orEmpty().filterIsInstance<JsonObject>()
                if (services.isEmpty()) Text("No reading", style = Type.Body)
                services.forEach { v ->
                    val running = v.bool("running") == true
                    val loop = v.bool("crashloop") == true
                    Row(Modifier.fillMaxWidth().padding(vertical = 7.dp), verticalAlignment = Alignment.CenterVertically) {
                        Dot(if (loop) Doors.Error else if (running) Doors.Ok else Doors.Faint)
                        Spacer(Modifier.width(12.dp))
                        Text(v.string("name") ?: "?", style = Type.Mono, modifier = Modifier.weight(1f))
                        Text(if (loop) "crash loop" else if (running) "running" else "stopped", style = Type.Body)
                    }
                }
            }
        }
        s?.obj("power")?.let { p ->
            Section("Power") {
                Panel(Modifier.fillMaxWidth()) {
                    KeyValue("Source", p.string("source") ?: "—", mono = false)
                    p.obj("battery")?.let { b ->
                        KeyValue("Battery", listOfNotNull(b.string("status"), b.double("capacity_percent")?.let { "%.0f %%".format(it) }, b.double("voltage_v")?.let { "%.2f V".format(it) }).joinToString("  ·  ").ifEmpty { "—" })
                    }
                }
            }
        }
        Generic("Wi-Fi", t.wifi)
        Generic("Radio", t.radio)
        Generic("Mesh service", t.mesh)
        Spacer(Modifier.height(12.dp))
    }
}

// ---- connection ------------------------------------------------------------

@OptIn(ExperimentalLayoutApi::class)
@Composable
fun ConnectionScreen(link: Link, unit: PairedUnit, onBack: () -> Unit) {
    val state by link.session.state.collectAsStateWithLifecycle()
    val scopes by link.session.scopes.collectAsStateWithLifecycle()
    val l = look(state, unit)
    val hello = (state as? LinkState.Online)?.hello
    val scope = rememberCoroutineScope()
    var confirm by remember { mutableStateOf(false) }
    var looking by remember { mutableStateOf(false) }
    var lookResult by remember { mutableStateOf<String?>(null) }
    val mine by produceState<Pair<String, String>?>(null) {
        value = withContext(Dispatchers.IO) {
            try {
                Fingerprint.display(link.identity.fingerprint()) to link.identity.protection()
            } catch (e: Exception) {
                null
            }
        }
    }
    Page {
        TopBar("Connection", onBack)
        Panel(Modifier.fillMaxWidth()) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Text(unit.name, style = Type.Title, modifier = Modifier.weight(1f), maxLines = 1, overflow = TextOverflow.Ellipsis)
                Pill(l.pill, l.color, l.pulse)
            }
            Spacer(Modifier.height(8.dp))
            Text(l.line, style = Type.Body)
        }
        Section("Trust") {
            Panel(Modifier.fillMaxWidth()) {
                Label("The unit's key")
                Spacer(Modifier.height(6.dp))
                Text(Fingerprint.display(unit.pin), style = Type.Figure.copy(color = Doors.Accent))
                Spacer(Modifier.height(14.dp))
                Label("This phone's key")
                Spacer(Modifier.height(6.dp))
                Text(mine?.first ?: "····  ····  ····  ····", style = Type.Figure)
                Spacer(Modifier.height(12.dp))
                Hairline()
                KeyValue("Kept in", mine?.second ?: "—", mono = false)
                KeyValue("Transport", "TLS 1.3, both keys pinned", mono = false)
                KeyValue("Route", "Wi-Fi only", mono = false)
                Text("Compare these digits with the unit's Companion page. Nothing else decides who the app talks to.", style = Type.Body, modifier = Modifier.padding(top = 8.dp))
            }
        }
        Section("Session") {
            Panel(Modifier.fillMaxWidth()) {
                KeyValue("Address", "${unit.host}:${unit.port}")
                KeyValue("Protocol", hello?.proto?.let { "v$it" } ?: "—")
                KeyValue("Doors", listOfNotNull(hello?.version, hello?.build).joinToString("  ·  ").ifEmpty { "—" })
                KeyValue("Unit clock", if (hello == null) "—" else if (hello.clockSet) "Set" else "Not set", mono = false)
                Spacer(Modifier.height(8.dp))
                Label("Granted on the unit")
                Spacer(Modifier.height(8.dp))
                FlowRow(horizontalArrangement = Arrangement.spacedBy(8.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                    if (scopes.isEmpty()) Text("—", style = Type.Body)
                    scopes.sorted().forEach { Tag(it.replace('_', ' '), Doors.Accent) }
                }
                Spacer(Modifier.height(14.dp))
                Label("Served by this unit")
                Spacer(Modifier.height(8.dp))
                FlowRow(horizontalArrangement = Arrangement.spacedBy(8.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                    if (hello == null || hello.capabilities.isEmpty()) Text("—", style = Type.Body)
                    hello?.capabilities?.sorted()?.forEach { Tag(it, Doors.Ok) }
                }
            }
        }
        Spacer(Modifier.height(6.dp))
        if (state !is LinkState.Online) {
            PrimaryButton("Try now") { link.retry() }
            if (state !is LinkState.Stopped || (state as LinkState.Stopped).why == Stop.WRONG_UNIT) {
                GhostButton(if (looking) "Looking…" else "Look for it on this network") {
                    if (!looking) scope.launch {
                        looking = true
                        lookResult = if (link.locate()) "Found at a new address. Same key." else "Not found on this network. Nothing was changed."
                        looking = false
                    }
                }
                lookResult?.let { Text(it, style = Type.Body) }
                Text("Looks once, on this Wi-Fi network only, and accepts only the key above.", style = Type.Body, color = Doors.Muted)
            }
        }
        if (!confirm) {
            GhostButton("Forget this unit", color = Doors.Error) { confirm = true }
        } else {
            Panel(Modifier.fillMaxWidth(), tint = Doors.Error.copy(alpha = 0.08f)) {
                Text("Forget ${unit.name}?", style = Type.Title)
                Text("The unit is told when it can hear. To connect again you pair again, at the unit.", style = Type.Body, modifier = Modifier.padding(top = 6.dp, bottom = 14.dp))
                Row(horizontalArrangement = Arrangement.spacedBy(12.dp)) {
                    GhostButton("Keep", Modifier.weight(1f)) { confirm = false }
                    GhostButton("Forget", Modifier.weight(1f), color = Doors.Error) { scope.launch { link.forget() } }
                }
            }
        }
        Spacer(Modifier.height(12.dp))
    }
}

// ---- rooms not built yet ---------------------------------------------------

@Composable
fun RoomPlaceholder(link: Link, name: String, color: Color, capability: String, scope: String, lines: List<String>, onBack: () -> Unit) {
    val state by link.session.state.collectAsStateWithLifecycle()
    val scopes by link.session.scopes.collectAsStateWithLifecycle()
    val hello = (state as? LinkState.Online)?.hello
    Page {
        TopBar(name, onBack)
        Panel(Modifier.fillMaxWidth(), tint = Doors.Bg) {
            Threshold(0.06f, color, Modifier.fillMaxWidth().height(170.dp), searching = true)
            Spacer(Modifier.height(10.dp))
            Text("This room is not open yet", style = Type.Title)
            Text("$name arrives in a later release of the Companion. Nothing here talks to the unit today.", style = Type.Body, modifier = Modifier.padding(top = 6.dp))
        }
        Section("What will be here") {
            Panel(Modifier.fillMaxWidth()) {
                lines.forEach {
                    Row(Modifier.padding(vertical = 6.dp), verticalAlignment = Alignment.CenterVertically) {
                        Dot(color, 6.dp)
                        Spacer(Modifier.width(12.dp))
                        Text(it, style = Type.Body, color = Doors.Text)
                    }
                }
            }
        }
        Section("On this unit") {
            Panel(Modifier.fillMaxWidth()) {
                KeyValue("Gateway serves it", if (hello == null) "—" else if (capability in hello.capabilities) "Yes" else "Not yet", mono = false)
                KeyValue("Granted to this phone", if (hello == null) "—" else if (scope in scopes) "Yes" else "No", mono = false)
            }
        }
    }
}
