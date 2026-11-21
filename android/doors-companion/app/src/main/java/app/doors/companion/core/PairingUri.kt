// Copyright (c) 2026 Doors authors.
// SPDX-License-Identifier: Apache-2.0
package app.doors.companion.core

import java.net.URLDecoder
import java.util.Base64

/**
 * `doors-pair:1?h=<ipv4>&p=<port>&fp=<unit fingerprint>&t=<token>&n=<unit name>`
 * as the unit's panel shows it (PAIRING.md section 3). `fp` and `t` are
 * base64url without padding: 32 and 16 bytes.
 *
 * The host is an IPv4 literal on a private, link-local or loopback network
 * and is never resolved by name: the unit listens nowhere else, and a code
 * naming a public address is not a unit's.
 */
class PairingUri private constructor(
    val host: String,
    val port: Int,
    val fingerprint: ByteArray,
    val token: String,
    val name: String,
) {
    /** Never the token: this string may reach a log. */
    override fun toString(): String = "PairingUri($host:$port ${Fingerprint.display(fingerprint)})"

    class Invalid(message: String) : Exception(message)

    companion object {
        private const val PREFIX = "doors-pair:"
        private val IPV4 = Regex("""(\d{1,3})\.(\d{1,3})\.(\d{1,3})\.(\d{1,3})""")

        @Throws(Invalid::class)
        fun parse(text: String): PairingUri {
            val s = text.trim()
            if (s.length > 512) throw Invalid("too long to be a pairing code")
            if (!s.startsWith(PREFIX)) throw Invalid("not a DOORS pairing code")
            val q = s.indexOf('?')
            if (q < 0) throw Invalid("not a DOORS pairing code")
            if (s.substring(PREFIX.length, q) != "1") throw Invalid("this pairing code needs a newer app")
            val fields = HashMap<String, String>()
            for (part in s.substring(q + 1).split('&')) {
                val eq = part.indexOf('=')
                if (eq <= 0) continue
                // A repeated field is not something the unit writes.
                if (fields.put(part.substring(0, eq), part.substring(eq + 1)) != null) {
                    throw Invalid("the pairing code is malformed")
                }
            }
            val host = fields["h"] ?: throw Invalid("the pairing code has no address")
            if (!isLocalIpv4(host)) throw Invalid("the address is not on a local network")
            val port = fields["p"]?.toIntOrNull()?.takeIf { it in 1..65535 }
                ?: throw Invalid("the pairing code has no port")
            val fp = decode(fields["fp"], Fingerprint.LEN) ?: throw Invalid("the pairing code has no unit key")
            val token = fields["t"] ?: throw Invalid("the pairing code has no token")
            if (decode(token, 16) == null) throw Invalid("the pairing code has no token")
            val name = try {
                URLDecoder.decode(fields["n"] ?: "", "UTF-8")
            } catch (e: IllegalArgumentException) {
                ""
            }.filter { !it.isISOControl() }.take(64).ifBlank { "DOORS unit" }
            return PairingUri(host, port, fp, token, name)
        }

        private fun decode(v: String?, want: Int): ByteArray? = try {
            v?.let { Base64.getUrlDecoder().decode(it) }?.takeIf { it.size == want }
        } catch (e: IllegalArgumentException) {
            null
        }

        fun isLocalIpv4(host: String): Boolean {
            val m = IPV4.matchEntire(host) ?: return false
            val o = m.groupValues.drop(1).map { it.toInt() }
            if (o.any { it > 255 }) return false
            return o[0] == 10 || o[0] == 127 ||
                (o[0] == 172 && o[1] in 16..31) ||
                (o[0] == 192 && o[1] == 168) ||
                (o[0] == 169 && o[1] == 254)
        }
    }
}
