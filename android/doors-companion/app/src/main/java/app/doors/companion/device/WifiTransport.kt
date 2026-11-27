// Copyright (c) 2026 Doors authors.
// SPDX-License-Identifier: Apache-2.0
package app.doors.companion.device

import android.content.Context
import android.net.ConnectivityManager
import android.net.LinkProperties
import android.net.Network
import android.net.NetworkCapabilities
import android.net.NetworkRequest
import android.os.Build
import app.doors.companion.core.Ipv4
import app.doors.companion.core.Ipv4Net
import app.doors.companion.core.LinkInfo
import app.doors.companion.core.LocalPlatform
import app.doors.companion.core.LocalTransport
import app.doors.companion.core.OwnLink
import app.doors.companion.core.OwnLinks
import app.doors.companion.core.Transport
import java.io.IOException
import java.net.Inet4Address
import java.net.InetAddress
import java.net.InetSocketAddress
import java.net.NetworkInterface
import java.net.Socket
import java.util.concurrent.Executors
import java.util.concurrent.ScheduledExecutorService
import java.util.concurrent.TimeUnit

/**
 * The phone's side of [LocalTransport]. Two ways, and no other:
 * - the Wi-Fi network the phone joined, with sockets bound to it explicitly
 *   (CONTRACTS.md section 2), so a LAN without internet is not bypassed over
 *   mobile data;
 * - a hotspot or tethering link the phone hosts itself, which Android gives
 *   an app no joined network for (core/OwnLinks.kt), only for an address
 *   inside that link's subnet, and only after the kernel confirmed the route.
 * Without either, nothing connects.
 */
class WifiTransport(context: Context) : Transport {
    private val cm = context.getSystemService(ConnectivityManager::class.java)

    @Volatile
    private var network: Network? = null

    @Volatile
    private var address: Inet4Address? = null

    /** The own links as last looked at: only for [available]; a socket always looks again. */
    @Volatile
    private var seen: List<OwnLink> = emptyList()

    /** Called when Wi-Fi arrives, leaves or changes address, and when an own link appears, goes or changes. */
    var onChange: () -> Unit = {}

    private val callback = object : ConnectivityManager.NetworkCallback() {
        /** Networks announced and not yet told apart from a hotspot. Callbacks arrive on one thread. */
        private val fresh = HashSet<Network>()

        override fun onAvailable(n: Network) {
            fresh.add(n)
        }

        // Since API 26 onAvailable is followed at once by onCapabilitiesChanged
        // and then onLinkPropertiesChanged. The network is taken here, where a
        // hotspot of the phone's own is told apart: Android 15 and later may
        // report one as a network (NET_CAPABILITY_LOCAL_NETWORK, "Wi-Fi hotspot
        // networks to which the device is sharing its Internet connectivity").
        // It is an own link, never the Wi-Fi the phone joined.
        override fun onCapabilitiesChanged(n: Network, caps: NetworkCapabilities) {
            if (!fresh.remove(n) || isLocal(caps)) return
            network = n
            onChange()
        }

        override fun onLost(n: Network) {
            fresh.remove(n)
            if (network == n) {
                network = null
                address = null
                onChange()
            }
        }

        override fun onLinkPropertiesChanged(n: Network, lp: LinkProperties) {
            if (network != n) return
            val v4 = lp.linkAddresses.map { it.address }.filterIsInstance<Inet4Address>().firstOrNull()
            if (v4 != address) {
                address = v4
                onChange()
            }
        }
    }
    private var registered = false
    private var watch: ScheduledExecutorService? = null

