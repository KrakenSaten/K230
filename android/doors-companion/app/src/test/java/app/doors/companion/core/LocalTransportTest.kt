// Copyright (c) 2026 Doors authors.
// SPDX-License-Identifier: Apache-2.0
package app.doors.companion.core

import kotlinx.coroutines.runBlocking
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertThrows
import org.junit.Assert.assertTrue
import org.junit.Test
import java.io.IOException
import java.net.DatagramPacket
import java.net.DatagramSocket
import java.net.InetAddress
import java.net.InetSocketAddress
import java.net.ServerSocket
import java.net.Socket
import java.net.SocketTimeoutException

/**
 * The local transport's decisions, apart from Android: which way a socket
 * goes, that nothing is opened outside the phone's own subnets without a
 * joined Wi-Fi network, and that a socket on an own link is made only after
 * the kernel's route left from that link's address.
 */
class LocalTransportTest {
    private fun q(s: String): Int = s.split('.').fold(0) { a, o -> (a shl 8) or o.toInt() }

    private val hotspot = OwnLink("swlan0", Ipv4Net(q("192.168.43.1"), 24))
    private val mobile = q("10.20.30.40")

    private class Fake(
        var joined: Boolean = false,
        var joinedAddress: Int? = null,
        var links: List<OwnLink> = emptyList(),
        var seen: List<OwnLink>? = null,
        var source: (Int) -> Int? = { null },
    ) : LocalPlatform {
        val viaWifi = mutableListOf<Int>()
        val viaOwn = mutableListOf<Pair<Int, Int>>()
        val asked = mutableListOf<Int>()

        override fun joined() = joined
        override fun joinedAddress() = joinedAddress
        override fun ownLinks() = links
        override fun ownLinksSeen() = seen ?: links

        override fun connectJoined(to: Int, port: Int, timeoutMs: Int): Socket {
            synchronized(this) { viaWifi += to }
            return Socket()
        }

        override fun routeSource(to: Int, port: Int): Int? {
            synchronized(this) { asked += to }
            return source(to)
        }

        override fun connectFrom(from: Int, to: Int, port: Int, timeoutMs: Int): Socket {
            synchronized(this) { viaOwn += from to to }
            return Socket()
        }

        fun nothingOpened() = viaWifi.isEmpty() && viaOwn.isEmpty()
    }

    private fun refused(t: LocalTransport, host: String) {
        assertThrows(host, IOException::class.java) { runBlocking { t.connect(host, 47231, 1000) } }
    }

    // ---- connect ----

    @Test
    fun aHotspotClientIsReachedFromTheLinkOnceTheRouteIsConfirmed() = runBlocking {
        val p = Fake(links = listOf(hotspot), source = { hotspot.own })
        LocalTransport(p).connect("192.168.43.20", 47231, 1000).close()
        assertEquals(listOf(q("192.168.43.20")), p.asked)
        assertEquals(listOf(hotspot.own to q("192.168.43.20")), p.viaOwn)
        assertTrue(p.viaWifi.isEmpty())
    }

    @Test
    fun aRouteThatLeavesElsewhereOpensNothing() {
        // Mobile data, a VPN's tunnel, no route at all, the phone's other link.
        for (answer in listOf<Int?>(mobile, q("10.8.0.2"), null, q("192.168.42.129"))) {
            val p = Fake(joined = true, links = listOf(hotspot), source = { answer })
            refused(LocalTransport(p), "192.168.43.20")
            assertEquals(listOf(q("192.168.43.20")), p.asked)
            assertTrue("$answer", p.nothingOpened())
        }
    }

    @Test
    fun withoutJoinedWifiNothingOutsideTheHotspot() {
        val p = Fake(links = listOf(hotspot), source = { hotspot.own })
        for (h in listOf("192.168.1.10", "192.168.44.20", "10.0.0.1", "172.16.5.126")) refused(LocalTransport(p), h)
        assertTrue(p.asked.isEmpty())
        assertTrue(p.nothingOpened())
    }

    @Test
    fun outsideTheHotspotTheJoinedWifiIsUsedAsBefore() = runBlocking {
        val p = Fake(joined = true, joinedAddress = q("192.168.1.5"), links = listOf(hotspot), source = { hotspot.own })
        val t = LocalTransport(p)
        t.connect("192.168.1.10", 47231, 1000).close()
        t.connect("172.16.5.126", 47231, 1000).close()
        assertEquals(listOf(q("192.168.1.10"), q("172.16.5.126")), p.viaWifi)
        assertTrue("the joined Wi-Fi needs no route question", p.asked.isEmpty())
        t.connect("192.168.43.20", 47231, 1000).close()
        assertEquals(listOf(hotspot.own to q("192.168.43.20")), p.viaOwn)
    }

