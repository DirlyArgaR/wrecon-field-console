package com.example.wrecon

import android.app.Application
import android.hardware.usb.UsbDevice
import androidx.lifecycle.AndroidViewModel
import androidx.lifecycle.viewModelScope
import com.hoho.android.usbserial.driver.UsbSerialDriver
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import java.util.concurrent.ConcurrentHashMap
import java.util.concurrent.atomic.AtomicBoolean

data class MetaState(
    val link: LinkState = LinkState.Disconnected,
    val fw: String = "-",
    val chip: String = "-",
    val logs: List<String> = emptyList(),
    val tab: Int = 0,
)

class ScanViewModel(app: Application) : AndroidViewModel(app) {

    private val link = UsbSerialLink(app)

    private val _meta = MutableStateFlow(MetaState())
    val meta: StateFlow<MetaState> = _meta.asStateFlow()

    private val _config = MutableStateFlow(JamConfig())
    val config: StateFlow<JamConfig> = _config.asStateFlow()

    private val _wifi = MutableStateFlow<List<WifiAp>>(emptyList())
    val wifi: StateFlow<List<WifiAp>> = _wifi.asStateFlow()

    private val _clients = MutableStateFlow<List<ClientDev>>(emptyList())
    val clients: StateFlow<List<ClientDev>> = _clients.asStateFlow()

    private val _stats = MutableStateFlow(JamStats())
    val stats: StateFlow<JamStats> = _stats.asStateFlow()

    private val wifiMap = ConcurrentHashMap<String, WifiAp>()
    private val clientMap = ConcurrentHashMap<String, ClientDev>()
    private val listsDirty = AtomicBoolean(true)

    init {
        viewModelScope.launch {
            link.lines.collect { onLine(it) }
        }
        viewModelScope.launch {
            link.state.collect { st -> _meta.update { it.copy(link = st) } }
        }
        // Keepalive supaya watchdog firmware tahu app masih hidup.
        viewModelScope.launch {
            while (isActive) {
                delay(2000)
                if (_meta.value.link is LinkState.Connected) send(Protocol.ping())
            }
        }
        // Publish daftar hanya kalau berubah, maksimal ~3 fps.
        viewModelScope.launch {
            while (isActive) {
                delay(320)
                if (!listsDirty.getAndSet(false)) continue
                _wifi.value = wifiMap.values.sortedByDescending { it.rssi }.take(30)
                _clients.value = clientMap.values.sortedByDescending { it.rssi }.take(30)
            }
        }
    }

    fun devices(): List<UsbSerialDriver> = link.devices()
    fun hasPermission(device: UsbDevice) = link.hasPermission(device)
    fun requestPermission(device: UsbDevice) = link.requestPermission(device)

    fun open(driver: UsbSerialDriver) {
        link.open(driver)
        send(Protocol.status())
        send(Protocol.config())
    }

    fun disconnect() = link.close()

    fun refresh() {
        send(Protocol.status())
        send(Protocol.config())
    }

    fun toggleTarget(bssid: String) {
        val next = _config.value.targets.toMutableSet()
        if (!next.add(bssid)) next.remove(bssid)
        setTargets(next)
    }

    fun selectAllVisible() {
        val all = (_config.value.targets + wifiMap.keys).toSet()
        setTargets(all)
    }

    fun clearTargets() = setTargets(emptySet())

    fun addTargetMac(mac: String) {
        val clean = mac.trim().uppercase()
        if (!Regex("^([0-9A-F]{2}:){5}[0-9A-F]{2}$").matches(clean)) {
            log("MAC tidak valid")
            return
        }
        setTargets((_config.value.targets + clean).toSet())
    }

    private fun setTargets(set: Set<String>) {
        val list = set.toList().sorted()
        _config.update { it.copy(targets = list) }
        send(Protocol.targets(list))
    }

    fun startJam() = send(Protocol.jamStart())
    fun stopJam() = send(Protocol.jamStop())

    fun updateParams(block: (JamConfig) -> JamConfig) {
        val next = block(_config.value)
        _config.value = next
        send(Protocol.setParams(next))
    }

    fun clearScan() {
        wifiMap.clear()
        clientMap.clear()
        listsDirty.set(true)
        _wifi.value = emptyList()
        _clients.value = emptyList()
        send(Protocol.clear())
    }

    fun selectTab(tab: Int) = _meta.update { it.copy(tab = tab) }

    private fun send(line: String) {
        if (!link.send(line)) {
            link.markWriteError("USB write gagal")
            log("USB write gagal — sambung ulang")
        }
    }

    private fun log(s: String) {
        _meta.update { it.copy(logs = (listOf(s) + it.logs).take(3)) }
    }

    private fun onLine(line: String) {
        when (val msg = Protocol.parse(line)) {
            is Msg.Hello -> _meta.update { it.copy(fw = msg.fw, chip = msg.chip) }

            is Msg.Config -> {
                _config.value = msg.cfg
                // Turunkan beban serial: event lebih jarang, status lebih jarang.
                if (msg.cfg.repeatMs < 4000 || msg.cfg.statsMs < 5000) {
                    updateParams { it.copy(repeatMs = 4000, statsMs = 5000) }
                }
            }

            is Msg.Rsp -> {
                if (!msg.ok) log("${msg.cmd}: ${msg.msg}")
                else if (msg.cmd in setOf("jam_start", "jam_stop", "targets", "set")) {
                    log("${msg.cmd.uppercase()}: ${msg.msg}")
                }
            }
            is Msg.Status -> _stats.value = msg.stats

            is Msg.Wifi -> { wifiMap[msg.ap.bssid] = msg.ap; listsDirty.set(true) }
            is Msg.Client -> { clientMap[msg.dev.addr] = msg.dev; listsDirty.set(true) }

            null -> Unit
        }
    }
}
