// Copyright (c) 2026 Doors authors.
// SPDX-License-Identifier: Apache-2.0
package app.doors.companion.core

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertThrows
import org.junit.Assert.assertTrue
import org.junit.Test

/** The rules for the phone's own links (phase 0, F1): which subnet, never wider, never a network the platform reports. */
class OwnLinksTest {
    private fun q(s: String): Int = s.split('.').fold(0) { a, o -> (a shl 8) or o.toInt() }
    private fun net(s: String): Ipv4Net = s.split('/').let { Ipv4Net(q(it[0]), it[1].toInt()) }
    private fun link(name: String, vararg v4: String, up: Boolean = true, loopback: Boolean = false, p2p: Boolean = false) =
        LinkInfo(name, up, loopback, p2p, false, v4.map(::net))

    // ---- addresses ----

    @Test
    fun addressesRoundTripWithoutAResolver() {
        for (s in listOf("0.0.0.0", "10.0.0.1", "172.16.5.126", "192.168.43.1", "255.255.255.255")) {
            assertEquals(s, Ipv4.text(q(s)))
            assertEquals(q(s), Ipv4.of(Ipv4.bytes(q(s))))
        }
    }

    @Test
    fun theLocalRuleIsThePairingCodes() {
        for (s in listOf("10.0.2.2", "172.16.0.1", "192.168.43.7", "169.254.1.1", "127.0.0.1", "8.8.8.8", "100.64.0.1", "::1", "localhost", "10.0.0")) {
            assertEquals(s, PairingUri.isLocalIpv4(s), Ipv4.local(s) != null)
        }
        assertEquals(q("192.168.43.7"), Ipv4.local("192.168.43.7"))
    }

    @Test
    fun privateIsRfc1918Only() {
        for (s in listOf("10.0.0.0", "10.255.255.255", "172.16.0.0", "172.31.255.255", "192.168.0.0", "192.168.255.255")) assertTrue(s, Ipv4.isPrivate(q(s)))
        for (s in listOf("9.255.255.255", "11.0.0.0", "172.15.255.255", "172.32.0.0", "192.167.255.255", "192.169.0.0", "127.0.0.1", "169.254.1.1", "100.64.0.1", "192.0.0.4", "8.8.8.8")) {
            assertFalse(s, Ipv4.isPrivate(q(s)))
        }
    }

    // ---- the subnet of an own link ----

    @Test
    fun aHotspotsSubnetIsItsOwnSlash24() {
        assertEquals(Ipv4Net(q("192.168.43.1"), 24), OwnLinks.subnetOf(net("192.168.43.1/24")))
        assertEquals(Ipv4Net(q("10.137.4.1"), 24), OwnLinks.subnetOf(net("10.137.4.1/24")))
        assertEquals(Ipv4Net(q("172.20.9.1"), 28), OwnLinks.subnetOf(net("172.20.9.1/28")))
    }

    @Test
    fun neverWiderThanTheOwnSlash24() {
        val s = OwnLinks.subnetOf(net("10.42.7.1/8"))!!
        assertEquals(24, s.prefix)
        assertTrue(q("10.42.7.200") in s)
        assertFalse(q("10.42.8.1") in s)
        assertFalse(q("10.0.0.1") in s)
        assertThrows(IllegalArgumentException::class.java) { Ipv4Net(q("10.42.7.1"), 23).clients() }
    }

    @Test
    fun nothingButAPrivateAddressWithRoomForAClient() {
        for (s in listOf("192.168.43.1/31", "192.168.43.1/32", "100.64.3.4/10", "192.0.0.4/29", "169.254.7.1/16", "127.0.0.1/8", "8.8.8.8/24", "172.32.0.1/24")) {
            assertNull(s, OwnLinks.subnetOf(net(s)))
        }
        assertEquals(1, OwnLinks.subnetOf(net("192.168.43.1/30"))!!.clients().size)
    }

    @Test
    fun clientsAreNeitherTheEdgesNorThePhone() {
        val s = Ipv4Net(q("192.168.43.1"), 24)
        val c = s.clients()
        assertEquals(253, c.size)
        assertFalse(q("192.168.43.0") in c)
        assertFalse(q("192.168.43.1") in c)
        assertFalse(q("192.168.43.255") in c)
        assertTrue(c.all { it in s })
        assertTrue(s.holdsClient(q("192.168.43.2")) && s.holdsClient(q("192.168.43.254")))
        for (no in listOf("192.168.43.0", "192.168.43.1", "192.168.43.255", "192.168.44.2")) assertFalse(no, s.holdsClient(q(no)))
        assertEquals(13, Ipv4Net(q("172.20.9.1"), 28).clients().size)
    }

    // ---- which interfaces are own links ----

    /** A phone hosting a hotspot while on mobile data, with a VPN, a Wi-Fi Direct group and CLAT. */
    private val phone = listOf(
        link("lo", "127.0.0.1/8", loopback = true),
        link("rmnet_data0", "10.20.30.40/30"),
        link("v4-rmnet_data0", "192.0.0.4/32"),
        link("tun0", "10.8.0.2/24", p2p = true),
        link("p2p-wlan0-0", "192.168.49.1/24"),
        link("rndis0", "192.168.42.129/24", up = false),
        link("swlan0", "192.168.43.1/24"),
    )

