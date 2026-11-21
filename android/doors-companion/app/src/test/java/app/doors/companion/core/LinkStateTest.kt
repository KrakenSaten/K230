// Copyright (c) 2026 Doors authors.
// SPDX-License-Identifier: Apache-2.0
package app.doors.companion.core

import app.doors.companion.device.UnitStore
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.cancelAndJoin
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.launch
import kotlinx.coroutines.runBlocking
import kotlinx.coroutines.withContext
import kotlinx.coroutines.withTimeout
import org.junit.After
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertThrows
import org.junit.Assert.assertTrue
import org.junit.Test
import java.io.DataInputStream
import java.io.OutputStream
import java.net.InetAddress
import java.net.InetSocketAddress
import java.net.ServerSocket
import java.net.Socket
import java.nio.file.Files
import java.util.concurrent.atomic.AtomicInteger
import javax.net.ssl.SSLContext
import javax.net.ssl.SSLServerSocket
import javax.net.ssl.SSLSocket

/**
 * What the loop may conclude from what it sees, against a unit played in this
 * JVM (TLS 1.3, a real P-256 identity, the gateway's framing):
 *
 *  - no answer, a timeout, a lost session and a door closed after the
 *    handshake are troubles to wait out, never a revocation, however often;
 *  - only the unit's own word, over a connection its pinned key
 *    authenticated, is a revocation;
 *  - the loop tries saved addresses and never sweeps the network; a sweep is
 *    explicit, and accepts only the pinned key;
 *  - a stored pin is never replaced.
 */
class LinkStateTest {
    private val scope = CoroutineScope(Dispatchers.Default + Job())
    private val servers = ArrayList<ServerSocket>()
    private val unitKey = TestIdentity.create()
    private val phone = TestIdentity.create()

    @After
    fun close() {
        servers.forEach { it.close() }
    }

    /** A unit at [host]: every connection is handed to [serve] after the handshake. */
    private fun unit(host: String, port: Int = 0, identity: TestIdentity = unitKey, serve: (DataInputStream, OutputStream) -> Unit): Int {
        val ctx = SSLContext.getInstance("TLSv1.3")
        ctx.init(arrayOf(identity.keyManager()), null, null)
        val server = ctx.serverSocketFactory.createServerSocket(port, 16, InetAddress.getByName(host)) as SSLServerSocket
        servers += server
        Thread {
            while (true) {
                val s = try {
                    server.accept() as SSLSocket
                } catch (e: Exception) {
                    return@Thread
                }
                Thread {
                    try {
                        s.use {
                            it.startHandshake()
                            serve(DataInputStream(it.inputStream), it.outputStream)
                        }
                    } catch (_: Exception) {
                    }
                }.start()
            }
        }.apply { isDaemon = true }.start()
        return server.localPort
    }

    private fun readFrame(i: DataInputStream): String = ByteArray(i.readInt()).also { i.readFully(it) }.toString(Charsets.UTF_8)

    private fun write(o: OutputStream, json: String) {
        val b = json.toByteArray()
        o.write(byteArrayOf((b.size ushr 24).toByte(), (b.size ushr 16).toByte(), (b.size ushr 8).toByte(), b.size.toByte()) + b)
        o.flush()
    }

    private fun hello(i: DataInputStream, o: OutputStream, paired: Boolean = true) {
        readFrame(i)
        write(o, """{"id":1,"result":{"proto":1,"device_id":"${Fingerprint.hex(unitKey.fingerprint())}","name":"u","paired":$paired,"scopes":[],"capabilities":[]}}""")
    }

    private class Net : Transport {
        val probes = AtomicInteger()
        val connects = AtomicInteger()
        var answering: List<String> = emptyList()

        override suspend fun connect(host: String, port: Int, timeoutMs: Int): Socket = withContext(Dispatchers.IO) {
            connects.incrementAndGet()
            Socket().apply { connect(InetSocketAddress(host, port), timeoutMs) }
        }

        override suspend fun probe(port: Int): List<String> {
            probes.incrementAndGet()
            return answering
        }

        override fun available() = true
    }

