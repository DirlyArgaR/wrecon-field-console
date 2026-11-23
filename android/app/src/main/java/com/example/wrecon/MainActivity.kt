package com.example.wrecon

import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.hardware.usb.UsbDevice
import android.hardware.usb.UsbManager
import android.os.Build
import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.compose.material3.darkColorScheme
import androidx.compose.ui.graphics.Color
import androidx.core.content.IntentCompat
import com.example.wrecon.ui.ScanScreen

class MainActivity : ComponentActivity() {

    private lateinit var vm: ScanViewModel

    private val permissionReceiver = object : BroadcastReceiver() {
        override fun onReceive(context: Context?, intent: Intent?) {
            if (intent?.action != UsbSerialLink.ACTION_PERMISSION) return
            val device = IntentCompat.getParcelableExtra(
                intent, UsbManager.EXTRA_DEVICE, UsbDevice::class.java
            ) ?: return
            if (intent.getBooleanExtra(UsbManager.EXTRA_PERMISSION_GRANTED, false)) {
                connect(device)
            }
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)

        val filter = IntentFilter(UsbSerialLink.ACTION_PERMISSION)
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            registerReceiver(permissionReceiver, filter, Context.RECEIVER_NOT_EXPORTED)
        } else {
            @Suppress("DEPRECATION")
            registerReceiver(permissionReceiver, filter)
        }

        vm = ScanViewModel(application)

        // Auto-start kalau activity dibuka dari USB_DEVICE_ATTACHED
        if (intent.action == UsbManager.ACTION_USB_DEVICE_ATTACHED) {
            IntentCompat.getParcelableExtra(
                intent, UsbManager.EXTRA_DEVICE, UsbDevice::class.java
            )?.let { tryConnect(it) }
        }

        setContent {
            androidx.compose.material3.MaterialTheme(
                colorScheme = darkColorScheme(
                    primary = Color(0xFF5CE1C6),
                    onPrimary = Color(0xFF0A0D0E),
                    secondary = Color(0xFFFF5F56),
                    surface = Color(0xFF101617),
                    background = Color(0xFF0A0D0E),
                )
            ) {
                ScanScreen(vm = vm, onConnect = { connectFirstAvailable() })
            }
        }
    }

    private fun connectFirstAvailable() {
        val drivers = vm.devices()
        if (drivers.isEmpty()) return
        tryConnect(drivers.first().device)
    }

    private fun tryConnect(device: UsbDevice) {
        if (vm.hasPermission(device)) connect(device) else vm.requestPermission(device)
    }

    private fun connect(device: UsbDevice) {
        vm.devices().firstOrNull { it.device.deviceId == device.deviceId }?.let { vm.open(it) }
    }

    override fun onDestroy() {
        unregisterReceiver(permissionReceiver)
        vm.disconnect()
        super.onDestroy()
    }
}
