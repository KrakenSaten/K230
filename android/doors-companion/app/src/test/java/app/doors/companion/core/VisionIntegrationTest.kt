// Copyright (c) 2026 Doors authors.
// SPDX-License-Identifier: Apache-2.0
package app.doors.companion.core

import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.async
import kotlinx.coroutines.cancelAndJoin
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch
import kotlinx.coroutines.runBlocking
import kotlinx.coroutines.withContext
import kotlinx.coroutines.withTimeout
import kotlinx.serialization.json.Json
import kotlinx.serialization.json.JsonArray
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.JsonPrimitive
import kotlinx.serialization.json.buildJsonObject
import kotlinx.serialization.json.put
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Assume.assumeTrue
import org.junit.Test
import java.io.BufferedReader
import java.io.File
import java.io.InputStreamReader
import java.net.InetSocketAddress
import java.net.Socket

/**
 * The Vision room's reader against the real gatewayd in front of the real
 * shell with the real Vision app and the real pos-vision (the simulator, on
 * the fake camera), through the harness (harness/run-gateway.sh with
 * DOORS_SHELL_BIN and DOORS_VISION_HELPER naming a pos-vision built with
 * libjpeg). Skipped without a harness, without a shell behind it, or without
 * a Vision helper.
 *
 * What travels here is a real JPEG made by the helper from the picture it
 * made for the unit's screen: helper, shared memory, Vision app, shell
 * socket, gatewayd, TLS, this client. Only the camera is not one.
 *
 * A "tap" is the simulator's own finger on its panel: STOP and ALLOW are a
 * person at the unit.
 */
class VisionIntegrationTest {
    private val ctlAddr = System.getProperty("doors.gateway.ctl").orEmpty()
    private val keep = System.getProperty("doors.vision.keep").orEmpty()

    private val transport = object : Transport {
        override suspend fun connect(host: String, port: Int, timeoutMs: Int): Socket = withContext(Dispatchers.IO) {
            var last: java.io.IOException? = null
            repeat(8) {
                try {
                    return@withContext Socket().apply { connect(InetSocketAddress(host, port), timeoutMs) }
                } catch (e: java.net.ConnectException) {
                    last = e
                    Thread.sleep(400)
                }
            }
            throw last!!
        }

        override suspend fun probe(port: Int): List<String> = emptyList()
        override fun available() = true
    }

    private fun ctl(method: String, params: JsonObject? = null): JsonObject {
        val (h, p) = ctlAddr.split(":")
        Socket(h, p.toInt()).use { s ->
            s.soTimeout = 30_000
            s.getOutputStream().write((buildJsonObject {
                put("method", method)
                if (params != null) put("params", params)
            }.toString() + "\n").toByteArray())
            val out = Json.parseToJsonElement(BufferedReader(InputStreamReader(s.getInputStream())).readLine()) as JsonObject
            if (out["error"] != null) throw AssertionError("$method: $out")
            return out
        }
    }

    private fun unit(method: String, params: JsonObject? = null): JsonObject =
        ctl("harness.unit", buildJsonObject {
            put("method", method)
            if (params != null) put("params", params)
        }).obj("result")!!

    private fun tap(text: String) = unit("shell.tap", buildJsonObject { put("text", text) }).bool("tapped") == true
    private fun unitShare() = unit("vision.status").string("share")

    private suspend fun until(what: String, ms: Long = 20_000, cond: () -> Boolean) {
        val end = System.currentTimeMillis() + ms
        while (!cond()) {
            if (System.currentTimeMillis() > end) throw AssertionError("never: $what")
            delay(50)
        }
    }

    private class Phone(val session: GatewaySession, val vision: GatewayVision, val fingerprint: String) {
        val view get() = vision.view.value
        val frame get() = vision.frame.value
    }

    private suspend fun pairPhone(scope: CoroutineScope, name: String, scopes: List<String>): Pair<Phone, PairedUnit> {
        val id = TestIdentity.create()
        val vision = GatewayVision(pollMs = 1_000)
        val session = GatewaySession(transport, id, "test", features = listOf(vision), backoff = { 300 })
        val code = PairingUri.parse(ctl("gateway.pair_begin").obj("result")!!.string("uri")!!)
        var waited = false
        val pairing = scope.async { session.pair(code, name) { waited = true } }
        withTimeout(10_000) { while (!waited) delay(50) }
        delay(400)
        ctl("gateway.pair_decide", buildJsonObject {
            put("allow", true)
            put("scopes", JsonArray(scopes.map { JsonPrimitive(it) }))
        })
        val unit = (pairing.await() as PairOutcome.Paired).unit
        return Phone(session, vision, Fingerprint.hex(id.fingerprint())) to unit
    }

