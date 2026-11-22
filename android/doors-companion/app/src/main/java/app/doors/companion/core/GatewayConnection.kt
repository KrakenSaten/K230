// Copyright (c) 2026 Doors authors.
// SPDX-License-Identifier: Apache-2.0
package app.doors.companion.core

import kotlinx.coroutines.CompletableDeferred
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.channels.BufferOverflow
import kotlinx.coroutines.flow.MutableSharedFlow
import kotlinx.coroutines.flow.SharedFlow
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.Semaphore
import kotlinx.coroutines.sync.withLock
import kotlinx.coroutines.sync.withPermit
import kotlinx.coroutines.withContext
import kotlinx.coroutines.withTimeout
import kotlinx.serialization.json.Json
import kotlinx.serialization.json.JsonArray
import kotlinx.serialization.json.JsonElement
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.JsonPrimitive
import kotlinx.serialization.json.booleanOrNull
import kotlinx.serialization.json.buildJsonObject
import kotlinx.serialization.json.contentOrNull
import kotlinx.serialization.json.doubleOrNull
import kotlinx.serialization.json.longOrNull
import kotlinx.serialization.json.put
import java.io.Closeable
import java.io.DataInputStream
import java.io.IOException
import java.io.OutputStream
import java.net.Socket
import java.util.concurrent.ConcurrentHashMap
import java.util.concurrent.atomic.AtomicLong

/** The gateway answered with an error (gateway.md section 3). */
class GatewayError(val code: Int, message: String) : IOException(message) {
    companion object {
        const val UNKNOWN_METHOD = 1
        const val INVALID = 2
        const val POLICY = 3
        const val BACKEND = 4
        const val BUSY = 5
        const val NOT_SUPPORTED = 6
        const val NOT_PERMITTED = 7
        const val NO_PROTO = 8
        const val UPSTREAM = 9
        const val RATE = 10
    }
}

class Reply(val result: JsonObject, val monoMs: Long?)
class GatewayEvent(val name: String, val data: JsonObject, val monoMs: Long?)

/** `gateway.hello`. Fields a candidate does not get are null or empty. */
class Hello(
    val proto: Int,
    val deviceId: String,
    val name: String,
    val paired: Boolean,
    val version: String?,
    val build: String?,
    val model: String?,
    val scopes: Set<String>,
    val capabilities: Set<String>,
    val clockSet: Boolean,
    val wallMs: Long?,
    val bootId: String?,
    /** `limits.send_per_min`: how many sends a minute the gateway takes from one phone. */
    val sendPerMin: Int? = null,
)

/**
 * One connection's framing and request/response matching (gateway.md section
 * 1): a 4-byte big-endian length, then one UTF-8 JSON object. Requests to the
 * gateway are at most 64 KiB, answers at most 256 KiB; a longer or malformed
 * frame ends the connection. Unknown fields and events are ignored.
 */
class GatewayConnection(private val socket: Socket) : Closeable {
    private val input = DataInputStream(socket.getInputStream().buffered())
    private val output: OutputStream = socket.getOutputStream()
    private val pending = ConcurrentHashMap<Long, CompletableDeferred<Reply>>()
    private val nextId = AtomicLong(1)
    private val inFlight = Semaphore(MAX_IN_FLIGHT)
    private val writeLock = Any()
    // Wide enough for a burst the gateway may send at once; a collector that
    // still falls behind loses the oldest, and MeshSync notices by the seq.
    private val _events = MutableSharedFlow<GatewayEvent>(extraBufferCapacity = 1024, onBufferOverflow = BufferOverflow.DROP_OLDEST)

    val events: SharedFlow<GatewayEvent> = _events

    /** Completes when the connection has ended, with the reason. */
    val closed = CompletableDeferred<IOException>()

    /** The unit said gateway.revoked on this connection. Set on the reader's thread, before the close is seen. */
    @Volatile
    var revoked: Boolean = false
        private set

    @Volatile
    var lastReceivedAt: Long = System.nanoTime()
        private set

    init {
        Thread({ readLoop() }, "doors-gateway-reader").apply { isDaemon = true }.start()
    }

    private fun readLoop() {
        var reason = IOException("closed")
        try {
            while (!socket.isClosed) dispatch(readFrame())
        } catch (e: IOException) {
            reason = e
        } catch (e: RuntimeException) {
            reason = IOException("malformed frame", e)
        }
        closed.complete(reason)
        try {
            socket.close()
        } catch (_: IOException) {
        }
        for (id in pending.keys) pending.remove(id)?.completeExceptionally(reason)
    }

    private fun readFrame(): JsonObject {
        val n = input.readInt()
        if (n <= 0 || n > FRAME_IN_MAX) throw IOException("frame of $n bytes")
        val buf = ByteArray(n)
        input.readFully(buf)
        lastReceivedAt = System.nanoTime()
        val text = Charsets.UTF_8.newDecoder().decode(java.nio.ByteBuffer.wrap(buf)).toString()
        return JSON.parseToJsonElement(text) as? JsonObject ?: throw IOException("frame is not an object")
    }

