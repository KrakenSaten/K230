// Copyright (c) 2026 Doors authors.
// SPDX-License-Identifier: Apache-2.0
package app.doors.companion.core

import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Job
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.launch
import kotlin.math.abs
import kotlin.math.sin
import kotlin.random.Random

/**
 * A camera that is not one: a drawn street with drawn traffic, made on the
 * phone so that the Vision screen, its overlays and its preview path can be
 * built and tested without a unit that shares a picture. No camera of the
 * phone or of any unit is opened, nothing is sent, and every view it gives
 * says [VisionView.simulated]. The "tap on the unit" is played by itself.
 *
 * It goes through the same [VisionSource] as a real transport will: pictures
 * into a conflated flow, boxes in picture pixels, the helper's figures once a
 * second. The numbers are in the range the unit has measured for each mode
 * (docs/apps/VISION.md), not measurements.
 */
class SimulatedVision(
    private val scope: CoroutineScope,
    private val now: () -> Long = System::currentTimeMillis,
    private val tickMs: Long = 100,
    seed: Long = 11,
) : VisionSource {
    private val _view = MutableStateFlow(
        VisionView(
            simulated = true, access = VisionAccess.OPEN, canControl = true, owner = CameraOwner.VISION,
            mode = VisionMode.TRACK, modes = VisionMode.entries.toSet(),
            settings = VisionSetting.entries.associateWith { it.values[if (it == VisionSetting.SEG_VIEW || it == VisionSetting.TRACK_LINE || it == VisionSetting.TRAFFIC_LINE || it == VisionSetting.TRAFFIC_RANGE || it == VisionSetting.COLOR_TOL) 1 else 0].first },
            model = "simulated detector  ·  320",
        ),
    )
    override val view: StateFlow<VisionView> = _view

    private val _frame = MutableStateFlow<VisionFrame?>(null)
    override val frame: StateFlow<VisionFrame?> = _frame

    private val rnd = Random(seed)
    private val meter = StreamMeter()
    private var job: Job? = null
    private var tick = 0L
    private var seq = 0L
    private var askedAt = 0L
    private var nextId = 1

    // Three pictures are kept and reused in turn: a reader has two frame times to copy one out.
    private val pool = Array(3) { IntArray(W * H) }
    private val classes = ByteArray(W * H)

    private class Actor(var id: Int, val cls: Int, val lane: Int, var x: Float, val speed: Float, val color: Int)

    private val actors = ArrayList<Actor>()

    override fun watch(on: Boolean) {
        job?.cancel()
        job = null
        if (!on) {
            // Leaving the screen ends a preview, here as it will on a unit.
            _frame.value = null
            _view.update { it.copy(preview = PreviewState.OFF, stream = StreamStats()) }
            return
        }
        job = scope.launch {
            while (true) {
                step()
                delay(tickMs)
            }
        }
    }

    override fun dismissNotice() = _view.update { it.copy(notice = null) }

    override suspend fun startPreview() = act {
        meter.reset()
        askedAt = now()
        _view.update { it.copy(preview = PreviewState.ASKING, previewAtMs = askedAt) }
    }

    override suspend fun stopPreview() = act {
        _frame.value = null
        _view.update { it.copy(preview = PreviewState.OFF, stream = StreamStats()) }
    }

    override suspend fun setMode(mode: VisionMode) = act {
        if (!mode.remote) _view.update { it.copy(notice = "${mode.word} is chosen on the unit only, and is never shared.") }
        else _view.update { it.copy(mode = mode, countAb = 0, countBa = 0, fpsHistory = emptyList(), inferHistory = emptyList()) }
    }

    override suspend fun set(setting: VisionSetting, value: String) = act {
        if (setting.accepts(value)) _view.update { it.copy(settings = it.settings + (setting to value)) }
    }

    override suspend fun resetCounts() = act { _view.update { it.copy(countAb = 0, countBa = 0) } }

    private suspend fun act(what: () -> Unit) {
        if (_view.value.busy) return
        _view.update { it.copy(busy = true, notice = null) }
        try {
            delay(ANSWER_MS)
            what()
        } finally {
            _view.update { it.copy(busy = false) }
        }
    }

    // ---- the made-up world ------------------------------------------------------

    private fun step() {
        tick++
        val v = _view.value
        val mode = v.mode ?: VisionMode.DETECT
        if (v.preview == PreviewState.ASKING && now() - askedAt >= TAP_AFTER_MS) {
            _view.update { it.copy(preview = PreviewState.LIVE) }
        }
        move(mode)
        val scene = VisionScene(seq + 1, W, H, boxes(mode))
        if (_view.value.preview == PreviewState.LIVE) {
            seq++
            val px = pool[(seq % pool.size).toInt()]
            draw(px, mode, v.settings)
            val f = VisionFrame(seq, W, H, FramePixels.Argb(px), now(), 60 + rnd.nextLong(50), scene.copy(seq = seq))
            _frame.value = f
            _view.update { it.copy(stream = meter.take(f, 0)) }
        }
        // What is in the picture travels with the picture, and only then.
        _view.update { it.copy(scene = if (it.preview == PreviewState.LIVE) scene else VisionScene(), readAtMs = now()) }
        if (tick % (1000 / tickMs) == 0L) stats(mode)
    }

    private fun stats(mode: VisionMode) {
        val (fps, infer) = when (mode) {
            VisionMode.SEGMENT -> 75 to 92
            VisionMode.INSTANCES -> 150 to 48
            VisionMode.TRAFFIC -> 190 to 33
            VisionMode.TRACK -> 170 to 34
            VisionMode.DETECT -> 165 to 34
            else -> 100 to 0
        }
        val s = VisionStats(
            fpsX10 = fps + rnd.nextInt(-12, 13), inferMs = if (infer == 0) 0 else infer + rnd.nextInt(-3, 4),
            preMs = 4 + rnd.nextInt(3), postMs = 2 + rnd.nextInt(2), cpuPct = 38 + rnd.nextInt(12), rssKb = 7_300 + rnd.nextLong(300),
        )
        _view.update { it.copy(stats = s, fpsHistory = (it.fpsHistory + s.fpsX10).takeLast(VisionLimits.HISTORY), inferHistory = (it.inferHistory + s.inferMs).takeLast(VisionLimits.HISTORY)) }
    }

    private fun move(mode: VisionMode) {
        if (actors.size < 5 && rnd.nextInt(14) == 0) {
            val lane = rnd.nextInt(3)
            val cls = if (lane == 2) (if (rnd.nextInt(3) == 0) BICYCLE else PERSON) else if (rnd.nextInt(6) == 0) TRUCK else CAR
            val right = lane != 1
            val speed = when (cls) {
                PERSON -> 1.1f
                BICYCLE -> 2.6f
                else -> 4.5f + rnd.nextFloat() * 3f
            } * (if (right) 1 else -1)
            actors += Actor(nextId++, cls, lane, if (right) -60f else W + 60f, speed, PAINT[rnd.nextInt(PAINT.size)])
        }
        val line = W / 2f
        val it = actors.iterator()
        while (it.hasNext()) {
            val a = it.next()
            val before = a.x
            a.x += a.speed
            if (mode.ids && (before < line) != (a.x < line) && (mode != VisionMode.TRAFFIC || a.cls != PERSON)) {
                _view.update { v -> if (a.speed > 0) v.copy(countAb = v.countAb + 1) else v.copy(countBa = v.countBa + 1) }
            }
            if (a.x < -80 || a.x > W + 80) it.remove()
        }
    }

    private fun rectOf(a: Actor): IntArray {
        val (w, h) = when (a.cls) {
            PERSON -> 12 to 34
            BICYCLE -> 26 to 26
            TRUCK -> 96 to 46
            else -> 62 to 26
        }
        val scale = if (a.lane == 0) 1.25f else if (a.lane == 1) 0.9f else 0.8f
        val bw = (w * scale).toInt()
        val bh = (h * scale).toInt()
        val base = LANE_Y[a.lane]
        return intArrayOf(a.x.toInt() - bw / 2, base - bh, bw, bh)
    }

    private fun boxes(mode: VisionMode): List<VisionBox> {
        if (!mode.boxes) return emptyList()
        return actors.filter { mode != VisionMode.TRAFFIC || it.cls != PERSON }.mapNotNull { a ->
            val r = rectOf(a)
            val x0 = r[0].coerceAtLeast(0)
            val x1 = (r[0] + r[2]).coerceAtMost(W)
            if (x1 - x0 < 8) return@mapNotNull null
            VisionBox(
                id = if (mode.ids) a.id else 0, cls = a.cls, label = LABELS[a.cls],
                confidence = (640 + (a.id * 37) % 300 + rnd.nextInt(-25, 26)).coerceIn(350, 990),
                x = x0 + rnd.nextInt(-1, 2), y = r[1] + rnd.nextInt(-1, 2), w = x1 - x0, h = r[3],
                dir = if (!mode.ids) 0 else if (a.speed > 0) 2 else 1,
                kmh10 = if (mode == VisionMode.TRAFFIC && a.cls != BICYCLE) (abs(a.speed) * 62).toInt() else 0,
            )
        }.take(VisionLimits.MAX_BOXES)
    }

    private fun fill(px: IntArray, x0: Int, y0: Int, w: Int, h: Int, color: Int, cls: Int) {
        val xa = x0.coerceIn(0, W)
        val xb = (x0 + w).coerceIn(0, W)
        for (y in y0.coerceIn(0, H) until (y0 + h).coerceIn(0, H)) {
            val row = y * W
            for (x in xa until xb) {
                px[row + x] = color
                classes[row + x] = cls.toByte()
            }
        }
    }

    private fun draw(px: IntArray, mode: VisionMode, settings: Map<VisionSetting, String>) {
        // Dusk over a street: sky, a row of buildings, pavement, two lanes.
        for (y in 0 until H) {
            val row = y * W
            val c = when {
                y < HORIZON -> shade(0xFF1B2538.toInt(), 0xFF6B5B6E.toInt(), y / HORIZON.toFloat())
                y < SIDEWALK -> 0xFF4A4744.toInt()
                else -> shade(0xFF2E2F33.toInt(), 0xFF1E1F22.toInt(), (y - SIDEWALK) / (H - SIDEWALK).toFloat())
            }
            val k = (if (y < HORIZON) SKY else if (y < SIDEWALK) WALK else ROAD).toByte()
            for (x in 0 until W) {
                px[row + x] = c
                classes[row + x] = k
            }
        }
        var bx = 0
        var i = 0
        while (bx < W) {
            val bw = 38 + (i * 53) % 44
            val bh = 46 + (i * 71) % 64
            fill(px, bx, HORIZON - bh, bw - 3, bh, if (i % 2 == 0) 0xFF2B2E3A.toInt() else 0xFF343040.toInt(), BUILDING)
            var wy = HORIZON - bh + 8
            while (wy < HORIZON - 8) {
                var wx = bx + 6
                while (wx < bx + bw - 10) {
                    if ((wx * 7 + wy * 13 + i) % 5 < 2) fill(px, wx, wy, 5, 7, 0xFFE2B866.toInt(), BUILDING)
                    wx += 11
                }
                wy += 14
            }
            bx += bw
            i++
        }
        val dash = ((tick * 6) % 48).toInt()
        var dx = -dash
        while (dx < W) {
            fill(px, dx, (LANE_Y[0] + LANE_Y[1]) / 2 - 8, 26, 3, 0xFFB9B3A6.toInt(), ROAD)
            dx += 48
        }
        actors.sortedBy { LANE_Y[it.lane] }.forEach { a ->
            val r = rectOf(a)
            val k = OBJECT0 + (a.id % 8)
            when (a.cls) {
                PERSON -> {
                    fill(px, r[0] + r[2] / 4, r[1], r[2] / 2, r[3] / 4, 0xFFD8B49A.toInt(), k)
                    fill(px, r[0], r[1] + r[3] / 4, r[2], r[3] * 3 / 4, a.color, k)
                }
                BICYCLE -> {
                    fill(px, r[0], r[1] + r[3] / 2, r[2], 3, 0xFFB9B3A6.toInt(), k)
                    fill(px, r[0] + r[2] / 3, r[1], r[2] / 3, r[3] * 2 / 3, a.color, k)
                    fill(px, r[0], r[1] + r[3] * 2 / 3, r[3] / 3, r[3] / 3, 0xFF141414.toInt(), k)
                    fill(px, r[0] + r[2] - r[3] / 3, r[1] + r[3] * 2 / 3, r[3] / 3, r[3] / 3, 0xFF141414.toInt(), k)
                }
                else -> {
                    fill(px, r[0], r[1] + r[3] / 3, r[2], r[3] * 2 / 3 - 4, a.color, k)
                    fill(px, r[0] + r[2] / 5, r[1], r[2] * 3 / 5, r[3] / 3, shade(a.color, 0xFF000000.toInt(), 0.35f), k)
                    fill(px, r[0] + r[2] / 6, r[1] + r[3] - 8, 9, 8, 0xFF101010.toInt(), k)
                    fill(px, r[0] + r[2] * 5 / 6 - 9, r[1] + r[3] - 8, 9, 8, 0xFF101010.toInt(), k)
                    fill(px, if (a.speed > 0) r[0] + r[2] - 4 else r[0], r[1] + r[3] / 2, 4, 4, 0xFFFFF0B0.toInt(), k)
                }
            }
        }
        when (mode) {
            VisionMode.SEGMENT, VisionMode.INSTANCES -> {
                val view = settings[VisionSetting.SEG_VIEW] ?: "overlay"
                if (view != "original") {
                    for (p in px.indices) {
                        val k = classes[p].toInt()
                        val tint = if (mode == VisionMode.SEGMENT) SEG[k.coerceAtMost(OBJECT0)] else if (k >= OBJECT0) TRACK_HUE[k - OBJECT0] else 0
                        if (tint != 0) px[p] = if (view == "mask") tint else shade(px[p], tint, 0.55f)
                        else if (view == "mask") px[p] = 0xFF0B0B0A.toInt()
                    }
                }
            }
            VisionMode.EDGE -> {
                val hard = settings[VisionSetting.EDGE_HARD] == "1"
                val threshold = if (hard) 46 else 18
                for (y in 0 until H - 1) {
                    val row = y * W
                    for (x in 0 until W - 1) {
                        val a = luma(px[row + x])
                        val g = abs(a - luma(px[row + x + 1])) + abs(a - luma(px[row + x + W]))
                        px[row + x] = if (g > threshold) 0xFFEEEAE2.toInt() else 0xFF0B0B0A.toInt()
                    }
                }
            }
            VisionMode.COLOR -> {
                // Everything that is not the chosen colour goes grey; the chosen one is the cars' red.
                val reach = when (settings[VisionSetting.COLOR_TOL]) {
                    "low" -> 50
                    "high" -> 150
                    else -> 95
                }
                for (p in px.indices) {
                    val c = px[p]
                    val r = (c shr 16) and 255
                    val g = (c shr 8) and 255
                    val b = c and 255
                    if (abs(r - 0xC9) + abs(g - 0x4A) + abs(b - 0x43) > reach) {
                        val l = luma(c) * 2 / 3
                        px[p] = (0xFF shl 24) or (l shl 16) or (l shl 8) or l
                    }
                }
            }
            VisionMode.TRACE -> {
                // A painted line along the near lane, and where the tracer finds it.
                val dark = settings[VisionSetting.TRACE_DARK] != "0"
                for (x in 0 until W) {
                    val y = H - 26 + (8 * sin((x + tick * 3) / 46.0)).toInt()
                    fill(px, x, y, 1, 5, if (dark) 0xFF0E0E0E.toInt() else 0xFFEDE7DA.toInt(), ROAD)
                    if (x % 24 == 0) fill(px, x - 1, y - 3, 3, 11, 0xFFD8BF94.toInt(), ROAD)
                }
            }
            else -> Unit
        }
    }

    companion object {
        const val W = 480
        const val H = 270
        const val TAP_AFTER_MS = 1_800L
        const val ANSWER_MS = 180L
        private const val HORIZON = 132
        private const val SIDEWALK = 150
        private val LANE_Y = intArrayOf(250, 205, 149)

        private const val PERSON = 0
        private const val BICYCLE = 1
        private const val CAR = 2
        private const val TRUCK = 3
        private val LABELS = listOf("person", "bicycle", "car", "truck")
        private val PAINT = intArrayOf(0xFFC94A43.toInt(), 0xFF3E6FA8.toInt(), 0xFFD9D4C8.toInt(), 0xFF2F8F6B.toInt(), 0xFFE0A63E.toInt(), 0xFF55585F.toInt())

        // Pixel classes of the drawn street, and the colours SEGMENT gives them.
        private const val SKY = 0
        private const val BUILDING = 1
        private const val WALK = 2
        private const val ROAD = 3
        private const val OBJECT0 = 4
        private val SEG = intArrayOf(0xFF4682B4.toInt(), 0xFF464646.toInt(), 0xFFF423E8.toInt(), 0xFF804080.toInt(), 0xFF00008E.toInt())

        /** The identity hues of the unit's theme: a track keeps one. Also what the screen's overlay uses. */
        val TRACK_HUE = intArrayOf(
            0xFFF2917F.toInt(), 0xFFE8AC6A.toInt(), 0xFFD9CB6E.toInt(), 0xFF9FD08A.toInt(),
            0xFF74D1C4.toInt(), 0xFF86BFF0.toInt(), 0xFFB7A8F2.toInt(), 0xFFE89DD2.toInt(),
        )

        private fun luma(c: Int): Int = (((c shr 16) and 255) * 3 + ((c shr 8) and 255) * 6 + (c and 255)) / 10

        private fun shade(a: Int, b: Int, t: Float): Int {
            val u = t.coerceIn(0f, 1f)
            fun ch(shift: Int) = (((a shr shift) and 255) * (1 - u) + ((b shr shift) and 255) * u).toInt() shl shift
            return (0xFF shl 24) or ch(16) or ch(8) or ch(0)
        }
    }
}