    private fun grant(phone: Phone, on: Boolean) = ctl("gateway.scope", buildJsonObject {
        put("client", phone.fingerprint)
        put("scope", "vision")
        put("on", on)
    })

    /** Width and height from a baseline JPEG's frame header, or null. */
    private fun jpegSize(b: ByteArray): Pair<Int, Int>? {
        if (b.size < 4 || b[0] != 0xFF.toByte() || b[1] != 0xD8.toByte()) return null
        var i = 2
        while (i + 9 < b.size) {
            if (b[i] != 0xFF.toByte()) return null
            val marker = b[i + 1].toInt() and 0xFF
            val len = ((b[i + 2].toInt() and 0xFF) shl 8) or (b[i + 3].toInt() and 0xFF)
            if (marker == 0xC0 || marker == 0xC1 || marker == 0xC2) {
                return (((b[i + 7].toInt() and 0xFF) shl 8) or (b[i + 8].toInt() and 0xFF)) to
                    (((b[i + 5].toInt() and 0xFF) shl 8) or (b[i + 6].toInt() and 0xFF))
            }
            i += 2 + len
        }
        return null
    }

    @Test
    fun theVisionRoomAgainstTheRealGatewayShellAndHelper() = runBlocking {
        assumeTrue("no gateway harness named", ctlAddr.isNotEmpty())
        val bench = try {
            ctl("harness.shell", buildJsonObject { put("op", "helpers") }).obj("result")!!
        } catch (_: AssertionError) {
            null
        }
        assumeTrue("the harness runs no real shell (DOORS_SHELL_BIN)", bench != null)
        assumeTrue("the harness has no Vision helper (DOORS_VISION_HELPER)", bench!!.bool("vision") == true)
        val scope = CoroutineScope(Dispatchers.Default + Job())

        // ---- a clean bench: nobody paired, the unit on its home screen ----
        ctl("gateway.pair_cancel")
        for (c in ctl("gateway.clients").obj("result")!!.array("clients").filterIsInstance<JsonObject>()) {
            ctl("gateway.revoke", buildJsonObject { put("client", c.string("fingerprint")!!) })
        }
        unit("shell.home")
        delay(800)
        assertEquals("home", unit("shell.info").string("current"))

        // ---- paired, but not authorised for Vision: nothing of it is offered ----
        val (one, unitOne) = pairPhone(scope, "Vision phone one", listOf("status"))
        val loopOne = scope.launch { one.session.run(unitOne) }
        one.vision.watch(true)
        until("Vision is not offered to a phone without the permission") { one.view.access == VisionAccess.NOT_SERVED }
        unit("shell.open", buildJsonObject { put("id", "vision") })
        until("Vision is live on the unit") { unit("vision.status").string("owner") == "vision" }
        delay(1_500)
        assertEquals("the unit shares nothing with a phone that was not authorised", "off", unitShare())
        assertNull(one.frame)

        // ---- authorised once, on the unit: the picture comes by itself ----
        grant(one, true)
        until("the picture is live: access ${one.view.access}, preview ${one.view.preview}, ${one.view.notice}", 30_000) {
            one.view.access == VisionAccess.OPEN && one.view.preview == PreviewState.LIVE
        }
        until("a picture arrives") { one.frame != null }
        val first = one.frame!!
        val jpeg = (first.pixels as FramePixels.Jpeg).bytes
        assertTrue("it is a JPEG", jpeg.size > 4 && jpeg[0] == 0xFF.toByte() && jpeg[1] == 0xD8.toByte() &&
            jpeg[jpeg.size - 2] == 0xFF.toByte() && jpeg[jpeg.size - 1] == 0xD9.toByte())
        assertTrue("within the bounds", jpeg.size <= 64 * 1024 && first.w in 1..VisionLimits.MAX_SIDE && first.h in 1..VisionLimits.MAX_SIDE)
        assertEquals("of the size its frame says", first.w to first.h, jpegSize(jpeg))
        assertEquals("the unit names this phone as the viewer", "live", unitShare())
        assertEquals(one.fingerprint.take(16), unit("vision.status").string("client"))
        if (keep.isNotEmpty()) File(keep).writeBytes(jpeg)

        // ---- in order, at the viewer's pace, with the detector's boxes and figures ----
        val seen = ArrayList<Long>()
        val t0 = System.currentTimeMillis()
        var last = first.seq
        while (System.currentTimeMillis() - t0 < 4_000) {
            val f = one.frame
            if (f != null && f.seq != last) {
                assertTrue("pictures only go forward: ${f.seq} after $last", f.seq > last)
                last = f.seq
                seen += f.seq
            }
            delay(10)
        }
        println("VisionIntegrationTest: ${seen.size} pictures in 4 s, ${jpeg.size} bytes the first, ${first.w} x ${first.h}, " +
            "stream ${one.view.stream.fpsX10 / 10.0} fps ${one.view.stream.kbps} kbit/s age ${one.view.stream.ageMs} ms")
        assertTrue("pictures keep coming: ${seen.size} in 4 s", seen.size >= 4)
        assertTrue("never more than five a second: ${seen.size} in 4 s", seen.size <= 22)
        assertTrue("the detector's figures are read", (one.view.stats?.fpsX10 ?: 0) > 0)
        assertTrue("and its boxes, inside the picture", one.frame!!.scene.boxes.isNotEmpty() &&
            one.frame!!.scene.boxes.all { it.x >= -4 && it.y >= -4 && it.x + it.w <= first.w + 4 && it.y + it.h <= first.h + 4 })
        assertEquals(VisionMode.entries.first { it.wire == unit("vision.status").string("mode") }, one.view.mode)

        // ---- a second authorised phone: one viewer, the other is told so ----
        val (two, unitTwo) = pairPhone(scope, "Vision phone two", listOf("status", "vision"))
        val loopTwo = scope.launch { two.session.run(unitTwo) }
        two.vision.watch(true)
        until("the second phone is told another has the picture: ${two.view.preview}") { two.view.preview == PreviewState.BUSY }
        delay(1_500)
        assertNull("and is sent none", two.frame)
        assertEquals(one.fingerprint.take(16), unit("vision.status").string("client"))

        // ---- a mode from the phone, and one it may not choose ----
        one.vision.setMode(VisionMode.TRAFFIC)
        until("the unit is in TRAFFIC") { unit("vision.status").string("mode") == "traffic" }
        one.vision.setMode(VisionMode.FACE)
        delay(500)
        assertEquals("FACE is not chosen from a phone", "traffic", unit("vision.status").string("mode"))
        until("the picture goes on in the new mode") { val s = one.frame?.seq ?: 0; s > last }

        // ---- STOP on the unit: at once, and it holds against the phone ----
        assertTrue("STOP, tapped on the panel", tap("STOP"))
        until("the unit stopped at the tap (${unitShare()})", 1_000) { unitShare() == "stopped" }
        until("the phone is told: stopped on the unit (${one.view.preview})") { one.view.preview == PreviewState.STOPPED }
        assertNull("its picture is let go", one.frame)
        delay(2_500)
        assertEquals("the phone did not start it again", "stopped", unitShare())
        one.vision.startPreview()
        delay(800)
        assertEquals("nor can a tap on the phone", "stopped", unitShare())
        assertNull(one.frame)

        // ---- ALLOW on the unit: sharing may begin again, and does by itself ----
        assertTrue("ALLOW, tapped on the panel", tap("ALLOW"))
        until("a phone is shown the picture again", 15_000) { unitShare() == "live" }
        until("pictures arrive again") { one.frame != null || two.frame != null }

        // ---- the permission taken away on the unit: ends at once ----
        val viewer = if (unit("vision.status").string("client") == one.fingerprint.take(16)) one else two
        val other = if (viewer === one) two else one
        grant(viewer, false)
        until("the unit no longer shows it to that phone") { unit("vision.status").string("client") != viewer.fingerprint.take(16) }
        until("that phone has no picture and no access (${viewer.view.access})") { viewer.frame == null && viewer.view.access != VisionAccess.OPEN }
        until("the other authorised phone takes over by itself", 15_000) { other.view.preview == PreviewState.LIVE && other.frame != null }

        // ---- the viewer drops off the network: the unit stops showing it ----
        val before = unit("vision.status").string("client")
        assertEquals(other.fingerprint.take(16), before)
        (if (other === one) loopOne else loopTwo).cancelAndJoin()
        until("the unit ended the share when its viewer went", 12_000) { unitShare() == "off" }

        // ---- Vision left: nothing is served, nothing is left running ----
        unit("shell.home")
        delay(800)
        assertEquals(false, unit("vision.status").bool("available"))
        for (c in ctl("gateway.clients").obj("result")!!.array("clients").filterIsInstance<JsonObject>()) {
            ctl("gateway.revoke", buildJsonObject { put("client", c.string("fingerprint")!!) })
        }
        loopOne.cancelAndJoin()
        loopTwo.cancelAndJoin()
    }
}
