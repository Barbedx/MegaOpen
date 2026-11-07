package ua.megaopen.companion.ui

import android.content.Context
import android.content.Intent
import android.provider.Settings as SystemSettings
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.FlowRow
import androidx.compose.foundation.layout.ExperimentalLayoutApi
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Bluetooth
import androidx.compose.material.icons.filled.Usb
import androidx.compose.material.icons.filled.Wifi
import androidx.compose.material.icons.filled.WifiOff
import androidx.compose.material3.Button
import androidx.compose.material3.Icon
import androidx.compose.material3.ListItem
import androidx.compose.material3.ListItemDefaults
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.unit.dp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import kotlinx.coroutines.launch
import org.json.JSONObject
import ua.megaopen.companion.AppGraph
import ua.megaopen.companion.R
import ua.megaopen.companion.alerts.Phrases
import ua.megaopen.companion.board.BoardConnection
import ua.megaopen.companion.link.BleLink
import ua.megaopen.companion.media.Cover
import ua.megaopen.companion.media.MediaSender
import ua.megaopen.companion.media.NowPlaying
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale
import ua.megaopen.companion.voice.Speaker

@OptIn(ExperimentalLayoutApi::class)
@Composable
fun HomeScreen(graph: AppGraph, openBoard: () -> Unit) {
    val ctx = LocalContext.current
    val scope = rememberCoroutineScope()
    val state by graph.connection.state.collectAsStateWithLifecycle()
    val board by graph.connection.board.collectAsStateWithLifecycle()
    val s by graph.settings.values.collectAsStateWithLifecycle()
    val said by graph.lastSaid.collectAsStateWithLifecycle()

    Page {
        Section(stringResource(R.string.home_board)) {
            val icon = when (state) {
                is BoardConnection.State.Usb -> Icons.Filled.Usb
                is BoardConnection.State.Ble -> Icons.Filled.Bluetooth
                is BoardConnection.State.Wifi -> Icons.Filled.Wifi
                else -> Icons.Filled.WifiOff
            }
            ListItem(
                headlineContent = { Text(stateText(ctx, state)) },
                supportingContent = {
                    val fw = when (val st = state) {
                        is BoardConnection.State.Usb -> st.fw
                        is BoardConnection.State.Ble -> st.fw
                        is BoardConnection.State.Wifi -> st.fw
                        else -> ""
                    }
                    Text(if (fw.isNotEmpty()) stringResource(R.string.home_fw, fw) else stringResource(R.string.home_how))
                },
                leadingContent = { Icon(icon, null) },
                colors = ListItemDefaults.colors(containerColor = Color.Transparent),
            )
            Ways(graph, state)
            Button(onClick = openBoard, enabled = board != null) { Text(stringResource(R.string.home_open_page)) }
        }

        MediaSection(graph)

        Section(stringResource(R.string.home_voice)) {
            SwitchRow(stringResource(R.string.voice_on), s.voiceOn, { v -> scope.launch { graph.settings.setVoiceOn(v) } })
            Text(if (said.isEmpty()) stringResource(R.string.home_said_none) else stringResource(R.string.home_said, said),
                style = MaterialTheme.typography.bodyMedium)
            when {
                graph.speaker.state == Speaker.State.NoEngine -> Note(stringResource(R.string.voice_no_engine))
                graph.speaker.langOk == false -> Note(stringResource(R.string.voice_no_lang))
            }
        }

        Section(stringResource(R.string.home_test)) {
            Note(stringResource(R.string.home_test_note))
            FlowRow(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                for (k in Phrases.KNOWN) {
                    OutlinedButton(enabled = board != null, onClick = {
                        val b = board ?: return@OutlinedButton
                        scope.launch {
                            val msg = try {
                                val r = b.postJson("/api/alerts/test", JSONObject().put("sig", k.sig))
                                if (r.code == 200) ctx.getString(R.string.home_test_sent, b.label)
                                else ctx.getString(R.string.error_board, "HTTP ${r.code} ${r.text()}")
                            } catch (e: Exception) {
                                ctx.getString(R.string.error_board, e.message ?: e.javaClass.simpleName)
                            }
                            toast(ctx, msg)
                        }
                    }) { Text(stringResource(k.label)) }
                }
            }
        }
    }
}

