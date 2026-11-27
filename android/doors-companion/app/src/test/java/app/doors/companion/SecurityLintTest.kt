// Copyright (c) 2026 Doors authors.
// SPDX-License-Identifier: Apache-2.0
package app.doors.companion

import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test
import java.io.File

/**
 * The lint CONTRACTS.md section 2 asks for, as a test over the sources: no
 * accept-all trust manager, no permissive name check, no TLS below 1.3, no
 * platform trust store, no cleartext, no "insecure" variant or switch, no
 * cloud or telemetry dependency, no permission beyond the contract's, no
 * socket made outside the transport.
 *
 * It reads text, so it holds the wording of the rules, not their meaning: the
 * unit tests of PinnedTrustManager and the suite against a real gatewayd hold
 * the behaviour.
 */
class SecurityLintTest {
    private val root = File(System.getProperty("doors.project.dir") ?: "..").let { if (File(it, "app").isDirectory) it else it.parentFile }
    private val main = File(root, "app/src/main")
    private val sources = main.walkTopDown().filter { it.isFile && it.extension in setOf("kt", "java", "xml") }.toList()
    private val gradle = listOf("app/build.gradle.kts", "build.gradle.kts", "settings.gradle.kts").map { File(root, it) }

    private fun offenders(files: List<File>, pattern: Regex) =
        files.filter { f -> f.readLines().any { pattern.containsMatchIn(it.replace("http://schemas.android.com", "")) } }.map { it.name }

    @Test
    fun sourcesAreFound() {
        assertTrue("no sources under ${main.absolutePath}", sources.size > 8)
    }

    @Test
    fun nothingThatWeakensTls() {
        val forbidden = mapOf(
            "a name verifier" to Regex("HostnameVerifier|ALLOW_ALL"),
            "the platform trust store" to Regex("TrustManagerFactory|SSLContext\\.getDefault|SSLSocketFactory\\.getDefault|HttpsURLConnection"),
            "an old or unspecified protocol" to Regex("\"(TLS|SSL|TLSv1|TLSv1\\.[012]|SSLv[23])\""),
            "cleartext" to Regex("http://|HttpURLConnection|cleartextTrafficPermitted=\"true\"|usesCleartextTraffic=\"true\""),
            "a developer switch" to Regex("BuildConfig\\.DEBUG|(?i)insecure|(?i)trust.?all|(?i)skip.?verif|(?i)disable.?(pin|tls|verif)"),
        )
        for ((what, pattern) in forbidden) assertEquals(what, emptyList<String>(), offenders(sources, pattern))
    }

    @Test
    fun oneTrustManagerAndOnePlaceThatMakesAContext() {
        assertEquals(listOf("PinnedTrustManager.kt"), offenders(sources, Regex("class .*: *X509(Extended)?TrustManager|object *: *X509(Extended)?TrustManager")))
        assertEquals(listOf("GatewayTls.kt"), offenders(sources, Regex("SSLContext\\.getInstance")))
        val tm = sources.single { it.name == "PinnedTrustManager.kt" }.readText()
        assertEquals("every server entry point runs the check", 3, Regex("override fun checkServerTrusted\\([^)]*\\) = check\\(chain\\)").findAll(tm).count())
        assertEquals("no other server entry point", 3, Regex("fun checkServerTrusted").findAll(tm).count())
        val tls = sources.single { it.name == "GatewayTls.kt" }.readText()
        assertTrue(tls.contains("const val PROTOCOL = \"TLSv1.3\"") && tls.contains("enabledProtocols = arrayOf(PROTOCOL)"))
    }

    @Test
    fun socketsAreMadeOnlyWhereTheirWayIsChecked() {
        // A socket of its own is made by the transport only: bound to the
        // joined Wi-Fi network (WifiTransport), or left unbound from an own
        // link's address once the kernel's route was confirmed (LocalTransport).
        val makers = Regex("\\bSocket\\(|DatagramSocket\\(|SocketChannel|DatagramChannel|ServerSocket|socketFactory\\.createSocket\\(\\)")
        assertEquals(listOf("LocalTransport.kt", "WifiTransport.kt"), offenders(sources, makers).sorted())
        val wifi = sources.single { it.name == "WifiTransport.kt" }.readText()
        assertEquals("WifiTransport makes sockets from the joined network's factory only", 0, Regex("\\bSocket\\(|DatagramSocket\\(").findAll(wifi).count())
        val local = sources.single { it.name == "LocalTransport.kt" }.readText()
        assertEquals("one unbound TCP socket", 1, Regex("\\bSocket\\(").findAll(local).count())
        assertTrue("with no proxy", local.contains("Socket(Proxy.NO_PROXY)"))
        assertTrue(
            "the own-link socket waits for the route",
            Regex("routeSource\\(to, port\\) != path\\.link\\.own\\) \\{\\s*throw IOException[^\\n]*\\n\\s*}\\s*platform\\.connectFrom\\(").containsMatchIn(local),
        )
        // A process bound to one network would take every socket off the local network's lookup.
        assertEquals(emptyList<String>(), offenders(sources, Regex("bindProcessToNetwork|setProcessDefaultNetwork")))
    }

    @Test
    fun noVariantNoCloudNoTelemetry() {
        val forbidden = Regex("(?i)productFlavors|flavorDimensions|debuggable|firebase|crashlytics|analytics|play-services|gms|sentry|okhttp|retrofit|datadog|appcenter")
        assertEquals(emptyList<String>(), offenders(gradle, forbidden))
        assertEquals(emptyList<String>(), offenders(sources, Regex("(?i)firebase|crashlytics|analytics|com\\.google\\.android\\.gms")))
    }

    @Test
    fun onlyTheContractsPermissionsAndNoBackup() {
        val manifest = File(main, "AndroidManifest.xml").readText()
        val asked = Regex("uses-permission android:name=\"android\\.permission\\.([A-Z_]+)\"").findAll(manifest).map { it.groupValues[1] }.toSet()
        val allowed = setOf("INTERNET", "ACCESS_NETWORK_STATE", "CAMERA", "POST_NOTIFICATIONS", "FOREGROUND_SERVICE")
        assertTrue("asks for ${asked - allowed}", allowed.containsAll(asked))
        assertTrue(manifest.contains("android:allowBackup=\"false\""))
    }
}