    @Test
    fun onlyTheHotspotIsAnOwnLink() {
        val own = OwnLinks.select(phone, setOf("rmnet_data0", "tun0"), listOf(net("10.20.30.40/30"), net("10.8.0.2/24")))
        assertEquals(listOf(OwnLink("swlan0", Ipv4Net(q("192.168.43.1"), 24))), own)
    }

    @Test
    fun theInterfaceOfAReportedNetworkIsNeverAnOwnLink() {
        // Mobile data with a private address and room for a client is still mobile data.
        val cell = listOf(link("rmnet_data0", "10.20.30.40/29"))
        assertEquals(emptyList<OwnLink>(), OwnLinks.select(cell, setOf("rmnet_data0"), emptyList()))
        // The Wi-Fi the phone joined goes by its own path.
        val wifi = listOf(link("wlan0", "192.168.1.5/24"))
        assertEquals(emptyList<OwnLink>(), OwnLinks.select(wifi, setOf("wlan0"), listOf(net("192.168.1.5/24"))))
    }

    @Test
    fun aHotspotBesideJoinedWifiIsAnOwnLink() {
        val both = listOf(link("wlan0", "192.168.1.5/24"), link("wlan1", "192.168.43.1/24"))
        assertEquals(listOf("wlan1"), OwnLinks.select(both, setOf("wlan0"), listOf(net("192.168.1.5/24"))).map { it.name })
    }

    @Test
    fun aSubnetInDoubtIsLeftOut() {
        val hotspot = listOf(link("wlan1", "192.168.43.1/24"))
        assertEquals(emptyList<OwnLink>(), OwnLinks.select(hotspot, setOf("wlan0"), listOf(net("192.168.43.77/24"))))
        assertEquals(emptyList<OwnLink>(), OwnLinks.select(hotspot, setOf("wlan0"), listOf(net("192.168.0.9/16"))))
        val twice = listOf(link("wlan1", "192.168.43.1/24"), link("rndis0", "192.168.43.129/25"))
        assertEquals(emptyList<OwnLink>(), OwnLinks.select(twice, emptySet(), emptyList()))
        val apart = listOf(link("wlan1", "192.168.43.1/24"), link("rndis0", "192.168.42.129/24"))
        assertEquals(listOf("wlan1", "rndis0"), OwnLinks.select(apart, emptySet(), emptyList()).map { it.name })
    }

    @Test
    fun wiFiDirectIsNotTethering() {
        assertTrue(OwnLinks.isWifiDirect("p2p-wlan0-0"))
        assertTrue(OwnLinks.isWifiDirect("p2p0"))
        assertFalse(OwnLinks.isWifiDirect("swlan0"))
        assertEquals(emptyList<OwnLink>(), OwnLinks.select(listOf(link("p2p-wlan0-0", "192.168.49.1/24")), emptySet(), emptyList()))
    }

    // ---- which way a socket goes ----

    private val hotspot = OwnLink("swlan0", Ipv4Net(q("192.168.43.1"), 24))

    @Test
    fun aHotspotClientGoesByTheOwnLinkEvenBesideWifi() {
        assertEquals(Path.Own(hotspot), OwnLinks.pathFor(q("192.168.43.20"), joined = false, links = listOf(hotspot)))
        assertEquals(Path.Own(hotspot), OwnLinks.pathFor(q("192.168.43.20"), joined = true, links = listOf(hotspot)))
    }

    @Test
    fun everythingElseNeedsTheJoinedWifi() {
        assertEquals(Path.Joined, OwnLinks.pathFor(q("192.168.1.10"), joined = true, links = listOf(hotspot)))
        assertNull(OwnLinks.pathFor(q("192.168.1.10"), joined = false, links = listOf(hotspot)))
        assertNull(OwnLinks.pathFor(q("192.168.44.20"), joined = false, links = listOf(hotspot)))
        // The phone itself and the subnet's edges are not clients.
        for (no in listOf("192.168.43.1", "192.168.43.0", "192.168.43.255")) assertNull(no, OwnLinks.pathFor(q(no), joined = false, links = listOf(hotspot)))
    }

    @Test
    fun theProbeLooksAtTheOwnSubnetsOnly() {
        val wifiOnly = OwnLinks.probeTargets(q("192.168.1.5"), emptyList())
        assertEquals(253, wifiOnly.size)
        assertTrue(wifiOnly.all { (h, p) -> p == Path.Joined && (h ushr 8) == (q("192.168.1.0") ushr 8) && h != q("192.168.1.5") })

        val hotspotOnly = OwnLinks.probeTargets(null, listOf(hotspot))
        assertEquals(253, hotspotOnly.size)
        assertTrue(hotspotOnly.all { (h, p) -> p == Path.Own(hotspot) && hotspot.subnet.holdsClient(h) })

        val both = OwnLinks.probeTargets(q("192.168.1.5"), listOf(hotspot))
        assertEquals(506, both.size)
        assertEquals(both.size, both.map { it.first }.distinct().size)

        val narrow = OwnLink("rndis0", Ipv4Net(q("172.20.9.1"), 28))
        assertEquals(13, OwnLinks.probeTargets(null, listOf(narrow)).size)
        assertEquals(emptyList<Pair<Int, Path>>(), OwnLinks.probeTargets(null, emptyList()))
    }
}
