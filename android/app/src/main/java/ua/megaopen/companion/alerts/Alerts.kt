package ua.megaopen.companion.alerts

import androidx.annotation.StringRes
import ua.megaopen.companion.R
import ua.megaopen.companion.settings.Settings
import java.util.Locale

/** An alert the board raised (src/app/CarTask.cpp's ring, /api/alerts or a link event). */
data class Alert(val seq: Long, val sig: String, val value: Long, val text: String, val agoMs: Long = 0)

/**
 * The alerts the board raises by itself (src/vehicle/AlertRules.cpp factory())
 * and what to say for each. Pure but for the label resource: unit-tested.
 */
object Phrases {
    class Kind(val sig: String, @StringRes val label: Int, private val uk: String, private val en: String) {
        fun default(lang: String): String = when (lang.ifEmpty { Locale.getDefault().language }) {
            "uk", "ru" -> uk
            else -> en
        }
    }

    val KNOWN = listOf(
        Kind("drive_no_lights", R.string.alert_lights, "Увімкніть фари", "Turn the headlights on"),
        Kind("drive_handbrake", R.string.alert_handbrake, "Зніміть з ручника", "Release the handbrake"),
        Kind("fuel_low", R.string.alert_fuel, "Мало пального", "Fuel is low"),
    )

    fun kind(sig: String): Kind? = KNOWN.firstOrNull { it.sig == sig }

    /** What to say for this alert with these settings, or null: nothing. */
    fun say(a: Alert, s: Settings.Values): String? {
        val k = kind(a.sig)
        val text = when {
            k != null -> if (k.sig in s.off) return null else s.phrases[k.sig] ?: k.default(s.voiceLang)
            s.others -> a.text
            else -> return null
        }
        return text.replace("{v}", a.value.toString()).trim().ifEmpty { null }
    }
}
