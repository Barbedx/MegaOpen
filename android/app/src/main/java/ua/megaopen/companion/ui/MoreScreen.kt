package ua.megaopen.companion.ui

import android.content.Intent
import android.net.Uri
import android.os.Build
import android.provider.Settings as SystemSettings
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.material3.Button
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.unit.dp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import kotlinx.coroutines.launch
import ua.megaopen.companion.AppGraph
import ua.megaopen.companion.BuildConfig
import ua.megaopen.companion.R
import ua.megaopen.companion.board.Discovery
import ua.megaopen.companion.link.UsbLink
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
        SwitchRow(stringResource(R.string.conn_autostart), s.autostart, { v -> scope.launch { graph.settings.setAutostart(v) } },
            note = stringResource(R.string.conn_autostart_note))
    }
}

@Composable
private fun DiagnosticsSection(graph: AppGraph) {
    val ctx = LocalContext.current
    val state by graph.connection.state.collectAsStateWithLifecycle()
    val usb by graph.usb.state.collectAsStateWithLifecycle()
    val st by graph.usb.stats.collectAsStateWithLifecycle()

    Section(stringResource(R.string.diag_title)) {
        Text(stateText(ctx, state))
        val usbText = when (val u = usb) {
            UsbLink.State.Unplugged -> stringResource(R.string.diag_usb_unplugged)
            UsbLink.State.NoPermission -> stringResource(R.string.diag_usb_permission)
            UsbLink.State.Connecting -> stringResource(R.string.diag_usb_connecting)
            is UsbLink.State.Up -> stringResource(R.string.diag_usb_up, u.hello.optString("fw"), u.hello.optString("build"))
            is UsbLink.State.Failed -> stringResource(R.string.diag_usb_failed, u.why)
        }
        Text(usbText)
        Note(stringResource(R.string.diag_usb_stats, st.rxFrames, st.txFrames, st.bad, st.requests, st.failures, st.netRequests))
        if (st.lastError.isNotEmpty()) Note(stringResource(R.string.diag_last_error, st.lastError))
        Note(stringResource(R.string.diag_about, BuildConfig.VERSION_NAME, BuildConfig.RELEASES_REPO))
    }
}
