package ua.megaopen.companion.ui

import android.content.Intent
import android.net.Uri
import android.os.Build
import android.provider.Settings as SystemSettings
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.selection.selectable
import androidx.compose.foundation.selection.selectableGroup
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Bluetooth
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Button
import androidx.compose.material3.Icon
import androidx.compose.material3.ListItem
import androidx.compose.material3.ListItemDefaults
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.RadioButton
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateListOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.unit.dp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.launch
import kotlinx.coroutines.withTimeoutOrNull
import ua.megaopen.companion.AppGraph
import ua.megaopen.companion.BuildConfig
import ua.megaopen.companion.R
import ua.megaopen.companion.board.Discovery
import ua.megaopen.companion.link.BleLink
import ua.megaopen.companion.link.BlePermissions
import ua.megaopen.companion.link.LinkStats
import ua.megaopen.companion.link.UsbSeen
import ua.megaopen.companion.settings.ConnMode
import ua.megaopen.companion.update.Releases

@Composable
fun MoreScreen(graph: AppGraph) {
    Page {
        UpdatesSection(graph)
        ConnectionSection(graph)
        DiagnosticsSection(graph)
    }
}

@Composable
private fun UpdatesSection(graph: AppGraph) {
    val ctx = LocalContext.current
    val scope = rememberCoroutineScope()
    val s by graph.settings.values.collectAsStateWithLifecycle()
    var status by remember { mutableStateOf("") }
    var found by remember { mutableStateOf<Releases.Release?>(null) }
    var busy by remember { mutableStateOf(false) }
    var pct by remember { mutableStateOf(-1) }

    Section(stringResource(R.string.upd_title)) {
        Text(stringResource(R.string.upd_version, BuildConfig.VERSION_NAME, s.installedTag.ifEmpty { BuildConfig.RELEASE_TAG }.ifEmpty { "—" }))
        SwitchRow(stringResource(R.string.upd_pre), s.pre, { v -> found = null; scope.launch { graph.settings.setPre(v) } },
            note = stringResource(R.string.upd_pre_note))
        Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            OutlinedButton(enabled = !busy, onClick = {
                busy = true
                status = ctx.getString(R.string.upd_checking)
                scope.launch {
                    try {
                        val r = graph.updater.newest(s.pre)
                        val newer = r != null && Releases.isNewer(r, Releases.mine(BuildConfig.VERSION_NAME, s.installedTag, BuildConfig.RELEASE_TAG))
                        found = if (newer) r else null
                        status = if (newer) ctx.getString(R.string.upd_available, r!!.tag) else ctx.getString(R.string.upd_none)
                    } catch (e: Exception) {
                        status = ctx.getString(R.string.upd_error, e.message ?: e.javaClass.simpleName)
                    } finally {
                        busy = false
                    }
                }
            }) { Text(stringResource(R.string.upd_check)) }
            val r = found
            if (r != null) Button(enabled = !busy, onClick = {
                if (Build.VERSION.SDK_INT >= 26 && !ctx.packageManager.canRequestPackageInstalls()) {
                    toast(ctx, ctx.getString(R.string.upd_allow))
                    ctx.startActivity(Intent(SystemSettings.ACTION_MANAGE_UNKNOWN_APP_SOURCES, Uri.parse("package:${ctx.packageName}")))
                    return@Button
                }
                busy = true
                scope.launch {
                    try {
                        graph.settings.setPendingTag(r.tag)
                        graph.updater.install(r) { p -> pct = p }
                        status = ctx.getString(R.string.upd_confirm)
                    } catch (e: Exception) {
                        status = ctx.getString(R.string.upd_error, e.message ?: e.javaClass.simpleName)
                    } finally {
                        busy = false
                        pct = -1
                    }
                }
            }) { Text(stringResource(R.string.upd_install, r.tag)) }
        }
        if (pct >= 0) LinearProgressIndicator(progress = { pct / 100f }, modifier = Modifier.fillMaxWidth())
        if (status.isNotEmpty()) Note(status)
    }
}

