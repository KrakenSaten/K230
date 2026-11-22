// Copyright (c) 2026 Doors authors.
// SPDX-License-Identifier: Apache-2.0
package app.doors.companion.core

import java.math.BigInteger
import java.security.MessageDigest
import java.security.PublicKey
import java.security.cert.CertificateException
import java.security.cert.X509Certificate
import java.security.interfaces.ECPublicKey
import java.security.spec.ECFieldFp

/**
 * A key's identity: the SHA-256 of its DER SubjectPublicKeyInfo (PAIRING.md
 * section 1). The SPKI is rebuilt here from the curve point, so a P-256 key
 * has one fingerprint however a certificate happened to encode it, and a key
 * that is not on the named curve P-256 has none.
 */
object Fingerprint {
    const val LEN = 32

    // SEQUENCE { SEQUENCE { id-ecPublicKey, prime256v1 }, BIT STRING (uncompressed point) }
    private val SPKI_PREFIX = parseHex("3059301306072a8648ce3d020106082a8648ce3d030107034200")

    private val P = BigInteger("ffffffff00000001000000000000000000000000ffffffffffffffffffffffff", 16)
    private val A = BigInteger("ffffffff00000001000000000000000000000000fffffffffffffffffffffffc", 16)
    private val B = BigInteger("5ac635d8aa3a93e7b3ebbd55769886bc651d06b0cc53b0f63bce3c3e27d2604b", 16)
    private val N = BigInteger("ffffffff00000000ffffffffffffffffbce6faada7179e84f3b9cac2fc632551", 16)
    private val GX = BigInteger("6b17d1f2e12c4247f8bce6e563a440f277037d812deb33a0f4a13945d898c296", 16)
    private val GY = BigInteger("4fe342e2fe1a7f9b8ee7eb4a7c0f9e162bce33576b315ececbb6406837bf51f5", 16)

    @Throws(CertificateException::class)
    fun ofCertificate(cert: X509Certificate): ByteArray = ofPublicKey(cert.publicKey)

    @Throws(CertificateException::class)
    fun ofPublicKey(key: PublicKey): ByteArray {
        if (key !is ECPublicKey) throw CertificateException("the key is not an EC key")
        val spec = key.params ?: throw CertificateException("the key has no curve")
        val field = spec.curve.field
        if (field !is ECFieldFp || field.p != P || spec.curve.a != A || spec.curve.b != B ||
            spec.order != N || spec.cofactor != 1 ||
            spec.generator.affineX != GX || spec.generator.affineY != GY
        ) {
            throw CertificateException("the key is not on P-256")
        }
        val w = key.w
        val md = MessageDigest.getInstance("SHA-256")
        md.update(SPKI_PREFIX)
        md.update(0x04)
        md.update(fixed32(w.affineX))
        md.update(fixed32(w.affineY))
        return md.digest()
    }

    private fun fixed32(v: BigInteger): ByteArray {
        if (v.signum() < 0 || v.bitLength() > 256) throw CertificateException("the point is out of range")
        val raw = v.toByteArray()
        val out = ByteArray(32)
        val n = minOf(raw.size, 32)
        System.arraycopy(raw, raw.size - n, out, 32 - n, n)
        return out
    }

    /** Constant time for equal lengths. */
    fun equal(a: ByteArray, b: ByteArray): Boolean = MessageDigest.isEqual(a, b)

    fun hex(bytes: ByteArray): String {
        val sb = StringBuilder(bytes.size * 2)
        for (b in bytes) {
            sb.append(Character.forDigit((b.toInt() shr 4) and 0xf, 16))
            sb.append(Character.forDigit(b.toInt() and 0xf, 16))
        }
        return sb.toString()
    }

    fun parseHex(s: String): ByteArray {
        require(s.length % 2 == 0) { "odd hex length" }
        return ByteArray(s.length / 2) {
            val hi = Character.digit(s[it * 2], 16)
            val lo = Character.digit(s[it * 2 + 1], 16)
            require(hi >= 0 && lo >= 0) { "not hex" }
            ((hi shl 4) or lo).toByte()
        }
    }

    /** What people compare: 16 hex digits in groups of four, "3F2A 91C4 0B7E D115". */
    fun display(bytes: ByteArray): String =
        hex(bytes.copyOf(8)).uppercase().chunked(4).joinToString(" ")
}
