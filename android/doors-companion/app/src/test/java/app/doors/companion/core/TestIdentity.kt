// Copyright (c) 2026 Doors authors.
// SPDX-License-Identifier: Apache-2.0
package app.doors.companion.core

import org.bouncycastle.asn1.x500.X500Name
import org.bouncycastle.cert.jcajce.JcaX509CertificateConverter
import org.bouncycastle.cert.jcajce.JcaX509v3CertificateBuilder
import org.bouncycastle.operator.jcajce.JcaContentSignerBuilder
import java.math.BigInteger
import java.net.Socket
import java.security.KeyPair
import java.security.KeyPairGenerator
import java.security.Principal
import java.security.PrivateKey
import java.security.SecureRandom
import java.security.cert.X509Certificate
import java.security.spec.ECGenParameterSpec
import java.util.Date
import javax.net.ssl.SSLEngine
import javax.net.ssl.X509ExtendedKeyManager
import javax.net.ssl.X509KeyManager

/**
 * A phone's identity for the host suites: the same shape as the Keystore's
 * (P-256, one self-signed certificate, 1970 to 9999, CN=doors-companion),
 * with the key in the JVM because a host has no Android Keystore.
 */
class TestIdentity(private val key: PrivateKey, val certificate: X509Certificate) : ClientIdentity {
    override fun fingerprint(): ByteArray = Fingerprint.ofCertificate(certificate)

    override fun keyManager(): X509KeyManager = object : X509ExtendedKeyManager() {
        override fun chooseClientAlias(keyType: Array<out String>?, issuers: Array<out Principal>?, socket: Socket?) = "id"
        override fun chooseEngineClientAlias(keyType: Array<out String>?, issuers: Array<out Principal>?, engine: SSLEngine?) = "id"
        override fun getClientAliases(keyType: String?, issuers: Array<out Principal>?) = arrayOf("id")
        override fun getCertificateChain(alias: String?) = arrayOf(certificate)
        override fun getPrivateKey(alias: String?) = key
        override fun getServerAliases(keyType: String?, issuers: Array<out Principal>?): Array<String>? = null
        override fun chooseServerAlias(keyType: String?, issuers: Array<out Principal>?, socket: Socket?): String? = null
    }

    companion object {
        fun keyPair(algorithm: String, curve: String?): KeyPair =
            KeyPairGenerator.getInstance(algorithm).apply {
                if (curve != null) initialize(ECGenParameterSpec(curve)) else initialize(2048)
            }.generateKeyPair()

        fun create(): TestIdentity {
            val kp = keyPair("EC", "secp256r1")
            val name = X500Name("CN=doors-companion")
            val holder = JcaX509v3CertificateBuilder(
                name, BigInteger(63, SecureRandom()).add(BigInteger.ONE), Date(0L), Date(253402300799000L), name, kp.public,
            ).build(JcaContentSignerBuilder("SHA256withECDSA").build(kp.private))
            return TestIdentity(kp.private, JcaX509CertificateConverter().getCertificate(holder))
        }
    }
}
