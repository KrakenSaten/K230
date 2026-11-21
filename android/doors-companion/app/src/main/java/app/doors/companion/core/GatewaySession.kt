// Copyright (c) 2026 Doors authors.
// SPDX-License-Identifier: Apache-2.0
package app.doors.companion.core

import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.TimeoutCancellationException
import kotlinx.coroutines.channels.Channel
import kotlinx.coroutines.coroutineScope
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import kotlinx.coroutines.withTimeoutOrNull
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.buildJsonObject
import kotlinx.serialization.json.put
import java.io.IOException
import java.net.Socket
import kotlin.random.Random

/** A unit this phone has paired with. The pin is the whole of its identity. */
data class PairedUnit(
    val fingerprint: String,   // 64 hex: the unit's device_id
    val name: String,
    val host: String,
    val port: Int,
    val pairedAtMs: Long,
) {
    val pin: ByteArray get() = Fingerprint.parseHex(fingerprint)
}

/** How sockets reach the unit. On the phone: bound to the Wi-Fi network, never mobile data. */
interface Transport {
    /** A connected TCP socket, or an IOException. */
    suspend fun connect(host: String, port: Int, timeoutMs: Int): Socket

    /** Other addresses on the phone's own Wi-Fi subnet (at most a /24) where the port answers. */
    suspend fun probe(port: Int): List<String>

    /** False while there is no Wi-Fi network to use. */
    fun available(): Boolean
}

sealed interface LinkState {
    data object Idle : LinkState
    data object NoWifi : LinkState
    data class Connecting(val attempt: Int, val searching: Boolean = false) : LinkState
    data class Online(val hello: Hello) : LinkState
    data class Waiting(val retryInMs: Long, val reason: String, val notOnNetwork: Boolean) : LinkState

    /** The loop has ended and will not run again by itself. */
    data class Stopped(val why: Stop) : LinkState
}

enum class Stop {
    /** The unit removed this phone (gateway.revoked, or it no longer admits our key). */
    REVOKED,

    /** The address answers with another key. Only "forget and pair again" is offered. */
    WRONG_UNIT,

    /** No protocol version in common. */
    INCOMPATIBLE,
}

/** One service's last answer, or why there is none. */
data class Reading(val data: JsonObject? = null, val error: Int? = null, val atMs: Long = 0)

data class Telemetry(
    val info: Reading = Reading(),
    val system: Reading = Reading(),
    val wifi: Reading = Reading(),
    val radio: Reading = Reading(),
    val mesh: Reading = Reading(),
) {
    val updatedAtMs: Long get() = maxOf(system.atMs, wifi.atMs, radio.atMs, mesh.atMs)
}

sealed interface PairOutcome {
    data class Paired(val unit: PairedUnit, val scopes: Set<String>) : PairOutcome
    data object Refused : PairOutcome
    data object WrongUnit : PairOutcome
    data class Failed(val reason: String, val detail: String? = null) : PairOutcome
}

/**
 * The connection life cycle of PAIRING.md section 7: connect, pin, hello;
 * find the unit again on its subnet; back off 1 s doubling to 60 s with
 * jitter, reset by a network change or the person; ping every 30 s and close
 * after 90 s of silence; stop for good when the unit has revoked this phone
 * or answers with another key.
 *
 * [run] is the loop. It lives only as long as its caller's scope: the app
 * runs it while it is in front, and nothing here survives that.
 */
