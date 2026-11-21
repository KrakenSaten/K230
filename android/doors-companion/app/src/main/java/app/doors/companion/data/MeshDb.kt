// Copyright (c) 2026 Doors authors.
// SPDX-License-Identifier: Apache-2.0
package app.doors.companion.data

import android.content.Context
import androidx.room.Dao
import androidx.room.Database
import androidx.room.Embedded
import androidx.room.Entity
import androidx.room.Index
import androidx.room.Insert
import androidx.room.PrimaryKey
import androidx.room.Query
import androidx.room.Room
import androidx.room.RoomDatabase
import androidx.room.Transaction
import androidx.room.Update
import androidx.room.Upsert
import app.doors.companion.core.MarkKind
import app.doors.companion.core.MeshBatch
import app.doors.companion.core.MeshChannel
import app.doors.companion.core.MeshCursor
import app.doors.companion.core.MeshIdentity
import app.doors.companion.core.MeshMessage
import app.doors.companion.core.MeshNode
import app.doors.companion.core.MeshStore
import kotlinx.coroutines.flow.Flow
import java.io.File

/**
 * The phone's copy of the mesh, per paired unit (`unit` is its fingerprint).
 * The unit keeps no messages on storage, so this file is the only history
 * there is. It sits where the platform never backs up or transfers, and is
 * emptied when the unit is forgotten.
 */

/**
 * A message. `(unit, runId, msgId)` names it for good: a state change (an
 * ACK, a copy heard back) updates the row, and the row keeps its `ord`, the
 * order in which this phone first learned of messages, which is the order
 * the gateway's journal gave them in.
 */
@Entity(
    tableName = "messages",
    indices = [Index(value = ["unit", "runId", "msgId"], unique = true), Index(value = ["unit", "conv", "ord"])],
)
data class MessageRow(
    @PrimaryKey(autoGenerate = true) val ord: Long = 0,
    val unit: String,
    val runId: String,
    val msgId: Long,
    val seq: Long,
    val conv: String,
    val outgoing: Boolean,
    val channelKind: Boolean,
    val peerKey: String?,
    val peerName: String?,
    val channel: Int?,
    val channelName: String?,
    val channelHash: String?,
    val senderName: String?,
    val body: String,
    val sentAt: Long?,
    val state: String,
    val ackExpected: Boolean,
    val attempts: Int?,
    val route: String?,
    val pathHops: Int?,
    val transmitted: Boolean?,
    val heardBack: Int?,
    val heardBackHops: Int?,
    val snrDb: Double?,
    val rssiDbm: Double?,
    /** The phone's estimate of when the unit saw it; set once. */
    val heardAtMs: Long,
)

/** A break in continuity, placed after the newest message the phone had when it was found. Never deleted, never edited. */
@Entity(tableName = "marks", indices = [Index(value = ["unit", "afterOrd"])])
data class MarkRow(
    @PrimaryKey(autoGenerate = true) val id: Long = 0,
    val unit: String,
    val afterOrd: Long,
    val kind: String,
    val atMs: Long,
)

@Entity(tableName = "nodes", primaryKeys = ["unit", "publicKey"])
data class NodeRow(
    val unit: String,
    val publicKey: String,
    val name: String,
    val type: Int,
    val pathKnown: Boolean,
    val hops: Int?,
    val direct: Boolean?,
    val snrDb: Double?,
    val rssiDbm: Double?,
    val advertHops: Int?,
    val lat: Double?,
    val lon: Double?,
    val heardAtMs: Long?,
)

@Entity(tableName = "channels", primaryKeys = ["unit", "slot"])
data class ChannelRow(
    val unit: String,
    val slot: Int,
    val name: String,
    val hash: String,
    val keyBits: Int?,
    val wellKnown: String?,
    val textLimit: Int?,
) {
    val conv: String get() = MeshMessage.channelKey(slot, hash)
}

/** The cursor and the unit's own node. */
@Entity(tableName = "units")
data class UnitRow(
    @PrimaryKey val unit: String,
    val journalId: String? = null,
    val runId: String? = null,
    val seq: Long? = null,
    val nodeKey: String? = null,
    val nodeName: String? = null,
    val syncedAtMs: Long? = null,
)

/** How far the person has read a thread. The phone's own; the unit never hears of it. */
@Entity(tableName = "reads", primaryKeys = ["unit", "conv"])
data class ReadRow(val unit: String, val conv: String, val lastOrd: Long)

data class ConversationRow(@Embedded val last: MessageRow, val unread: Int, val total: Int)

@Dao
abstract class MeshDao {
    // ---- what the sync writes ----
    @Query("SELECT * FROM messages WHERE unit = :unit AND runId = :runId AND msgId = :msgId")
    protected abstract suspend fun find(unit: String, runId: String, msgId: Long): MessageRow?

    @Insert
    protected abstract suspend fun insert(row: MessageRow): Long

    @Update
    protected abstract suspend fun update(row: MessageRow)