    @Test
    fun aNonLocalAddressIsNeverTried() {
        val p = Fake(joined = true, joinedAddress = q("192.168.1.5"), links = listOf(hotspot), source = { hotspot.own })
        for (h in listOf("8.8.8.8", "100.64.0.1", "192.0.0.4", "localhost", "unit.local", "::1", "192.168.43")) refused(LocalTransport(p), h)
        assertTrue(p.asked.isEmpty())
        assertTrue(p.nothingOpened())
    }

    @Test
    fun thePhoneAndTheSubnetsEdgesAreNotClients() {
        val p = Fake(links = listOf(hotspot), source = { hotspot.own })
        for (h in listOf("192.168.43.1", "192.168.43.0", "192.168.43.255")) refused(LocalTransport(p), h)
        assertTrue(p.nothingOpened())
    }

    @Test
    fun theLinksAreLookedAtAgainForEverySocket() {
        // The hotspot was seen a moment ago and is gone now.
        val p = Fake(links = emptyList(), seen = listOf(hotspot), source = { hotspot.own })
        val t = LocalTransport(p)
        assertTrue(t.available())
        refused(t, "192.168.43.20")
        assertTrue(p.nothingOpened())
    }

    @Test
    fun availableWithJoinedWifiOrAnOwnLink() {
        assertFalse(LocalTransport(Fake()).available())
        assertTrue(LocalTransport(Fake(joined = true)).available())
        assertTrue(LocalTransport(Fake(links = listOf(hotspot))).available())
    }

    // ---- probe ----

    @Test
    fun theProbeStaysInsideTheOwnSubnetsAndAsksEveryRoute() = runBlocking {
        val p = Fake(joined = true, joinedAddress = q("192.168.1.5"), links = listOf(hotspot), source = { hotspot.own })
        val found = LocalTransport(p).probe(47231)
        assertEquals(506, found.size)
        assertEquals(253, p.viaWifi.size)
        assertTrue(p.viaWifi.all { (it ushr 8) == (q("192.168.1.0") ushr 8) && it != q("192.168.1.5") })
        assertEquals(253, p.viaOwn.size)
        assertTrue(p.viaOwn.all { (from, to) -> from == hotspot.own && hotspot.subnet.holdsClient(to) })
        assertEquals(p.viaOwn.map { it.second }.sorted(), p.asked.sorted())
    }

    @Test
    fun aProbeWhoseRoutesLeaveElsewhereOpensNothingThere() = runBlocking {
        val p = Fake(links = listOf(hotspot), source = { mobile })
        assertEquals(emptyList<String>(), LocalTransport(p).probe(47231))
        assertEquals(253, p.asked.size)
        assertTrue(p.nothingOpened())
    }

    @Test
    fun aStaleWifiAddressIsNotProbed() = runBlocking {
        val p = Fake(joined = false, joinedAddress = q("192.168.1.5"), links = listOf(hotspot), source = { hotspot.own })
        LocalTransport(p).probe(47231)
        assertTrue(p.viaWifi.isEmpty())
        assertEquals(253, p.viaOwn.size)
    }

    // ---- the kernel's answer, on this host ----

    @Test
    fun askingTheRouteSendsNothing() {
        val loopback = q("127.0.0.1")
        DatagramSocket(0, InetAddress.getByName("127.0.0.1")).use { listener ->
            listener.soTimeout = 300
            assertEquals(loopback, Unbound.source(loopback, listener.localPort))
            assertThrows(SocketTimeoutException::class.java) { listener.receive(DatagramPacket(ByteArray(64), 64)) }
        }
    }

    @Test
    fun anUnboundSocketLeavesFromTheAddressItWasGiven() {
        ServerSocket(0, 1, InetAddress.getByName("127.0.0.1")).use { server ->
            Unbound.connect(q("127.0.0.1"), q("127.0.0.1"), server.localPort, 1000).use { s ->
                assertTrue(s.isConnected)
                assertEquals(InetAddress.getByName("127.0.0.1"), s.localAddress)
                assertEquals(InetSocketAddress(InetAddress.getByName("127.0.0.1"), server.localPort), s.remoteSocketAddress)
            }
        }
    }

    @Test
    fun anAddressTheHostDoesNotHoldCannotBeLeftFrom() {
        // 192.0.2.1 (TEST-NET-1) is held by no interface here, as a hotspot's address is not once it stops.
        ServerSocket(0, 1, InetAddress.getByName("127.0.0.1")).use { server ->
            assertThrows(IOException::class.java) { Unbound.connect(q("192.0.2.1"), q("127.0.0.1"), server.localPort, 1000) }
        }
    }
}
