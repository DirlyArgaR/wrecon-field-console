package com.example.wrecon.ui

import androidx.compose.foundation.BorderStroke
import androidx.compose.foundation.background
import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Button
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.FilterChip
import androidx.compose.material3.FilterChipDefaults
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Slider
import androidx.compose.material3.Switch
import androidx.compose.material3.Tab
import androidx.compose.material3.TabRow
import androidx.compose.material3.Text
import androidx.compose.material3.TopAppBar
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import com.example.wrecon.ClientDev
import com.example.wrecon.JamConfig
import com.example.wrecon.JamStats
import com.example.wrecon.LinkState
import com.example.wrecon.ScanViewModel
import com.example.wrecon.WifiAp
import kotlin.math.abs

private val Accent = Color(0xFF5CE1C6)
private val Danger = Color(0xFFFF5F56)
private val Muted = Color(0xFF6F8685)
private val Panel = Color(0xFF141D1E)
private val PanelBorder = Color(0xFF233033)

@OptIn(androidx.compose.material3.ExperimentalMaterial3Api::class)
@Composable
fun ScanScreen(vm: ScanViewModel, onConnect: () -> Unit) {
    val s by vm.ui.collectAsStateWithLifecycle()
    val link by vm.linkState.collectAsStateWithLifecycle()
    val chScroll = rememberScrollState()

    Scaffold(
        containerColor = Color(0xFF0A0D0E),
        topBar = {
            TopAppBar(
                title = {
                    Column {
                        Text("WRECON", color = Accent, fontWeight = FontWeight.Bold, fontSize = 17.sp)
                        Text("WIFI JAMMER / TARGET CONSOLE", color = Muted, fontSize = 9.sp)
                    }
                },
                actions = {
                    OutlinedButton(onClick = { vm.refresh() }) { Text("SYNC") }
                    Spacer(Modifier.width(6.dp))
                    Button(onClick = onConnect) { Text("CONNECT USB") }
                }
            )
        }
    ) { pad ->
        // Satu LazyColumn penuh: semua panel ikut tergulir, jadi daftar target
        // tidak pernah dapat tinggi 0 dan seluruh UI bisa di-scroll.
        LazyColumn(
            modifier = Modifier.padding(pad).fillMaxSize(),
            contentPadding = PaddingValues(12.dp),
            verticalArrangement = Arrangement.spacedBy(10.dp)
        ) {
            item { StatusCard(link, s.fw, s.chip, s.stats) }
            item { StatsRow(s.stats) }

            item {
                TargetCard(
                    targets = s.cfg.targets,
                    onRemove = { vm.toggleTarget(it) },
                    onSelectAll = { vm.selectAllVisible() },
                    onClear = { vm.clearTargets() },
                    onAdd = { vm.addTargetMac(it) },
                )
            }

            item {
                ConfigCard(
                    cfg = s.cfg,
                    chScroll = chScroll,
                    onChannel = { ch -> vm.updateParams { it.copy(channel = ch) } },
                    onInterval = { v -> vm.updateParams { it.copy(intervalMs = v) } },
                    onBurst = { v -> vm.updateParams { it.copy(burst = v) } },
                    onGap = { v -> vm.updateParams { it.copy(burstGapMs = v) } },
                    onReason = { v -> vm.updateParams { it.copy(reason = v) } },
                    onHop = { v -> vm.updateParams { it.copy(hop = v) } },
                    onBroadcast = { v -> vm.updateParams { it.copy(broadcast = v) } },
                    onDisassoc = { v -> vm.updateParams { it.copy(disassoc = v) } },
                )
            }

            item {
                Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    Button(
                        onClick = { vm.startJam() },
                        modifier = Modifier.weight(1f),
                        colors = ButtonDefaults.buttonColors(containerColor = Danger),
                    ) { Text("START DEAUTH") }
                    OutlinedButton(
                        onClick = { vm.stopJam() },
                        modifier = Modifier.weight(1f),
                    ) { Text("STOP") }
                    OutlinedButton(onClick = { vm.clearScan() }) { Text("CLEAR") }
                }
            }

            if (s.logs.isNotEmpty()) {
                item { Text(s.logs.first(), color = Danger, fontSize = 11.sp) }
            }

            item {
                TabRow(selectedTabIndex = s.tab) {
                    Tab(s.tab == 0, { vm.selectTab(0) }, text = { Text("ACCESS POINTS") })
                    Tab(s.tab == 1, { vm.selectTab(1) }, text = { Text("CLIENTS") })
                }
            }

            if (s.tab == 0) {
                items(s.wifi, key = { it.bssid }) { ap ->
                    ApRow(ap, ap.bssid in s.cfg.targets) { vm.toggleTarget(ap.bssid) }
                }
            } else {
                items(s.clients, key = { it.addr }) { c -> ClientRow(c) }
            }
        }
    }
}

