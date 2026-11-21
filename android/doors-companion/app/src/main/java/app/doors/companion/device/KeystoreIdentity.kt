// Copyright (c) 2026 Doors authors.
// SPDX-License-Identifier: Apache-2.0
package app.doors.companion.device

import android.os.Build
import android.security.keystore.KeyGenParameterSpec
import android.security.keystore.KeyInfo
import android.security.keystore.KeyProperties
import app.doors.companion.core.ClientIdentity
import app.doors.companion.core.Fingerprint
import java.math.BigInteger
import java.net.Socket
import java.security.KeyFactory
import java.security.KeyPairGenerator
import java.security.KeyStore
import java.security.Principal
import java.security.PrivateKey
import java.security.ProviderException
import java.security.SecureRandom
import java.security.cert.X509Certificate
import java.security.spec.ECGenParameterSpec
import java.util.Date
import javax.net.ssl.SSLEngine
import javax.net.ssl.X509ExtendedKeyManager
import javax.net.ssl.X509KeyManager
import javax.security.auth.x500.X500Principal

/**
 * This install's one key (PAIRING.md section 1): ECDSA P-256, generated in
 * the Android Keystore at first use, signing only, never exportable; in
 * StrongBox where the phone has one, otherwise wherever the Keystore keeps
 * it (the TEE on every phone that has one). The private key never enters the
 * app's memory: TLS asks the Keystore for each signature.
 *
 * The certificate is only the container TLS needs for the public key, with
 * the fixed fields of PAIRING.md section 2 (1970 to 9999, CN=doors-companion).
 */
class KeystoreIdentity : ClientIdentity {
    private val store: KeyStore by lazy { KeyStore.getInstance(PROVIDER).apply { load(null) } }

    @Synchronized
    private fun ensure() {
        if (store.containsAlias(ALIAS)) return
        try {
            generate(strongBox = true)
        } catch (e: ProviderException) {
            // No StrongBox on this phone (StrongBoxUnavailableException is one of these).
            generate(strongBox = false)
        }
    }

    private fun generate(strongBox: Boolean) {
        val spec = KeyGenParameterSpec.Builder(ALIAS, KeyProperties.PURPOSE_SIGN)
            .setAlgorithmParameterSpec(ECGenParameterSpec("secp256r1"))
            // SHA-256 for the signature TLS 1.3 asks of this key; NONE because
            // the platform's TLS hands the Keystore a digest it made itself.
            .setDigests(KeyProperties.DIGEST_SHA256, KeyProperties.DIGEST_NONE)
            .setCertificateSubject(X500Principal("CN=doors-companion"))
            .setCertificateSerialNumber(BigInteger(63, SecureRandom()).add(BigInteger.ONE))
            .setCertificateNotBefore(Date(0L))
            .setCertificateNotAfter(Date(253402300799000L)) // 9999-12-31T23:59:59Z
            .setIsStrongBoxBacked(strongBox)
            .build()
        KeyPairGenerator.getInstance(KeyProperties.KEY_ALGORITHM_EC, PROVIDER).apply { initialize(spec) }.generateKeyPair()
    }

    private fun certificate(): X509Certificate {
        ensure()
        return store.getCertificate(ALIAS) as X509Certificate
    }

    private fun privateKey(): PrivateKey {
        ensure()
        return store.getKey(ALIAS, null) as PrivateKey
    }

    override fun fingerprint(): ByteArray = Fingerprint.ofCertificate(certificate())

    /** Where the key lives, for the person to read: "StrongBox", "Trusted environment" or "Software". */
    fun protection(): String = try {
        val key = privateKey()
        val info = KeyFactory.getInstance(key.algorithm, PROVIDER).getKeySpec(key, KeyInfo::class.java)
        if (Build.VERSION.SDK_INT >= 31) {
            when (info.securityLevel) {
                KeyProperties.SECURITY_LEVEL_STRONGBOX -> "StrongBox"
                KeyProperties.SECURITY_LEVEL_TRUSTED_ENVIRONMENT -> "Trusted environment"
                KeyProperties.SECURITY_LEVEL_SOFTWARE -> "Software"
                else -> "Keystore"
            }
        } else {
            @Suppress("DEPRECATION")
            if (info.isInsideSecureHardware) "Secure hardware" else "Software"
        }
    } catch (e: Exception) {
        "Keystore"
    }

    override fun keyManager(): X509KeyManager = object : X509ExtendedKeyManager() {
        override fun chooseClientAlias(keyType: Array<out String>?, issuers: Array<out Principal>?, socket: Socket?) = ALIAS
        override fun chooseEngineClientAlias(keyType: Array<out String>?, issuers: Array<out Principal>?, engine: SSLEngine?) = ALIAS
        override fun getClientAliases(keyType: String?, issuers: Array<out Principal>?) = arrayOf(ALIAS)
        override fun getCertificateChain(alias: String?): Array<X509Certificate> = arrayOf(certificate())
        override fun getPrivateKey(alias: String?): PrivateKey = privateKey()
        override fun getServerAliases(keyType: String?, issuers: Array<out Principal>?): Array<String>? = null
        override fun chooseServerAlias(keyType: String?, issuers: Array<out Principal>?, socket: Socket?): String? = null
    }

    private companion object {
        const val PROVIDER = "AndroidKeyStore"
        const val ALIAS = "doors-companion-identity-v1"
    }
}
