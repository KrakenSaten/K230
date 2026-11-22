// Copyright (c) 2026 Doors authors.
// SPDX-License-Identifier: Apache-2.0
package app.doors.companion.core

import kotlinx.coroutines.flow.StateFlow
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.JsonPrimitive
import kotlinx.serialization.json.longOrNull

/*
 * PacketShark as a paired phone may see it (gateway.md section 9, ADR-016
 * amendment 1, which is PROPOSED): the capture's state and aggregate figures,
 * and nothing that names a host. Everything here is built from counts, rates
 * and sizes. The parser takes the fields it knows by name and keeps nothing
 * else of an answer, so a field that should never have been sent cannot reach
 * a screen or a store through it.
 */

enum class CaptureState(val wire: String) {
    IDLE("idle"), CAPTURING("capturing"), HOLD("hold"), SAVING("saving"), STOPPED("stopped"), ENDED("ended");

    /** A capture exists and can be held or stopped. */
    val running: Boolean get() = this == CAPTURING || this == HOLD

    companion object {
        fun of(wire: String?): CaptureState? = entries.firstOrNull { it.wire == wire }
    }
}

/** The eight classes PacketShark sorts frames into, in its own order. */
enum class TrafficClass(val wire: String, val word: String) {
    TCP("tcp", "TCP"), UDP("udp", "UDP"), DNS("dns", "DNS"), DHCP("dhcp", "DHCP"),
    ARP("arp", "ARP"), ICMP("icmp", "ICMP"), IPV6("ipv6", "IPv6"), OTHER("other", "Other"),
}

/** PacketShark's six findings, in the order `findings[]` carries their totals. */
enum class CaptureSignal(val word: String) {
    RETRANS("Retrans"), TCP_RST("TCP reset"), DNS_FAIL("DNS fail"),
    UNREACH("Unreach"), NO_ANSWER("No answer"), NO_DNS_REPLY("No DNS reply"),
}

/** The DNS and TCP diagnosis: totals since START. A figure the unit did not send is zero. */
data class CaptureDiag(
    val dnsQueries: Long = 0, val dnsRetries: Long = 0, val dnsAnswered: Long = 0, val dnsLate: Long = 0,
    val dnsNoReply: Long = 0, val dnsWaiting: Long = 0,
    val dnsNxdomain: Long = 0, val dnsServfail: Long = 0, val dnsRefused: Long = 0,
    val dnsRttSumUs: Long = 0, val dnsRttMaxUs: Long = 0,
    val tcpAttempts: Long = 0, val tcpConnected: Long = 0, val tcpTimed: Long = 0,
    val tcpHsSumUs: Long = 0, val tcpHsMaxUs: Long = 0,
    val tcpRefused: Long = 0, val tcpAborted: Long = 0,
) {
    /** Mean DNS response time in microseconds, over the answers that were timed; null when there are none. */
    val dnsRttAvgUs: Long? get() = (dnsAnswered + dnsLate).takeIf { it > 0 }?.let { dnsRttSumUs / it }
    val tcpHsAvgUs: Long? get() = tcpTimed.takeIf { it > 0 }?.let { tcpHsSumUs / it }
    val dnsFailures: Long get() = dnsNoReply + dnsServfail + dnsRefused
    val tcpFailures: Long get() = tcpRefused + tcpAborted
}

