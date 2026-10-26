package ua.megaopen.companion.settings

import android.content.Context
import androidx.datastore.core.DataStore
import androidx.datastore.preferences.core.MutablePreferences
import androidx.datastore.preferences.core.Preferences
import androidx.datastore.preferences.core.booleanPreferencesKey
import androidx.datastore.preferences.core.edit
import androidx.datastore.preferences.core.intPreferencesKey
import androidx.datastore.preferences.core.stringPreferencesKey
import androidx.datastore.preferences.preferencesDataStore
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.flow.SharingStarted
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.map
import kotlinx.coroutines.flow.stateIn

private val Context.store: DataStore<Preferences> by preferencesDataStore(name = "settings")

/** Every setting the owner makes in the app, one DataStore, read as a StateFlow. */
class Settings(ctx: Context, scope: CoroutineScope) {
    data class Values(
        val loaded: Boolean = false,
        /** Say the board's alerts. The service runs while this is on. */
        val voiceOn: Boolean = true,
        /** Empty: find the board (USB first, then mDNS and the hotspot's addresses). */
        val host: String = "",
        val autostart: Boolean = true,
        /** "uk", "en" or "" (the system's). */
        val voiceLang: String = "uk",
        val volume: Int = 100,
        /** Alerts the app has no phrase for: said with the board's text. */
        val others: Boolean = true,
        /** Updates: the rc channel. */
        val pre: Boolean = false,
        /** The release this install came from, "" when put on by hand. */
        val installedTag: String = "",
        val pendingTag: String = "",
        val off: Set<String> = emptySet(),
        val phrases: Map<String, String> = emptyMap(),
        /** Seconds between repeats while an alert's condition lasts; 0 = said once. Unset: the alert's default. */
        val repeat: Map<String, Int> = emptyMap(),
        /** An Android notification with each alert, there while it lasts. */
        val notify: Boolean = true,
    ) {
        fun repeatOf(sig: String, default: Int) = repeat[sig] ?: default
    }

    private val store = ctx.applicationContext.store

    val values: StateFlow<Values> = store.data.map { read(it) }.stateIn(scope, SharingStarted.Eagerly, Values())

    private fun read(p: Preferences): Values {
        val off = HashSet<String>()
        val say = HashMap<String, String>()
        val rep = HashMap<String, Int>()
        for ((k, v) in p.asMap()) {
            if (k.name.startsWith(REP) && v is Int) rep[k.name.removePrefix(REP)] = v
            if (k.name.startsWith(OFF) && v == true) off += k.name.removePrefix(OFF)
            if (k.name.startsWith(SAY) && v is String && v.isNotBlank()) say[k.name.removePrefix(SAY)] = v
        }
        return Values(
            loaded = true,
            voiceOn = p[VOICE_ON] ?: true,
            host = p[HOST] ?: "",
            autostart = p[AUTOSTART] ?: true,
            voiceLang = p[LANG] ?: "uk",
            volume = p[VOLUME] ?: 100,
            others = p[OTHERS] ?: true,
            pre = p[PRE] ?: false,
            installedTag = p[TAG] ?: "",
            pendingTag = p[PENDING] ?: "",
            off = off,
            phrases = say,
            repeat = rep,
            notify = p[NOTIFY] ?: true,
        )
    }

    private suspend fun edit(f: (MutablePreferences) -> Unit) { store.edit { f(it) } }

    suspend fun setVoiceOn(v: Boolean) = edit { it[VOICE_ON] = v }
    suspend fun setHost(v: String) = edit { it[HOST] = v.trim() }
    suspend fun setAutostart(v: Boolean) = edit { it[AUTOSTART] = v }
    suspend fun setVoiceLang(v: String) = edit { it[LANG] = v }
    suspend fun setVolume(v: Int) = edit { it[VOLUME] = v.coerceIn(0, 100) }
    suspend fun setOthers(v: Boolean) = edit { it[OTHERS] = v }
    suspend fun setPre(v: Boolean) = edit { it[PRE] = v }
    suspend fun setAlertOn(sig: String, on: Boolean) = edit { it[booleanPreferencesKey(OFF + sig)] = !on }
    suspend fun setPhrase(sig: String, v: String) = edit { it[stringPreferencesKey(SAY + sig)] = v.trim() }
    suspend fun setPendingTag(v: String) = edit { it[PENDING] = v }
    suspend fun setRepeat(sig: String, seconds: Int) = edit { it[intPreferencesKey(REP + sig)] = seconds.coerceIn(0, 600) }
    suspend fun setNotify(v: Boolean) = edit { it[NOTIFY] = v }

    /** The install went through: the pending tag is this install's now. */
    suspend fun settleTag() = edit {
        val t = it[PENDING] ?: ""
        if (t.isNotEmpty()) {
            it[TAG] = t
            it[PENDING] = ""
        }
    }

    private companion object {
        val VOICE_ON = booleanPreferencesKey("voiceOn")
        val HOST = stringPreferencesKey("host")
        val AUTOSTART = booleanPreferencesKey("autostart")
        val LANG = stringPreferencesKey("voiceLang")
        val VOLUME = intPreferencesKey("volume")
        val OTHERS = booleanPreferencesKey("others")
        val PRE = booleanPreferencesKey("pre")
        val TAG = stringPreferencesKey("installedTag")
        val PENDING = stringPreferencesKey("pendingTag")
        val NOTIFY = booleanPreferencesKey("notify")
        const val OFF = "off_"
        const val SAY = "say_"
        const val REP = "rep_"
    }
}
