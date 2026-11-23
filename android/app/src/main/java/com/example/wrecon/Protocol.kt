package com.example.wrecon

import org.json.JSONArray
import org.json.JSONObject

/*
 * Protokol NDJSON. Satu baris = satu objek JSON.
 * Device -> app : type = hello | config | rsp | status | wifi | ble
 * App -> device : cmd  = mode | set | status | ping | reset
 */

data class WifiAp(
    val bssid: String,
    val ssid: String,
    val hidden: Boolean,
    val ch: Int,
    val rssi: Int,
    val auth: String,
    val hits: Int,
    val ts: Long,
)

data class BleDev(
    val addr: String,
    val addrType: String,
    val name: String,
    val rssi: Int,
    val connectable: Boolean,
    val hits: Int,
    val ts: Long,
)

data class DeviceConfig(
    val mode: String = "off",
    val wifiDwellMs: Int = 400,
    val bleScanMs: Int = 1000,
    val bleIntervalMs: Int = 200,
    val bleWindowMs: Int = 60,
    val repeatMs: Int = 2000,
    val dedupMs: Int = 60000,
    val statsMs: Int = 5000,
    val rssiMin: Int = -95,
    val channels: List<Int> = (1..13).toList(),
)

sealed class Msg {
    data class Hello(val fw: String, val chip: String) : Msg()
    data class Config(val cfg: DeviceConfig) : Msg()
    data class Rsp(val ok: Boolean, val cmd: String, val msg: String) : Msg()
    data class Status(
        val mode: String,
        val running: Boolean,
        val wifiFrames: Int,
        val wifiUnique: Int,
        val bleEvents: Int,
        val bleUnique: Int,
        val dropped: Int,
        val channel: Int,
    ) : Msg()
    data class Wifi(val ap: WifiAp) : Msg()
    data class Ble(val dev: BleDev) : Msg()
}

object Protocol {
    fun parse(line: String): Msg? = try {
        val o = JSONObject(line)
        when (o.optString("type")) {
            "hello" -> Msg.Hello(
                o.optString("fw", "?"),
                o.optString("chip", "?"),
            )

            "config" -> {
                val p = o.optJSONObject("params") ?: JSONObject()
                val ch = p.optJSONArray("channels")
                val channels = buildList {
                    if (ch != null) for (i in 0 until ch.length()) add(ch.getInt(i))
                }
                Msg.Config(
                    DeviceConfig(
                        mode = o.optString("mode", "off"),
                        wifiDwellMs = p.optInt("wifi_dwell_ms", 400),
                        bleScanMs = p.optInt("ble_scan_ms", 1000),
                        bleIntervalMs = p.optInt("ble_interval_ms", 200),
                        bleWindowMs = p.optInt("ble_window_ms", 60),
                        repeatMs = p.optInt("repeat_ms", 2000),
                        dedupMs = p.optInt("dedup_ms", 60000),
                        statsMs = p.optInt("stats_ms", 5000),
                        rssiMin = p.optInt("rssi_min", -95),
                        channels = channels.ifEmpty { (1..13).toList() },
                    )
                )
            }

            "rsp" -> Msg.Rsp(
                o.optBoolean("ok"),
                o.optString("cmd"),
                o.optString("msg"),
            )

            "status" -> {
                val s = o.optJSONObject("stats") ?: JSONObject()
                Msg.Status(
                    mode = o.optString("mode", "off"),
                    running = o.optBoolean("running"),
                    wifiFrames = s.optInt("wifi_frames"),
                    wifiUnique = s.optInt("wifi_unique"),
                    bleEvents = s.optInt("ble_events"),
                    bleUnique = s.optInt("ble_unique"),
                    dropped = s.optInt("tx_dropped"),
                    channel = o.optInt("wifi_channel", 0),
                )
            }

            "wifi" -> Msg.Wifi(
                WifiAp(
                    bssid = o.optString("bssid"),
                    ssid = if (o.optBoolean("hidden")) "(hidden)" else o.optString("ssid", ""),
                    hidden = o.optBoolean("hidden"),
                    ch = o.optInt("ch"),
                    rssi = o.optInt("rssi"),
                    auth = o.optString("auth", "open"),
                    hits = o.optInt("hits", 1),
                    ts = o.optLong("ts"),
                )
            )

            "ble" -> Msg.Ble(
                BleDev(
                    addr = o.optString("addr"),
                    addrType = o.optString("addr_type", "public"),
                    name = o.optString("name", "(no name)"),
                    rssi = o.optInt("rssi"),
                    connectable = o.optBoolean("conn"),
                    hits = o.optInt("hits", 1),
                    ts = o.optLong("ts"),
                )
            )

            else -> null
        }
    } catch (_: Exception) {
        null
    }

    fun mode(mode: String): String = JSONObject()
        .put("cmd", "mode")
        .put("mode", mode)
        .toString()

    fun setParams(cfg: DeviceConfig): String {
        val params = JSONObject()
            .put("wifi_dwell_ms", cfg.wifiDwellMs)
            .put("ble_scan_ms", cfg.bleScanMs)
            .put("ble_interval_ms", cfg.bleIntervalMs)
            .put("ble_window_ms", cfg.bleWindowMs)
            .put("repeat_ms", cfg.repeatMs)
            .put("dedup_ms", cfg.dedupMs)
            .put("stats_ms", cfg.statsMs)
            .put("rssi_min", cfg.rssiMin)
            .put("channels", JSONArray(cfg.channels))
        return JSONObject().put("cmd", "set").put("params", params).toString()
    }

    fun status(): String = JSONObject().put("cmd", "status").toString()
    fun ping(): String = JSONObject().put("cmd", "ping").toString()
    fun reset(): String = JSONObject().put("cmd", "reset").toString()
}