@Composable
private fun StatusCard(link: LinkState, fw: String, chip: String, st: JamStats) {
    val connected = link is LinkState.Connected
    val detail = when (link) {
        is LinkState.Connected -> "${link.device} · ${link.baud}"
        is LinkState.Error -> link.message
        LinkState.Disconnected -> "belum tersambung"
    }
    Card(
        colors = CardDefaults.cardColors(containerColor = Panel),
        border = BorderStroke(1.dp, PanelBorder),
        shape = RoundedCornerShape(10.dp),
        modifier = Modifier.fillMaxWidth(),
    ) {
        Column(Modifier.padding(12.dp)) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Box(
                    Modifier.size(9.dp).clip(RoundedCornerShape(50))
                        .background(if (connected) Accent else Danger)
                )
                Spacer(Modifier.width(8.dp))
                Text(
                    if (connected) "LINK AKTIF" else "LINK MATI",
                    color = if (connected) Accent else Danger,
                    fontWeight = FontWeight.SemiBold,
                )
                Spacer(Modifier.weight(1f))
                Text("FW $fw · $chip", color = Muted, fontSize = 10.sp)
            }
            Spacer(Modifier.height(4.dp))
            Text(detail, color = Muted, fontSize = 10.sp)
            Spacer(Modifier.height(6.dp))
            Text(
                if (st.jamming) "● TRANSMIT JALAN · ch ${st.channel}" else "○ IDLE · ch ${st.channel}",
                color = if (st.jamming) Danger else Accent,
                fontSize = 12.sp,
            )
        }
    }
}

@Composable
private fun StatsRow(st: JamStats) {
    Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
        Kpi("AP", st.aps.toString(), Modifier.weight(1f))
        Kpi("CLIENT", st.clients.toString(), Modifier.weight(1f))
        Kpi("TX", st.tx.toString(), Modifier.weight(1f))
        Kpi("ERROR", st.txErrors.toString(), Modifier.weight(1f))
    }
}

@Composable
private fun Kpi(label: String, value: String, modifier: Modifier = Modifier) {
    Card(
        colors = CardDefaults.cardColors(containerColor = Panel),
        shape = RoundedCornerShape(8.dp),
        modifier = modifier,
    ) {
        Column(Modifier.padding(horizontal = 10.dp, vertical = 8.dp)) {
            Text(label, color = Muted, fontSize = 9.sp)
            Text(value, color = Accent, fontSize = 16.sp, fontWeight = FontWeight.Bold)
        }
    }
}