    private fun paired(port: Int, host: String = "127.0.0.1", known: List<String> = emptyList()) =
        PairedUnit(Fingerprint.hex(unitKey.fingerprint()), "u", host, port, 0, known)

    /** Runs the loop until it has waited [rounds] times; returns every state it passed through. */
    private fun observe(net: Net, unit: PairedUnit, rounds: Int, moved: (PairedUnit) -> Unit = {}): List<LinkState> = runBlocking {
        val session = GatewaySession(net, phone, "test", onUnitMoved = moved, backoff = { 15 }, helloTimeoutMs = 250)
        val seen = ArrayList<LinkState>()
        val watch = scope.launch { session.state.collect { synchronized(seen) { seen += it } } }
        val loop = scope.launch { session.run(unit) }
        try {
            withTimeout(20_000) {
                while (synchronized(seen) { seen.count { it is LinkState.Waiting } < rounds && seen.none { it is LinkState.Stopped } }) delay(10)
            }
        } finally {
            loop.cancelAndJoin()
            watch.cancelAndJoin()
        }
        synchronized(seen) { ArrayList(seen) }
    }

    private fun troubles(states: List<LinkState>) = states.filterIsInstance<LinkState.Waiting>().map { it.trouble }.toSet()

    // ---- 1. what is, and is not, a revocation ----

    @Test
    fun aDoorClosedAfterTheHandshakeIsNeverARevocation() {
        val port = unit("127.0.0.1") { _, _ -> } // proves its key, then closes: every time
        val states = observe(Net(), paired(port), rounds = 8)
        assertTrue("never stopped: $states", states.none { it is LinkState.Stopped })
        assertEquals(setOf(Trouble.NOT_ADMITTED), troubles(states))
    }

    @Test
    fun noAnswerIsANetworkTroubleHoweverOften() {
        val port = ServerSocket(0).use { it.localPort } // nothing listens here
        val net = Net()
        val states = observe(net, paired(port), rounds = 8)
        assertTrue(states.none { it is LinkState.Stopped })
        assertEquals(setOf(Trouble.UNREACHABLE), troubles(states))
        assertEquals("the loop never sweeps the network", 0, net.probes.get())
        assertEquals("one saved address, one connection a round", 8, net.connects.get())
    }

    @Test
    fun aSilentUnitIsATimeoutNotARevocation() {
        val port = unit("127.0.0.1") { i, _ -> readFrame(i); Thread.sleep(5_000) } // takes the hello, never answers
        val states = observe(Net(), paired(port), rounds = 5)
        assertTrue("never stopped: $states", states.none { it is LinkState.Stopped })
        assertEquals(setOf(Trouble.UNREACHABLE), troubles(states))
    }

    @Test
    fun aSessionThatKeepsDroppingIsLostNotRevokedAndBacksOff() {
        val port = unit("127.0.0.1") { i, o -> hello(i, o) } // lets the phone in, then drops it
        val net = Net()
        val states = observe(net, paired(port), rounds = 6)
        assertTrue(states.none { it is LinkState.Stopped })
        assertEquals(setOf(Trouble.LOST), troubles(states))
        assertTrue(states.any { it is LinkState.Online })
        // A session that did not hold does not reset the back-off.
        val session = GatewaySession(net, phone, "test", backoff = { attempt -> attempts += attempt; 15 })
        runBlocking {
            val loop = scope.launch { session.run(paired(port)) }
            withTimeout(10_000) { while (attempts.size < 4) delay(10) }
            loop.cancelAndJoin()
        }
        assertEquals(listOf(0, 1, 2, 3), attempts.take(4))
    }

    private val attempts = ArrayList<Int>()

    @Test
    fun theUnitsOwnWordIsARevocation() {
        val told = unit("127.0.0.1") { i, o ->
            hello(i, o)
            write(o, """{"event":"gateway.revoked","data":{}}""")
        }
        assertEquals(LinkState.Stopped(Stop.REVOKED), observe(Net(), paired(told), rounds = 99).last { it is LinkState.Stopped })
        val unknown = unit("127.0.0.1") { i, o -> hello(i, o, paired = false) }
        assertEquals(LinkState.Stopped(Stop.REVOKED), observe(Net(), paired(unknown), rounds = 99).last { it is LinkState.Stopped })
    }

