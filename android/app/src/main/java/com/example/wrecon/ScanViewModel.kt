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
    val cfg: DeviceConfig = DeviceConfig(),
    val wifi: List<WifiAp> = emptyList(),
    val ble: List<BleDev> = emptyList(),
    val status: Msg.Status? = null,
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
    private val bleMap = LinkedHashMap<String, BleDev>()

    init {
        viewModelScope.launch {
            link.lines.collect { line -> onLine(line) }
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
        send(Protocol.status())   // minta config + status terbaru setelah connect
    }
    fun disconnect() = link.close()

    fun setMode(mode: String) {
        _ui.update { it.copy(cfg = it.cfg.copy(mode = mode)) }
        send(Protocol.mode(mode))
    }

    fun updateParams(block: (DeviceConfig) -> DeviceConfig) {
        val next = block(_ui.value.cfg)
        _ui.update { it.copy(cfg = next) }
        send(Protocol.setParams(next))
    }

    fun toggleChannel(ch: Int) {
        updateParams { c ->
            c.copy(channels = if (ch in c.channels) c.channels - ch else c.channels + ch)
        }
    }

    fun refresh() = send(Protocol.status())
    fun clear() {
        wifiMap.clear()
        bleMap.clear()
        _ui.update { it.copy(wifi = emptyList(), ble = emptyList()) }
    }

    fun selectTab(tab: Int) = _ui.update { it.copy(tab = tab) }

    private fun send(line: String) {
        if (!link.send(line)) log("USB belum terhubung")
    }

    private fun log(s: String) {
        _ui.update { it.copy(logs = (listOf(s) + it.logs).take(6)) }
    }

    private fun onLine(line: String) {
        when (val msg = Protocol.parse(line)) {
            is Msg.Hello -> _ui.update { it.copy(fw = msg.fw, chip = msg.chip) }

            is Msg.Config -> _ui.update { it.copy(cfg = msg.cfg) }

            is Msg.Rsp -> if (!msg.ok) log("${msg.cmd}: ${msg.msg}")

            is Msg.Status -> _ui.update { it.copy(status = msg) }

            is Msg.Wifi -> {
                wifiMap[msg.ap.bssid] = msg.ap
                _ui.update { it.copy(wifi = wifiMap.values.sortedByDescending { ap -> ap.rssi }) }
            }

            is Msg.Ble -> {
                bleMap[msg.dev.addr] = msg.dev
                _ui.update { it.copy(ble = bleMap.values.sortedByDescending { d -> d.rssi }) }
            }

            null -> Unit
        }
    }
}
