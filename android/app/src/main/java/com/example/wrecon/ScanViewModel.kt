package com.example.wrecon

import android.app.Application
import android.hardware.usb.UsbDevice
import androidx.lifecycle.AndroidViewModel
import androidx.lifecycle.viewModelScope
import com.hoho.android.usbserial.driver.UsbSerialDriver
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import java.util.concurrent.ConcurrentHashMap

data class UiState(
    val link: LinkState = LinkState.Disconnected,
    val cfg: JamConfig = JamConfig(),
    val wifi: List<WifiAp> = emptyList(),
    val clients: List<ClientDev> = emptyList(),
    val stats: JamStats = JamStats(),
    val fw: String = "-",
    val chip: String = "-",
    val logs: List<String> = emptyList(),
    val tab: Int = 0,
)

class ScanViewModel(app: Application) : AndroidViewModel(app) {

    private val link = UsbSerialLink(app)
    val linkState: StateFlow<LinkState> = link.state

    private val _ui = MutableStateFlow(UiState())
    val ui: StateFlow<UiState> = _ui.asStateFlow()

    private val wifiMap = ConcurrentHashMap<String, WifiAp>()
    private val clientMap = ConcurrentHashMap<String, ClientDev>()

    init {
        viewModelScope.launch {
            link.lines.collect { onLine(it) }
        }
        viewModelScope.launch {
            link.state.collect { st -> _ui.update { it.copy(link = st) } }
        }
        // Publish daftar maksimal ~4 fps. Firmware bisa mengirim puluhan
        // event/detik; tanpa ini Compose recompose + sort tiap baris masuk.
        viewModelScope.launch {
            while (isActive) {
                delay(250)
                _ui.update { st ->
                    st.copy(
                        wifi = wifiMap.values.sortedByDescending { it.rssi },
                        clients = clientMap.values.sortedByDescending { it.rssi },
                    )
                }
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
        val next = _ui.value.cfg.targets.toMutableSet()
        if (!next.add(bssid)) next.remove(bssid)
        setTargets(next)
    }

    fun selectAllVisible() {
        val all = (_ui.value.cfg.targets + wifiMap.keys).toSet()
        setTargets(all)
    }

    fun clearTargets() = setTargets(emptySet())

    fun addTargetMac(mac: String) {
        val clean = mac.trim().uppercase()
        if (!Regex("^([0-9A-F]{2}:){5}[0-9A-F]{2}$").matches(clean)) {
            log("MAC tidak valid")
            return
        }
        setTargets((_ui.value.cfg.targets + clean).toSet())
    }

    private fun setTargets(set: Set<String>) {
        val list = set.toList().sorted()
        _ui.update { it.copy(cfg = it.cfg.copy(targets = list)) }
        send(Protocol.targets(list))
    }

    fun startJam() = send(Protocol.jamStart())
    fun stopJam() = send(Protocol.jamStop())

    fun updateParams(block: (JamConfig) -> JamConfig) {
        val next = block(_ui.value.cfg)
        _ui.update { it.copy(cfg = next) }
        send(Protocol.setParams(next))
    }

    fun clearScan() {
        wifiMap.clear()
        clientMap.clear()
        _ui.update { it.copy(wifi = emptyList(), clients = emptyList()) }
        send(Protocol.clear())
    }

    fun selectTab(tab: Int) = _ui.update { it.copy(tab = tab) }

    private fun send(line: String) {
        if (!link.send(line)) log("USB belum terhubung")
    }

    private fun log(s: String) {
        _ui.update { it.copy(logs = (listOf(s) + it.logs).take(5)) }
    }

    private fun onLine(line: String) {
        when (val msg = Protocol.parse(line)) {
            is Msg.Hello -> _ui.update { it.copy(fw = msg.fw, chip = msg.chip) }
            is Msg.Config -> {
                _ui.update { it.copy(cfg = msg.cfg) }
                // Kurangi beban serial/UI: firmware default repeat 1500 ms.
                if (msg.cfg.repeatMs < 3000) updateParams { it.copy(repeatMs = 3000) }
            }
            is Msg.Rsp -> if (!msg.ok) log("${msg.cmd}: ${msg.msg}")
            is Msg.Status -> _ui.update { it.copy(stats = msg.stats) }
            is Msg.Wifi -> wifiMap[msg.ap.bssid] = msg.ap
            is Msg.Client -> clientMap[msg.dev.addr] = msg.dev
            null -> Unit
        }
    }
}
