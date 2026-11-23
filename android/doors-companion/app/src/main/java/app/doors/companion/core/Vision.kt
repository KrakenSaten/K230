// Copyright (c) 2026 Doors authors.
// SPDX-License-Identifier: Apache-2.0
package app.doors.companion.core

import kotlinx.coroutines.flow.StateFlow

/*
 * Vision as a paired phone may see it. This is the phone side of a PROPOSED
 * contract (docs/design/companion/VISION_PREVIEW.md): nothing on a unit
 * serves it. The camera stays with the Vision app on the unit, which owns
 * it alone (ADR-006); a phone is shown the picture Vision already makes for
 * its own screen, smaller and slower. The phone is authorized once, on the
 * unit (the scope `vision`); after that the preview needs no confirmation.
 *
 * The types here are what a screen needs, whoever fills them: the gateway
 * reader when there is one, and the labelled simulation today.
 */

/** Vision's modes, by the words of the helper protocol (docs/apps/VISION.md). */
enum class VisionMode(val wire: String, val word: String, val group: Group, val remote: Boolean = true) {
    DETECT("detect", "Detect", Group.GENERAL),
    TRACK("track", "Track", Group.GENERAL),
    INSTANCES("instances", "Instances", Group.GENERAL),
    TRAFFIC("traffic", "Traffic", Group.ROAD),
    SEGMENT("segment", "Segment", Group.ROAD),

    // The people modes are the unit's own: no picture of them leaves it, and a
    // phone can neither choose them nor see them (no biometrics over the gateway).
    FACE("face", "Face", Group.PEOPLE, remote = false),
    RECOGNIZE("recognize", "Recognize", Group.PEOPLE, remote = false),
    READ("read", "Read", Group.TEXT, remote = false),
    COLOR("color", "Color", Group.TOOLS),
    EDGE("edge", "Edge", Group.TOOLS),
    TRACE("trace", "Line trace", Group.TOOLS);

    enum class Group(val word: String) { GENERAL("General"), ROAD("Road"), PEOPLE("People"), TEXT("Text"), TOOLS("Tools") }

    /** Boxes with a class and a confidence are what this mode says. */
    val boxes: Boolean get() = this == DETECT || this == TRACK || this == TRAFFIC || this == INSTANCES

    /** Boxes carry an id that stays with the object. */
    val ids: Boolean get() = this == TRACK || this == TRAFFIC || this == INSTANCES

    companion object {
        fun of(wire: String?): VisionMode? = entries.firstOrNull { it.wire == wire }
    }
}

/**
 * A setting of a mode that a phone may change: one of a few named values,
 * exactly the ones Vision's own sheets offer and its settings file stores.
 */
enum class VisionSetting(val key: String, val word: String, val mode: VisionMode?, val values: List<Pair<String, String>>) {
    TRACK_LINE("track.line", "Counting line", VisionMode.TRACK, listOf("off" to "Off", "across" to "Across", "down" to "Down")),
    TRAFFIC_RANGE("traffic.range", "Detection range", VisionMode.TRAFFIC, listOf("near" to "Near", "normal" to "Normal", "far" to "Far")),
    TRAFFIC_LINE("traffic.line", "Count line", VisionMode.TRAFFIC, listOf("off" to "Off", "across" to "Across", "down" to "Down")),
    TRAFFIC_SPEED("traffic.speed", "Speed lines", VisionMode.TRAFFIC, listOf("off" to "Off", "narrow" to "Narrow", "wide" to "Wide")),
    COLOR_TOL("color.tol", "Tolerance", VisionMode.COLOR, listOf("low" to "Low", "med" to "Medium", "high" to "High")),
    EDGE_HARD("edge.hard", "Edges", VisionMode.EDGE, listOf("0" to "Soft", "1" to "Hard")),
    TRACE_DARK("trace.dark", "Line is", VisionMode.TRACE, listOf("1" to "Dark", "0" to "Light")),
    SEG_VIEW("seg.view", "Picture", null, listOf("original" to "Original", "overlay" to "Overlay", "mask" to "Mask"));

