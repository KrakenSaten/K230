// Copyright (c) 2026 Doors authors.
// SPDX-License-Identifier: Apache-2.0
package app.doors.companion.core

import java.io.IOException
import java.net.Socket
import java.security.cert.X509Certificate
import javax.net.ssl.SSLContext
import javax.net.ssl.SSLHandshakeException
import javax.net.ssl.SSLSocket
import javax.net.ssl.X509KeyManager

/** The phone's own key: what it presents, and its fingerprint. */
interface ClientIdentity {
    fun keyManager(): X509KeyManager
    fun fingerprint(): ByteArray
}

/**
 * The one way a connection to a unit is made: TLS 1.3 only, this phone's key
 * presented, the unit's key pinned. There is no other path, switch or variant.
 */
object GatewayTls {
    const val PROTOCOL = "TLSv1.3"

    /**
     * Wraps a connected socket and completes the handshake. A fresh context
     * per connection: nothing is cached between connections, so every one
     * repeats the full check (PAIRING.md section 2, no resumption in v1).
     */
    @Throws(IOException::class)
    fun handshake(raw: Socket, host: String, port: Int, identity: ClientIdentity, pin: ByteArray): SSLSocket {
        val ctx = SSLContext.getInstance(PROTOCOL)
        ctx.init(arrayOf(identity.keyManager()), arrayOf(PinnedTrustManager(pin)), null)
        val tls = ctx.socketFactory.createSocket(raw, host, port, true) as SSLSocket
        try {
            tls.useClientMode = true
            tls.enabledProtocols = arrayOf(PROTOCOL)
            tls.startHandshake()
            // What the trust manager decided is read back from the finished
            // session, as the unit does on its side.
            val session = tls.session
            if (session.protocol != PROTOCOL) throw SSLHandshakeException("not TLS 1.3")
            val certs = session.peerCertificates
            if (certs.size != 1) throw SSLHandshakeException("the unit must present exactly one certificate")
            val fp = Fingerprint.ofCertificate(certs[0] as X509Certificate)
            if (!Fingerprint.equal(fp, pin)) throw PinMismatchException()
            return tls
        } catch (e: Throwable) {
            try {
                tls.close()
            } catch (_: IOException) {
            }
            throw e
        }
    }

    /** True when a failure, at any depth, is the unit presenting another key. */
    fun isPinMismatch(e: Throwable?): Boolean {
        var c = e
        var depth = 0
        while (c != null && depth < 8) {
            if (c is PinMismatchException) return true
            c = c.cause
            depth++
        }
        return false
    }
}
