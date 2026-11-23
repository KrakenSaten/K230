// Copyright (c) 2026 Doors authors.
// SPDX-License-Identifier: Apache-2.0
package app.doors.companion.core

import app.doors.companion.ui.FitTransform
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.cancel
import kotlinx.coroutines.cancelAndJoin
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.launch
import kotlinx.coroutines.runBlocking
import kotlinx.coroutines.withTimeout
import kotlinx.serialization.json.Json
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.jsonObject
import org.junit.After
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test
import java.io.DataInputStream
import java.net.InetAddress
import java.net.ServerSocket
import java.net.Socket
import java.util.Base64
import java.util.concurrent.CopyOnWriteArrayList
import java.util.concurrent.atomic.AtomicReference

/**
 * Vision on the phone, to the proposed contract (VISION_PREVIEW.md):
 *
 *  - one picture is held, the newest; boxes and pixels share one transform;
 *  - nothing is asked of a unit that does not serve Vision, has not granted
 *    the scope, or while the screen is away; leaving the screen ends sharing;
 *  - an authorized phone gets the picture by itself, with nothing confirmed on
 *    the unit; a preview stopped on the unit or held by another phone is not
 *    asked for again; a pause on the phone holds;
 *  - a picture outside the bounds is not kept, and every picture is acknowledged;
 *  - the people and text modes are neither chosen nor shown from a phone;
 *  - simulated pictures always say that they are.
 *
 * The gateway is played in this JVM: no gatewayd serves these methods.
 */
class VisionTest {
    private val scope = CoroutineScope(Dispatchers.Default + Job())
    private val servers = ArrayList<ServerSocket>()

    @After
    fun close() {
        scope.cancel()
        servers.forEach { it.close() }
    }

    private fun frame(seq: Long, at: Long = seq * 100, bytes: Int = 1000) =
        VisionFrame(seq, 4, 2, FramePixels.Jpeg(ByteArray(bytes)), at, 80, VisionScene(seq, 4, 2))

    // ---- 1. the buffer and the meter ----

    @Test
    fun oneNewestPictureIsHeld() {
        val slot = FrameSlot()
        assertNull(slot.take())
        slot.put(frame(1))
        slot.put(frame(2))
        slot.put(frame(3))
        assertEquals(3, slot.take()!!.seq)
        assertEquals(2, slot.skipped)
        assertNull("a picture is taken once", slot.take())
        slot.put(frame(4))
        slot.clear()
        assertNull(slot.take())
    }

    @Test
    fun theStreamIsMeasuredOverItsLastSeconds() {
        val m = StreamMeter(windowMs = 2_000)
        var s = StreamStats()
        for (i in 1..30L) s = m.take(frame(i, at = i * 125, bytes = 2_500), 0)
        assertEquals(80, s.fpsX10)
        assertEquals(160, s.kbps)
        assertEquals(30, s.frames)
        assertEquals(80L, s.ageMs)
        m.reset()
        assertEquals(0, m.take(frame(1), 0).fpsX10)
    }

    // ---- 2. where a box lands on the picture ----

    @Test
    fun boxesAndPixelsShareOneTransform() {
        // A wide picture in a tall space, whole: bars above and below.
        val fit = FitTransform.of(480, 270, 1080f, 1920f, fill = false)
        assertEquals(2.25f, fit.scale, 0.001f)
        assertEquals(0f, fit.x(0f), 0.01f)
        assertEquals((1920f - 270 * 2.25f) / 2, fit.y(0f), 0.01f)
        assertEquals(1080f, fit.x(480f), 0.01f)
        // The same picture covering the space: the sides are cut, the height is exact.
        val cover = FitTransform.of(480, 270, 1080f, 1920f, fill = true)
        assertEquals(1920f / 270, cover.scale, 0.001f)
        assertEquals(0f, cover.y(0f), 0.01f)
        assertEquals(1920f, cover.y(270f), 0.01f)
        assertEquals(540f, cover.x(240f), 0.01f)
        // Nothing to place is nothing, not a division by zero.
        assertEquals(0f, FitTransform.of(0, 0, 100f, 100f, false).scale)
    }

    // ---- 3. modes and settings ----

    @Test
    fun peopleAndTextStayOnTheUnit() {
        assertEquals(setOf(VisionMode.FACE, VisionMode.RECOGNIZE, VisionMode.READ), VisionMode.entries.filter { !it.remote }.toSet())
        assertEquals(VisionMode.TRAFFIC, VisionMode.of("traffic"))
        assertNull(VisionMode.of("spy"))
        assertTrue(VisionSetting.SEG_VIEW.shownIn(VisionMode.SEGMENT) && VisionSetting.SEG_VIEW.shownIn(VisionMode.INSTANCES))
        assertFalse(VisionSetting.SEG_VIEW.shownIn(VisionMode.TRACK))
        assertTrue(VisionSetting.TRAFFIC_RANGE.accepts("far"))
        assertFalse(VisionSetting.TRAFFIC_RANGE.accepts("everything"))
    }