/** One `capture.status` answer or `capture.stats` event. */
data class CaptureStatus(
    /** False when PacketShark is not the app in front on the unit: nothing else is known then. */
    val available: Boolean,
    val state: CaptureState = CaptureState.IDLE,
    val reason: String = "none",
    val iface: String = "",
    val fullBytes: Boolean = false,
    val elapsedMs: Long = 0,
    /** The capture's own time limit in seconds; 0 is none. */
    val limitS: Long = 0,
    val seen: Long = 0,
    val stored: Long = 0,
    val held: Long = 0,
    val kernelDrops: Long = 0,
    val ifaceDrops: Long = 0,
    val shed: Long = 0,
    val bytes: Long = 0,
    val pps: Long = 0,
    val bps: Long = 0,
    val fillPermille: Int = 0,
    /** Frames per class since START, indexed by [TrafficClass.ordinal]. */
    val classes: List<Long> = ZERO_CLASSES,
    /** Findings since START, indexed by [CaptureSignal.ordinal]. */
    val findings: List<Long> = ZERO_SIGNALS,
    val diag: CaptureDiag = CaptureDiag(),
    val requestedByCompanion: Boolean = false,
) {
    val drops: Long get() = kernelDrops + ifaceDrops + shed

    companion object {
        val ZERO_CLASSES: List<Long> = List(TrafficClass.entries.size) { 0L }
        val ZERO_SIGNALS: List<Long> = List(CaptureSignal.entries.size) { 0L }
        private val REASONS = setOf("none", "user", "left", "limit", "link", "helper_crashed", "helper_hung", "refused", "timeout", "end", "error")
        private val IFACE = Regex("[a-z][a-z0-9]{0,14}")

        /**
         * Reads the fields of gateway.md section 9, by name. A count is never
         * negative, a word is one of the words the contract lists, and an
         * interface name is an interface name: what does not fit is left at
         * its default rather than shown.
         */
        fun parse(o: JsonObject, event: Boolean = false): CaptureStatus {
            // An answer says whether PacketShark is there. A `capture.stats` event is only sent while it is.
            if (o.bool("available") == false || (!event && o.bool("available") != true)) return CaptureStatus(available = false)
            fun n(key: String) = (o.long(key) ?: 0).coerceAtLeast(0)
            val cls = o.obj("classes")
            val d = o.obj("diag")
            fun dn(key: String) = (d?.long(key) ?: 0).coerceAtLeast(0)
            val found = o.array("findings").map { ((it as? JsonPrimitive)?.takeIf { p -> !p.isString }?.longOrNull ?: 0).coerceAtLeast(0) }
            return CaptureStatus(
                available = true,
                state = CaptureState.of(o.string("state")) ?: CaptureState.IDLE,
                reason = o.string("reason")?.takeIf { it in REASONS } ?: "none",
                iface = o.string("iface")?.takeIf { IFACE.matches(it) }.orEmpty(),
                fullBytes = o.string("keep") == "full",
                elapsedMs = n("elapsed_ms"),
                limitS = n("limit_s"),
                seen = n("seen"), stored = n("stored"), held = n("held"),
                kernelDrops = n("kernel_drops"), ifaceDrops = n("iface_drops"), shed = n("shed"),
                bytes = n("bytes"), pps = n("pps"), bps = n("bps"),
                fillPermille = n("fill_permille").coerceAtMost(1000).toInt(),
                classes = TrafficClass.entries.map { (cls?.long(it.wire) ?: 0).coerceAtLeast(0) },
                findings = CaptureSignal.entries.map { found.getOrElse(it.ordinal) { 0L } },
                diag = CaptureDiag(
                    dnsQueries = dn("dns_queries"), dnsRetries = dn("dns_retries"), dnsAnswered = dn("dns_answered"),
                    dnsLate = dn("dns_late"), dnsNoReply = dn("dns_noreply"), dnsWaiting = dn("dns_waiting"),
                    dnsNxdomain = dn("dns_nxdomain"), dnsServfail = dn("dns_servfail"), dnsRefused = dn("dns_refused"),
                    dnsRttSumUs = dn("dns_rtt_sum_us"), dnsRttMaxUs = dn("dns_rtt_max_us"),
                    tcpAttempts = dn("tcp_attempts"), tcpConnected = dn("tcp_connected"), tcpTimed = dn("tcp_timed"),
                    tcpHsSumUs = dn("tcp_hs_sum_us"), tcpHsMaxUs = dn("tcp_hs_max_us"),
                    tcpRefused = dn("tcp_refused"), tcpAborted = dn("tcp_aborted"),
                ),
                requestedByCompanion = o.bool("requested_by_companion") == true,
            )
        }
    }
}

/** One second of the traffic graph, made on the phone from two consecutive readings. */
data class CaptureSample(
    val elapsedMs: Long,
    val pps: Long,
    val bps: Long,
    /** Frames per second per class, indexed by [TrafficClass.ordinal]. */
    val classes: List<Long>,
    val drops: Long,
)

/** Why the figures are, or are not, there. */
enum class CaptureAccess {
    /** No session with the unit. */
    OFFLINE,

    /** The unit's gateway does not serve capture (no `capture` capability). */
    NOT_SERVED,

    /** The unit has not granted this phone `capture_view`. */
    NOT_GRANTED,