    private val platform = object : LocalPlatform {
        override fun joined(): Boolean = network != null

        override fun joinedAddress(): Int? = address?.takeIf { network != null }?.let { Ipv4.of(it.address) }

        override fun connectJoined(to: Int, port: Int, timeoutMs: Int): Socket {
            val n = network ?: throw IOException("no Wi-Fi network")
            val s = n.socketFactory.createSocket()
            try {
                s.connect(InetSocketAddress(InetAddress.getByAddress(Ipv4.bytes(to)), port), timeoutMs)
                return s
            } catch (e: IOException) {
                s.close()
                throw e
            }
        }

        override fun ownLinks(): List<OwnLink> = scan()

        override fun ownLinksSeen(): List<OwnLink> = seen
    }
    private val local = LocalTransport(platform)

    fun start() {
        if (registered) return
        val request = NetworkRequest.Builder()
            .addTransportType(NetworkCapabilities.TRANSPORT_WIFI)
            .removeCapability(NetworkCapabilities.NET_CAPABILITY_INTERNET)
            .build()
        cm.registerNetworkCallback(request, callback)
        registered = true
        seen = scan()
        // A hotspot that starts or stops changes no network an app is told
        // about, so the own links are looked at again every few seconds, and
        // only while the app is in front.
        watch = Executors.newSingleThreadScheduledExecutor { r -> Thread(r, "doors-own-links").apply { isDaemon = true } }.also {
            it.scheduleWithFixedDelay(::look, WATCH_MS, WATCH_MS, TimeUnit.MILLISECONDS)
        }
    }

    fun stop() {
        if (!registered) return
        cm.unregisterNetworkCallback(callback)
        registered = false
        watch?.shutdownNow()
        watch = null
        network = null
        address = null
        seen = emptyList()
    }

    override fun available(): Boolean = local.available()

    override suspend fun connect(host: String, port: Int, timeoutMs: Int): Socket = local.connect(host, port, timeoutMs)

    override suspend fun probe(port: Int): List<String> = local.probe(port)

    private fun look() {
        val now = scan()
        if (now != seen) {
            seen = now
            onChange()
        }
    }

    /**
     * The own links now: the interfaces Java lists, less those of every
     * network the platform reports other than a local one. Anything the
     * platform will not say leaves no own link, so this path fails closed.
     */
    private fun scan(): List<OwnLink> = try {
        // The one synchronous list of every network: a callback that had not
        // yet reported mobile data would let its interface through.
        @Suppress("DEPRECATION")
        val all = (cm.allNetworks.toList() + listOfNotNull(cm.activeNetwork)).distinct()
        val reported = all.filter { n -> cm.getNetworkCapabilities(n)?.let { !isLocal(it) } ?: true }.mapNotNull { cm.getLinkProperties(it) }
        val names = reported.mapNotNull { it.interfaceName }.toSet()
        val subnets = reported.flatMap { lp ->
            lp.linkAddresses.mapNotNull { la -> (la.address as? Inet4Address)?.let { Ipv4Net(Ipv4.of(it.address), la.prefixLength) } }
        }
        val interfaces = NetworkInterface.getNetworkInterfaces()?.toList().orEmpty().mapNotNull(::describe)
        OwnLinks.select(interfaces, names, subnets)
    } catch (e: Exception) {
        emptyList()
    }

    private fun describe(i: NetworkInterface): LinkInfo? = try {
        LinkInfo(
            name = i.name,
            up = i.isUp,
            loopback = i.isLoopback,
            pointToPoint = i.isPointToPoint,
            virtual = i.isVirtual,
            ipv4 = i.interfaceAddresses.mapNotNull { a ->
                (a.address as? Inet4Address)?.let { Ipv4Net(Ipv4.of(it.address), a.networkPrefixLength.toInt()) }
            },
        )
    } catch (e: Exception) {
        null
    }

    private companion object {
        const val WATCH_MS = 3_000L

        fun isLocal(caps: NetworkCapabilities): Boolean =
            Build.VERSION.SDK_INT >= Build.VERSION_CODES.VANILLA_ICE_CREAM &&
                caps.hasCapability(NetworkCapabilities.NET_CAPABILITY_LOCAL_NETWORK)
    }
}