    // ---- 4. what is asked of a unit ----

    private fun json(s: String): JsonObject = Json.parseToJsonElement(s).jsonObject

    private inner class FakeUnit(private val answers: (String, JsonObject) -> String?) {
        val asked = CopyOnWriteArrayList<Pair<String, JsonObject>>()
        private val server = ServerSocket(0, 4, InetAddress.getLoopbackAddress()).also { servers += it }

        @Volatile
        private var peer: Socket? = null

        init {
            Thread {
                try {
                    val s = server.accept()
                    peer = s
                    val i = DataInputStream(s.getInputStream())
                    while (true) {
                        val o = json(ByteArray(i.readInt()).also { i.readFully(it) }.toString(Charsets.UTF_8))
                        val method = o.string("method") ?: continue
                        val params = o.obj("params") ?: JsonObject(emptyMap())
                        asked += method to params
                        val a = answers(method, params) ?: continue
                        write(if (a.startsWith("!")) """{"id":${o.long("id")},"error":{"code":${a.drop(1)},"message":"x"}}""" else """{"id":${o.long("id")},"result":$a}""")
                    }
                } catch (_: Exception) {
                }
            }.apply { isDaemon = true }.start()
        }

        fun write(frame: String) {
            val b = frame.toByteArray()
            synchronized(this) {
                peer!!.getOutputStream().apply {
                    write(byteArrayOf((b.size ushr 24).toByte(), (b.size ushr 16).toByte(), (b.size ushr 8).toByte(), b.size.toByte()) + b)
                    flush()
                }
            }
        }

        fun connect() = GatewayConnection(Socket(InetAddress.getLoopbackAddress(), server.localPort))
        fun methods() = asked.map { it.first }
    }

    private fun hello(vararg capabilities: String) = Hello(1, "00", "u", true, null, null, null, emptySet(), capabilities.toSet(), false, null, null)
    private val paired = PairedUnit("00".repeat(32), "u", "127.0.0.1", 1, 0)

    private fun status(mode: String = "track", share: String = "off") =
        """{"available":true,"owner":"vision","mode":"$mode","modes":["detect","track","traffic","face"],"share":"$share",
           "settings":{"track.line":"across","traffic.range":"wide open"},"model":"det-r0","labels":["person","car"],
           "stats":{"fps_x10":172,"infer_ms":34,"pre_ms":5,"post_ms":2,"cpu_pct":44,"rss_kb":7300,"count_ab":3,"count_ba":1}}"""

    private fun serving(share: () -> String = { "off" }, mode: () -> String = { "track" }): (String, JsonObject) -> String? = { method, params ->
        when (method) {
            "gateway.subscribe" -> """{"topics":${params["topics"]}}"""
            "vision.status" -> status(mode(), share())
            "vision.share" -> """{"state":"${if (params.bool("on") == true) "live" else "off"}"}"""
            "vision.ack", "vision.mode", "vision.set", "vision.reset" -> "{}"
            else -> "!1"
        }
    }

    private suspend fun await(what: String, check: () -> Boolean) {
        withTimeout(5_000) { while (!check()) delay(10) }
        assertTrue(what, check())
    }

    private fun picture(seq: Long, w: Int = 4, h: Int = 2, bytes: Int = 64) =
        """{"event":"vision.frame","data":{"seq":$seq,"w":$w,"h":$h,"age_ms":90,"jpeg":"${Base64.getEncoder().encodeToString(ByteArray(bytes))}",
           "boxes":[[7,1,873,1,0,2,2,2,412],[1,2,3],["x",1,1,1,1,1,1,1,1]]}}"""

    @Test
    fun aUnitThatDoesNotServeVisionIsAskedNothing() = runBlocking {
        val unit = FakeUnit(serving())
        val vision = GatewayVision(pollMs = 50)
        vision.watch(true)
        val job = scope.launch { vision.run(paired, unit.connect(), hello("status", "mesh", "capture"), MutableStateFlow(setOf("vision"))) }
        await("not served") { vision.view.value.access == VisionAccess.NOT_SERVED }
        vision.startPreview()
        vision.setMode(VisionMode.DETECT)
        delay(150)
        assertEquals(emptyList<String>(), unit.methods())
        assertNotNull(vision.view.value.notice)
        assertNull(vision.frame.value)
        job.cancelAndJoin()
        assertEquals(VisionAccess.OFFLINE, vision.view.value.access)
    }

