// Copyright (c) 2026 Doors authors.
// SPDX-License-Identifier: Apache-2.0
package app.doors.companion.core

import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Job
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.launch
import kotlin.math.PI
import kotlin.math.sin
import kotlin.random.Random

/**
 * A capture that never happened: figures made on the phone so that the
 * PacketShark screen can be built, shown and tested without a unit that
 * serves capture. Every view it gives says [CaptureView.simulated], and the
 * screen says so wherever the figures are. It talks to nothing, reads nothing
 * of the phone's own network, and its "request" is answered by itself.
 *
 * The traffic is a made-up office link: mostly TCP in slow waves with a burst
 * now and then, a little of everything else, a few findings.
 */
class SimulatedCapture(
    private val scope: CoroutineScope,
    private val now: () -> Long = System::currentTimeMillis,
    private val tickMs: Long = 1_000,
    seed: Long = 7,
) : CaptureSource {
    private val _view = MutableStateFlow(CaptureView(simulated = true, access = CaptureAccess.OPEN, canControl = true))
    override val view: StateFlow<CaptureView> = _view

    private val feed = CaptureFeed()
    private val rnd = Random(seed)
    private var job: Job? = null
    private var s = CaptureStatus(available = true)
    private var burst = 0
    private var requested: Pair<String, Int>? = null
    private var requestTicks = 0

    override fun watch(on: Boolean) {
        job?.cancel()
        job = null
        if (!on) return
        job = scope.launch {
            if (s.state == CaptureState.IDLE && s.seen == 0L) {
                // Opens a little over a minute into a capture, so the graph has something to say.
                begin("eth0", 900, byCompanion = false)
                feed.take(s)
                repeat(PREFILL_S) {
                    step()
                    feed.take(s)
                }
            }
            publish()
            while (true) {
                delay(tickMs)
                step()
                publish()
            }
        }
    }

    override fun dismissNotice() = _view.update { it.copy(notice = null) }

    override suspend fun hold(on: Boolean) = command(if (on) CaptureCommand.HOLD else CaptureCommand.RESUME) {
        if (!s.state.running) return@command "Nothing is capturing."
        s = s.copy(state = if (on) CaptureState.HOLD else CaptureState.CAPTURING, held = 0)
        null
    }

    override suspend fun stop() = command(CaptureCommand.STOP) {
        if (!s.state.running) return@command "Nothing is capturing."
        s = s.copy(state = CaptureState.STOPPED, reason = "user", pps = 0, bps = 0)
        null
    }

    override suspend fun requestStart(iface: String, limitS: Int) = command(CaptureCommand.REQUEST_START) {
        if (!CaptureLimits.ifaceOk(iface) || limitS !in CaptureLimits.LIMITS_S) return@command "That is not a capture the unit offers."
        if (s.state.running) return@command "A capture is already running."
        requested = iface to limitS
        requestTicks = 0
        _view.update { it.copy(request = StartRequest.SHOWN, requestAtMs = now()) }
        null
    }

    private suspend fun command(what: CaptureCommand, act: () -> String?) {
        if (_view.value.busy != null) return
        _view.update { it.copy(busy = what, notice = null) }
        var notice: String? = null
        try {
            delay(ANSWER_MS)
            notice = act()
            publish()
        } finally {
            _view.update { it.copy(busy = null, notice = notice) }
        }
    }

    private fun begin(iface: String, limitS: Int, byCompanion: Boolean) {
        feed.reset()
        burst = 0
        s = CaptureStatus(available = true, state = CaptureState.CAPTURING, iface = iface, limitS = limitS.toLong(), requestedByCompanion = byCompanion)
    }

    private fun publish() {
        feed.take(s)
        _view.update { it.copy(status = s, history = feed.history, readAtMs = now()) }
    }

    /** One second of made-up traffic. */
    private fun step() {
        val asked = requested
        if (asked != null && ++requestTicks >= TAP_AFTER_S) {
            // Nobody tapped anything: the simulation plays the person at the unit.
            requested = null
            begin(asked.first, asked.second, byCompanion = true)
            _view.update { it.copy(request = StartRequest.STARTED) }
            return
        }
        if (!s.state.running) return
        val t = s.elapsedMs / 1000.0
        if (burst == 0 && rnd.nextInt(40) == 0) burst = 4 + rnd.nextInt(5)
        val lift = if (burst > 0) 3.2 else 1.0
        if (burst > 0) burst--
        val tcp = ((620 + 380 * sin(2 * PI * t / 47) + rnd.nextInt(-90, 90)) * lift).toLong().coerceAtLeast(20)
        val per = listOf(
            tcp,
            (70 + 40 * sin(2 * PI * t / 23) + rnd.nextInt(0, 30)).toLong(),
            rnd.nextInt(1, 8).toLong(),
            if (rnd.nextInt(30) == 0) 1L else 0L,
            rnd.nextInt(0, 4).toLong(),
            if (rnd.nextInt(5) == 0) 1L else 0L,
            rnd.nextInt(4, 18).toLong(),
            rnd.nextInt(0, 3).toLong(),
        )
        val pps = per.sum()
        val bytes = tcp * rnd.nextInt(780, 980) + (pps - tcp) * 140
        val shed = if (lift > 1 && rnd.nextInt(6) == 0) rnd.nextInt(2, 12).toLong() else 0L
        val nx = if (rnd.nextInt(50) == 0) 1L else 0L
        val dns = per[TrafficClass.DNS.ordinal]
        val asks = (dns + 1) / 2
        val lost = if (rnd.nextInt(45) == 0) 1L else 0L
        val answered = (asks - lost).coerceAtLeast(0)
        val tries = rnd.nextInt(0, 4).toLong()
        val refusedNow = if (rnd.nextInt(60) == 0) 1L else 0L
        val joined = (tries - refusedNow).coerceAtLeast(0)
        val d = s.diag
        val stored = s.stored + pps - shed
        val elapsed = s.elapsedMs + 1000
        val limited = s.limitS > 0 && elapsed >= s.limitS * 1000
        s = s.copy(
            state = if (limited) CaptureState.ENDED else s.state,
            reason = if (limited) "limit" else s.reason,
            elapsedMs = elapsed,
            seen = s.seen + pps, stored = stored,
            held = if (s.state == CaptureState.HOLD) s.held + pps else 0,
            shed = s.shed + shed,
            bytes = s.bytes + bytes,
            pps = if (limited) 0 else pps, bps = if (limited) 0 else bytes * 8,
            fillPermille = (stored * 1000 / RING).coerceAtMost(1000).toInt(),
            classes = s.classes.mapIndexed { i, v -> v + per[i] },
            findings = s.findings.mapIndexed { i, v ->
                v + when (CaptureSignal.entries[i]) {
                    CaptureSignal.RETRANS -> if (rnd.nextInt(6) == 0) rnd.nextInt(1, 4).toLong() else 0L
                    CaptureSignal.TCP_RST -> refusedNow
                    CaptureSignal.DNS_FAIL -> nx
                    CaptureSignal.UNREACH -> if (rnd.nextInt(200) == 0) 1L else 0L
                    CaptureSignal.NO_ANSWER -> 0L
                    CaptureSignal.NO_DNS_REPLY -> lost
                }
            },
            diag = d.copy(
                dnsQueries = d.dnsQueries + asks, dnsAnswered = d.dnsAnswered + answered, dnsNoReply = d.dnsNoReply + lost,
                dnsRetries = d.dnsRetries + lost,
                dnsNxdomain = d.dnsNxdomain + nx,
                dnsRttSumUs = d.dnsRttSumUs + answered * rnd.nextInt(9_000, 38_000),
                dnsRttMaxUs = maxOf(d.dnsRttMaxUs, if (answered > 0) rnd.nextInt(20_000, 140_000).toLong() else 0L),
                tcpAttempts = d.tcpAttempts + tries, tcpConnected = d.tcpConnected + joined, tcpTimed = d.tcpTimed + joined,
                tcpHsSumUs = d.tcpHsSumUs + joined * rnd.nextInt(2_000, 26_000),
                tcpHsMaxUs = maxOf(d.tcpHsMaxUs, if (joined > 0) rnd.nextInt(8_000, 90_000).toLong() else 0L),
                tcpRefused = d.tcpRefused + refusedNow,
            ),
        )
    }

    companion object {
        const val PREFILL_S = 75
        const val TAP_AFTER_S = 4
        const val ANSWER_MS = 250L

        /** Frames the made-up store holds before it is full. */
        const val RING = 400_000L
    }
}
