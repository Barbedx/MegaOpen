package ua.megaopen.voice

import android.content.Context
import android.media.AudioAttributes
import android.media.AudioFocusRequest
import android.media.AudioManager
import android.os.Build
import android.os.Bundle
import android.speech.tts.TextToSpeech
import android.speech.tts.UtteranceProgressListener
import java.util.Locale
import java.util.concurrent.atomic.AtomicInteger

/**
 * Text to speech over the music: the music ducks while a phrase is said
 * (AUDIOFOCUS_GAIN_TRANSIENT_MAY_DUCK) and comes back after.
 */
class Speaker(ctx: Context, private val prefs: Prefs) : TextToSpeech.OnInitListener {
    private val am = ctx.getSystemService(Context.AUDIO_SERVICE) as AudioManager
    private val attrs = AudioAttributes.Builder()
        .setUsage(AudioAttributes.USAGE_ASSISTANCE_NAVIGATION_GUIDANCE)
        .setContentType(AudioAttributes.CONTENT_TYPE_SPEECH)
        .build()
    private val focus: AudioFocusRequest? =
        if (Build.VERSION.SDK_INT >= 26)
            AudioFocusRequest.Builder(AudioManager.AUDIOFOCUS_GAIN_TRANSIENT_MAY_DUCK)
                .setAudioAttributes(attrs).build()
        else null
    private val pending = ArrayList<String>()
    private val talking = AtomicInteger(0)
    private var ready = false
    private var applied: String? = null
    private var seq = 0
    private val tts = TextToSpeech(ctx.applicationContext, this)

    /** null until the first phrase; false when the voice for the language is missing. */
    @Volatile var langOk: Boolean? = null
        private set
    @Volatile var failed = false
        private set

    override fun onInit(status: Int) {
        if (status != TextToSpeech.SUCCESS) {
            failed = true
            return
        }
        tts.setAudioAttributes(attrs)
        tts.setOnUtteranceProgressListener(object : UtteranceProgressListener() {
            override fun onStart(utteranceId: String?) {}
            override fun onDone(utteranceId: String?) = finished()
            @Deprecated("Deprecated in Java")
            override fun onError(utteranceId: String?) = finished()
        })
        synchronized(pending) {
            ready = true
            for (t in pending) speakNow(t)
            pending.clear()
        }
    }

    fun say(text: String) {
        if (text.isBlank()) return
        synchronized(pending) {
            if (!ready) {
                if (pending.size < 8) pending += text
                return
            }
            speakNow(text)
        }
    }

    private fun speakNow(text: String) {
        if (applied != prefs.lang) {
            applied = prefs.lang
            val loc = when (prefs.lang) {
                "uk" -> Locale("uk", "UA")
                "en" -> Locale.US
                else -> Locale.getDefault()
            }
            val r = tts.setLanguage(loc)
            langOk = r != TextToSpeech.LANG_MISSING_DATA && r != TextToSpeech.LANG_NOT_SUPPORTED
        }
        val p = Bundle()
        p.putFloat(TextToSpeech.Engine.KEY_PARAM_VOLUME, prefs.volume / 100f)
        if (talking.getAndIncrement() == 0) grab()
        if (tts.speak(text, TextToSpeech.QUEUE_ADD, p, "mo${seq++}") != TextToSpeech.SUCCESS) finished()
    }

    private fun finished() {
        if (talking.decrementAndGet() <= 0) {
            talking.set(0)
            drop()
        }
    }

    @Suppress("DEPRECATION")
    private fun grab() {
        if (Build.VERSION.SDK_INT >= 26 && focus != null) am.requestAudioFocus(focus)
        else am.requestAudioFocus(null, AudioManager.STREAM_MUSIC, AudioManager.AUDIOFOCUS_GAIN_TRANSIENT_MAY_DUCK)
    }

    @Suppress("DEPRECATION")
    private fun drop() {
        if (Build.VERSION.SDK_INT >= 26 && focus != null) am.abandonAudioFocusRequest(focus)
        else am.abandonAudioFocus(null)
    }

    fun shutdown() {
        tts.stop()
        tts.shutdown()
        drop()
    }
}