    @Test
    fun withoutTheScopeOrOffScreenNothingIsReadAndLeavingEndsSharing() = runBlocking {
        var share = "off"
        val unit = FakeUnit(serving({ share }))
        val scopes = MutableStateFlow(setOf("status"))
        val vision = GatewayVision(pollMs = 5_000)
        vision.watch(true)
        val job = scope.launch { vision.run(paired, unit.connect(), hello("vision"), scopes) }
        await("not granted") { vision.view.value.access == VisionAccess.NOT_GRANTED }
        vision.watch(false)
        scopes.value = setOf("vision")
        await("waiting") { vision.view.value.access == VisionAccess.WAITING }
        delay(120)
        assertEquals(emptyList<String>(), unit.methods())

        vision.watch(true)
        // The figures are written just after the state: wait for both.
        await("open") { vision.view.value.access == VisionAccess.OPEN && vision.view.value.stats != null }
        val v = vision.view.value
        assertEquals(VisionMode.TRACK, v.mode)
        assertEquals(setOf(VisionMode.DETECT, VisionMode.TRACK, VisionMode.TRAFFIC, VisionMode.FACE), v.modes)
        assertEquals(CameraOwner.VISION, v.owner)
        assertEquals("across", v.settings[VisionSetting.TRACK_LINE])
        assertNull("a value Vision does not offer is not shown", v.settings[VisionSetting.TRAFFIC_RANGE])
        assertEquals(172, v.stats?.fpsX10)
        assertEquals(3, v.countAb)
        assertFalse(v.simulated)

        // The picture was asked for by itself, once, and nothing waits for a tap anywhere.
        await("asked by itself") { unit.asked.count { it.first == "vision.share" && it.second.bool("on") == true } == 1 }
        await("live") { vision.view.value.preview == PreviewState.LIVE }
        share = "live"
        // One permission: a mode is sent with the same scope.
        vision.setMode(VisionMode.DETECT)
        assertEquals("detect", unit.asked.last { it.first == "vision.mode" }.second.string("mode"))
        unit.write(picture(1))
        await("a picture") { vision.frame.value?.seq == 1L }
        await("acknowledged") { unit.asked.any { it.first == "vision.ack" && it.second.long("seq") == 1L } }
        val f = vision.frame.value!!
        assertEquals(1, f.scene.boxes.size)
        assertEquals(VisionBox(7, 1, "car", 873, 1, 0, 2, 2, 2, 412), f.scene.boxes[0])
        assertEquals(PreviewState.LIVE, vision.view.value.preview)

        // Too large, by its own word or by its bytes: acknowledged, never kept.
        unit.write(picture(2, w = 4000))
        unit.write(picture(3, bytes = VisionLimits.MAX_FRAME_BYTES + 4096))
        await("both acknowledged") { unit.asked.count { it.first == "vision.ack" } == 3 }
        assertEquals(1L, vision.frame.value?.seq)

        // The screen goes: sharing is ended on the unit, the topic given back, the picture let go.
        vision.watch(false)
        await("ended") { unit.asked.any { it.first == "vision.share" && it.second.bool("on") == false } }
        await("topic given back") { unit.asked.last().let { it.first == "gateway.subscribe" && "vision" !in it.second.strings("topics") } }
        assertNull(vision.frame.value)
        job.cancelAndJoin()
    }

    private fun shareRequests(unit: FakeUnit) = unit.asked.count { it.first == "vision.share" && it.second.bool("on") == true }

    @Test
    fun aPreviewStoppedOnTheUnitOrHeldByAnotherPhoneIsNotAskedForAgain() = runBlocking {
        // A unit that keeps its own word: sharing is what the last request made it, unless it is told otherwise here.
        val share = AtomicReference("stopped")
        val base = serving({ share.get() })
        val unit = FakeUnit { method, params ->
            if (method == "vision.share") {
                share.set(if (params.bool("on") == true) "live" else "off")
                """{"state":"${share.get()}"}"""
            } else base(method, params)
        }
        val vision = GatewayVision(pollMs = 40)
        vision.watch(true)
        val job = scope.launch { vision.run(paired, unit.connect(), hello("vision"), MutableStateFlow(setOf("vision"))) }
        await("stopped on the unit") { vision.view.value.preview == PreviewState.STOPPED }
        share.set("busy")
        await("another viewer") { vision.view.value.preview == PreviewState.BUSY }
        delay(200)
        assertEquals("nothing is asked against a STOP or another viewer", 0, shareRequests(unit))
        // Resumed on the unit: the phone asks again by itself.
        share.set("off")
        await("asked again") { shareRequests(unit) == 1 }
        await("live") { vision.view.value.preview == PreviewState.LIVE }

        // Paused here: told to the unit, and not started again by a later reading.
        vision.stopPreview()
        await("the unit is told") { unit.asked.any { it.first == "vision.share" && it.second.bool("on") == false } }
        delay(200)
        assertEquals(PreviewState.OFF, vision.view.value.preview)
        assertEquals(1, shareRequests(unit))
        vision.startPreview()
        await("live again") { vision.view.value.preview == PreviewState.LIVE }
        delay(200)
        assertEquals("asked once more, by the tap or by the reader, never by both", 2, shareRequests(unit))
        job.cancelAndJoin()
    }

