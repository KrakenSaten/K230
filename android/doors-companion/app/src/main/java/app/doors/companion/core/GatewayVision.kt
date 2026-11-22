// Copyright (c) 2026 Doors authors.
// SPDX-License-Identifier: Apache-2.0
package app.doors.companion.core

import kotlinx.coroutines.CoroutineStart
import kotlinx.coroutines.NonCancellable
import kotlinx.coroutines.TimeoutCancellationException
import kotlinx.coroutines.awaitCancellation
import kotlinx.coroutines.channels.Channel
import kotlinx.coroutines.coroutineScope
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.collectLatest
import kotlinx.coroutines.flow.combine
import kotlinx.coroutines.flow.distinctUntilChanged
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import kotlinx.coroutines.withTimeoutOrNull
import kotlinx.serialization.json.JsonArray
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.JsonPrimitive
import kotlinx.serialization.json.buildJsonObject
import kotlinx.serialization.json.intOrNull
import kotlinx.serialization.json.put
import java.io.IOException
import java.util.Base64
import java.util.concurrent.atomic.AtomicReference

/**
 * Vision over a live session, to the PROPOSED contract of
 * docs/design/companion/VISION_PREVIEW.md section 5. No gatewayd names the
 * `vision` capability, so against every unit that exists this stays at
 * [VisionAccess.NOT_SERVED] and sends nothing; it is here so that the screen
 * is written against the transport's real shape, and so that the contract can
 * be reviewed with a client beside it.
 *
 * Its rules, each of which the proposal also puts on the unit:
 *  - nothing is asked without the capability, `vision_view`, and the screen in front;
 *  - a picture is asked for only by a person's tap, and given up when the screen goes;
 *  - one picture is acknowledged before the next is sent (the gateway's window is one);
 *  - a picture larger than the bounds, or of a mode that is never shared, is not kept.
 */