    private fun dispatch(msg: JsonObject) {
        val mono = msg.long("mono_ms")
        val event = msg.string("event")
        if (event != null) {
            if (event == "gateway.revoked") revoked = true
            _events.tryEmit(GatewayEvent(event, msg.obj("data") ?: EMPTY, mono))
            return
        }
        val id = msg.long("id") ?: return
        val waiter = pending.remove(id) ?: return
        val error = msg.obj("error")
        if (error != null) {
            waiter.completeExceptionally(GatewayError((error.long("code") ?: 0).toInt(), error.string("message") ?: "error"))
        } else {
            waiter.complete(Reply(msg.obj("result") ?: EMPTY, mono))
        }
    }

    /** One request, one answer. At most 8 wait at once, as the gateway serves them. */
    suspend fun call(method: String, params: JsonObject? = null, timeoutMs: Long = CALL_TIMEOUT_MS): Reply =
        inFlight.withPermit {
            val id = nextId.getAndIncrement()
            val waiter = CompletableDeferred<Reply>()
            pending[id] = waiter
            try {
                val frame = buildJsonObject {
                    put("id", id)
                    put("method", method)
                    if (params != null) put("params", params)
                }.toString().toByteArray(Charsets.UTF_8)
                if (frame.size > FRAME_OUT_MAX) throw IOException("request too large")
                withContext(Dispatchers.IO) {
                    synchronized(writeLock) {
                        val head = byteArrayOf(
                            (frame.size ushr 24).toByte(), (frame.size ushr 16).toByte(),
                            (frame.size ushr 8).toByte(), frame.size.toByte(),
                        )
                        output.write(head + frame)
                        output.flush()
                    }
                }
                withTimeout(timeoutMs) { waiter.await() }
            } finally {
                pending.remove(id)
            }
        }

    private val topicLock = Mutex()
    private val topics = LinkedHashSet<String>()

    /**
     * `gateway.subscribe` replaces the connection's whole set of topics, and
     * more than one feature lives on a connection: the set is kept here, so
     * that one feature's subscription does not end another's. The answer names
     * the topics the gateway really holds for this client now.
     */
    suspend fun subscribe(add: Set<String> = emptySet(), remove: Set<String> = emptySet(), timeoutMs: Long = CALL_TIMEOUT_MS): Reply =
        topicLock.withLock {
            val want = LinkedHashSet(topics).apply { addAll(add); removeAll(remove) }
            val r = try {
                call("gateway.subscribe", buildJsonObject { put("topics", JsonArray(want.map { JsonPrimitive(it) })) }, timeoutMs)
            } catch (e: Throwable) {
                // What was to be given back stays given back: the next subscription does not ask for it again.
                topics.removeAll(remove)
                throw e
            }
            topics.clear()
            topics.addAll(want)
            r
        }

    suspend fun hello(appVersion: String, timeoutMs: Long = CALL_TIMEOUT_MS): Hello {
        val r = call("gateway.hello", buildJsonObject {
            put("proto_min", PROTO)
            put("proto_max", PROTO)
            put("app", "app.doors.companion")
            put("app_version", appVersion)
        }, timeoutMs).result
        return Hello(
            proto = (r.long("proto") ?: 0).toInt(),
            deviceId = r.string("device_id") ?: "",
            name = r.string("name") ?: "",
            paired = r.bool("paired") ?: false,
            version = r.string("version"),
            build = r.string("build"),
            model = r.string("model"),
            scopes = r.strings("scopes"),
            capabilities = r.strings("capabilities"),
            clockSet = r.bool("clock_set") ?: false,
            wallMs = r.long("wall_ms"),
            bootId = r.string("boot_id"),
            sendPerMin = r.obj("limits")?.long("send_per_min")?.toInt()?.takeIf { it > 0 },
        )
    }

    /**
     * Closing a TLS socket writes to it (the close alert), and the callers
     * are on the main thread: the session's own coroutines, also the one
     * that hears gateway.revoked. So the close has a thread of its own;
     * [closed] completes when the reader has seen it.
     */
    override fun close() {
        Thread({
            try {
                socket.close()
            } catch (_: IOException) {
            } catch (_: RuntimeException) {
            }
        }, "doors-gateway-close").apply { isDaemon = true }.start()
    }

    companion object {
        const val PROTO = 1
        const val FRAME_OUT_MAX = 64 * 1024
        const val FRAME_IN_MAX = 256 * 1024
        const val MAX_IN_FLIGHT = 8
        const val CALL_TIMEOUT_MS = 10_000L
        private val JSON = Json { ignoreUnknownKeys = true }
        private val EMPTY = JsonObject(emptyMap())
    }
}

// Tolerant readers: a missing field or one of another type is null, never a crash.
fun JsonObject.string(key: String): String? = (this[key] as? JsonPrimitive)?.takeIf { it.isString }?.contentOrNull
fun JsonObject.long(key: String): Long? = (this[key] as? JsonPrimitive)?.takeIf { !it.isString }?.let { it.longOrNull ?: it.doubleOrNull?.toLong() }
fun JsonObject.double(key: String): Double? = (this[key] as? JsonPrimitive)?.takeIf { !it.isString }?.doubleOrNull
fun JsonObject.bool(key: String): Boolean? = (this[key] as? JsonPrimitive)?.takeIf { !it.isString }?.booleanOrNull
fun JsonObject.obj(key: String): JsonObject? = this[key] as? JsonObject
fun JsonObject.array(key: String): List<JsonElement> = (this[key] as? JsonArray) ?: emptyList()
fun JsonObject.strings(key: String): Set<String> =
    array(key).mapNotNull { (it as? JsonPrimitive)?.takeIf { p -> p.isString }?.content }.toSet()