@OptIn(androidx.compose.material3.ExperimentalMaterial3Api::class)
@Composable
private fun TargetCard(
    targets: List<String>,
    onRemove: (String) -> Unit,
    onSelectAll: () -> Unit,
    onClear: () -> Unit,
    onAdd: (String) -> Unit,
) {
    var manual by remember { mutableStateOf("") }
    Card(colors = CardDefaults.cardColors(containerColor = Panel), shape = RoundedCornerShape(10.dp)) {
        Column(Modifier.padding(12.dp)) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Text("TARGET", color = Accent, fontWeight = FontWeight.SemiBold, fontSize = 12.sp)
                Spacer(Modifier.width(6.dp))
                Text("${targets.size}", color = Danger, fontSize = 11.sp)
                Spacer(Modifier.weight(1f))
                OutlinedButton(onClick = onSelectAll) { Text("SELECT ALL", fontSize = 10.sp) }
                Spacer(Modifier.width(6.dp))
                OutlinedButton(onClick = onClear) { Text("CLEAR", fontSize = 10.sp) }
            }
            Spacer(Modifier.height(8.dp))
            if (targets.isEmpty()) {
                Text("Belum ada target. Pilih AP dari daftar di bawah.", color = Muted, fontSize = 11.sp)
            } else {
                Row(
                    Modifier.horizontalScroll(rememberScrollState()),
                    horizontalArrangement = Arrangement.spacedBy(6.dp)
                ) {
                    targets.forEach { mac ->
                        FilterChip(
                            selected = true,
                            onClick = { onRemove(mac) },
                            label = { Text(mac, fontSize = 10.sp) },
                            colors = FilterChipDefaults.filterChipColors(
                                selectedContainerColor = Color(0xFF162223),
                                selectedLabelColor = Accent,
                            ),
                        )
                    }
                }
            }
            Spacer(Modifier.height(8.dp))
            Row(verticalAlignment = Alignment.CenterVertically) {
                OutlinedTextField(
                    value = manual,
                    onValueChange = { manual = it },
                    placeholder = { Text("AA:BB:CC:DD:EE:FF", fontSize = 10.sp) },
                    singleLine = true,
                    modifier = Modifier.weight(1f),
                )
                Spacer(Modifier.width(6.dp))
                Button(onClick = { onAdd(manual); manual = "" }) { Text("ADD") }
            }
        }
    }
}

@OptIn(androidx.compose.material3.ExperimentalMaterial3Api::class)
@Composable
private fun ConfigCard(
    cfg: JamConfig,
    chScroll: androidx.compose.foundation.ScrollState,
    onChannel: (Int) -> Unit,
    onInterval: (Int) -> Unit,
    onBurst: (Int) -> Unit,
    onGap: (Int) -> Unit,
    onReason: (Int) -> Unit,
    onHop: (Boolean) -> Unit,
    onBroadcast: (Boolean) -> Unit,
    onDisassoc: (Boolean) -> Unit,
) {
    Card(colors = CardDefaults.cardColors(containerColor = Panel), shape = RoundedCornerShape(10.dp)) {
        Column(Modifier.padding(12.dp)) {
            Text("CHANNEL", color = Muted, fontSize = 9.sp)
            Row(Modifier.horizontalScroll(chScroll), horizontalArrangement = Arrangement.spacedBy(5.dp)) {
                (1..13).forEach { ch ->
                    FilterChip(
                        selected = ch == cfg.channel,
                        onClick = { onChannel(ch) },
                        label = { Text("$ch", fontSize = 10.sp) },
                    )
                }
            }

            Spacer(Modifier.height(6.dp))
            SliderRow("INTERVAL", cfg.intervalMs, 1f..200f, onInterval)
            SliderRow("BURST", cfg.burst, 1f..40f, onBurst)
            SliderRow("GAP", cfg.burstGapMs, 0f..1000f, onGap)

            Spacer(Modifier.height(4.dp))
            Text("REASON", color = Muted, fontSize = 9.sp)
            Row(horizontalArrangement = Arrangement.spacedBy(6.dp)) {
                listOf(1, 4, 7, 8).forEach { r ->
                    FilterChip(
                        selected = r == cfg.reason,
                        onClick = { onReason(r) },
                        label = { Text("$r", fontSize = 10.sp) },
                    )
                }
            }

            Spacer(Modifier.height(4.dp))
            SwitchRow("HOP", cfg.hop, onHop)
            SwitchRow("BROADCAST", cfg.broadcast, onBroadcast)
            SwitchRow("DISASSOC", cfg.disassoc, onDisassoc)
        }
    }
}

