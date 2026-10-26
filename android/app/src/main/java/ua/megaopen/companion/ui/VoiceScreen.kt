package ua.megaopen.companion.ui

import android.content.Intent
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.PlayArrow
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.SegmentedButton
import androidx.compose.material3.SegmentedButtonDefaults
import androidx.compose.material3.SingleChoiceSegmentedButtonRow
import androidx.compose.material3.Slider
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
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import kotlinx.coroutines.launch
import ua.megaopen.companion.AppGraph
import ua.megaopen.companion.R
import ua.megaopen.companion.alerts.Alert
import ua.megaopen.companion.alerts.Phrases

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun VoiceScreen(graph: AppGraph) {
    val ctx = LocalContext.current
    val scope = rememberCoroutineScope()
    val s by graph.settings.values.collectAsStateWithLifecycle()
    val say: (String) -> Unit = { graph.speaker.say(it, s.voiceLang, s.volume) }

    Page {
        Section(stringResource(R.string.voice_title)) {
            SwitchRow(stringResource(R.string.voice_on), s.voiceOn, { v -> scope.launch { graph.settings.setVoiceOn(v) } })
            Text(stringResource(R.string.voice_lang))
            val langs = listOf("uk" to R.string.lang_uk, "en" to R.string.lang_en, "" to R.string.lang_system)
            SingleChoiceSegmentedButtonRow(Modifier.fillMaxWidth()) {
                langs.forEachIndexed { i, (id, label) ->
                    SegmentedButton(selected = s.voiceLang == id, onClick = { scope.launch { graph.settings.setVoiceLang(id) } },
                        shape = SegmentedButtonDefaults.itemShape(i, langs.size)) { Text(stringResource(label)) }
                }
            }
            Text(stringResource(R.string.voice_volume, s.volume))
            var vol by remember(s.volume) { mutableStateOf(s.volume.toFloat()) }
            Slider(value = vol, onValueChange = { vol = it }, valueRange = 0f..100f,
                onValueChangeFinished = {
                    scope.launch { graph.settings.setVolume(vol.toInt()) }
                    graph.speaker.say(ctx.getString(R.string.voice_sample), s.voiceLang, vol.toInt())
                })
            OutlinedButton(onClick = {
                try { ctx.startActivity(Intent("com.android.settings.TTS_SETTINGS")) }
                catch (e: Exception) { toast(ctx, ctx.getString(R.string.voice_no_tts_settings)) }
            }) { Text(stringResource(R.string.voice_tts_settings)) }
            Note(stringResource(R.string.voice_engine_note))
        }

        Section(stringResource(R.string.voice_what)) {
            for (k in Phrases.KNOWN) {
                SwitchRow(stringResource(k.label), k.sig !in s.off, { v -> scope.launch { graph.settings.setAlertOn(k.sig, v) } })
                var text by remember(k.sig) { mutableStateOf(s.phrases[k.sig].orEmpty()) }
                Row(verticalAlignment = Alignment.CenterVertically) {
                    OutlinedTextField(
                        value = text,
                        onValueChange = { text = it; scope.launch { graph.settings.setPhrase(k.sig, it) } },
                        placeholder = { Text(k.default(s.voiceLang)) },
                        singleLine = true,
                        modifier = Modifier.weight(1f),
                    )
                    IconButton(onClick = { Phrases.say(Alert(0, k.sig, 1, ""), s.copy(off = emptySet()))?.let(say) }) {
                        Icon(Icons.Filled.PlayArrow, stringResource(R.string.voice_play))
                    }
                }
                // While it lasts: once, or again every N seconds until it is fixed.
                val rep = s.repeatOf(k.sig, k.repeatS)
                val choices = listOf(0, 15, 30, 60)
                SingleChoiceSegmentedButtonRow(Modifier.fillMaxWidth()) {
                    choices.forEachIndexed { i, sec ->
                        SegmentedButton(selected = rep == sec, onClick = { scope.launch { graph.settings.setRepeat(k.sig, sec) } },
                            shape = SegmentedButtonDefaults.itemShape(i, choices.size)) {
                            Text(if (sec == 0) stringResource(R.string.repeat_once) else stringResource(R.string.repeat_every, sec))
                        }
                    }
                }
            }
            SwitchRow(stringResource(R.string.voice_others), s.others, { v -> scope.launch { graph.settings.setOthers(v) } },
                note = stringResource(R.string.voice_others_note))
            Note(stringResource(R.string.voice_phrase_note))
            Note(stringResource(R.string.voice_speeds_note))
        }

        Section(stringResource(R.string.notify_title)) {
            SwitchRow(stringResource(R.string.notify_on), s.notify, { v -> scope.launch { graph.settings.setNotify(v) } },
                note = stringResource(R.string.notify_note))
        }
    }
}