@Composable
private fun ConnectionSection(graph: AppGraph) {
    val ctx = LocalContext.current
    val scope = rememberCoroutineScope()
    val s by graph.settings.values.collectAsStateWithLifecycle()
    var host by remember(s.loaded) { mutableStateOf(s.host) }
    var finding by remember { mutableStateOf(false) }

    Section(stringResource(R.string.conn_title)) {
        Text(stringResource(R.string.conn_mode))
        Column(Modifier.selectableGroup()) {
            for ((m, label) in listOf(ConnMode.Auto to R.string.conn_mode_auto, ConnMode.Usb to R.string.conn_mode_usb,
                ConnMode.Ble to R.string.conn_mode_ble, ConnMode.Wifi to R.string.conn_mode_wifi)) {
                Row(
                    Modifier.fillMaxWidth().selectable(selected = s.connMode == m, role = Role.RadioButton,
                        onClick = { scope.launch { graph.settings.setConnMode(m) } }),
                    verticalAlignment = Alignment.CenterVertically,
                ) {
                    RadioButton(selected = s.connMode == m, onClick = null)
                    Spacer(Modifier.width(8.dp))
                    Text(stringResource(label))
                }
            }
        }
        if (s.connMode.ble) BleBoardRow(graph)
        if (s.connMode.wifi) {
            Note(stringResource(R.string.conn_note))
            Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                OutlinedTextField(value = host, onValueChange = { host = it; scope.launch { graph.settings.setHost(it) } },
                    label = { Text(stringResource(R.string.conn_host)) }, placeholder = { Text(stringResource(R.string.conn_host_auto)) },
                    singleLine = true, modifier = Modifier.weight(1f))
                OutlinedButton(enabled = !finding, onClick = {
                    finding = true
                    scope.launch {
                        val h = Discovery(ctx).find()
                        finding = false
                        toast(ctx, if (h != null) ctx.getString(R.string.conn_found, h) else ctx.getString(R.string.conn_not_found))
                    }
                }) { Text(stringResource(if (finding) R.string.conn_finding else R.string.conn_find)) }
            }
        }
        SwitchRow(stringResource(R.string.conn_autostart), s.autostart, { v -> scope.launch { graph.settings.setAutostart(v) } },
            note = stringResource(R.string.conn_autostart_note))
    }
}

// The board on Bluetooth: chosen once (a picker over the paired devices and
// a scan), then connected to by its address. The permissions are asked here.
@Composable
private fun BleBoardRow(graph: AppGraph) {
    val ctx = LocalContext.current
    val scope = rememberCoroutineScope()
    val s by graph.settings.values.collectAsStateWithLifecycle()
    val ble by graph.ble.state.collectAsStateWithLifecycle()
    var picking by remember { mutableStateOf(false) }
    val pick = {
        if (BlePermissions.adapter(ctx)?.isEnabled != true) toast(ctx, ctx.getString(R.string.conn_ble_off)) else picking = true
    }
    val askScan = rememberLauncherForActivityResult(ActivityResultContracts.RequestMultiplePermissions()) { res ->
        if (res.values.all { it }) pick() else toast(ctx, ctx.getString(R.string.conn_ble_denied))
    }
    val askConnect = rememberLauncherForActivityResult(ActivityResultContracts.RequestMultiplePermissions()) { res ->
        if (!res.values.all { it }) toast(ctx, ctx.getString(R.string.conn_ble_denied))
    }

    Text(stringResource(R.string.conn_ble))
    Note(if (s.bleAddress.isEmpty()) stringResource(R.string.conn_ble_none)
         else stringResource(R.string.conn_ble_chosen, s.bleName.ifEmpty { s.bleAddress }, s.bleAddress))
    Note(bleText(ctx, ble, rememberClock()))
    Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
        OutlinedButton(onClick = {
            if (BlePermissions.has(ctx, BlePermissions.scan)) pick() else askScan.launch(BlePermissions.scan)
        }) { Text(stringResource(R.string.conn_ble_pick)) }
        if (s.bleAddress.isNotEmpty()) OutlinedButton(onClick = { scope.launch { graph.settings.setBleBoard("", "") } }) {
            Text(stringResource(R.string.conn_ble_forget))
        }
        if (ble is BleLink.State.NoPermission) Button(onClick = { askConnect.launch(BlePermissions.connect) }) {
            Text(stringResource(R.string.conn_ble_permission))
        }
    }
    Note(stringResource(R.string.ble_hint))
    if (picking) BlePicker(graph) { picking = false }
}

