// Copyright (c) 2026 Doors authors.
// SPDX-License-Identifier: Apache-2.0
package app.doors.companion.core

import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.JsonPrimitive

/**
 * What the gateway passes on of the mesh (gateway.md section 7), as the phone
 * keeps it. Everything here is read: nothing in this file, or in MeshSync,
 * sends a message or changes the node.
 */

/**
 * One message. Its identity is `(runId, id)`: meshcored's ids start again at
 * 1 with every run, so the run is part of the name, and a message stored
 * under an old run stays what it was when a new run reuses its number.
 */
data class MeshMessage(
    val runId: String,
    val id: Long,
    val seq: Long,
    val outgoing: Boolean,
    val channelKind: Boolean,
    val peerKey: String?,
    val peerName: String?,
    val channel: Int?,
    val channelName: String?,
    val channelHash: String?,
    /** A channel message's sender, as the sender claimed it. Nothing proves it. */
    val senderName: String?,
    val text: String,
    /** The sender's own clock, in seconds. Shown as a claim, never sorted by. */
    val timestamp: Long?,
    val monoMs: Long?,
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
) {
    /**
     * Which thread this belongs to. A direct thread is its peer's public key.
     * A channel is its slot and the byte that was on the air: a slot may be
     * emptied and taken by another channel, and the hash travels with it so
     * that the two are not read as one. (One byte: two keys can share it.)
     */
    val conversation: String
        get() = if (channelKind) channelKey(channel ?: -1, channelHash) else "d:" + peerKey.orEmpty().lowercase()

    companion object {
        fun channelKey(slot: Int, hash: String?) = "c:$slot:" + hash.orEmpty().lowercase()

        /** Null when the object is not a message this phone can name. */
        fun parse(runId: String, seq: Long, m: JsonObject): MeshMessage? {
            val id = m.long("id") ?: return null
            val kind = m.string("kind") ?: return null
            val channelKind = when (kind) {
                "channel" -> true
                "direct" -> false
                else -> return null
            }
            val peer = m.string("peer_public_key")
            val slot = m.long("channel")?.toInt()
            if (id < 1 || runId.isEmpty() || (channelKind && slot == null) || (!channelKind && !isKey(peer))) return null
            return MeshMessage(
                runId = runId, id = id, seq = seq,
                outgoing = m.string("direction") == "out",
                channelKind = channelKind,
                peerKey = peer?.lowercase(), peerName = m.string("peer_name"),
                channel = slot, channelName = m.string("channel_name"), channelHash = m.string("channel_hash"),
                senderName = m.string("sender_name"),
                text = m.string("text").orEmpty(),
                timestamp = m.long("timestamp"), monoMs = m.long("mono_ms"),
                state = m.string("state") ?: "received",
                ackExpected = m.bool("ack_expected") ?: false,
                attempts = m.long("attempts")?.toInt(),
                route = m.string("route"), pathHops = m.long("path_hops")?.toInt(),
                transmitted = m.bool("transmitted"),
                heardBack = m.long("heard_back")?.toInt(), heardBackHops = m.long("heard_back_hops")?.toInt(),
                snrDb = m.double("snr_db"), rssiDbm = m.double("rssi_dbm"),
            )
        }

        fun isKey(s: String?) = s != null && s.length == 64 && s.all { it in '0'..'9' || it in 'a'..'f' || it in 'A'..'F' }

        /** A channel text is "<name>: <body>" inside the payload; the body alone. */
        fun body(text: String, senderName: String?): String =
            if (!senderName.isNullOrEmpty() && text.startsWith("$senderName: ")) text.substring(senderName.length + 2) else text
    }
}

data class MeshNode(
    val publicKey: String,
    val name: String,
    /** MeshCore's advert type: 1 chat, 2 repeater, 3 room, 4 sensor. */
    val type: Int,
    val pathKnown: Boolean,
    val hops: Int?,
    val direct: Boolean?,
    val lastHeardMonoMs: Long?,
    val lastSnrDb: Double?,
    val lastRssiDbm: Double?,
    val advertHops: Int?,
    val lat: Double?,
    val lon: Double?,
) {
    companion object {
        fun parse(n: JsonObject): MeshNode? {
            val key = n.string("public_key")?.takeIf { MeshMessage.isKey(it) } ?: return null
            return MeshNode(
                publicKey = key.lowercase(), name = n.string("name").orEmpty(), type = (n.long("type") ?: 0).toInt(),
                pathKnown = n.bool("path_known") ?: false, hops = n.long("hops")?.toInt(), direct = n.bool("direct"),
                lastHeardMonoMs = n.long("last_heard_mono_ms")?.takeIf { it > 0 },
                lastSnrDb = n.double("last_snr_db"), lastRssiDbm = n.double("last_rssi_dbm"),
                advertHops = n.long("advert_hops")?.toInt(), lat = n.double("lat"), lon = n.double("lon"),
            )
        }
    }
}

