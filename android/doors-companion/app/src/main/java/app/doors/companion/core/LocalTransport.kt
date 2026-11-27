// Copyright (c) 2026 Doors authors.
// SPDX-License-Identifier: Apache-2.0
package app.doors.companion.core

import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.async
import kotlinx.coroutines.awaitAll
import kotlinx.coroutines.coroutineScope
import kotlinx.coroutines.sync.Semaphore
import kotlinx.coroutines.sync.withPermit
import kotlinx.coroutines.withContext
import java.io.IOException
import java.net.DatagramSocket
import java.net.Inet4Address
import java.net.InetAddress
import java.net.InetSocketAddress
import java.net.Proxy
import java.net.Socket

/** What the phone gives [LocalTransport]. On the phone: device/WifiTransport.kt. */
interface LocalPlatform {
    /** True while the phone has joined a Wi-Fi network. */
    fun joined(): Boolean

    /** The phone's IPv4 address on that network, or null while it has joined none or has no address yet. */
    fun joinedAddress(): Int?

    /** A socket bound to the joined Wi-Fi network explicitly, connected to [to]; an IOException without one. */
    fun connectJoined(to: Int, port: Int, timeoutMs: Int): Socket

    /** The phone's own links as they are now ([OwnLinks.select]). Every socket looks again. */
    fun ownLinks(): List<OwnLink>

    /** The own links as last looked at, perhaps a few seconds old: only for deciding whether to try. */
    fun ownLinksSeen(): List<OwnLink> = ownLinks()

    /** The source address the kernel picks for an unbound socket of this app to [to], or null. Sends nothing. */
    fun routeSource(to: Int, port: Int): Int? = Unbound.source(to, port)

    /** An unbound socket from [from], connected to [to]. Called only after [routeSource] answered [from]. */
    fun connectFrom(from: Int, to: Int, port: Int, timeoutMs: Int): Socket = Unbound.connect(from, to, port, timeoutMs)
}

/**
 * How sockets reach the unit on the phone (CONTRACTS.md section 2, with the
 * change proposed for phase 0 F1): bound to the Wi-Fi network the phone
 * joined, as before; or, for an address inside the subnet of a hotspot or
 * tethering link the phone itself hosts, left unbound from the phone's address
 * on that link, and only after the kernel confirmed that the route to that
 * address leaves from there. Nothing else: no address outside those subnets
 * is reached without a joined Wi-Fi network, and nothing goes over mobile
 * data or a VPN.
 */
class LocalTransport(private val platform: LocalPlatform) : Transport {
    override fun available(): Boolean = platform.joined() || platform.ownLinksSeen().isNotEmpty()

    override suspend fun connect(host: String, port: Int, timeoutMs: Int): Socket = withContext(Dispatchers.IO) {
        val to = Ipv4.local(host) ?: throw IOException("not a local address")
        val path = OwnLinks.pathFor(to, platform.joined(), platform.ownLinks())
            ?: throw IOException("no Wi-Fi network, and no hotspot of this phone holds the address")
        open(path, to, port, timeoutMs)
    }

    /** The phone's own subnets, each at most a /24: which hosts answer on the port. No TLS, nothing sent. */
    override suspend fun probe(port: Int): List<String> = coroutineScope {
        val targets = withContext(Dispatchers.IO) {
            OwnLinks.probeTargets(platform.joinedAddress()?.takeIf { platform.joined() }, platform.ownLinks())
        }
        val gate = Semaphore(PROBE_PARALLEL)
        targets.map { (to, path) ->
            async(Dispatchers.IO) {
                gate.withPermit {
                    try {
                        open(path, to, port, PROBE_MS).close()
                        Ipv4.text(to)
                    } catch (e: IOException) {
                        null
                    }
                }
            }
        }.awaitAll().filterNotNull()
    }

    private fun open(path: Path, to: Int, port: Int, timeoutMs: Int): Socket {
        if (Ipv4.local(Ipv4.text(to)) == null) throw IOException("not a local address")
        return when (path) {
            Path.Joined -> platform.connectJoined(to, port, timeoutMs)
            is Path.Own -> {
                // The kernel's own answer for an unbound socket of this app,
                // asked before any packet leaves: the route to this address
                // leaves from the link's address, so it is the phone's local
                // network, not mobile data and not a VPN.
                if (!path.link.subnet.holdsClient(to) || platform.routeSource(to, port) != path.link.own) {
                    throw IOException("the route to the address does not stay on the phone's own link")
                }
                platform.connectFrom(path.link.own, to, port, timeoutMs)
            }
        }
    }

    companion object {
        const val PROBE_MS = 300
        const val PROBE_PARALLEL = 48
    }
}

/** Sockets bound to no network, so that netd's local network is looked up for them first. */
object Unbound {
    /**
     * Asks the kernel which source address a socket of this app to [to] gets,
     * by connecting a UDP socket: that only picks the route, it sends nothing.
     * Null when there is no route or the answer is not IPv4.
     */
    fun source(to: Int, port: Int): Int? = try {
        DatagramSocket().use { d ->
            d.connect(InetSocketAddress(address(to), port))
            (d.localAddress as? Inet4Address)?.let { Ipv4.of(it.address) }
        }
    } catch (e: IOException) {
        null
    } catch (e: RuntimeException) {
        null
    }

    /**
     * A TCP socket from [from] to [to], with no proxy. Bound to that source
     * address, it cannot leave once the address is gone: the kernel refuses a
     * route for a source the host no longer holds.
     */
    fun connect(from: Int, to: Int, port: Int, timeoutMs: Int): Socket {
        val s = Socket(Proxy.NO_PROXY)
        try {
            s.bind(InetSocketAddress(address(from), 0))
            s.connect(InetSocketAddress(address(to), port), timeoutMs)
            return s
        } catch (e: IOException) {
            s.close()
            throw e
        }
    }

    private fun address(a: Int): InetAddress = InetAddress.getByAddress(Ipv4.bytes(a))
}