@Composable
private fun SliderRow(label: String, value: Int, range: ClosedFloatingPointRange<Float>, onChange: (Int) -> Unit) {
    Row(verticalAlignment = Alignment.CenterVertically) {
        Text(label, color = Muted, fontSize = 10.sp, modifier = Modifier.width(64.dp))
        Slider(
            value = value.toFloat().coerceIn(range.start, range.endInclusive),
            valueRange = range,
            onValueChange = { onChange(it.toInt()) },
            modifier = Modifier.weight(1f),
        )
        Text(value.toString(), color = Accent, fontSize = 10.sp, modifier = Modifier.width(40.dp))
    }
}

@Composable
private fun SwitchRow(label: String, checked: Boolean, onChange: (Boolean) -> Unit) {
    Row(verticalAlignment = Alignment.CenterVertically) {
        Text(label, color = Muted, fontSize = 10.sp, modifier = Modifier.weight(1f))
        Switch(checked = checked, onCheckedChange = onChange)
    }
}

@OptIn(androidx.compose.material3.ExperimentalMaterial3Api::class)
@Composable
private fun ApRow(ap: WifiAp, selected: Boolean, onClick: () -> Unit) {
    Card(
        colors = CardDefaults.cardColors(
            containerColor = if (selected) Color(0xFF162223) else Panel,
        ),
        shape = RoundedCornerShape(8.dp),
        modifier = Modifier.fillMaxWidth(),
    ) {
        Row(
            Modifier.padding(horizontal = 10.dp, vertical = 8.dp).fillMaxWidth(),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            FilterChip(selected = selected, onClick = onClick, label = { Text("PICK", fontSize = 9.sp) })
            Spacer(Modifier.width(8.dp))
            Column(Modifier.weight(1f)) {
                Text(ap.ssid.ifEmpty { "(hidden)" }, color = if (selected) Accent else Color.White, fontSize = 12.sp)
                Text("${ap.bssid} · ch ${ap.ch} · ${ap.auth} · ${ap.hits}x", color = Muted, fontSize = 9.sp)
            }
            Signal(ap.rssi)
        }
    }
}

@Composable
private fun ClientRow(c: ClientDev) {
    Card(colors = CardDefaults.cardColors(containerColor = Panel), shape = RoundedCornerShape(8.dp)) {
        Row(Modifier.padding(horizontal = 10.dp, vertical = 8.dp).fillMaxWidth()) {
            Column(Modifier.weight(1f)) {
                Text(c.addr, color = Color.White, fontSize = 12.sp)
                Text("→ ${c.bssid} · ch ${c.ch} · ${c.hits}x", color = Muted, fontSize = 9.sp)
            }
            Signal(c.rssi)
        }
    }
}

@Composable
private fun Signal(rssi: Int) {
    val level = ((abs(rssi) - 30).coerceIn(0, 70) / 14f).coerceIn(0f, 4f)
    val activeBars = (5f - level).toInt().coerceIn(0, 4)
    Column(horizontalAlignment = Alignment.End) {
        Text("$rssi", color = Muted, fontSize = 9.sp)
        Spacer(Modifier.height(3.dp))
        Row(
            Modifier.width(28.dp).height(14.dp),
            verticalAlignment = Alignment.Bottom,
            horizontalArrangement = Arrangement.spacedBy(2.dp)
        ) {
            (0..3).forEach { i ->
                val active = i < activeBars
                Box(
                    Modifier.weight(1f).height(((i + 1) * 3).dp)
                        .clip(RoundedCornerShape(2.dp))
                        .background(if (active) Accent else PanelBorder)
                )
            }
        }
    }
}
