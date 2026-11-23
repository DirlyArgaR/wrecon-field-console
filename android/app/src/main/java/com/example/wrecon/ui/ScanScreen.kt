package com.example.wrecon.ui

import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.rememberScrollState
import androidx.compose.material3.Button
import androidx.compose.material3.FilterChip
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Slider
import androidx.compose.material3.Tab
import androidx.compose.material3.TabRow
import androidx.compose.material3.Text
import androidx.compose.material3.TopAppBar
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.remember
import androidx.compose.ui.Modifier
import androidx.compose.ui.unit.dp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import com.example.wrecon.ScanViewModel

@OptIn(androidx.compose.material3.ExperimentalMaterial3Api::class)
@Composable
fun ScanScreen(vm: ScanViewModel, onConnect: () -> Unit) {
    val s by vm.ui.collectAsStateWithLifecycle()
    val link by vm.linkState.collectAsStateWithLifecycle()
    val scroll = rememberScrollState()

    Scaffold(
        topBar = {
            TopAppBar(
                title = { Text("WRecon — passive scan") },
                actions = {
                    OutlinedButton(onClick = onConnect) { Text("Connect USB") }
                }
            )
        }
    ) { pad ->
        Column(
            Modifier
                .padding(pad)
                .padding(16.dp)
                .fillMaxSize(),
            verticalArrangement = Arrangement.spacedBy(12.dp)
        ) {
            Text("Link: ${link.javaClass.simpleName}  |  FW: ${s.fw}  |  ${s.chip}")
            s.status?.let { st ->
                Text("Mode ${st.mode} · WiFi ${st.wifiUnique} AP · BLE ${st.bleUnique} dev " +
                     "· ch ${st.channel} · drop ${st.dropped}")
            }

            // ---- mode ----
            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                listOf("off", "wifi", "ble", "both").forEach { m ->
                    FilterChip(
                        selected = s.cfg.mode == m,
                        onClick = { vm.setMode(m) },
                        label = { Text(m) }
                    )
                }
            }

            // ---- parameter ----
            Text("RSSI minimum: ${s.cfg.rssiMin} dBm")
            Slider(
                value = s.cfg.rssiMin.toFloat(),
                valueRange = -100f..-30f,
                onValueChange = { v -> vm.updateParams { it.copy(rssiMin = v.toInt()) } }
            )

            Text("WiFi dwell per channel: ${s.cfg.wifiDwellMs} ms")
            Slider(
                value = s.cfg.wifiDwellMs.toFloat(),
                valueRange = 50f..2000f,
                onValueChange = { v -> vm.updateParams { it.copy(wifiDwellMs = v.toInt()) } }
            )

            Text("BLE interval / window: ${s.cfg.bleIntervalMs} / ${s.cfg.bleWindowMs} ms")
            Row(
                Modifier.horizontalScroll(scroll),
                horizontalArrangement = Arrangement.spacedBy(6.dp)
            ) {
                (1..13).forEach { ch ->
                    FilterChip(
                        selected = ch in s.cfg.channels,
                        onClick = { vm.toggleChannel(ch) },
                        label = { Text("$ch") }
                    )
                }
            }

            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                Button(onClick = { vm.refresh() }) { Text("Refresh status") }
                OutlinedButton(onClick = { vm.clear() }) { Text("Clear view") }
            }

            if (s.logs.isNotEmpty()) {
                Text(s.logs.first(), color = MaterialTheme.colorScheme.error)
            }

            // ---- hasil ----
            TabRow(selectedTabIndex = s.tab) {
                Tab(selected = s.tab == 0, onClick = { vm.selectTab(0) }, text = { Text("WiFi") })
                Tab(selected = s.tab == 1, onClick = { vm.selectTab(1) }, text = { Text("BLE") })
            }

            LazyColumn(Modifier.fillMaxWidth().weight(1f)) {
                if (s.tab == 0) {
                    items(s.wifi, key = { it.bssid }) { ap ->
                        ResultRow(ap.bssid, "${ap.ssid} · ch ${ap.ch} · ${ap.auth} · ${ap.hits}x", ap.rssi)
                    }
                } else {
                    items(s.ble, key = { it.addr }) { d ->
                        ResultRow(d.addr, "${d.name} · ${d.addrType} · ${d.hits}x", d.rssi)
                    }
                }
            }
        }
    }
}

@Composable
private fun ResultRow(title: String, detail: String, rssi: Int) {
    Column(Modifier.fillMaxWidth().padding(vertical = 6.dp)) {
        Text(title, style = MaterialTheme.typography.bodyMedium)
        Text("$detail · $rssi dBm", style = MaterialTheme.typography.bodySmall)
    }
}
