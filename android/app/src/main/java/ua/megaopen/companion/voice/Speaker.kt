package ua.megaopen.companion.voice

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
 * (AUDIOFOCUS_GAIN_TRANSIENT_MAY_DUCK) and comes back after. The voice is the
 * device's own engine; a missing language is reported, not guessed at.
 */
class Speaker(ctx: Context) : TextToSpeech.OnInitListener {
    enum class State { Starting, Ready, NoEngine }

    private val am = ctx.getSystemService(Context.AUDIO_SERVICE) as AudioManager
    private val attrs = AudioAttributes.Builder()
        .setUsage(AudioAttributes.USAGE_ASSISTANCE_NAVIGATION_GUIDANCE)
        .setContentType(AudioAttributes.CONTENT_TYPE_SPEECH)
        .build()
    private val focus: AudioFocusRequest? =
        if (Build.VERSION.SDK_INT >= 26)
            AudioFocusRequest.Builder(AudioManager.AUDIOFOCUS_GAIN_TRANSIENT_MAY_DUCK).setAudioAttributes(attrs).build()
        else null
    private val talking = AtomicInteger(0)
    private val pending = ArrayList<Triple<String, String, Int>>()
    private var applied: String? = null
    private var seq = 0
    private val tts = TextToSpeech(ctx.applicationContext, this)

    @Volatile var state = State.Starting
        private set
    /** null until a phrase was said; false when the language has no voice on this device. */
    @Volatile var langOk: Boolean? = null
        private set

    override fun onInit(status: Int) {
        if (status != TextToSpeech.SUCCESS) {
            state = State.NoEngine
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
            state = State.Ready
            for ((t, l, v) in pending) speakNow(t, l, v)
            pending.clear()
        }
    }

    /** lang: "uk", "en" or "" (the system's); volume 0..100. Any thread. */
    fun say(text: String, lang: String, volume: Int) {
        if (text.isBlank()) return
        synchronized(pending) {
            when (state) {
                State.Starting -> if (pending.size < 8) pending += Triple(text, lang, volume)
                State.Ready -> speakNow(text, lang, volume)
                State.NoEngine -> {}
            }
        }
    }

    private fun speakNow(text: String, lang: String, volume: Int) {
        if (applied != lang) {
            applied = lang
            val loc = when (lang) {
                "uk" -> Locale("uk", "UA")
                "en" -> Locale.US
                else -> Locale.getDefault()
            }
            val r = tts.setLanguage(loc)
            langOk = r != TextToSpeech.LANG_MISSING_DATA && r != TextToSpeech.LANG_NOT_SUPPORTED
        }
        val p = Bundle()
        p.putFloat(TextToSpeech.Engine.KEY_PARAM_VOLUME, volume / 100f)
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