data class MeshChannel(
    val slot: Int,
    val name: String,
    val hash: String,
    val keyBits: Int?,
    val wellKnown: String?,
    val textLimit: Int?,
) {
    val conversation: String get() = MeshMessage.channelKey(slot, hash)

    companion object {
        fun parse(c: JsonObject): MeshChannel? {
            val slot = c.long("channel")?.toInt() ?: return null
            return MeshChannel(slot, c.string("name").orEmpty(), c.string("channel_hash").orEmpty(), c.long("key_bits")?.toInt(), c.string("well_known"), c.long("text_limit")?.toInt())
        }
    }
}

/** The unit's own node. The public half only: no method reports the other. */
data class MeshIdentity(val publicKey: String, val name: String)

/** Where this phone stands in the gateway's journal. Meaningless under another journal or run. */
data class MeshCursor(val journalId: String, val runId: String, val seq: Long)

/**
 * A place in the stored history where this phone cannot say that nothing is
 * missing. It is written once and never taken back: no later sync can show
 * that the messages in between arrived, so nothing ever claims they did.
 */
enum class MarkKind {
    /** The gateway said so (`gap: true`): messages passed that this phone did not get. */
    GAP,

    /** meshcored started again. What it took in while nobody read it, if anything, is not known. */
    MESH_RESTARTED,

    /** The gateway started again and built its journal from what meshcored still held. */
    GATEWAY_RESTARTED,

    /** Both at once: the unit itself started again. */
    UNIT_RESTARTED,
}

data class MeshMark(val kind: MarkKind, val atMs: Long)

/** One sync step, written as one: the marks first, then the messages, then where the cursor now stands. */
class MeshBatch(val marks: List<MeshMark>, val messages: List<Heard>, val cursor: MeshCursor)

/** A message and the phone's estimate of when the unit saw it, used only when it is new here. */
class Heard(val message: MeshMessage, val atMs: Long)

/**
 * Where the phone keeps the mesh. The unit keeps no history (gateway.md
 * section 7: "a client that wants history keeps it"), so this is the history.
 */
interface MeshStore {
    suspend fun cursor(unit: String): MeshCursor?

    /** Atomic. A message known by `(unit, runId, id)` is brought up to date in place; it is never stored twice. */
    suspend fun commit(unit: String, batch: MeshBatch)

    /** [whole]: this is the unit's complete list, and a node not in it is no longer on the unit. */
    suspend fun putNodes(unit: String, nodes: List<Pair<MeshNode, Long?>>, whole: Boolean)
    suspend fun removeNode(unit: String, publicKey: String)
    suspend fun putChannels(unit: String, channels: List<MeshChannel>)
    suspend fun putChannel(unit: String, channel: MeshChannel)
    suspend fun removeChannel(unit: String, slot: Int)
    suspend fun putIdentity(unit: String, identity: MeshIdentity)
}

/** One second of the radio, as counts (gateway.md section 7, topic `mesh.activity`). */
data class ActivityBin(
    val atMs: Long,
    val rxFrames: Int,
    val rxBytes: Long,
    val txOk: Int,
    val txFailed: Int,
    val txUnknown: Int,
    val byType: Map<String, Int>,
) {
    companion object {
        fun parse(d: JsonObject, atMs: Long) = ActivityBin(
            atMs, (d.long("rx_frames") ?: 0).toInt(), d.long("rx_bytes") ?: 0, (d.long("tx_ok") ?: 0).toInt(),
            (d.long("tx_failed") ?: 0).toInt(), (d.long("tx_unknown") ?: 0).toInt(),
            d.obj("by_type")?.entries?.mapNotNull { (k, v) -> (v as? JsonPrimitive)?.content?.toDoubleOrNull()?.let { k to it.toInt() } }?.toMap().orEmpty(),
        )
    }
}