@Composable
private fun BlePicker(graph: AppGraph, done: () -> Unit) {
    val scope = rememberCoroutineScope()
    val known by graph.connection.boardName.collectAsStateWithLifecycle()
    val found = remember { mutableStateListOf<BleLink.Found>() }
    var scanning by remember { mutableStateOf(true) }
    LaunchedEffect(Unit) {
        found.addAll(graph.ble.bonded(known))
        try {
            withTimeoutOrNull(SCAN_MS) {
                graph.ble.scan(known).collect { f ->
                    val i = found.indexOfFirst { it.address == f.address }
                    if (i < 0) found += f
                    else found[i] = f.copy(name = f.name.ifEmpty { found[i].name }, bonded = f.bonded || found[i].bonded)
                }
            }
        } catch (e: CancellationException) {
            throw e
        } catch (e: Exception) {
            // The scan did not start (Bluetooth busy or off): the paired list stays.
        }
        scanning = false
    }
    AlertDialog(
        onDismissRequest = done,
        title = { Text(stringResource(R.string.pick_title)) },
        text = {
            Column(Modifier.verticalScroll(rememberScrollState()), verticalArrangement = Arrangement.spacedBy(4.dp)) {
                if (scanning) {
                    Note(stringResource(R.string.pick_scanning))
                    LinearProgressIndicator(Modifier.fillMaxWidth())
                }
                for (f in found) {
                    ListItem(
                        headlineContent = { Text(f.name.ifEmpty { f.address }) },
                        supportingContent = {
                            val parts = listOfNotNull(f.address, if (f.bonded) stringResource(R.string.pick_paired) else null,
                                f.rssi?.let { stringResource(R.string.pick_rssi, it) })
                            Text(parts.joinToString(" · "))
                        },
                        leadingContent = { Icon(Icons.Filled.Bluetooth, null) },
                        colors = ListItemDefaults.colors(containerColor = Color.Transparent),
                        modifier = Modifier.clickable {
                            scope.launch {
                                graph.settings.setBleBoard(f.address, f.name)
                                done()
                            }
                        },
                    )
                }
                if (!scanning && found.isEmpty()) {
                    Note(stringResource(R.string.pick_none))
                    Note(stringResource(R.string.ble_hint))
                    if (Build.VERSION.SDK_INT < 31) Note(stringResource(R.string.pick_location_note))
                }
            }
        },
        confirmButton = { TextButton(onClick = done) { Text(stringResource(R.string.pick_cancel)) } },
    )
}

private const val SCAN_MS = 15_000L

@Composable
private fun DiagnosticsSection(graph: AppGraph) {
    val ctx = LocalContext.current
    val state by graph.connection.state.collectAsStateWithLifecycle()
    val usb by graph.usb.state.collectAsStateWithLifecycle()
    val usbSt by graph.usb.stats.collectAsStateWithLifecycle()
    val ble by graph.ble.state.collectAsStateWithLifecycle()
    val bleSt by graph.ble.stats.collectAsStateWithLifecycle()
    val now = rememberClock()

    Section(stringResource(R.string.diag_title)) {
        Text(stateText(ctx, state))
        Text(usbText(ctx, usb))
        LinkStatsNote(usbSt)
        Text(bleText(ctx, ble, now))
        LinkStatsNote(bleSt)
        UsbDevices(graph)
        Note(stringResource(R.string.diag_about, BuildConfig.VERSION_NAME, BuildConfig.RELEASES_REPO))
    }
}

@Composable
private fun LinkStatsNote(st: LinkStats) {
    Note(stringResource(R.string.diag_link_stats, st.rxFrames, st.txFrames, st.bad, st.requests, st.failures, st.netRequests))
    if (st.lastError.isNotEmpty()) Note(stringResource(R.string.diag_last_error, st.lastError))
}

// Every USB device the head unit sees (UsbSeen), the board marked, and what
// came and went: the board missing while a phone in the same cable and port
// shows up says the port works and the board, its cable or its power does not.
@Composable
private fun UsbDevices(graph: AppGraph) {
    val seen by graph.usbSeen.devices.collectAsStateWithLifecycle()
    val log by graph.usbSeen.log.collectAsStateWithLifecycle()
    val noName = stringResource(R.string.diag_usb_noname)
    val label = { d: UsbSeen.Dev -> d.name.ifEmpty { noName } }
    Text(stringResource(R.string.diag_usb_seen))
    if (seen.isEmpty()) Note(stringResource(R.string.diag_usb_seen_none))
    seen.forEach { d ->
        Note(stringResource(if (d.ours) R.string.diag_usb_dev_ours else R.string.diag_usb_dev, label(d), d.id, d.classText))
    }
    when {
        seen.any { it.rom } -> Note(stringResource(R.string.diag_usb_rom))
        seen.isNotEmpty() && seen.none { it.ours } -> Note(stringResource(R.string.diag_usb_others))
    }
    val clock = remember { SimpleDateFormat("HH:mm:ss", Locale.ROOT) }
    log.asReversed().forEach { e ->
        Note(stringResource(if (e.came) R.string.diag_usb_came else R.string.diag_usb_went, clock.format(Date(e.atMs)), label(e.dev), e.dev.id))
    }
}
