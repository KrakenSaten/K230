// Copyright (c) 2026 Doors authors.
// SPDX-License-Identifier: Apache-2.0
package app.doors.companion.core

import java.net.Socket
import java.security.cert.CertificateException
import java.security.cert.X509Certificate
import javax.net.ssl.SSLEngine
import javax.net.ssl.X509ExtendedTrustManager

/** The unit answered with a key that is not the pinned one. Never retried, never accepted. */
class PinMismatchException : CertificateException("this is not the unit you paired")

/**
 * The only trust decision the app makes (PAIRING.md section 2): the peer sent
 * exactly one certificate, its key is on P-256, and the SHA-256 of its
 * SubjectPublicKeyInfo is the one pinned fingerprint, compared in constant
 * time. No authority, no chain, no date, no name takes part, and nothing is
 * delegated to the platform. It is narrower than standard validation, not
 * wider: it accepts one key.
 *
 * This is the only trust manager in the app (SecurityLintTest holds that).
 */
class PinnedTrustManager(pin: ByteArray) : X509ExtendedTrustManager() {
    private val pin: ByteArray = pin.copyOf()

    init {
        require(this.pin.size == Fingerprint.LEN) { "a pin is 32 bytes" }
    }

    private fun check(chain: Array<out X509Certificate>?) {
        if (chain == null || chain.size != 1) {
            throw CertificateException("the unit must present exactly one certificate")
        }
        if (!Fingerprint.equal(Fingerprint.ofCertificate(chain[0]), pin)) {
            throw PinMismatchException()
        }
    }

    override fun checkServerTrusted(chain: Array<out X509Certificate>?, authType: String?) = check(chain)
    override fun checkServerTrusted(chain: Array<out X509Certificate>?, authType: String?, socket: Socket?) = check(chain)
    override fun checkServerTrusted(chain: Array<out X509Certificate>?, authType: String?, engine: SSLEngine?) = check(chain)

    // The app is only ever a TLS client.
    override fun checkClientTrusted(chain: Array<out X509Certificate>?, authType: String?): Unit =
        throw CertificateException("this app accepts no client")

    override fun checkClientTrusted(chain: Array<out X509Certificate>?, authType: String?, socket: Socket?): Unit =
        throw CertificateException("this app accepts no client")

    override fun checkClientTrusted(chain: Array<out X509Certificate>?, authType: String?, engine: SSLEngine?): Unit =
        throw CertificateException("this app accepts no client")

    override fun getAcceptedIssuers(): Array<X509Certificate> = emptyArray()
}