    @Insert
    protected abstract suspend fun insert(mark: MarkRow)

    @Query("SELECT COALESCE(MAX(ord), 0) FROM messages WHERE unit = :unit")
    protected abstract suspend fun newest(unit: String): Long

    @Query("INSERT OR IGNORE INTO units (unit) VALUES (:unit)")
    protected abstract suspend fun touch(unit: String)

    @Query("UPDATE units SET journalId = :journalId, runId = :runId, seq = :seq, syncedAtMs = :atMs WHERE unit = :unit")
    protected abstract suspend fun setCursor(unit: String, journalId: String, runId: String, seq: Long, atMs: Long)

    @Query("UPDATE units SET nodeKey = :key, nodeName = :name WHERE unit = :unit")
    protected abstract suspend fun setIdentity(unit: String, key: String, name: String)

    @Query("SELECT * FROM units WHERE unit = :unit")
    abstract suspend fun unitNow(unit: String): UnitRow?

    @Transaction
    open suspend fun commit(unit: String, batch: MeshBatch, atMs: Long) {
        // Marks go where the history stood before this batch: between the
        // last message the phone had and the first one it gets now.
        val before = newest(unit)
        for (m in batch.marks) insert(MarkRow(unit = unit, afterOrd = before, kind = m.kind.name, atMs = m.atMs))
        for (h in batch.messages) {
            val m = h.message
            val old = find(unit, m.runId, m.id)
            val row = MessageRow(
                ord = old?.ord ?: 0, unit = unit, runId = m.runId, msgId = m.id, seq = m.seq, conv = m.conversation,
                outgoing = m.outgoing, channelKind = m.channelKind, peerKey = m.peerKey, peerName = m.peerName,
                channel = m.channel, channelName = m.channelName, channelHash = m.channelHash, senderName = m.senderName,
                body = m.text, sentAt = m.timestamp, state = m.state, ackExpected = m.ackExpected, attempts = m.attempts,
                route = m.route, pathHops = m.pathHops, transmitted = m.transmitted, heardBack = m.heardBack,
                heardBackHops = m.heardBackHops, snrDb = m.snrDb, rssiDbm = m.rssiDbm, heardAtMs = old?.heardAtMs ?: h.atMs,
            )
            if (old == null) insert(row) else if (old != row) update(row)
        }
        touch(unit)
        setCursor(unit, batch.cursor.journalId, batch.cursor.runId, batch.cursor.seq, atMs)
    }

    @Query("SELECT * FROM nodes WHERE unit = :unit")
    protected abstract suspend fun nodesNow(unit: String): List<NodeRow>

    @Query("DELETE FROM nodes WHERE unit = :unit")
    protected abstract suspend fun clearNodes(unit: String)

    @Upsert
    protected abstract suspend fun put(nodes: List<NodeRow>)

    @Query("DELETE FROM nodes WHERE unit = :unit AND publicKey = :key")
    abstract suspend fun removeNode(unit: String, key: String)

    @Transaction
    open suspend fun putNodes(unit: String, nodes: List<Pair<MeshNode, Long?>>, whole: Boolean) {
        // A node the unit lists without a time (not heard since its service
        // started) keeps the time this phone knew for it.
        val known = nodesNow(unit).associateBy { it.publicKey }
        if (whole) clearNodes(unit)
        put(nodes.map { (n, at) ->
            NodeRow(unit, n.publicKey, n.name, n.type, n.pathKnown, n.hops, n.direct, n.lastSnrDb, n.lastRssiDbm, n.advertHops, n.lat, n.lon, at ?: known[n.publicKey]?.heardAtMs)
        })
    }

    @Query("DELETE FROM channels WHERE unit = :unit")
    protected abstract suspend fun clearChannels(unit: String)

    @Upsert
    abstract suspend fun putChannels(channels: List<ChannelRow>)

    @Query("DELETE FROM channels WHERE unit = :unit AND slot = :slot")
    abstract suspend fun removeChannel(unit: String, slot: Int)

    @Transaction
    open suspend fun replaceChannels(unit: String, channels: List<ChannelRow>) {
        clearChannels(unit)
        putChannels(channels)
    }

    @Transaction
    open suspend fun putIdentity(unit: String, key: String, name: String) {
        touch(unit)
        setIdentity(unit, key, name)
    }

    // ---- what the screens read ----
    @Query(
        "SELECT m.*, " +
            "(SELECT COUNT(*) FROM messages u WHERE u.unit = m.unit AND u.conv = m.conv AND u.outgoing = 0 AND u.ord > " +
            "COALESCE((SELECT r.lastOrd FROM reads r WHERE r.unit = m.unit AND r.conv = m.conv), 0)) AS unread, " +
            "(SELECT COUNT(*) FROM messages t WHERE t.unit = m.unit AND t.conv = m.conv) AS total " +
            "FROM messages m WHERE m.ord IN (SELECT MAX(ord) FROM messages WHERE unit = :unit GROUP BY conv) ORDER BY m.ord DESC",
    )
    abstract fun conversations(unit: String): Flow<List<ConversationRow>>