    /** The setting belongs on [m]'s panel. `seg.view` is shared by the two modes that draw masks. */
    fun shownIn(m: VisionMode): Boolean = if (this == SEG_VIEW) m == VisionMode.SEGMENT || m == VisionMode.INSTANCES else m == mode

    fun accepts(value: String): Boolean = values.any { it.first == value }

    companion object {
        fun of(key: String?): VisionSetting? = entries.firstOrNull { it.key == key }
    }
}

/** A track as the helper says it on a `det` line: a box in picture pixels. */
data class VisionBox(
    /** The track's id; 0 when the mode has none. */
    val id: Int,
    val cls: Int,
    val label: String,
    /** Per-mille, as the helper says it: 873 is 87.3 %. */
    val confidence: Int,
    val x: Int,
    val y: Int,
    val w: Int,
    val h: Int,
    /** 0 none, 1 left, 2 right, 3 up, 4 down, on the picture. */
    val dir: Int = 0,
    /** Tenths of km/h measured on this track, or 0. */
    val kmh10: Int = 0,
)

/** What is in one picture: its boxes, in the pixels of a picture [w] by [h]. */
data class VisionScene(val seq: Long = 0, val w: Int = 0, val h: Int = 0, val boxes: List<VisionBox> = emptyList())

/** The pixels of a preview picture, as they travel or as they were made. */
sealed interface FramePixels {
    /** A baseline JPEG, as the gateway would carry it. */
    class Jpeg(val bytes: ByteArray) : FramePixels

    /** 0xAARRGGBB, row by row: what the simulation draws. */
    class Argb(val pixels: IntArray) : FramePixels
}

/** One preview picture. Never stored: it lives until the next one replaces it. */
class VisionFrame(
    val seq: Long,
    val w: Int,
    val h: Int,
    val pixels: FramePixels,
    /** The phone's clock when it arrived. */
    val receivedAtMs: Long,
    /** How old the picture was on arrival, by the unit's own clock; null when not said. */
    val ageMs: Long?,
    /** The scene of this very picture, so that boxes and pixels agree. */
    val scene: VisionScene,
) {
    val bytes: Int get() = when (pixels) {
        is FramePixels.Jpeg -> pixels.bytes.size
        is FramePixels.Argb -> pixels.pixels.size * 4
    }
}

/** The helper's `stats` line: once a second while Vision runs. */
data class VisionStats(
    val fpsX10: Int = 0,
    val inferMs: Int = 0,
    val preMs: Int = 0,
    val postMs: Int = 0,
    val cpuPct: Int = 0,
    val rssKb: Long = 0,
    val dropped: Long = 0,
)

/** Who has the camera on the unit. It has one owner at a time (ADR-006). */
enum class CameraOwner { NONE, VISION, CAMERA, OTHER }

/** Why the picture is, or is not, there. */
enum class VisionAccess {
    /** No session with the unit. */
    OFFLINE,

    /** The unit's gateway does not serve Vision. */
    NOT_SERVED,

    /** The unit has not granted this phone `vision`. */
    NOT_GRANTED,

    /** Asked; no answer yet. */
    WAITING,

    /** Vision is not the app in front on the unit. */
    CLOSED,

    /** Vision is open; its state is being read. */
    OPEN,
}

/** Where the preview of this phone stands. It starts by itself for an authorized phone; nothing is confirmed on the unit. */
enum class PreviewState {
    /** No picture is coming to this phone. */
    OFF,

    /** Pictures arrive. */
    LIVE,

    /** Sharing, but no picture came for a while. */
    STALLED,

    /** STOP was tapped on the unit. It stays so until a person there resumes or Vision is opened again. */
    STOPPED,

    /** Another phone is being shown the picture; the unit has one viewer. */
    BUSY,
}

/** What the stream itself is doing, measured on the phone. */
data class StreamStats(
    val fpsX10: Int = 0,
    val kbps: Int = 0,
    val ageMs: Long? = null,
    /** Pictures that arrived and were never shown because a newer one was already there. */
    val skipped: Long = 0,
    val frames: Long = 0,
)

