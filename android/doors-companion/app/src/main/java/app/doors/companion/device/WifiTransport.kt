// Copyright (c) 2026 Doors authors.
// SPDX-License-Identifier: Apache-2.0
package app.doors.companion.device

import android.content.Context
import android.net.ConnectivityManager
import android.net.LinkProperties
import android.net.Network
import android.net.NetworkCapabilities
import android.net.NetworkRequest
import app.doors.companion.core.PairingUri
import app.doors.companion.core.Transport
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.async
import kotlinx.coroutines.awaitAll
import kotlinx.coroutines.coroutineScope
import kotlinx.coroutines.sync.Semaphore
import kotlinx.coroutines.sync.withPermit
import kotlinx.coroutines.withContext
import java.io.IOException
import java.net.Inet4Address
import java.net.InetAddress
import java.net.InetSocketAddress
import java.net.Socket

/**
 * Sockets bound to the Wi-Fi network explicitly (CONTRACTS.md section 2), so
 * a hotspot or a LAN without internet is not bypassed over mobile data. There
 * is no other route: without Wi-Fi nothing connects.
 */
class WifiTransport(context: Context) : Transport {
    private val cm = context.getSystemService(ConnectivityManager::class.java)

    @Volatile
    private var network: Network? = null

    @Volatile
    private var address: Inet4Address? = null

    /** Called when Wi-Fi arrives, leaves or changes address. */
    var onChange: () -> Unit = {}

    private val callback = object : ConnectivityManager.NetworkCallback() {
        override fun onAvailable(n: Network) {
            network = n
            onChange()
        }

        override fun onLost(n: Network) {
            if (network == n) {
                network = null
                address = null
                onChange()
            }
        }

        override fun onLinkPropertiesChanged(n: Network, lp: LinkProperties) {
            if (network != null && network != n) return
            val v4 = lp.linkAddresses.map { it.address }.filterIsInstance<Inet4Address>().firstOrNull()
            if (v4 != address) {
                address = v4
                onChange()
            }
        }
    }
    private var registered = false

    fun start() {
        if (registered) return
        val request = NetworkRequest.Builder()
            .addTransportType(NetworkCapabilities.TRANSPORT_WIFI)
            .removeCapability(NetworkCapabilities.NET_CAPABILITY_INTERNET)
            .build()
        cm.registerNetworkCallback(request, callback)
        registered = true
    }

    fun stop() {
        if (!registered) return
        cm.unregisterNetworkCallback(callback)
        registered = false
        network = null
        address = null
    }

    override fun available(): Boolean = network != null

    override suspend fun connect(host: String, port: Int, timeoutMs: Int): Socket = withContext(Dispatchers.IO) {
        val n = network ?: throw IOException("no Wi-Fi network")
        val s = n.socketFactory.createSocket()
        try {
            s.connect(InetSocketAddress(literal(host), port), timeoutMs)
            s
        } catch (e: IOException) {
            s.close()
            throw e
        }
    }

    /** The phone's own /24, never wider: which hosts answer on the port. No TLS, nothing sent. */
    override suspend fun probe(port: Int): List<String> = coroutineScope {
        val n = network ?: return@coroutineScope emptyList()
        val own = address?.address ?: return@coroutineScope emptyList()
        val gate = Semaphore(48)
        (1..254).filter { it != (own[3].toInt() and 0xff) }.map { last ->
            async(Dispatchers.IO) {
                gate.withPermit {
                    val host = "${own[0].toInt() and 0xff}.${own[1].toInt() and 0xff}.${own[2].toInt() and 0xff}.$last"
                    val s = n.socketFactory.createSocket()
                    try {
                        s.connect(InetSocketAddress(literal(host), port), 300)
                        host
                    } catch (e: IOException) {
                        null
                    } finally {
                        try {
                            s.close()
                        } catch (_: IOException) {
                        }
                    }
                }
            }
        }.awaitAll().filterNotNull()
    }

    /** An IPv4 literal to an address without asking any resolver. */
    private fun literal(host: String): InetAddress {
        if (!PairingUri.isLocalIpv4(host)) throw IOException("not a local address")
        val o = host.split('.').map { it.toInt().toByte() }
        return InetAddress.getByAddress(byteArrayOf(o[0], o[1], o[2], o[3]))
    }
}