    @Test
    fun anotherKeyIsAnAuthenticationFailureAndThePhoneSaysNothing() {
        val spoke = AtomicInteger()
        val port = unit("127.0.0.1", identity = TestIdentity.create()) { i, _ -> if (i.read() >= 0) spoke.incrementAndGet() }
        val states = observe(Net(), paired(port), rounds = 99)
        assertEquals(LinkState.Stopped(Stop.WRONG_UNIT), states.last { it is LinkState.Stopped })
        assertTrue(states.none { it is LinkState.Online })
        Thread.sleep(200)
        assertEquals("no request reached the other key", 0, spoke.get())
    }

    // ---- 2. finding the unit again ----

    @Test
    fun aSavedAddressIsTriedAndThePinStays() {
        val port = unit("127.0.0.2") { i, o -> hello(i, o); Thread.sleep(400) }
        val net = Net()
        var moved: PairedUnit? = null
        val before = paired(port, host = "127.0.0.1", known = listOf("127.0.0.2")) // nothing at .1
        val states = observe(net, before, rounds = 1) { moved = it }
        assertTrue(states.any { it is LinkState.Online })
        assertEquals("127.0.0.2", moved!!.host)
        assertEquals(listOf("127.0.0.1"), moved!!.knownHosts)
        assertEquals("the same pin", before.fingerprint, moved!!.fingerprint)
        assertEquals(0, net.probes.get())
    }

    @Test
    fun anotherKeyAtAnOldSavedAddressIsSkippedNotTrusted() {
        val port = unit("127.0.0.2", identity = TestIdentity.create()) { _, _ -> }
        var moved: PairedUnit? = null
        val states = observe(Net(), paired(port, host = "127.0.0.1", known = listOf("127.0.0.2")), rounds = 3) { moved = it }
        assertTrue(states.none { it is LinkState.Online || it is LinkState.Stopped })
        assertEquals(setOf(Trouble.UNREACHABLE), troubles(states))
        assertNull(moved)
    }

    @Test
    fun anExplicitSearchAcceptsOnlyThePinnedKey() = runBlocking {
        val port = unit("127.0.0.2", identity = TestIdentity.create()) { _, _ -> } // an impostor on the port
        unit("127.0.0.3", port) { _, _ -> }                                       // the unit
        val net = Net()
        val session = GatewaySession(net, phone, "test")
        val unit = paired(port, host = "127.0.0.1")

        net.answering = listOf("127.0.0.2")
        assertNull("an impostor is not the unit", session.locate(unit))

        net.answering = listOf("127.0.0.2", "127.0.0.3")
        val found = session.locate(unit)!!
        assertEquals("127.0.0.3", found.host)
        assertEquals(unit.fingerprint, found.fingerprint)
        assertEquals(listOf("127.0.0.1"), found.knownHosts)
        assertEquals("one sweep per request", 2, net.probes.get())

        net.answering = (1..40).map { "127.0.1.$it" } // many hosts answer the port: the attempts are bounded
        val before = net.connects.get()
        assertNull(session.locate(unit))
        assertEquals(GatewaySession.LOCATE_MAX, net.connects.get() - before)
    }

    @Test
    fun aStoredPinIsNeverReplaced() {
        val store = UnitStore(Files.createTempDirectory("doors-store").toFile())
        val unit = paired(47231)
        store.save(unit)
        store.save(unit.movedTo("127.0.0.9")) // an address may change
        val other = unit.copy(fingerprint = Fingerprint.hex(TestIdentity.create().fingerprint()))
        assertThrows(IllegalStateException::class.java) { store.save(other) }
        assertEquals(unit.fingerprint, store.unit.value!!.fingerprint)
        assertEquals("127.0.0.9", store.unit.value!!.host)
        assertFalse(store.unit.value!!.knownHosts.isEmpty())
        store.clear() // forgetting is the only way to another pin
        store.save(other)
    }
}
