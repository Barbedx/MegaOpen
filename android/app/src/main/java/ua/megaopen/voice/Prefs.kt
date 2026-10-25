package ua.megaopen.voice

import android.content.Context

/** Everything the owner sets in the app, in one SharedPreferences file. */
class Prefs(ctx: Context) {
    private val sp = ctx.applicationContext.getSharedPreferences("voice", Context.MODE_PRIVATE)

    var enabled: Boolean
        get() = sp.getBoolean("enabled", false)
        set(v) = sp.edit().putBoolean("enabled", v).apply()

    /** Empty: find the board (mDNS, then the local /24). */
    var host: String
        get() = sp.getString("host", "") ?: ""
        set(v) = sp.edit().putString("host", v.trim()).apply()

    var autostart: Boolean
        get() = sp.getBoolean("autostart", true)
        set(v) = sp.edit().putBoolean("autostart", v).apply()

    /** "uk", "en" or "" (the system's). */
    var lang: String
        get() = sp.getString("lang", "uk") ?: "uk"
        set(v) = sp.edit().putString("lang", v).apply()

    /** 0..100 */
    var volume: Int
        get() = sp.getInt("volume", 100)
        set(v) = sp.edit().putInt("volume", v.coerceIn(0, 100)).apply()

    /** Speak alerts the app has no phrase for, with the board's text. */
    var others: Boolean
        get() = sp.getBoolean("others", true)
        set(v) = sp.edit().putBoolean("others", v).apply()

    var intervalMs: Long
        get() = sp.getLong("interval", 1500L)
        set(v) = sp.edit().putLong("interval", v.coerceIn(500L, 10000L)).apply()

    /** Updates: the rc channel (Updater.newest). */
    var pre: Boolean
        get() = sp.getBoolean("pre", false)
        set(v) = sp.edit().putBoolean("pre", v).apply()

    /** The release tag this install came from ("" when put on by hand). */
    var installedTag: String
        get() = sp.getString("tag", "") ?: ""
        set(v) = sp.edit().putString("tag", v).apply()

    /** The tag handed to the installer, until it reports. */
    var pendingTag: String
        get() = sp.getString("pendingTag", "") ?: ""
        set(v) = sp.edit().putString("pendingTag", v).apply()

    /** The install went through: the pending tag is now this one's. */
    fun settleTag() {
        val t = pendingTag
        if (t.isNotEmpty()) sp.edit().putString("tag", t).putString("pendingTag", "").apply()
    }

    fun alertOn(sig: String) = sp.getBoolean("on_$sig", true)
    fun setAlertOn(sig: String, v: Boolean) = sp.edit().putBoolean("on_$sig", v).apply()

    /** The owner's own phrase, or "" for the default one. */
    fun ownPhrase(sig: String) = sp.getString("say_$sig", "") ?: ""
    fun setOwnPhrase(sig: String, v: String) = sp.edit().putString("say_$sig", v.trim()).apply()

    fun phrase(k: Phrases.Kind): String = ownPhrase(k.sig).ifEmpty { k.default(lang) }
}
