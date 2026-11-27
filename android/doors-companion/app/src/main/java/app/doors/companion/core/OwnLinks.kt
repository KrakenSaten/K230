// Copyright (c) 2026 Doors authors.
// SPDX-License-Identifier: Apache-2.0
package app.doors.companion.core

/**
 * The phone's own links: a Wi-Fi hotspot or another tethering interface that
 * the phone hosts, with the unit as its client (RELAY_PROPOSAL.md section 16,
 * phase 0, F1).
 *
 * The phone has not joined such a link, so the Wi-Fi network it joined does
 * not reach it. Android's tethering puts the interface and its subnet in
 * netd's local network, and netd looks that up first for every socket that is
 * not bound to a network, before the default network (DOCUMENTED, AOSP:
 * RouteController addLocalNetworkRules at RULE_PRIORITY_LOCAL_NETWORK;
 * IpServer addRoutesToLocalNetwork; on Android 15 and later, optionally, a
 * network with NET_CAPABILITY_LOCAL_NETWORK, looked up at the same priority).
 * So a socket left unbound reaches a client of the phone's hotspot. These
 * rules say which addresses that socket may go to; [LocalTransport] asks the
 * kernel for the route before it opens one.
 */
object OwnLinks {
    /** Never wider than this, whatever the interface says: the probe limit of PAIRING.md section 7. */
    const val WIDEST = 24

    /**
     * The phone's own tethering links among [interfaces]: up, not loopback,
     * not point-to-point, not a sub-interface, not a Wi-Fi Direct group, and
     * not the interface of a network the platform reports ([networkNames]:
     * the Wi-Fi the phone joined, mobile data, a VPN, Ethernet). Each RFC 1918
     * address gives one subnet, clamped to the phone's /24. A subnet that
     * overlaps a reported network's ([networkSubnets]) or another own link's
     * is left out: the route there would be in doubt.
     */
    fun select(interfaces: List<LinkInfo>, networkNames: Set<String>, networkSubnets: List<Ipv4Net>): List<OwnLink> {
        val candidates = interfaces
            .filter { it.up && !it.loopback && !it.pointToPoint && !it.virtual }
            .filter { it.name !in networkNames && !isWifiDirect(it.name) }
            .flatMap { i -> i.ipv4.mapNotNull { a -> subnetOf(a)?.let { OwnLink(i.name, it) } } }
            .filter { l -> networkSubnets.none { it.overlaps(l.subnet) } }
        return candidates.filterIndexed { i, l -> candidates.withIndex().none { (j, o) -> j != i && o.subnet.overlaps(l.subnet) } }
    }

    /**
     * The subnet a client of the phone's address [a] may hold: an RFC 1918
     * address (what tethering assigns) with room for one client, never wider
     * than the phone's own /24.
     */
    fun subnetOf(a: Ipv4Net): Ipv4Net? {
        if (!Ipv4.isPrivate(a.address) || a.prefix > 30) return null
        return Ipv4Net(a.address, maxOf(a.prefix, WIDEST))
    }

    /**
     * Wi-Fi Direct groups are in the local network too, but they are not
     * tethering. wpa_supplicant names their interfaces p2p-<interface>-<n>
     * (DOCUMENTED); a vendor that names them otherwise leaves a link that is
     * still the phone's own, never mobile data.
     */
    fun isWifiDirect(name: String): Boolean = name.startsWith("p2p")

    /** The own link that [host] is a client on: inside its subnet, and neither its first or last address nor the phone's own. */
    fun linkFor(host: Int, links: List<OwnLink>): OwnLink? = links.firstOrNull { it.subnet.holdsClient(host) }

    /**
     * Which way a socket to [host] goes: an own link whose subnet holds it,
     * else the Wi-Fi network the phone joined, else none. The pairing code's
     * local-address rule is checked by the caller as well.
     */
    fun pathFor(host: Int, joined: Boolean, links: List<OwnLink>): Path? =
        linkFor(host, links)?.let { Path.Own(it) } ?: if (joined) Path.Joined else null

