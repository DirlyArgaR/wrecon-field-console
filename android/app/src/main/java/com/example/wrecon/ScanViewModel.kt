package com.example.wrecon

import android.app.Application
import android.hardware.usb.UsbDevice
import androidx.lifecycle.AndroidViewModel
import androidx.lifecycle.viewModelScope
import com.hoho.android.usbserial.driver.UsbSerialDriver
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.launch

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

    private val wifiMap = LinkedHashMap<String, WifiAp>()
    private val clientMap = LinkedHashMap<String, ClientDev>()

    init {
        viewModelScope.launch {
            link.lines.collect { onLine(it) }
        }
        viewModelScope.launch {
            link.state.collect { st -> _ui.update { it.copy(link = st) } }
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
            is Msg.Config -> _ui.update { it.copy(cfg = msg.cfg) }
            is Msg.Rsp -> if (!msg.ok) log("${msg.cmd}: ${msg.msg}")
            is Msg.Status -> _ui.update { it.copy(stats = msg.stats) }
            is Msg.Wifi -> {
                wifiMap[msg.ap.bssid] = msg.ap
                _ui.update { it.copy(wifi = wifiMap.values.sortedByDescending { a -> a.rssi }) }
            }
            is Msg.Client -> {
                clientMap[msg.dev.addr] = msg.dev
                _ui.update { it.copy(clients = clientMap.values.sortedByDescending { c -> c.rssi }) }
            }
            null -> Unit
        }
    }
}
