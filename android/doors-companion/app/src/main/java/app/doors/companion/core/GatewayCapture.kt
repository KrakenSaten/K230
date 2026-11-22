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
 * The contract is ADR-016 amendment 1 (accepted 2026-11-22) as gatewayd
 * serves it since G1:
 *  - the gateway names the `capture` capability to a phone that holds
 *    `capture_view`; a phone that is granted the scope during a session asks
 *    anyway, and a gateway that does not know the method is "not served";
 *  - `capture.hold` and `capture.stop` answer `{state, changing}`: the state
 *    when asked, and whether the capture was told to change;
 *  - `capture.request_start` answers `{shown: true}`; what the person at the
 *    unit does arrives as a `capture.state` event: `capturing`, or `idle`
 *    with `refused` or `timeout`. A request starts nothing by itself.
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

    /** The gateway named the capability in its hello. */
    private var named = false

    /** This gateway does not know the capture methods at all. */
    @Volatile
    private var unserved = false

    override fun watch(on: Boolean) {
        watching.value = on
    }

    override fun dismissNotice() = _view.update { it.copy(notice = null) }

    override suspend fun run(unit: PairedUnit, conn: GatewayConnection, hello: Hello, scopes: StateFlow<Set<String>>) {
        try {
            named = CAPABILITY in hello.capabilities
            unserved = false
            // A gateway that serves capture names it to a phone that may read.
            // One that was told the phone may read and still did not name it
            // does not serve it: nothing is asked of it.
            if (!named && SCOPE_VIEW in scopes.value) {
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
                    if (unserved) awaitCancellation() // asked once, and not again until the grant changes
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
                // What became of a request is said by the event alone: by the
                // time the status is read again, setup is plain setup.
                if (e != null && e.name == EVENT_STATE) outcome(e.data)
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
            // A gateway that never named the capability and does not know
            // the method does not serve capture.
            GatewayError.UNKNOWN_METHOD -> if (!named) {
                unserved = true
                feed.reset()
                _view.update { CaptureView(access = CaptureAccess.NOT_SERVED) }
            } else _view.update { it.copy(access = CaptureAccess.CLOSED, status = null, history = emptyList()) }
            // PacketShark's side of it is away (the shell, or a unit without it): not readable now.
            GatewayError.NOT_SUPPORTED, GatewayError.UPSTREAM ->
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
        return verdict(s.state, s.reason) ?: r
    }

    /** What a state and its reason say of a request that is shown, or null when they say nothing yet. */
    private fun verdict(state: CaptureState?, reason: String?): StartRequest? = when {
        state?.running == true -> StartRequest.STARTED
        state == CaptureState.IDLE && reason == "refused" -> StartRequest.REFUSED
        state == CaptureState.IDLE && reason == "timeout" -> StartRequest.TIMED_OUT
        else -> null
    }

    /** A `capture.state` event: `{state, reason, iface, keep, limit_s}`. Only its two words are read. */
    private fun outcome(o: JsonObject) {
        val v = verdict(CaptureState.of(o.string("state")), o.string("reason")) ?: return
        _view.update { if (it.request == StartRequest.SHOWN) it.copy(request = v) else it }
    }

    /** The sheet closes itself after a minute; a phone that heard nothing by then says so. */
    private fun expireRequest() = _view.update { v ->
        if (v.request == StartRequest.SHOWN && now() - v.requestAtMs > CaptureLimits.SHEET_MS + 5_000) v.copy(request = StartRequest.TIMED_OUT) else v
    }

    override suspend fun hold(on: Boolean) = command(if (on) CaptureCommand.HOLD else CaptureCommand.RESUME) { conn ->
        if (_view.value.status?.state?.running != true) return@command "Nothing is capturing."
        answer(conn.call(METHOD_HOLD, buildJsonObject { put("on", on) }, CALL_MS).result, if (on) CaptureState.HOLD else CaptureState.CAPTURING)
    }

    override suspend fun stop() = command(CaptureCommand.STOP) { conn ->
        if (_view.value.status?.state?.running != true) return@command "Nothing is capturing."
        answer(conn.call(METHOD_STOP, timeoutMs = CALL_MS).result, CaptureState.STOPPED)
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

    /**
     * `{state, changing}`: the state when asked, and whether the capture was
     * told to change. Told to, it is shown as [target] at once; the unit's
     * own word follows as an event. Not told to (it was already so), it is
     * shown as the unit said it is.
     */
    private fun answer(r: JsonObject, target: CaptureState): String? {
        val state = (if (r.bool("changing") == true) target else CaptureState.of(r.string("state"))) ?: return null
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
                GatewayError.BUSY -> "Another request is waiting on the unit."
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