    /**
     * Where a probe looks: the joined Wi-Fi network's /24 around [joinedAddress]
     * and each own link's subnet, each at most a /24, each address once, each
     * by the way [pathFor] sends it.
     */
    fun probeTargets(joinedAddress: Int?, links: List<OwnLink>): List<Pair<Int, Path>> {
        val hosts = (joinedAddress?.let { Ipv4Net(it, WIDEST).clients() }.orEmpty() + links.flatMap { it.subnet.clients() }).distinct()
        return hosts.mapNotNull { h -> pathFor(h, joinedAddress != null, links)?.let { h to it } }
    }
}

/** Which way a socket goes. */
sealed interface Path {
    /** Bound to the Wi-Fi network the phone joined. */
    data object Joined : Path

    /** Left unbound, from the phone's own address on a link it hosts, after the kernel's route was confirmed. */
    data class Own(val link: OwnLink) : Path
}

/** A link the phone hosts: its interface, and the subnet its clients hold (the phone's own address with a prefix of at least [OwnLinks.WIDEST]). */
data class OwnLink(val name: String, val subnet: Ipv4Net) {
    val own: Int get() = subnet.address

    override fun toString() = "$name ${Ipv4.text(own)}/${subnet.prefix}"
}

/** One interface as the platform lists it, reduced to what [OwnLinks.select] reads. */
data class LinkInfo(
    val name: String,
    val up: Boolean,
    val loopback: Boolean,
    val pointToPoint: Boolean,
    val virtual: Boolean,
    val ipv4: List<Ipv4Net>,
)

/** An IPv4 address and its prefix length. */
data class Ipv4Net(val address: Int, val prefix: Int) {
    init {
        require(prefix in 0..32) { "prefix $prefix" }
    }

    private val mask: Int get() = maskOf(prefix)
    val base: Int get() = address and mask
    private val last: Int get() = base or mask.inv()

    operator fun contains(a: Int): Boolean = (a and mask) == base

    fun overlaps(o: Ipv4Net): Boolean {
        val m = maskOf(minOf(prefix, o.prefix))
        return (address and m) == (o.address and m)
    }

    /** Inside, and neither the first or last address nor [address] itself. */
    fun holdsClient(a: Int): Boolean = a in this && a != base && a != last && a != address

    /** Every address a client may hold. Only ever for a /24 or narrower. */
    fun clients(): List<Int> {
        require(prefix >= OwnLinks.WIDEST) { "wider than a /${OwnLinks.WIDEST}" }
        val size = 1 shl (32 - prefix)
        return (1 until size - 1).map { base + it }.filter { it != address }
    }

    override fun toString() = "${Ipv4.text(base)}/$prefix"

    private companion object {
        fun maskOf(prefix: Int): Int = if (prefix == 0) 0 else -1 shl (32 - prefix)
    }
}

/** IPv4 addresses as Ints, the first octet in the high byte. Nothing here asks a resolver. */
object Ipv4 {
    fun of(bytes: ByteArray): Int {
        require(bytes.size == 4)
        return bytes.fold(0) { a, b -> (a shl 8) or (b.toInt() and 0xff) }
    }

    fun bytes(a: Int): ByteArray = byteArrayOf((a ushr 24).toByte(), (a ushr 16).toByte(), (a ushr 8).toByte(), a.toByte())

    fun text(a: Int): String = "${a ushr 24}.${(a ushr 16) and 0xff}.${(a ushr 8) and 0xff}.${a and 0xff}"

    /** A dotted quad that passes the pairing code's local-address rule ([PairingUri.isLocalIpv4]), or null. */
    fun local(host: String): Int? {
        if (!PairingUri.isLocalIpv4(host)) return null
        return host.split('.').fold(0) { a, o -> (a shl 8) or o.toInt() }
    }

    /** RFC 1918 only (10/8, 172.16/12, 192.168/16): what Android's tethering assigns. */
    fun isPrivate(a: Int): Boolean = (a ushr 24) == 10 || (a ushr 20) == 0xac1 || (a ushr 16) == 0xc0a8
}