    /** Asked; no answer yet. */
    WAITING,

    /** PacketShark is not the app in front on the unit (`available: false`). */
    CLOSED,

    /** The figures are being read. */
    OPEN,
}

/** Where a start request stands. A request starts nothing: a person at the unit does. */
enum class StartRequest { NONE, SHOWN, STARTED, REFUSED, TIMED_OUT }

enum class CaptureCommand { HOLD, RESUME, STOP, REQUEST_START }

/** What a screen needs, whoever made it. [simulated] is never true for figures read from a unit. */
data class CaptureView(
    val simulated: Boolean = false,
    val access: CaptureAccess = CaptureAccess.OFFLINE,
    /** The unit granted `capture_control` as well. */
    val canControl: Boolean = false,
    val status: CaptureStatus? = null,
    /** The last [CaptureFeed.KEPT] seconds, oldest first. Memory only. */
    val history: List<CaptureSample> = emptyList(),
    val readAtMs: Long = 0,
    /** A command that is waiting for the unit's answer. */
    val busy: CaptureCommand? = null,
    /** Why the last command did nothing, in words for a person; null when it worked or none was given. */
    val notice: String? = null,
    val request: StartRequest = StartRequest.NONE,
    val requestAtMs: Long = 0,
)

/**
 * A source of capture figures and the three things a phone may ask for. The
 * screen is written against this: the gateway reader and the labelled
 * simulation are both one of these.
 */
interface CaptureSource {
    val view: StateFlow<CaptureView>

    /**
     * The figures are on screen, or no longer. They are read only while they
     * are shown: the unit marks a companion that reads them (ADR-016 rule 20),
     * and the mark should be true.
     */
    fun watch(on: Boolean)

    suspend fun hold(on: Boolean)
    suspend fun stop()

    /** Asks the unit to show its start sheet. [limitS] is one of [CaptureLimits]. */
    suspend fun requestStart(iface: String, limitS: Int)

    fun dismissNotice()
}

/** What a request may ask for, and how often (ADR-016 rule 1a; gateway.md section 9). */
object CaptureLimits {
    val LIMITS_S = listOf(300, 900, 3600)
    const val REQUEST_EVERY_MS = 30_000L
    const val SHEET_MS = 60_000L
    /** What the gateway takes: the kind and number of an interface, as a status may show it. */
    private val IFACE = Regex("(eth|wlan|usb|synth)[0-9]{1,2}")

    fun ifaceOk(name: String) = IFACE.matches(name)
}

/**
 * Turns readings into a view: keeps the newest, and makes the graph's seconds
 * from the difference between two of them. Pure, so the gateway reader, the
 * simulation and the tests share it.
 */
class CaptureFeed {
    private var last: CaptureStatus? = null
    private val samples = ArrayDeque<CaptureSample>()

    fun reset() {
        last = null
        samples.clear()
    }

    val history: List<CaptureSample> get() = samples.toList()

    /** Takes a reading. Returns true when it added a second to the graph. */
    fun take(s: CaptureStatus): Boolean {
        // A reading from while PacketShark was not in front is no baseline: the next one starts afresh.
        val before = last?.takeIf { it.available }
        last = s
        if (!s.available || s.state == CaptureState.IDLE) {
            samples.clear()
            return false
        }
        // Another capture began: the graph begins again with it.
        if (before != null && (s.elapsedMs < before.elapsedMs || s.seen < before.seen)) samples.clear()
        if (s.state != CaptureState.CAPTURING && s.state != CaptureState.HOLD) return false
        val dt = if (before == null) 0 else s.elapsedMs - before.elapsedMs
        // The first reading has nothing to be compared with, and two readings
        // of the same moment (an answer and an event) are one second, not two.
        if (before == null || dt < MIN_STEP_MS) {
            if (before != null && dt in 0 until MIN_STEP_MS) last = before
            return false
        }
        val perClass = s.classes.indices.map { i -> ((s.classes[i] - before.classes[i]).coerceAtLeast(0) * 1000 / dt) }
        samples += CaptureSample(s.elapsedMs, s.pps, s.bps, perClass, (s.drops - before.drops).coerceAtLeast(0))
        while (samples.size > KEPT) samples.removeFirst()
        return true
    }

    companion object {
        const val KEPT = 120
        const val MIN_STEP_MS = 400L
    }
}
