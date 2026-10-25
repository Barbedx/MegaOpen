package ua.megaopen.voice

import java.util.Locale

/** The alerts the board raises by itself (src/vehicle/AlertRules.cpp factory()). */
object Phrases {
    class Kind(val sig: String, val label: Int, private val uk: String, private val en: String) {
        fun default(lang: String): String = when (lang.ifEmpty { Locale.getDefault().language }) {
            "uk", "ru" -> uk
            else -> en
        }
    }

    val KNOWN = listOf(
        Kind("drive_no_lights", R.string.a_lights, "Увімкніть фари", "Turn the headlights on"),
        Kind("drive_handbrake", R.string.a_brake, "Зніміть з ручника", "Release the handbrake"),
        Kind("fuel_low", R.string.a_fuel, "Мало пального", "Fuel is low"),
    )

    fun kind(sig: String): Kind? = KNOWN.firstOrNull { it.sig == sig }
}
