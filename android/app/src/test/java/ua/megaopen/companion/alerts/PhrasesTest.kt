package ua.megaopen.companion.alerts

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Test
import ua.megaopen.companion.settings.Settings

class PhrasesTest {
    private val s = Settings.Values(loaded = true, voiceLang = "uk")
    private fun a(sig: String, text: String = "board text", v: Long = 1) = Alert(1, sig, v, text)

    @Test fun factoryAlertsHaveTheirPhraseInTheVoicesLanguage() {
        assertEquals("Увімкніть фари", Phrases.say(a("drive_no_lights"), s))
        assertEquals("Turn the headlights on", Phrases.say(a("drive_no_lights"), s.copy(voiceLang = "en")))
    }

    @Test fun ownPhraseWithTheValue() {
        val own = s.copy(phrases = mapOf("fuel_low" to "Залишилось {v} літрів"))
        assertEquals("Залишилось 7 літрів", Phrases.say(a("fuel_low", v = 7), own))
    }

    @Test fun switchedOffSaysNothing() {
        assertNull(Phrases.say(a("drive_handbrake"), s.copy(off = setOf("drive_handbrake"))))
    }

    @Test fun aRuleIsShownEvenWhenItIsNotSaid() {
        val quiet = Settings.Values()
        assertNull(Phrases.say(a("gear", "gear: 3"), quiet))
        assertEquals("gear: 3", Phrases.show(a("gear", "gear: 3"), quiet))
        assertEquals("Увімкніть фари", Phrases.show(a("drive_no_lights"), s))
        assertNull(Phrases.show(a("drive_handbrake"), s.copy(off = setOf("drive_handbrake"))))
        assertNull(Phrases.show(a("gear", "  "), quiet))
    }

    @Test fun otherAlertsSayTheBoardsTextOnlyWhenAsked() {
        val on = s.copy(others = true)
        assertEquals("coolant_temp: 110 C", Phrases.say(a("coolant_temp", "coolant_temp: 110 C"), on))
        assertNull(Phrases.say(a("gear", "gear: 3"), Settings.Values()))     // the default: a rule is not said
        assertNull(Phrases.say(a("coolant_temp", "x"), s.copy(others = false)))
        assertNull(Phrases.say(a("coolant_temp", "  "), on))
    }
}