    @Query(
        "SELECT COUNT(*) FROM messages m WHERE m.unit = :unit AND m.outgoing = 0 AND m.ord > " +
            "COALESCE((SELECT r.lastOrd FROM reads r WHERE r.unit = m.unit AND r.conv = m.conv), 0)",
    )
    abstract fun unread(unit: String): Flow<Int>

    /** The newest [limit] of a thread, newest first. The screen asks for more as it is scrolled back. */
    @Query("SELECT * FROM messages WHERE unit = :unit AND conv = :conv ORDER BY ord DESC LIMIT :limit")
    abstract fun thread(unit: String, conv: String, limit: Int): Flow<List<MessageRow>>

    @Query("SELECT COUNT(*) FROM messages WHERE unit = :unit AND conv = :conv")
    abstract fun threadSize(unit: String, conv: String): Flow<Int>

    @Query("SELECT COUNT(*) FROM messages WHERE unit = :unit")
    abstract fun stored(unit: String): Flow<Int>

    @Query("SELECT * FROM marks WHERE unit = :unit ORDER BY id")
    abstract fun marks(unit: String): Flow<List<MarkRow>>

    @Query("SELECT * FROM nodes WHERE unit = :unit ORDER BY heardAtMs IS NULL, heardAtMs DESC, name")
    abstract fun nodes(unit: String): Flow<List<NodeRow>>

    @Query("SELECT * FROM channels WHERE unit = :unit ORDER BY slot")
    abstract fun channels(unit: String): Flow<List<ChannelRow>>

    @Query("SELECT * FROM units WHERE unit = :unit")
    abstract fun unit(unit: String): Flow<UnitRow?>

    @Query("INSERT INTO reads (unit, conv, lastOrd) VALUES (:unit, :conv, :ord) ON CONFLICT (unit, conv) DO UPDATE SET lastOrd = MAX(lastOrd, excluded.lastOrd)")
    abstract suspend fun markRead(unit: String, conv: String, ord: Long)

    // ---- forgetting a unit ----
    @Query("DELETE FROM messages WHERE unit = :unit")
    protected abstract suspend fun dropMessages(unit: String)

    @Query("DELETE FROM marks WHERE unit = :unit")
    protected abstract suspend fun dropMarks(unit: String)

    @Query("DELETE FROM units WHERE unit = :unit")
    protected abstract suspend fun dropUnit(unit: String)

    @Query("DELETE FROM reads WHERE unit = :unit")
    protected abstract suspend fun dropReads(unit: String)

    @Transaction
    open suspend fun forget(unit: String) {
        dropMessages(unit)
        dropMarks(unit)
        clearNodes(unit)
        clearChannels(unit)
        dropReads(unit)
        dropUnit(unit)
    }
}

@Database(
    entities = [MessageRow::class, MarkRow::class, NodeRow::class, ChannelRow::class, UnitRow::class, ReadRow::class],
    version = 1,
    exportSchema = false,
)
abstract class MeshDb : RoomDatabase() {
    abstract fun dao(): MeshDao

    companion object {
        fun open(context: Context): MeshDb =
            Room.databaseBuilder(context, MeshDb::class.java, File(context.noBackupFilesDir, "mesh.v1.db").path).build()
    }
}

/** MeshSync's store, on Room. */
class RoomMeshStore(private val dao: MeshDao, private val now: () -> Long = System::currentTimeMillis) : MeshStore {
    override suspend fun cursor(unit: String): MeshCursor? {
        val u = dao.unitNow(unit) ?: return null
        return MeshCursor(u.journalId ?: return null, u.runId ?: return null, u.seq ?: return null)
    }

    override suspend fun commit(unit: String, batch: MeshBatch) = dao.commit(unit, batch, now())
    override suspend fun putNodes(unit: String, nodes: List<Pair<MeshNode, Long?>>, whole: Boolean) = dao.putNodes(unit, nodes, whole)
    override suspend fun removeNode(unit: String, publicKey: String) = dao.removeNode(unit, publicKey)
    override suspend fun putChannels(unit: String, channels: List<MeshChannel>) = dao.replaceChannels(unit, channels.map { row(unit, it) })
    override suspend fun putChannel(unit: String, channel: MeshChannel) = dao.putChannels(listOf(row(unit, channel)))
    override suspend fun removeChannel(unit: String, slot: Int) = dao.removeChannel(unit, slot)
    override suspend fun putIdentity(unit: String, identity: MeshIdentity) = dao.putIdentity(unit, identity.publicKey, identity.name)

    private fun row(unit: String, c: MeshChannel) = ChannelRow(unit, c.slot, c.name, c.hash, c.keyBits, c.wellKnown, c.textLimit)
}

fun MarkRow.markKind(): MarkKind? = MarkKind.entries.firstOrNull { it.name == kind }