// While no way is up: what each allowed way is doing, and when it tries next.
@Composable
private fun Ways(graph: AppGraph, state: BoardConnection.State) {
    if (state is BoardConnection.State.Usb || state is BoardConnection.State.Ble || state is BoardConnection.State.Wifi) return
    val ctx = LocalContext.current
    val s by graph.settings.values.collectAsStateWithLifecycle()
    val usb by graph.usb.state.collectAsStateWithLifecycle()
    val ble by graph.ble.state.collectAsStateWithLifecycle()
    val now = rememberClock()
    val mode = s.connMode
    if (mode.usb) Note(usbText(ctx, usb))
    if (mode.ble) {
        Note(bleText(ctx, ble, now))
        if (ble is BleLink.State.Retry) Note(stringResource(R.string.ble_hint))
    }
    if (mode.wifi) wifiText(ctx, state, now)?.let { Note(it) }
}

@Composable
private fun MediaSection(graph: AppGraph) {
    val ctx = LocalContext.current
    val access by graph.media.access.collectAsStateWithLifecycle()
    val np by graph.media.now.collectAsStateWithLifecycle()
    val sent by graph.mediaSender.status.collectAsStateWithLifecycle()

    Section(stringResource(R.string.media_title)) {
        if (!access) {
            Note(stringResource(R.string.media_access_off))
            Button(onClick = { openListenerSettings(ctx) }) { Text(stringResource(R.string.media_access_open)) }
            return@Section
        }
        Note(stringResource(R.string.media_access_on))
        Row(horizontalArrangement = Arrangement.spacedBy(16.dp), verticalAlignment = Alignment.CenterVertically) {
            CoverView(np?.cover)
            Column(Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(4.dp)) {
                NowPlayingText(np)
            }
        }
        Note(sentText(ctx, sent))
    }
}

@Composable
private fun NowPlayingText(np: NowPlaying?) {
    if (np == null) {
        Text(stringResource(R.string.media_none), style = MaterialTheme.typography.bodyLarge)
        return
    }
    Text(np.title.ifEmpty { "—" }, style = MaterialTheme.typography.titleMedium)
    if (np.artist.isNotEmpty()) Text(np.artist, style = MaterialTheme.typography.bodyMedium)
    if (np.album.isNotEmpty()) Note(np.album)
    Note(stringResource(if (np.playing) R.string.media_playing else R.string.media_paused, np.app))
    if (np.cover == null) Note(stringResource(R.string.media_no_cover))
}

// The cover exactly as the board gets it: 48×48 bits, each a square.
@Composable
private fun CoverView(cover: Cover?) {
    val lit = MaterialTheme.colorScheme.onSurface
    val dark = MaterialTheme.colorScheme.surfaceVariant
    Canvas(Modifier.size(144.dp)) {
        drawRect(dark)
        if (cover == null) return@Canvas
        val cell = size.width / Cover.SIZE
        for (y in 0 until Cover.SIZE) for (x in 0 until Cover.SIZE) {
            if (cover.lit(x, y)) drawRect(lit, Offset(x * cell, y * cell), Size(cell, cell))
        }
    }
}

private fun sentText(ctx: Context, st: MediaSender.Status?): String {
    if (st == null) return ctx.getString(R.string.media_not_sent)
    val result = when {
        !st.ok -> st.error
        st.artSent -> ctx.getString(R.string.media_ok_art)
        else -> ctx.getString(R.string.media_ok)
    }
    val what = st.sent?.title?.ifEmpty { null } ?: if (st.sent == null) ctx.getString(R.string.media_none) else st.sent.app
    val time = SimpleDateFormat("HH:mm:ss", Locale.ROOT).format(Date(st.atMs))
    return ctx.getString(R.string.media_reply, time, what, st.board, result)
}

private fun openListenerSettings(ctx: Context) {
    try {
        ctx.startActivity(Intent(SystemSettings.ACTION_NOTIFICATION_LISTENER_SETTINGS))
    } catch (e: Exception) {
        toast(ctx, ctx.getString(R.string.media_no_settings))
    }
}