class GatewayVision(
    private val now: () -> Long = System::currentTimeMillis,
    private val pollMs: Long = POLL_MS,
) : SessionFeature, VisionSource {
    private val _view = MutableStateFlow(VisionView())
    override val view: StateFlow<VisionView> = _view

    private val _frame = MutableStateFlow<VisionFrame?>(null)
    override val frame: StateFlow<VisionFrame?> = _frame

    private val watching = MutableStateFlow(false)
    private val meter = StreamMeter()
    private var labels: List<String> = emptyList()

    @Volatile
    private var live: GatewayConnection? = null

    override fun watch(on: Boolean) {
        watching.value = on
    }

    override fun dismissNotice() = _view.update { it.copy(notice = null) }

    override suspend fun run(unit: PairedUnit, conn: GatewayConnection, hello: Hello, scopes: StateFlow<Set<String>>) {
        try {
            if (CAPABILITY !in hello.capabilities) {
                _view.value = VisionView(access = VisionAccess.NOT_SERVED)
                awaitCancellation()
            }
            live = conn
            combine(scopes, watching) { s, w -> Triple(SCOPE_VIEW in s, SCOPE_VIEW in s && SCOPE_CONTROL in s, w) }
                .distinctUntilChanged()
                .collectLatest { (granted, control, shown) ->
                    _frame.value = null
                    when {
                        !granted -> _view.value = VisionView(access = VisionAccess.NOT_GRANTED)
                        !shown -> _view.value = VisionView(access = VisionAccess.WAITING, canControl = control)
                        else -> {
                            _view.value = VisionView(access = VisionAccess.WAITING, canControl = control)
                            read(conn)
                        }
                    }
                }
        } catch (_: IOException) {
        } finally {
            live = null
            _frame.value = null
            _view.value = VisionView()
        }
    }

    private suspend fun read(conn: GatewayConnection) = coroutineScope {
        val inbox = Channel<GatewayEvent>(Channel.UNLIMITED)
        // One pending picture, the newest: a unit that ignored the window of one could not fill the phone with them.
        val newest = AtomicReference<GatewayEvent?>(null)
        val pump = launch(start = CoroutineStart.UNDISPATCHED) {
            conn.events.collect {
                if (it.name == "vision.frame") {
                    if (newest.getAndSet(it) == null) inbox.trySend(it)
                } else if (it.name.startsWith("vision.")) inbox.trySend(it)
            }
        }
        try {
            var subscribed = false
            while (true) {
                try {
                    if (!subscribed) subscribed = TOPIC in conn.subscribe(add = setOf(TOPIC), timeoutMs = CALL_MS).result.strings("topics")
                    status(conn.call("vision.status", timeoutMs = CALL_MS).result)
                } catch (e: GatewayError) {
                    when (e.code) {
                        GatewayError.NOT_PERMITTED -> _view.value = VisionView(access = VisionAccess.NOT_GRANTED)
                        GatewayError.UNKNOWN_METHOD, GatewayError.NOT_SUPPORTED, GatewayError.UPSTREAM -> closed()
                        else -> Unit
                    }
                } catch (_: TimeoutCancellationException) {
                }
                var e = withTimeoutOrNull(pollMs) { inbox.receive() }
                while (e != null && e.name != "vision.state") {
                    when (e.name) {
                        "vision.frame" -> newest.getAndSet(null)?.let { picture(conn, it.data) }
                        "vision.stats" -> stats(e.data)
                    }
                    stalled()
                    e = withTimeoutOrNull(pollMs) { inbox.receive() }
                }
                stalled()
            }
        } finally {
            pump.cancel()
            _frame.value = null
            // Leaving the screen ends the sharing on the unit as well, if it can still hear.
            withContext(NonCancellable) {
                try {
                    if (_view.value.preview != PreviewState.OFF) conn.call("vision.share", buildJsonObject { put("on", false) }, 2_000)
                    conn.subscribe(remove = setOf(TOPIC), timeoutMs = 2_000)
                } catch (_: IOException) {
                } catch (_: TimeoutCancellationException) {
                }
            }
        }
    }

    private fun closed() {
        _frame.value = null
        _view.update { VisionView(access = VisionAccess.CLOSED, canControl = it.canControl) }
    }

    private fun status(o: JsonObject) {
        if (o.bool("available") != true) return closed()
        labels = o.array("labels").mapNotNull { (it as? JsonPrimitive)?.takeIf { p -> p.isString }?.content?.take(24) }.take(MAX_LABELS)
        val mode = VisionMode.of(o.string("mode"))
        val share = when (o.string("share")) {
            "asking" -> PreviewState.ASKING
            "live" -> PreviewState.LIVE
            "refused" -> PreviewState.REFUSED
            "ended" -> PreviewState.ENDED
            else -> PreviewState.OFF
        }
        if (share != PreviewState.LIVE) _frame.value = null
        val settings = o.obj("settings")
        _view.update { v ->
            v.copy(
                access = VisionAccess.OPEN,
                owner = when (o.string("owner")) {
                    "vision" -> CameraOwner.VISION
                    "camera" -> CameraOwner.CAMERA
                    "none", null -> CameraOwner.NONE
                    else -> CameraOwner.OTHER
                },
                mode = mode,
                modes = o.strings("modes").mapNotNull { VisionMode.of(it) }.toSet(),
                settings = VisionSetting.entries.mapNotNull { s -> settings?.string(s.key)?.takeIf { s.accepts(it) }?.let { s to it } }.toMap(),
                model = o.string("model")?.take(48),
                // A stalled preview stays stalled until a picture says otherwise.
                preview = if (share == PreviewState.LIVE && v.preview == PreviewState.STALLED) v.preview else share,
                previewAtMs = if (share == PreviewState.ASKING && v.preview != PreviewState.ASKING) now() else v.previewAtMs,
                stream = if (share == PreviewState.LIVE) v.stream else StreamStats(),
                readAtMs = now(),
            )
        }
        o.obj("stats")?.let { stats(it) }
    }

    private fun stats(o: JsonObject) {
        fun n(key: String) = (o.long(key) ?: 0).coerceIn(0, 10_000_000)
        val s = VisionStats(n("fps_x10").toInt(), n("infer_ms").toInt(), n("pre_ms").toInt(), n("post_ms").toInt(), n("cpu_pct").coerceAtMost(100).toInt(), n("rss_kb"), n("dropped"))
        _view.update {
            it.copy(
                stats = s, countAb = n("count_ab"), countBa = n("count_ba"),
                fpsHistory = (it.fpsHistory + s.fpsX10).takeLast(VisionLimits.HISTORY), inferHistory = (it.inferHistory + s.inferMs).takeLast(VisionLimits.HISTORY),
            )
        }
    }

    /** One picture: checked against the bounds, kept as the newest, and acknowledged so that the next may come. */
    private suspend fun picture(conn: GatewayConnection, o: JsonObject) {
        val seq = o.long("seq") ?: return
        try {
            val v = _view.value
            val w = (o.long("w") ?: 0).toInt()
            val h = (o.long("h") ?: 0).toInt()
            val text = o.string("jpeg") ?: return
            if (v.preview !in SHARING || v.mode?.remote != true) return
            if (w !in 1..VisionLimits.MAX_SIDE || h !in 1..VisionLimits.MAX_SIDE || text.length > VisionLimits.MAX_FRAME_BYTES * 4 / 3 + 4) return
            val bytes = try {
                Base64.getDecoder().decode(text)
            } catch (_: IllegalArgumentException) {
                return
            }
            val boxes = o.array("boxes").take(VisionLimits.MAX_BOXES).mapNotNull { row ->
                val a = (row as? JsonArray)?.map { (it as? JsonPrimitive)?.takeIf { p -> !p.isString }?.intOrNull ?: return@mapNotNull null } ?: return@mapNotNull null
                if (a.size < 9) return@mapNotNull null
                VisionBox(
                    id = a[0].coerceAtLeast(0), cls = a[1].coerceAtLeast(0), label = labels.getOrNull(a[1]).orEmpty(), confidence = a[2].coerceIn(0, 1000),
                    x = a[3], y = a[4], w = a[5].coerceAtLeast(0), h = a[6].coerceAtLeast(0), dir = a[7].coerceIn(0, 4), kmh10 = a[8].coerceIn(0, 5000),
                )
            }
            val f = VisionFrame(seq, w, h, FramePixels.Jpeg(bytes), now(), o.long("age_ms")?.coerceIn(0, 60_000), VisionScene(seq, w, h, boxes))
            _frame.value = f
            val stream = meter.take(f, 0)
            _view.update { it.copy(preview = PreviewState.LIVE, scene = f.scene, stream = stream, readAtMs = now()) }
        } finally {
            // Acknowledged whether or not it was kept (the unit's window is one picture), but not
            // by a phone that stopped: without acknowledgements the unit ends the share by itself.
            if (_view.value.preview != PreviewState.OFF) try {
                conn.call("vision.ack", buildJsonObject { put("seq", seq) }, CALL_MS)
            } catch (_: IOException) {
            } catch (_: TimeoutCancellationException) {
            }
        }
    }

    private fun stalled() = _view.update { v ->
        val last = _frame.value?.receivedAtMs ?: v.previewAtMs
        if (v.preview == PreviewState.LIVE && now() - last > VisionLimits.STALL_MS) v.copy(preview = PreviewState.STALLED) else v
    }

    override suspend fun startPreview() = command(needControl = false) { conn ->
        val v = _view.value
        if (v.owner != CameraOwner.VISION) return@command "Vision does not have the camera on the unit."
        if (v.mode?.remote == false) return@command "${v.mode.word} is never shared."
        meter.reset()
        val state = conn.call("vision.share", buildJsonObject { put("on", true) }, CALL_MS).result.string("state")
        _view.update { it.copy(preview = if (state == "live") PreviewState.LIVE else PreviewState.ASKING, previewAtMs = now()) }
        null
    }

    /** Stopping is never refused and never waits for another command: the picture goes at once, the unit is told. */
    override suspend fun stopPreview() {
        val conn = live
        _frame.value = null
        _view.update { it.copy(preview = PreviewState.OFF, stream = StreamStats(), scene = VisionScene(), notice = null) }
        if (conn == null) return
        try {
            conn.call("vision.share", buildJsonObject { put("on", false) }, CALL_MS)
        } catch (e: IOException) {
            _view.update { it.copy(notice = "The unit did not confirm. This phone no longer answers its pictures, so the unit stops within seconds.") }
        } catch (e: TimeoutCancellationException) {
            _view.update { it.copy(notice = "The unit did not confirm. This phone no longer answers its pictures, so the unit stops within seconds.") }
        }
    }

    override suspend fun setMode(mode: VisionMode) = command(needControl = true) { conn ->
        if (!mode.remote) return@command "${mode.word} is chosen on the unit only."
        if (mode !in _view.value.modes) return@command "This unit has no model for ${mode.word}."
        conn.call("vision.mode", buildJsonObject { put("mode", mode.wire) }, CALL_MS)
        null
    }

    override suspend fun set(setting: VisionSetting, value: String) = command(needControl = true) { conn ->
        if (!setting.accepts(value)) return@command "That is not a value Vision offers."
        conn.call("vision.set", buildJsonObject {
            put("key", setting.key)
            put("value", value)
        }, CALL_MS)
        _view.update { it.copy(settings = it.settings + (setting to value)) }
        null
    }

    override suspend fun resetCounts() = command(needControl = true) { conn ->
        conn.call("vision.reset", timeoutMs = CALL_MS)
        null
    }

    private suspend fun command(needControl: Boolean, send: suspend (GatewayConnection) -> String?) {
        val conn = live
        val v = _view.value
        if (v.busy) return
        if (conn == null || v.access != VisionAccess.OPEN) {
            _view.update { it.copy(notice = "Vision is not readable on the unit right now.") }
            return
        }
        if (needControl && !v.canControl) {
            _view.update { it.copy(notice = "The unit has not granted this phone Vision control.") }
            return
        }
        _view.update { it.copy(busy = true, notice = null) }
        var notice: String? = null
        try {
            notice = try {
                send(conn)
            } catch (e: GatewayError) {
                when (e.code) {
                    GatewayError.POLICY -> "The unit refused it."
                    GatewayError.NOT_PERMITTED -> "Not granted to this phone on the unit."
                    GatewayError.RATE -> "Too many requests. Wait a little."
                    GatewayError.BUSY -> "Another phone is being shown the picture."
                    else -> "The unit did not take it."
                }
            } catch (_: TimeoutCancellationException) {
                "The unit did not answer."
            } catch (_: IOException) {
                "The connection was lost."
            }
        } finally {
            _view.update { it.copy(busy = false, notice = notice) }
        }
    }

    companion object {
        const val CAPABILITY = "vision"
        const val SCOPE_VIEW = "vision_view"
        const val SCOPE_CONTROL = "vision_control"
        const val TOPIC = "vision"
        const val POLL_MS = 3_000L
        const val CALL_MS = 5_000L
        const val MAX_LABELS = 400
        private val SHARING = setOf(PreviewState.ASKING, PreviewState.LIVE, PreviewState.STALLED)
    }
}
