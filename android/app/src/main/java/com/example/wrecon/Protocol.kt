package com.example.wrecon

import org.json.JSONArray
import org.json.JSONObject

/*
 * Protokol NDJSON firmware jammer-firmware (ESP32).
 * Device -> app : hello | config | rsp | status | wifi | client
 * App -> device : status | config | set | targets | jam_start | jam_stop | clear | reset
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

data class ClientDev(
    val addr: String,
    val bssid: String,
    val ch: Int,
    val rssi: Int,
    val hits: Int,
    val ts: Long,
)

data class JamConfig(
    val intervalMs: Int = 5,
    val burst: Int = 8,
    val burstGapMs: Int = 30,
    val channel: Int = 6,
    val hop: Boolean = false,
    val hopMs: Int = 200,
    val reason: Int = 7,
    val broadcast: Boolean = true,
    val disassoc: Boolean = false,
    val statsMs: Int = 2000,
    val repeatMs: Int = 1500,
    val rssiMin: Int = -95,
    val targets: List<String> = emptyList(),
)

data class JamStats(
    val frames: Int = 0,
    val aps: Int = 0,
    val clients: Int = 0,
    val tx: Int = 0,
    val txErrors: Int = 0,
    val dropped: Int = 0,
    val channel: Int = 0,
    val targets: Int = 0,
    val jamming: Boolean = false,
)

sealed class Msg {
    data class Hello(val fw: String, val chip: String) : Msg()
    data class Config(val cfg: JamConfig) : Msg()
    data class Rsp(val ok: Boolean, val cmd: String, val msg: String) : Msg()
    data class Status(val stats: JamStats) : Msg()
    data class Wifi(val ap: WifiAp) : Msg()
    data class Client(val dev: ClientDev) : Msg()
}

object Protocol {
    fun parse(line: String): Msg? = try {
        val o = JSONObject(line)
        when (o.optString("type")) {
            "hello" -> Msg.Hello(o.optString("fw", "?"), o.optString("chip", "?"))

            "config" -> {
                val p = o.optJSONObject("params") ?: JSONObject()
                val arr = o.optJSONArray("targets")
                val targets = buildList {
                    if (arr != null) for (i in 0 until arr.length()) add(arr.optString(i))
                }
                Msg.Config(
                    JamConfig(
                        intervalMs = p.optInt("interval_ms", 5),
                        burst = p.optInt("burst", 8),
                        burstGapMs = p.optInt("burst_gap_ms", 30),
                        channel = p.optInt("channel", 6),
                        hop = p.optBoolean("hop"),
                        hopMs = p.optInt("hop_ms", 200),
                        reason = p.optInt("reason", 7),
                        broadcast = p.optBoolean("broadcast", true),
                        disassoc = p.optBoolean("disassoc"),
                        statsMs = p.optInt("stats_ms", 2000),
                        repeatMs = p.optInt("repeat_ms", 1500),
                        rssiMin = p.optInt("rssi_min", -95),
                        targets = targets,
                    )
                )
            }

            "rsp" -> Msg.Rsp(o.optBoolean("ok"), o.optString("cmd"), o.optString("msg"))

            "status" -> {
                val s = o.optJSONObject("stats") ?: JSONObject()
                Msg.Status(
                    JamStats(
                        frames = s.optInt("frames"),
                        aps = s.optInt("aps"),
                        clients = s.optInt("clients"),
                        tx = s.optInt("tx"),
                        txErrors = s.optInt("tx_errors"),
                        dropped = s.optInt("rx_dropped"),
                        channel = o.optInt("channel"),
                        targets = o.optInt("targets"),
                        jamming = o.optBoolean("jamming"),
                    )
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

            "client" -> Msg.Client(
                ClientDev(
                    addr = o.optString("addr"),
                    bssid = o.optString("bssid"),
                    ch = o.optInt("ch"),
                    rssi = o.optInt("rssi"),
                    hits = o.optInt("hits", 1),
                    ts = o.optLong("ts"),
                )
            )

            else -> null
        }
    } catch (_: Exception) {
        null
    }

    fun status(): String = cmd("status")
    fun config(): String = cmd("config")
    fun jamStart(): String = cmd("jam_start")
    fun jamStop(): String = cmd("jam_stop")
    fun clear(): String = cmd("clear")
    fun reset(): String = cmd("reset")
    fun ping(): String = cmd("ping")

    fun targets(list: Collection<String>): String = JSONObject()
        .put("cmd", "targets")
        .put("targets", JSONArray(list))
        .toString()

    fun setParams(cfg: JamConfig): String {
        val p = JSONObject()
            .put("interval_ms", cfg.intervalMs)
            .put("burst", cfg.burst)
            .put("burst_gap_ms", cfg.burstGapMs)
            .put("channel", cfg.channel)
            .put("hop", cfg.hop)
            .put("hop_ms", cfg.hopMs)
            .put("reason", cfg.reason)
            .put("broadcast", cfg.broadcast)
            .put("disassoc", cfg.disassoc)
            .put("stats_ms", cfg.statsMs)
            .put("repeat_ms", cfg.repeatMs)
            .put("rssi_min", cfg.rssiMin)
        return JSONObject().put("cmd", "set").put("params", p).toString()
    }

    private fun cmd(name: String): String = JSONObject().put("cmd", name).toString()
}
