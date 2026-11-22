// Copyright (c) 2026 Doors authors.
// SPDX-License-Identifier: Apache-2.0
package app.doors.companion.core

import kotlinx.coroutines.CoroutineStart
import kotlinx.coroutines.NonCancellable
import kotlinx.coroutines.TimeoutCancellationException
import kotlinx.coroutines.awaitCancellation
import kotlinx.coroutines.channels.Channel
import kotlinx.coroutines.coroutineScope
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.collectLatest
import kotlinx.coroutines.flow.combine
import kotlinx.coroutines.flow.distinctUntilChanged
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import kotlinx.coroutines.withTimeoutOrNull
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.buildJsonObject
import kotlinx.serialization.json.put
import java.io.IOException

/**
 * Capture figures over a live session (gateway.md section 9). Nothing is asked
 * of a gateway that does not name the `capture` capability, of a unit that has
 * not granted `capture_view`, or while no screen shows the figures; a command
 * is sent only on a person's tap and only with `capture_control`.
 *
 * The four methods exist in the contract and not yet in any gatewayd: until
 * ADR-016 amendment 1 is accepted and implemented on the unit, a real unit
 * leaves this at [CaptureAccess.NOT_SERVED], and that is what is shown.
 */
class GatewayCapture(
    private val now: () -> Long = System::currentTimeMillis,
    private val pollMs: Long = POLL_MS,
) : SessionFeature, CaptureSource {
    private val _view = MutableStateFlow(CaptureView())
    override val view: StateFlow<CaptureView> = _view

    private val watching = MutableStateFlow(false)
    private val feed = CaptureFeed()

    @Volatile
    private var live: GatewayConnection? = null
    private var lastRequestMs = 0L

    /** How the capture stood when the request went out: only something else is an answer to it. */
    private var requestBase: Pair<CaptureState, String>? = null

    override fun watch(on: Boolean) {
        watching.value = on
    }

    override fun dismissNotice() = _view.update { it.copy(notice = null) }

    override suspend fun run(unit: PairedUnit, conn: GatewayConnection, hello: Hello, scopes: StateFlow<Set<String>>) {
        try {
            if (CAPABILITY !in hello.capabilities) {
                _view.update { CaptureView(access = CaptureAccess.NOT_SERVED) }
                awaitCancellation()
            }
            live = conn
            combine(scopes, watching) { s, w -> Triple(SCOPE_VIEW in s, SCOPE_VIEW in s && SCOPE_CONTROL in s, w) }
                .distinctUntilChanged()
                .collectLatest { (granted, control, shown) ->
                    feed.reset()
                    when {
                        !granted -> _view.value = CaptureView(access = CaptureAccess.NOT_GRANTED)
                        // Granted and not on screen: nothing is read, and what was read is not kept as if it were current.
                        !shown -> _view.value = CaptureView(access = CaptureAccess.WAITING, canControl = control)
                        else -> {
                            _view.value = CaptureView(access = CaptureAccess.WAITING, canControl = control)
                            read(conn)
                        }
                    }
                }
        } catch (_: IOException) {
            // The connection ended. The session loop reports that; this only stops.
        } finally {
            live = null
            feed.reset()
            _view.value = CaptureView()
        }
    }

    /** Holds the topic and follows it until the screen goes, the scope goes, or the session ends. */
    private suspend fun read(conn: GatewayConnection) = coroutineScope {
        val inbox = Channel<GatewayEvent>(Channel.UNLIMITED)
        val pump = launch(start = CoroutineStart.UNDISPATCHED) {
            conn.events.collect { if (it.name == EVENT_STATE || it.name == EVENT_STATS) inbox.trySend(it) }
        }
        try {
            var subscribed = false
            while (true) {
                try {
                    if (!subscribed) subscribed = TOPIC in conn.subscribe(add = setOf(TOPIC), timeoutMs = CALL_MS).result.strings("topics")
                    apply(conn.call(METHOD_STATUS, timeoutMs = CALL_MS).result)
                } catch (e: GatewayError) {
                    refused(e)
                } catch (_: TimeoutCancellationException) {
                    // No answer: the last reading stays, and ages on screen.
                }
                // The figures of a running capture come as events. A change of
                // state, or silence, is answered by asking: that is also how a
                // PacketShark that was opened or left on the unit is noticed.
                var e = withTimeoutOrNull(pollMs) { inbox.receive() }
                while (e != null && e.name == EVENT_STATS) {
                    apply(e.data, event = true)
                    e = withTimeoutOrNull(pollMs) { inbox.receive() }
                }
                expireRequest()
            }
        } finally {
            pump.cancel()
            // Stop being a reader on the unit as well, if it can still hear.
            withContext(NonCancellable) {
                try {
                    conn.subscribe(remove = setOf(TOPIC), timeoutMs = 2_000)
                } catch (_: IOException) {
                } catch (_: TimeoutCancellationException) {
                }
            }
        }
    }

    private fun refused(e: GatewayError) {
        when (e.code) {
            GatewayError.NOT_PERMITTED -> {
                feed.reset()
                _view.update { CaptureView(access = CaptureAccess.NOT_GRANTED) }
            }
            // The gateway named the capability and does not know the method,
            // or PacketShark's side of it is away: not readable now.
            GatewayError.UNKNOWN_METHOD, GatewayError.NOT_SUPPORTED, GatewayError.UPSTREAM ->
                _view.update { it.copy(access = CaptureAccess.CLOSED, status = null, history = emptyList()) }
            else -> Unit
        }
    }

    private fun apply(o: JsonObject, event: Boolean = false) {
        val s = CaptureStatus.parse(o, event)
        feed.take(s)
        _view.update { v ->
            if (!s.available) return@update v.copy(access = CaptureAccess.CLOSED, status = null, history = emptyList(), readAtMs = now(), request = settle(v.request, null))
            v.copy(access = CaptureAccess.OPEN, status = s, history = feed.history, readAtMs = now(), request = settle(v.request, s))
        }
    }

    /** What became of a request that is on the unit's screen, as the unit's own state tells it. */
    private fun settle(r: StartRequest, s: CaptureStatus?): StartRequest {
        if (r != StartRequest.SHOWN || s == null) return r
        if (!s.state.running && (s.state to s.reason) == requestBase) return r
        return when {
            s.state.running -> StartRequest.STARTED
            s.state == CaptureState.ENDED && s.reason == "refused" -> StartRequest.REFUSED
            s.state == CaptureState.ENDED && s.reason == "timeout" -> StartRequest.TIMED_OUT
            else -> r
        }
    }

    /** The sheet closes itself after a minute; a phone that heard nothing by then says so. */
    private fun expireRequest() = _view.update { v ->
        if (v.request == StartRequest.SHOWN && now() - v.requestAtMs > CaptureLimits.SHEET_MS + 5_000) v.copy(request = StartRequest.TIMED_OUT) else v
    }

    override suspend fun hold(on: Boolean) = command(if (on) CaptureCommand.HOLD else CaptureCommand.RESUME) { conn ->
        if (_view.value.status?.state?.running != true) return@command "Nothing is capturing."
        answer(conn.call(METHOD_HOLD, buildJsonObject { put("on", on) }, CALL_MS).result)
    }

    override suspend fun stop() = command(CaptureCommand.STOP) { conn ->
        if (_view.value.status?.state?.running != true) return@command "Nothing is capturing."
        answer(conn.call(METHOD_STOP, timeoutMs = CALL_MS).result)
    }

    override suspend fun requestStart(iface: String, limitS: Int) = command(CaptureCommand.REQUEST_START) { conn ->
        // Only what a tap on the unit could choose (ADR-016 rule 1a), checked here before the unit checks it again.
        if (!CaptureLimits.ifaceOk(iface) || limitS !in CaptureLimits.LIMITS_S) return@command "That is not a capture the unit offers."
        if (_view.value.status?.state?.running == true) return@command "A capture is already running."
        val wait = CaptureLimits.REQUEST_EVERY_MS - (now() - lastRequestMs)
        if (lastRequestMs != 0L && wait > 0) return@command "The unit takes one request every 30 seconds. Try again in ${(wait + 999) / 1000} s."
        lastRequestMs = now()
        requestBase = _view.value.status?.let { it.state to it.reason }
        val r = conn.call(METHOD_REQUEST, buildJsonObject {
            put("iface", iface)
            put("limit_s", limitS)
        }, CALL_MS).result
        if (r.bool("shown") == true) {
            _view.update { it.copy(request = StartRequest.SHOWN, requestAtMs = now()) }
            null
        } else "The unit did not show the request."
    }

    private fun answer(r: JsonObject): String? {
        val state = CaptureState.of(r.string("state")) ?: return null
        _view.update { v -> v.copy(status = v.status?.copy(state = state)) }
        return null
    }

    /** One command at a time, only with the scope, only while a session is open. [send] returns why nothing happened, or null. */
    private suspend fun command(what: CaptureCommand, send: suspend (GatewayConnection) -> String?) {
        val conn = live
        val v = _view.value
        if (v.busy != null) return
        if (conn == null || v.access == CaptureAccess.OFFLINE || v.access == CaptureAccess.NOT_SERVED) {
            _view.update { it.copy(notice = "The unit is not connected.") }
            return
        }
        if (!v.canControl) {
            _view.update { it.copy(notice = "The unit has not granted this phone capture control.") }
            return
        }
        _view.update { it.copy(busy = what, notice = null) }
        var notice: String? = null
        try {
            notice = try {
                send(conn)
            } catch (e: GatewayError) {
            when (e.code) {
                GatewayError.POLICY -> when (what) {
                    CaptureCommand.REQUEST_START -> "The unit refused the request: a capture is running, a request is already shown, or the unit is locked."
                    else -> "Nothing is capturing."
                }
                GatewayError.NOT_PERMITTED -> "The unit has not granted this phone capture control."
                GatewayError.RATE -> "The unit takes one request every 30 seconds."
                GatewayError.UPSTREAM, GatewayError.UNKNOWN_METHOD -> "PacketShark is not answering on the unit."
                else -> "The unit did not take it."
            }
        } catch (_: TimeoutCancellationException) {
            "The unit did not answer."
            } catch (_: IOException) {
                "The connection was lost."
            }
        } finally {
            // Also when the screen that asked has gone: a command never stays "waiting".
            _view.update { it.copy(busy = null, notice = notice) }
        }
    }

    companion object {
        const val CAPABILITY = "capture"
        const val SCOPE_VIEW = "capture_view"
        const val SCOPE_CONTROL = "capture_control"
        const val TOPIC = "capture"
        const val METHOD_STATUS = "capture.status"
        const val METHOD_HOLD = "capture.hold"
        const val METHOD_STOP = "capture.stop"
        const val METHOD_REQUEST = "capture.request_start"
        const val EVENT_STATE = "capture.state"
        const val EVENT_STATS = "capture.stats"
        const val POLL_MS = 3_000L
        const val CALL_MS = 5_000L
    }
}
