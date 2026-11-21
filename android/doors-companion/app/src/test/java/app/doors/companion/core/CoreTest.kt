// Copyright (c) 2026 Doors authors.
// SPDX-License-Identifier: Apache-2.0
package app.doors.companion.core

import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertThrows
import org.junit.Assert.assertTrue
import org.junit.Test
import java.security.MessageDigest
import java.security.cert.CertificateException
import java.util.Base64
import kotlin.random.Random

class CoreTest {
    private val a = TestIdentity.create()
    private val b = TestIdentity.create()

    // ---- fingerprint ----

    @Test
    fun fingerprintIsSha256OfTheSpki() {
        // The JDK writes a P-256 key as the same named-curve SPKI openssl does.
        val want = MessageDigest.getInstance("SHA-256").digest(a.certificate.publicKey.encoded)
        assertArrayEquals(want, Fingerprint.ofCertificate(a.certificate))
        assertEquals(32, want.size)
    }

    @Test
    fun otherKeysHaveNoFingerprint() {
        assertThrows(CertificateException::class.java) { Fingerprint.ofPublicKey(TestIdentity.keyPair("EC", "secp384r1").public) }
        assertThrows(CertificateException::class.java) { Fingerprint.ofPublicKey(TestIdentity.keyPair("RSA", null).public) }
    }

    @Test
    fun displayIsSixteenHexInFours() {
        val fp = Fingerprint.parseHex("3f2a91c40b7ed115" + "00".repeat(24))
        assertEquals("3F2A 91C4 0B7E D115", Fingerprint.display(fp))
        assertEquals("3f2a91c40b7ed115" + "00".repeat(24), Fingerprint.hex(fp))
    }

    // ---- the pin ----

    @Test
    fun thePinnedKeyIsAccepted() {
        PinnedTrustManager(a.fingerprint()).checkServerTrusted(arrayOf(a.certificate), "ECDHE_ECDSA")
    }

    @Test
    fun anotherKeyIsRefused() {
        assertThrows(PinMismatchException::class.java) {
            PinnedTrustManager(a.fingerprint()).checkServerTrusted(arrayOf(b.certificate), "ECDHE_ECDSA")
        }
    }

    @Test
    fun aSecondCertificateIsRefusedEvenBehindThePinnedOne() {
        val tm = PinnedTrustManager(a.fingerprint())
        assertThrows(CertificateException::class.java) { tm.checkServerTrusted(arrayOf(a.certificate, b.certificate), "ECDHE_ECDSA") }
        assertThrows(CertificateException::class.java) { tm.checkServerTrusted(arrayOf(), "ECDHE_ECDSA") }
        assertThrows(CertificateException::class.java) { tm.checkServerTrusted(null, "ECDHE_ECDSA") }
    }

    @Test
    fun everyEntryPointChecksAndNoIssuerIsTrusted() {
        val tm = PinnedTrustManager(a.fingerprint())
        assertThrows(PinMismatchException::class.java) { tm.checkServerTrusted(arrayOf(b.certificate), "x", null as java.net.Socket?) }
        assertThrows(PinMismatchException::class.java) { tm.checkServerTrusted(arrayOf(b.certificate), "x", null as javax.net.ssl.SSLEngine?) }
        assertThrows(CertificateException::class.java) { tm.checkClientTrusted(arrayOf(a.certificate), "x") }
        assertEquals(0, tm.acceptedIssuers.size)
        assertThrows(IllegalArgumentException::class.java) { PinnedTrustManager(ByteArray(31)) }
    }

    @Test
    fun aPinMismatchIsFoundUnderWrappers() {
        val wrapped = javax.net.ssl.SSLHandshakeException("x").apply { initCause(RuntimeException(PinMismatchException())) }
        assertTrue(GatewayTls.isPinMismatch(wrapped))
        assertFalse(GatewayTls.isPinMismatch(java.io.IOException("reset")))
    }

    // ---- the pairing code ----

    private fun b64(n: Int) = Base64.getUrlEncoder().withoutPadding().encodeToString(ByteArray(n) { (it + 1).toByte() })
    private fun uri(h: String = "192.168.1.20", p: String = "47231", fp: String = b64(32), t: String = b64(16), v: String = "1") =
        "doors-pair:$v?h=$h&p=$p&fp=$fp&t=$t&n=doors%2Dunit"

    @Test
    fun aUnitsCodeParses() {
        val u = PairingUri.parse(uri())
        assertEquals("192.168.1.20", u.host)
        assertEquals(47231, u.port)
        assertEquals(32, u.fingerprint.size)
        assertEquals("doors-unit", u.name)
        assertFalse("the token must not be printed", u.toString().contains(u.token))
    }

    @Test
    fun malformedCodesAreRefused() {
        for (bad in listOf(
            "https://example.com", uri(v = "2"), uri(h = "8.8.8.8"), uri(h = "unit.local"), uri(h = "10.0.0.256"),
            uri(p = "0"), uri(p = "70000"), uri(fp = b64(31)), uri(fp = "!!"), uri(t = b64(15)),
            uri() + "&fp=" + b64(32), "doors-pair:1", "x".repeat(600),
        )) {
            assertThrows(bad.take(40), PairingUri.Invalid::class.java) { PairingUri.parse(bad) }
        }
    }

    @Test
    fun onlyLocalAddresses() {
        for (ok in listOf("10.0.2.2", "172.16.0.1", "172.31.255.254", "192.168.0.1", "169.254.1.1", "127.0.0.1")) assertTrue(ok, PairingUri.isLocalIpv4(ok))
        for (no in listOf("172.32.0.1", "11.0.0.1", "1.1.1.1", "::1", "localhost", "10.0.0", "10.0.0.1.1")) assertFalse(no, PairingUri.isLocalIpv4(no))
    }

    // ---- back-off ----

    @Test
    fun backoffDoublesFromOneSecondToSixty() {
        val r = Random(7)
        val expect = listOf(1_000L, 2_000L, 4_000L, 8_000L, 16_000L, 32_000L, 60_000L, 60_000L, 60_000L)
        expect.forEachIndexed { i, base ->
            repeat(50) {
                val v = GatewaySession.backoffMs(i, r)
                assertTrue("attempt $i gave $v", v >= base * 0.8 - 1 && v <= base * 1.2 + 1)
            }
        }
        assertTrue(GatewaySession.backoffMs(1_000, r) <= 72_000)
    }
}