data class VisionView(
    val simulated: Boolean = false,
    val access: VisionAccess = VisionAccess.OFFLINE,
    val owner: CameraOwner = CameraOwner.NONE,
    val mode: VisionMode? = null,
    /** The modes the helper can run on this unit (its `caps` line). */
    val modes: Set<VisionMode> = emptySet(),
    val settings: Map<VisionSetting, String> = emptyMap(),
    /** The detector's name and input, as `ready` says them. */
    val model: String? = null,
    val stats: VisionStats? = null,
    /** The helper's frame rate over the last minute, oldest first, in tenths. */
    val fpsHistory: List<Int> = emptyList(),
    val inferHistory: List<Int> = emptyList(),
    /** The scene of the newest shared picture; empty while none is shared (what is in view is part of the picture). */
    val scene: VisionScene = VisionScene(),
    /** TRACK's and TRAFFIC's crossings of the line, one way and the other. */
    val countAb: Long = 0,
    val countBa: Long = 0,
    val preview: PreviewState = PreviewState.OFF,
    val previewAtMs: Long = 0,
    val stream: StreamStats = StreamStats(),
    val busy: Boolean = false,
    val notice: String? = null,
    val readAtMs: Long = 0,
)

/**
 * A source of Vision's state and of preview pictures. The screen and the
 * preview controller are written against this; the transport behind it is
 * not their business.
 */
interface VisionSource {
    val view: StateFlow<VisionView>

    /**
     * The newest picture, or null. Conflated: a reader that is slow sees the
     * newest and never a queue. Null again as soon as the preview is not live.
     */
    val frame: StateFlow<VisionFrame?>

    /** Vision's screen is in front on the phone, or no longer. Nothing is read otherwise, and a preview ends. */
    fun watch(on: Boolean)

    /** Shows the picture again after [stopPreview]. A source starts by itself when the screen comes to the front. */
    suspend fun startPreview()

    /** The person here does not want the picture for now; it is not started again until [startPreview] or the screen is opened anew. */
    suspend fun stopPreview()
    suspend fun setMode(mode: VisionMode)
    suspend fun set(setting: VisionSetting, value: String)

    /** TRACK and TRAFFIC: the counters back to zero. */
    suspend fun resetCounts()
    fun dismissNotice()
}

/** The bounds of a preview (proposed; VISION_PREVIEW.md section 6). The phone holds itself to them as well. */
object VisionLimits {
    const val MAX_SIDE = 640
    const val MAX_FRAME_BYTES = 96 * 1024
    const val MAX_FPS = 5
    const val MAX_BOXES = 24
    const val STALL_MS = 3_000L
    const val HISTORY = 60
}

/**
 * Keeps the newest picture and counts what it replaced unseen: the whole of
 * the phone's buffering. One picture is held, never a queue, so memory is
 * bounded by one frame and latency by one frame time.
 */
class FrameSlot {
    private var held: VisionFrame? = null
    private var taken = true

    var skipped: Long = 0
        private set

    @Synchronized
    fun put(f: VisionFrame) {
        if (!taken) skipped++
        held = f
        taken = false
    }

    /** The newest picture not yet taken, or null. */
    @Synchronized
    fun take(): VisionFrame? {
        if (taken) return null
        taken = true
        return held
    }

    @Synchronized
    fun clear() {
        held = null
        taken = true
    }
}

/** Frame rate, bit rate and age over the last second or so of arrivals. */
class StreamMeter(private val windowMs: Long = 2_000) {
    private val arrivals = ArrayDeque<Pair<Long, Int>>()
    private var frames = 0L

    fun reset() {
        arrivals.clear()
        frames = 0
    }

    fun take(f: VisionFrame, skipped: Long): StreamStats {
        frames++
        arrivals += f.receivedAtMs to f.bytes
        while (arrivals.size > 1 && f.receivedAtMs - arrivals.first().first > windowMs) arrivals.removeFirst()
        val span = f.receivedAtMs - arrivals.first().first
        val fps = if (span > 0) ((arrivals.size - 1) * 10_000L / span).toInt() else 0
        val kbps = if (span > 0) (arrivals.drop(1).sumOf { it.second.toLong() } * 8 / span).toInt() else 0
        return StreamStats(fps, kbps, f.ageMs, skipped, frames)
    }
}
