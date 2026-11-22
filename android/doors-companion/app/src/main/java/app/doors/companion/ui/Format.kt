// Copyright (c) 2026 Doors authors.
// SPDX-License-Identifier: Apache-2.0
package app.doors.companion.ui

import java.util.Locale
import kotlin.math.abs

/**
 * How figures are written on the instrument screens. One place, no locale of
 * the phone's: a count reads the same on every phone, as it does on the unit.
 */
object Fmt {
    private const val THIN = ' '

    /** 48417 as "48 417": digits in threes, a thin space between. */
    fun count(n: Long): String {
        val digits = abs(n).toString()
        val out = StringBuilder()
        for ((i, c) in digits.withIndex()) {
            if (i > 0 && (digits.length - i) % 3 == 0) out.append(THIN)
            out.append(c)
        }
        return if (n < 0) "-$out" else out.toString()
    }

    /** A count that must fit a small cell: 1284, 12.8k, 3.4M. */
    fun short(n: Long): String = when {
        n < 10_000 -> n.toString()
        n < 1_000_000 -> scaled(n / 1e3, "k")
        n < 1_000_000_000 -> scaled(n / 1e6, "M")
        else -> scaled(n / 1e9, "G")
    }

    /** A power of ten on a graph's axis: 10, 100, 1k, 10k, 1M. */
    fun axis(n: Long): String = when {
        n < 1_000 -> n.toString()
        n < 1_000_000 -> "${n / 1_000}k"
        n < 1_000_000_000 -> "${n / 1_000_000}M"
        else -> "${n / 1_000_000_000}G"
    }

    /** Bits per second: "9.6 Mb/s". Decimal steps, as link speeds are named. */
    fun bitrate(bps: Long): Pair<String, String> = when {
        bps < 1_000 -> bps.toString() to "b/s"
        bps < 1_000_000 -> number(bps / 1e3) to "kb/s"
        bps < 1_000_000_000 -> number(bps / 1e6) to "Mb/s"
        else -> number(bps / 1e9) to "Gb/s"
    }

    /** Bytes held or counted: "31.4 MB". Binary steps, as storage is named on the unit. */
    fun bytes(b: Long): String {
        val units = listOf("B", "KB", "MB", "GB", "TB")
        var v = b.toDouble()
        var i = 0
        while (v >= 1024 && i < units.lastIndex) {
            v /= 1024
            i++
        }
        return if (i == 0) "$b B" else "${number(v)} ${units[i]}"
    }

    /** A capture's clock: "04:13", and "1:04:13" from the first hour. */
    fun clock(ms: Long): String {
        val s = (ms / 1000).coerceAtLeast(0)
        val h = s / 3600
        val m = (s % 3600) / 60
        val sec = s % 60
        return if (h > 0) "%d:%02d:%02d".format(Locale.ROOT, h, m, sec) else "%02d:%02d".format(Locale.ROOT, m, sec)
    }

    /** A time limit as a person says it: "15 min", "1 h", "no limit". */
    fun limit(seconds: Long): String = when {
        seconds <= 0 -> "no limit"
        seconds % 3600 == 0L -> "${seconds / 3600} h"
        else -> "${seconds / 60} min"
    }

    /** Microseconds as a latency: "840 µs", "12.4 ms", "1.20 s". */
    fun latency(us: Long?): String = when {
        us == null -> "—"
        us < 1_000 -> "$us µs"
        us < 1_000_000 -> "${number(us / 1e3)} ms"
        else -> "%.2f s".format(Locale.ROOT, us / 1e6)
    }

    /** A share of a whole, in tenths of a percent: "71.2%". Nothing of nothing is a dash. */
    fun share(part: Long, whole: Long): String =
        if (whole <= 0) "—" else "%.1f%%".format(Locale.ROOT, part * 100.0 / whole)

    fun permille(p: Int): String = "${(p + 5) / 10}%"

    /** Three significant figures at most: 9.63 → "9.6", 96.3 → "96", 963 → "963". */
    private fun number(v: Double): String = if (v < 10) "%.1f".format(Locale.ROOT, v) else "%.0f".format(Locale.ROOT, v)

    private fun scaled(v: Double, unit: String): String = number(v) + unit
}