    @Test
    fun aUnitThatRefusesIsAskedOncePerState() = runBlocking {
        var mode = "track"
        val base = serving(mode = { mode })
        val unit = FakeUnit { method, params -> if (method == "vision.share" && params.bool("on") == true) "!3" else base(method, params) }
        val vision = GatewayVision(pollMs = 40)
        vision.watch(true)
        val job = scope.launch { vision.run(paired, unit.connect(), hello("vision"), MutableStateFlow(setOf("vision"))) }
        await("asked") { shareRequests(unit) == 1 }
        delay(250)
        assertEquals("a refusal is not answered with a stream of requests", 1, shareRequests(unit))
        assertEquals(PreviewState.OFF, vision.view.value.preview)
        mode = "detect"
        await("asked again in the new state") { shareRequests(unit) == 2 }
        job.cancelAndJoin()
    }

    @Test
    fun aModeThatIsNeverSharedShowsNoPictureAndCannotBeChosen() = runBlocking {
        val unit = FakeUnit(serving({ "live" }, { "face" }))
        val vision = GatewayVision(pollMs = 5_000)
        vision.watch(true)
        val job = scope.launch { vision.run(paired, unit.connect(), hello("vision"), MutableStateFlow(setOf("vision"))) }
        await("open") { vision.view.value.access == VisionAccess.OPEN }
        assertEquals(VisionMode.FACE, vision.view.value.mode)
        // A unit that sent a picture of a people mode all the same: it is not kept.
        unit.write(picture(1))
        await("acknowledged") { unit.methods().contains("vision.ack") }
        assertNull(vision.frame.value)
        vision.setMode(VisionMode.RECOGNIZE)
        vision.setMode(VisionMode.SEGMENT)
        assertFalse("neither a people mode nor one without a model is sent", unit.methods().contains("vision.mode"))
        vision.setMode(VisionMode.TRAFFIC)
        assertEquals("traffic", unit.asked.last { it.first == "vision.mode" }.second.string("mode"))
        vision.set(VisionSetting.TRAFFIC_RANGE, "everything")
        assertFalse(unit.methods().contains("vision.set"))
        vision.set(VisionSetting.TRAFFIC_RANGE, "far")
        assertEquals(setOf("key", "value"), unit.asked.last { it.first == "vision.set" }.second.keys)
        job.cancelAndJoin()
    }

    // ---- 5. the simulation ----

    @Test
    fun simulatedPicturesAlwaysSaySoAndStartByThemselves() = runBlocking {
        val sim = SimulatedVision(scope, tickMs = 10)
        assertTrue(sim.view.value.simulated)
        assertNull("no picture while the screen is away", sim.frame.value)
        sim.watch(true)
        assertEquals(PreviewState.LIVE, sim.view.value.preview)
        val f = withTimeout(8_000) { sim.frame.first { it != null && it.seq >= 3 } }!!
        assertEquals(SimulatedVision.W, f.w)
        assertEquals(SimulatedVision.W * SimulatedVision.H, (f.pixels as FramePixels.Argb).pixels.size)
        assertTrue(sim.view.value.simulated && sim.view.value.preview == PreviewState.LIVE)
        assertTrue(f.scene.boxes.all { it.x >= -2 && it.x + it.w <= SimulatedVision.W + 2 && it.confidence in 0..1000 })
        sim.setMode(VisionMode.FACE)
        assertEquals(VisionMode.TRACK, sim.view.value.mode)
        assertNotNull(sim.view.value.notice)
        sim.setMode(VisionMode.SEGMENT)
        assertEquals(VisionMode.SEGMENT, sim.view.value.mode)
        // A pause holds until the picture is asked for again.
        sim.stopPreview()
        assertNull(sim.frame.value)
        assertEquals(PreviewState.OFF, sim.view.value.preview)
        sim.startPreview()
        assertEquals(PreviewState.LIVE, sim.view.value.preview)
        // Leaving the screen ends the preview.
        sim.watch(false)
        assertNull(sim.frame.value)
        assertEquals(PreviewState.OFF, sim.view.value.preview)
    }
}
