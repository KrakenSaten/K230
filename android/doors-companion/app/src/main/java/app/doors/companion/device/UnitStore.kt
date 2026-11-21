// Copyright (c) 2026 Doors authors.
// SPDX-License-Identifier: Apache-2.0
package app.doors.companion.device

import app.doors.companion.core.Fingerprint
import app.doors.companion.core.PairedUnit
import app.doors.companion.core.PairingUri
import app.doors.companion.core.long
import app.doors.companion.core.string
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.serialization.json.Json
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.buildJsonObject
import kotlinx.serialization.json.put
import java.io.File

/**
 * The paired unit's pin, name and last address: app-private, in the
 * directory the platform never backs up or transfers. It holds no secret
 * (the phone's key is in the Keystore), but it is what the phone trusts, so a
 * damaged file is "not paired", never a guess.
 */
class UnitStore(dir: File) {
    private val file = File(dir, "unit.v1.json")
    private val _unit = MutableStateFlow(load())
    val unit: StateFlow<PairedUnit?> = _unit

    private fun load(): PairedUnit? = try {
        val o = Json.parseToJsonElement(file.readText()) as JsonObject
        val fp = o.string("fingerprint")!!
        val host = o.string("host")!!
        val port = o.long("port")!!.toInt()
        require(Fingerprint.parseHex(fp).size == Fingerprint.LEN && PairingUri.isLocalIpv4(host) && port in 1..65535)
        PairedUnit(fp.lowercase(), o.string("name") ?: "DOORS unit", host, port, o.long("paired_at_ms") ?: 0)
    } catch (e: Exception) {
        null
    }

    @Synchronized
    fun save(unit: PairedUnit) {
        val tmp = File(file.parentFile, file.name + ".tmp")
        tmp.writeText(buildJsonObject {
            put("fingerprint", unit.fingerprint)
            put("name", unit.name)
            put("host", unit.host)
            put("port", unit.port)
            put("paired_at_ms", unit.pairedAtMs)
        }.toString())
        if (!tmp.renameTo(file)) {
            file.delete()
            tmp.renameTo(file)
        }
        _unit.value = unit
    }

    @Synchronized
    fun clear() {
        file.delete()
        _unit.value = null
    }
}
