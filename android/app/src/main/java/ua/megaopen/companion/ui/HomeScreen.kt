package ua.megaopen.companion.ui

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.FlowRow
import androidx.compose.foundation.layout.ExperimentalLayoutApi
import androidx.compose.material.icons.Icons
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
                is BoardConnection.State.Wifi -> Icons.Filled.Wifi
                else -> Icons.Filled.WifiOff
            }
            ListItem(
                headlineContent = { Text(stateText(ctx, state)) },
                supportingContent = {
                    val fw = when (val st = state) {
                        is BoardConnection.State.Usb -> st.fw
                        is BoardConnection.State.Wifi -> st.fw
                        else -> ""
                    }
                    Text(if (fw.isNotEmpty()) stringResource(R.string.home_fw, fw) else stringResource(R.string.home_how))
                },
                leadingContent = { Icon(icon, null) },
                colors = ListItemDefaults.colors(containerColor = Color.Transparent),
            )
            Button(onClick = openBoard, enabled = board != null) { Text(stringResource(R.string.home_open_page)) }
        }

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
