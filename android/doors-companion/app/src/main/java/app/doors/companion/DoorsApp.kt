// Copyright (c) 2026 Doors authors.
// SPDX-License-Identifier: Apache-2.0
package app.doors.companion

import android.app.Application
import android.os.Build
import app.doors.companion.core.Fingerprint
import app.doors.companion.core.GatewaySession
import app.doors.companion.core.LinkState
import app.doors.companion.core.MeshSend
import app.doors.companion.core.MeshSync
import app.doors.companion.core.Stop
import app.doors.companion.core.PairOutcome
import app.doors.companion.core.PairingUri
import app.doors.companion.data.MeshDb
import app.doors.companion.data.RoomMeshStore
import app.doors.companion.data.RoomSendStore
import app.doors.companion.device.KeystoreIdentity
import app.doors.companion.device.UnitStore
import app.doors.companion.device.WifiTransport
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancelAndJoin
import kotlinx.coroutines.launch
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock

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

    /** The mesh as this phone has stored it, and the reader that fills it while a session is open. */
    val meshDb = MeshDb.open(app)
    val mesh = MeshSync(RoomMeshStore(meshDb.dao()))

    /** Sending, for a person who tapped Send while the session is open. It keeps no queue and starts nothing by itself. */
    val send = MeshSend(RoomSendStore(meshDb.dao()), mesh.link)
    val session = GatewaySession(transport, identity, VERSION, onUnitMoved = { store.save(it) }, features = listOf(mesh, send))

    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.Main.immediate)

    /** Draft writes happen in the order they were asked for. */
    private val drafts = Mutex()
    private var loop: Job? = null
    private var foreground = false

    init {
        // A send that a process before this one left on its way has no answer coming.
        scope.launch { store.unit.value?.let { send.recover(it.fingerprint) } }
        // Another network may be where the paired unit really is: a loop that
        // stopped at somebody else's key there starts again here. A confirmed
        // revocation stays stopped.
        transport.onChange = {
            if (session.state.value == LinkState.Stopped(Stop.WRONG_UNIT)) scope.launch { restart() } else session.kick()
        }
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
        val known = store.unit.value
        if (known != null && known.fingerprint != Fingerprint.hex(code.fingerprint)) {
            return PairOutcome.Failed("another unit is paired; forget it first")
        }
        loop?.cancelAndJoin()
        val outcome = session.pair(code, phoneName(), onWaiting)
        if (outcome is PairOutcome.Paired) {
            session.clear()
            store.save(outcome.unit)
        }
        restart()
        return outcome
    }

    /** The person asked to look for the paired unit on this network. True when it was found, by its pinned key. */
    suspend fun locate(): Boolean {
        val unit = store.unit.value ?: return false
        val found = session.locate(unit) ?: return false
        store.save(found)
        restart()
        return true
    }

    /** Forget the unit: tell it when it can hear, forget either way (PAIRING.md section 5). */
    suspend fun forget() {
        val unit = store.unit.value
        session.unpair()
        loop?.cancelAndJoin()
        store.clear()
        // What was read from a unit goes with the unit.
        if (unit != null) meshDb.dao().forget(unit.fingerprint)
        session.clear()
    }

    /**
     * What is typed and not sent is kept per thread, and outlives the screen
     * it was typed on: it is written from here, not from the screen's scope.
     */
    fun keepDraft(unit: String, conv: String, text: String) {
        scope.launch {
            drafts.withLock {
                if (text.isEmpty()) meshDb.dao().dropDraft(unit, conv) else meshDb.dao().saveDraft(unit, conv, text, System.currentTimeMillis())
            }
        }
    }

    /** The phone's claim about itself, 1 to 32 bytes, shown as a claim on the unit. */
    private fun phoneName(): String {
        var name = (Build.MODEL ?: "").filter { it.code in 0x20..0x7e }.trim().ifEmpty { "Android phone" }
        while (name.toByteArray().size > 32) name = name.dropLast(1)
        return name
    }

    companion object {
        const val VERSION = "0.3.0"
    }
}
