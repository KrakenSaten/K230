// Copyright (c) 2026 Doors authors.
// SPDX-License-Identifier: Apache-2.0
package app.doors.companion

import android.app.Application
import android.os.Build
import app.doors.companion.core.GatewaySession
import app.doors.companion.core.PairOutcome
import app.doors.companion.core.PairingUri
import app.doors.companion.device.KeystoreIdentity
import app.doors.companion.device.UnitStore
import app.doors.companion.device.WifiTransport
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancelAndJoin
import kotlinx.coroutines.launch

class DoorsApp : Application() {
    lateinit var link: Link
        private set

    override fun onCreate() {
        super.onCreate()
        link = Link(this)
    }
}

/**
 * The app's one connection to its one unit. The loop runs only between
 * [onForeground] and [onBackground]: nothing reconnects behind the person's
 * back (PAIRING.md section 7; the optional connection service is not in M3).
 */
class Link(app: Application) {
    val identity = KeystoreIdentity()
    val store = UnitStore(app.noBackupFilesDir)
    private val transport = WifiTransport(app)
    val session = GatewaySession(transport, identity, VERSION, onUnitMoved = { store.save(it) })

    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.Main.immediate)
    private var loop: Job? = null
    private var foreground = false

    init {
        transport.onChange = { session.kick() }
    }

    fun onForeground() {
        foreground = true
        transport.start()
        restart()
    }

    fun onBackground() {
        foreground = false
        loop?.cancel()
        transport.stop()
    }

    private fun restart() {
        val old = loop
        loop = scope.launch {
            old?.cancelAndJoin()
            val unit = store.unit.value ?: return@launch
            if (foreground) session.run(unit)
        }
    }

    /** The person asked: try now, whatever the back-off or a stopped loop says. */
    fun retry() {
        if (loop?.isActive == true) session.kick() else restart()
    }

    suspend fun pair(code: PairingUri, onWaiting: () -> Unit): PairOutcome {
        loop?.cancelAndJoin()
        val outcome = session.pair(code, phoneName(), onWaiting)
        if (outcome is PairOutcome.Paired) {
            session.clear()
            store.save(outcome.unit)
        }
        restart()
        return outcome
    }

    /** Forget the unit: tell it when it can hear, forget either way (PAIRING.md section 5). */
    suspend fun forget() {
        session.unpair()
        loop?.cancelAndJoin()
        store.clear()
        session.clear()
    }

    /** The phone's claim about itself, 1 to 32 bytes, shown as a claim on the unit. */
    private fun phoneName(): String {
        var name = (Build.MODEL ?: "").filter { it.code in 0x20..0x7e }.trim().ifEmpty { "Android phone" }
        while (name.toByteArray().size > 32) name = name.dropLast(1)
        return name
    }

    companion object {
        const val VERSION = "0.1.0"
    }
}