class GatewaySession(
    private val transport: Transport,
    private val identity: ClientIdentity,
    private val appVersion: String,
    private val onUnitMoved: (PairedUnit) -> Unit = {},
    private val now: () -> Long = System::currentTimeMillis,
) {
    private val _state = MutableStateFlow<LinkState>(LinkState.Idle)
    val state: StateFlow<LinkState> = _state

    private val _telemetry = MutableStateFlow(Telemetry())
    val telemetry: StateFlow<Telemetry> = _telemetry

    private val _scopes = MutableStateFlow<Set<String>>(emptySet())
    val scopes: StateFlow<Set<String>> = _scopes

    private val kicks = Channel<Unit>(Channel.CONFLATED)

    @Volatile
    private var live: GatewayConnection? = null

    /** A network change or the person: retry now, and start the back-off again. */
    fun kick() {
        kicks.trySend(Unit)
    }

    fun clear() {
        _telemetry.value = Telemetry()
        _scopes.value = emptySet()
        _state.value = LinkState.Idle
    }

    suspend fun run(initial: PairedUnit) {
        var unit = initial
        var attempt = 0
        var unreachable = 0
        var turnedAway = 0
        try {
            while (true) {
                if (!transport.available()) {
                    _state.value = LinkState.NoWifi
                    kicks.receive()
                    attempt = 0
                    continue
                }
                _state.value = LinkState.Connecting(attempt)
                var reason: String
                var verified = false
                try {
                    val tls = open(unit.host, unit.port, unit.pin)
                    verified = true
                    val conn = GatewayConnection(tls)
                    live = conn
                    try {
                        val hello = conn.hello(appVersion)
                        if (!hello.paired || !hello.deviceId.equals(unit.fingerprint, ignoreCase = true)) {
                            // Admitted only as a pairing candidate: the unit no longer knows this phone.
                            _state.value = LinkState.Stopped(Stop.REVOKED)
                            return
                        }
                        attempt = 0
                        unreachable = 0
                        turnedAway = 0
                        _scopes.value = hello.scopes
                        _state.value = LinkState.Online(hello)
                        if (serve(conn, hello)) {
                            _state.value = LinkState.Stopped(Stop.REVOKED)
                            return
                        }
                        reason = "the connection was lost"
                    } finally {
                        live = null
                        conn.close()
                    }
                } catch (e: CancellationException) {
                    if (e !is TimeoutCancellationException) throw e
                    reason = "the unit did not answer"
                } catch (e: GatewayError) {
                    if (e.code == GatewayError.NO_PROTO) {
                        _state.value = LinkState.Stopped(Stop.INCOMPATIBLE)
                        return
                    }
                    reason = e.message ?: "refused"
                } catch (e: IOException) {
                    if (GatewayTls.isPinMismatch(e)) {
                        _state.value = LinkState.Stopped(Stop.WRONG_UNIT)
                        return
                    }
                    if (verified) {
                        // The unit proved its key and then closed without an
                        // answer: it does not admit ours. Once could be its
                        // handshake budget; three times in a row is a removal.
                        turnedAway++
                        if (turnedAway >= 3) {
                            _state.value = LinkState.Stopped(Stop.REVOKED)
                            return
                        }
                        reason = "the unit closed the connection"
                    } else {
                        unreachable++
                        reason = "the unit did not answer"
                        if (unreachable >= 2) {
                            _state.value = LinkState.Connecting(attempt, searching = true)
                            val found = search(unit)
                            if (found != null) {
                                unit = unit.copy(host = found)
                                onUnitMoved(unit)
                                unreachable = 0
                                continue
                            }
                        }
                    }
                }
                val wait = backoffMs(attempt++)
                _state.value = LinkState.Waiting(wait, reason, notOnNetwork = unreachable >= 2)
                if (withTimeoutOrNull(wait) { kicks.receive() } != null) attempt = 0
            }
        } finally {
            live?.close()
            if (_state.value !is LinkState.Stopped) _state.value = LinkState.Idle
        }
    }

    /** Runs a live session until it ends. True when the unit revoked this phone. */
    private suspend fun serve(conn: GatewayConnection, hello: Hello): Boolean = coroutineScope {
        val events = launch {
            conn.events.collect { e ->
                when (e.name) {
                    "gateway.revoked" -> conn.close()
                    "gateway.scopes" -> _scopes.value = e.data.strings("scopes")
                }
            }
        }
        val keepAlive = launch {
            while (true) {
                delay(PING_MS)
                if ((System.nanoTime() - conn.lastReceivedAt) / 1_000_000 > SILENCE_MS) {
                    conn.close()
                    break
                }
                try {
                    conn.call("gateway.ping")
                } catch (_: IOException) {
                } catch (_: TimeoutCancellationException) {
                }
            }
        }
        val poll = launch {
            // No gateway.subscribe in this gateway build: status is read, not pushed.
            if ("status" !in hello.capabilities) return@launch
            read(conn, "system.info") { t, r -> t.copy(info = r) }
            while (true) {
                read(conn, "system.status") { t, r -> t.copy(system = r) }
                read(conn, "wifi.status") { t, r -> t.copy(wifi = r) }
                read(conn, "radio.status") { t, r -> t.copy(radio = r) }
                read(conn, "mesh.status") { t, r -> t.copy(mesh = r) }
                delay(POLL_MS)
            }
        }
        conn.closed.await()
        events.cancel()
        keepAlive.cancel()
        poll.cancel()
        conn.revoked
    }

    private suspend fun read(conn: GatewayConnection, method: String, put: (Telemetry, Reading) -> Telemetry) {
        try {
            val r = conn.call(method, timeoutMs = 5_000).result
            _telemetry.update { put(it, Reading(r, null, now())) }
        } catch (e: GatewayError) {
            // Not permitted, or the service behind it is away: the last value
            // stays, marked with why it is no longer fresh.
            _telemetry.update { t ->
                val old = when (method) {
                    "system.info" -> t.info
                    "system.status" -> t.system
                    "wifi.status" -> t.wifi
                    "radio.status" -> t.radio
                    else -> t.mesh
                }
                put(t, old.copy(error = e.code))
            }
        } catch (_: IOException) {
        } catch (_: TimeoutCancellationException) {
        }
    }

    /** Forget from the phone: tell the unit when it is reachable (PAIRING.md section 5). */
    suspend fun unpair(): Boolean {
        val conn = live ?: return false
        return try {
            conn.call("gateway.unpair", timeoutMs = 3_000)
            true
        } catch (_: IOException) {
            false
        } catch (_: TimeoutCancellationException) {
            false
        } finally {
            conn.close()
        }
    }

    /**
     * Pairing (PAIRING.md section 3). The unit's key is checked against the
     * code before the token is sent; the answer waits for a person's tap on
     * the unit. [onWaiting] is called once the token was taken.
     */
    suspend fun pair(code: PairingUri, phoneName: String, onWaiting: () -> Unit): PairOutcome {
        var conn: GatewayConnection? = null
        try {
            conn = GatewayConnection(open(code.host, code.port, code.fingerprint, PAIR_WAIT_MS))
            val hello = conn.hello(appVersion)
            val id = Fingerprint.hex(code.fingerprint)
            if (!hello.deviceId.equals(id, ignoreCase = true)) return PairOutcome.WrongUnit
            val unit = PairedUnit(id, hello.name.ifBlank { code.name }, code.host, code.port, now())
            if (hello.paired) return PairOutcome.Paired(unit, hello.scopes) // this phone was already known
            onWaiting()
            val r = conn.call("gateway.pair", buildJsonObject {
                put("token", code.token)
                put("name", phoneName)
            }, timeoutMs = PAIR_WAIT_MS).result
            return if (r.bool("paired") == true) PairOutcome.Paired(unit, r.strings("scopes")) else PairOutcome.Refused
        } catch (e: TimeoutCancellationException) {
            return PairOutcome.Failed("nobody answered on the unit")
        } catch (e: GatewayError) {
            return if (e.code == GatewayError.POLICY) PairOutcome.Refused else PairOutcome.Failed(e.message ?: "refused")
        } catch (e: IOException) {
            if (GatewayTls.isPinMismatch(e)) return PairOutcome.WrongUnit
            return PairOutcome.Failed("the unit could not be reached", e.toString())
        } finally {
            conn?.close()
        }
    }

    private suspend fun open(host: String, port: Int, pin: ByteArray, silenceMs: Long = SILENCE_MS): Socket {
        val raw = transport.connect(host, port, CONNECT_MS)
        return withContext(Dispatchers.IO) {
            try {
                raw.soTimeout = HANDSHAKE_MS
                val tls = GatewayTls.handshake(raw, host, port, identity, pin)
                tls.soTimeout = silenceMs.toInt()
                tls
            } catch (e: Throwable) {
                try {
                    raw.close()
                } catch (_: IOException) {
                }
                throw e
            }
        }
    }

    /** Step 3 of "find the unit again": TLS only where the port answers, and only the pinned key. */
    private suspend fun search(unit: PairedUnit): String? {
        for (host in transport.probe(unit.port)) {
            if (host == unit.host) continue
            try {
                open(host, unit.port, unit.pin).close()
                return host
            } catch (_: IOException) {
            }
        }
        return null
    }

    companion object {
        const val CONNECT_MS = 4_000
        const val HANDSHAKE_MS = 8_000
        const val PING_MS = 30_000L
        const val SILENCE_MS = 90_000L
        const val POLL_MS = 3_000L
        const val PAIR_WAIT_MS = 130_000L

        /** 1 s doubling to 60 s, with a fifth of jitter either way. */
        fun backoffMs(attempt: Int, random: Random = Random.Default): Long {
            val base = 1_000L shl attempt.coerceIn(0, 6)
            val capped = minOf(base, 60_000L)
            return (capped * (0.8 + random.nextDouble() * 0.4)).toLong()
        }
    }
}
