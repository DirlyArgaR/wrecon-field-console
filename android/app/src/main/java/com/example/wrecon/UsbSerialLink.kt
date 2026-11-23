package com.example.wrecon

import android.app.PendingIntent
import android.content.Context
import android.content.Intent
import android.hardware.usb.UsbDevice
import android.hardware.usb.UsbManager
import android.os.Build
import com.hoho.android.usbserial.driver.UsbSerialDriver
import com.hoho.android.usbserial.driver.UsbSerialPort
import com.hoho.android.usbserial.driver.UsbSerialProber
import com.hoho.android.usbserial.util.SerialInputOutputManager
import kotlinx.coroutines.flow.MutableSharedFlow
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.SharedFlow
import kotlinx.coroutines.flow.StateFlow

sealed interface LinkState {
    data object Disconnected : LinkState
    data class Connected(val device: String, val baud: Int) : LinkState
    data class Error(val message: String) : LinkState
}

/** Jembatan USB serial. Satu port, satu pembaca, satu penulis. */
class UsbSerialLink(private val ctx: Context) {

    companion object {
        const val ACTION_PERMISSION = "com.example.wrecon.USB_PERMISSION"
        const val BAUD_RATE = 921600
    }

    private val usb = ctx.getSystemService(Context.USB_SERVICE) as UsbManager
    private var port: UsbSerialPort? = null
    private var io: SerialInputOutputManager? = null
    private val pending = StringBuilder()

    private val _lines = MutableSharedFlow<String>(extraBufferCapacity = 2048)
    val lines: SharedFlow<String> = _lines

    private val _state = MutableStateFlow<LinkState>(LinkState.Disconnected)
    val state: StateFlow<LinkState> = _state

    fun devices(): List<UsbSerialDriver> =
        UsbSerialProber.getDefaultProber().findAllDrivers(usb)

    fun permissionIntent(): PendingIntent {
        val flag = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.M)
            PendingIntent.FLAG_IMMUTABLE else 0
        return PendingIntent.getBroadcast(
            ctx,
            0,
            Intent(ACTION_PERMISSION).setPackage(ctx.packageName),
            PendingIntent.FLAG_UPDATE_CURRENT or flag,
        )
    }

    fun hasPermission(device: UsbDevice): Boolean = usb.hasPermission(device)

    fun requestPermission(device: UsbDevice) = usb.requestPermission(device, permissionIntent())

    fun open(driver: UsbSerialDriver) {
        val connection = usb.openDevice(driver.device)
        if (connection == null) {
            _state.value = LinkState.Error("Gagal membuka perangkat USB")
            return
        }
        try {
            val p = driver.ports.first()
            p.open(connection)
            p.setParameters(BAUD_RATE, 8, UsbSerialPort.STOPBITS_1, UsbSerialPort.PARITY_NONE)
            p.dtr = true
            p.rts = true
            port = p
            io = SerialInputOutputManager(p, object : SerialInputOutputManager.Listener {
                override fun onNewData(data: ByteArray) {
                    pending.append(String(data, Charsets.US_ASCII))
                    while (true) {
                        val end = pending.indexOf('\n')
                        if (end < 0) break
                        val line = pending.substring(0, end).trim()
                        pending.delete(0, end + 1)
                        if (line.isNotEmpty()) _lines.tryEmit(line)
                    }
                    if (pending.length > 65536) pending.setLength(0)
                }

                override fun onRunError(e: Exception) {
                    _state.value = LinkState.Error(e.message ?: "Serial IO error")
                }
            }).apply { start() }

            _state.value = LinkState.Connected(driver.device.deviceName, BAUD_RATE)
        } catch (e: Exception) {
            _state.value = LinkState.Error(e.message ?: "Gagal membuka port")
        }
    }

    fun send(line: String): Boolean {
        return try {
            val p = port ?: return false
            p.write((line + "\n").toByteArray(Charsets.US_ASCII), 500) >= 0
        } catch (_: Exception) {
            false
        }
    }

    fun close() {
        io?.stop()
        io = null
        try { port?.close() } catch (_: Exception) {}
        port = null
        pending.setLength(0)
        _state.value = LinkState.Disconnected
    }
}
