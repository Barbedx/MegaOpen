package ua.megaopen.companion.media

import android.content.ComponentName
import android.content.Context
import android.content.pm.PackageManager
import android.graphics.Bitmap
import android.media.MediaMetadata
import android.media.session.MediaController
import android.media.session.MediaSessionManager
import android.media.session.PlaybackState
import android.os.Build
import android.os.Handler
import android.os.Looper
import android.service.notification.NotificationListenerService
import android.util.Log
import androidx.core.app.NotificationManagerCompat
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import ua.megaopen.companion.App

/**
 * Whatever plays on the head unit (Spotify, YouTube Music, a radio app, the
 * head unit's own player if it has a media session): Android's active media
 * sessions, which an app may read once the user gave it «Доступ до
 * сповіщень» ([MediaListener]). [now] is the session that plays, else the
 * one that played last; null when there is none.
 */
class MediaWatch(private val ctx: Context, private val scope: CoroutineScope) {
    private val _access = MutableStateFlow(false)
    /** The user gave the notification access the sessions need. */
    val access: StateFlow<Boolean> = _access
    private val _now = MutableStateFlow<NowPlaying?>(null)
    val now: StateFlow<NowPlaying?> = _now

    private val msm = ctx.getSystemService(MediaSessionManager::class.java)
    private val component = ComponentName(ctx, MediaListener::class.java)
    private val main = Handler(Looper.getMainLooper())
    private var listening = false
    private var controllers: List<MediaController> = emptyList()
    private val callbacks = HashMap<MediaController, MediaController.Callback>()
    private var lastPlaying = ""
    private var job: Job? = null
    private var coverOf: Pair<Bitmap, Cover>? = null

    private val sessions = MediaSessionManager.OnActiveSessionsChangedListener { list -> onSessions(list.orEmpty()) }

    /** Read the access again (the user may have just changed it) and follow the sessions if granted. On the main thread. */
    fun refresh() {
        val granted = NotificationManagerCompat.getEnabledListenerPackages(ctx).contains(ctx.packageName)
        _access.value = granted
        if (granted && !listening) {
            try {
                msm.addOnActiveSessionsChangedListener(sessions, component, main)
                listening = true
                onSessions(msm.getActiveSessions(component))
            } catch (e: SecurityException) {
                Log.w(TAG, "no access to media sessions: ${e.message}")
            }
        } else if (!granted && listening) {
            try { msm.removeOnActiveSessionsChangedListener(sessions) } catch (_: Exception) {}
            listening = false
            onSessions(emptyList())
        }
    }

    private fun onSessions(list: List<MediaController>) {
        for ((c, cb) in callbacks) c.unregisterCallback(cb)
        callbacks.clear()
        controllers = list
        for (c in list) {
            val cb = object : MediaController.Callback() {
                override fun onPlaybackStateChanged(state: PlaybackState?) = update()
                override fun onMetadataChanged(metadata: MediaMetadata?) = update()
                override fun onSessionDestroyed() = update()
            }
            c.registerCallback(cb, main)
            callbacks[c] = cb
        }
        update()
    }

    private fun playing(c: MediaController) = c.playbackState?.state in PLAYING_STATES

    // The session that plays; else the one we last saw playing; else the
    // system's first (its list is the most recently active first).
    private fun pick(): MediaController? {
        val live = controllers.filter { it.metadata != null || playing(it) }
        live.firstOrNull { playing(it) }?.let { lastPlaying = it.packageName; return it }
        return live.firstOrNull { it.packageName == lastPlaying } ?: live.firstOrNull()
    }

    private fun update() {
        val c = pick()
        job?.cancel()
        if (c == null) { _now.value = null; return }
        val md = c.metadata
        val st = c.playbackState
        val art = md?.let { it.getBitmap(MediaMetadata.METADATA_KEY_ART) ?: it.getBitmap(MediaMetadata.METADATA_KEY_ALBUM_ART)
            ?: it.getBitmap(MediaMetadata.METADATA_KEY_DISPLAY_ICON) }
        val base = NowPlaying(
            app = label(c.packageName), pkg = c.packageName,
            title = md?.text(MediaMetadata.METADATA_KEY_TITLE) ?: md?.text(MediaMetadata.METADATA_KEY_DISPLAY_TITLE).orEmpty(),
            artist = md?.text(MediaMetadata.METADATA_KEY_ARTIST) ?: md?.text(MediaMetadata.METADATA_KEY_ALBUM_ARTIST)
                ?: md?.text(MediaMetadata.METADATA_KEY_DISPLAY_SUBTITLE).orEmpty(),
            album = md?.text(MediaMetadata.METADATA_KEY_ALBUM).orEmpty(),
            playing = playing(c),
            positionMs = st?.position ?: 0, updatedAt = st?.lastPositionUpdateTime ?: 0,
            speed = st?.playbackSpeed?.takeIf { it > 0f } ?: 1f,
            durationMs = md?.getLong(MediaMetadata.METADATA_KEY_DURATION)?.takeIf { it > 0 } ?: 0,
            cover = null,
        )
        val known = coverOf?.takeIf { art != null && it.first === art }?.second
        if (art == null || known != null) { _now.value = base.copy(cover = known); return }
        job = scope.launch {
            val cover = withContext(Dispatchers.Default) { coverFrom(art) }
            if (cover != null) coverOf = art to cover
            _now.value = base.copy(cover = cover)
        }
    }

    private fun MediaMetadata.text(key: String): String? = getString(key)?.trim()?.takeIf { it.isNotEmpty() }

    private fun label(pkg: String): String = try {
        val pm = ctx.packageManager
        pm.getApplicationLabel(pm.getApplicationInfo(pkg, 0)).toString()
    } catch (e: PackageManager.NameNotFoundException) {
        pkg
    }

    private fun coverFrom(b: Bitmap): Cover? = try {
        val bm = if (Build.VERSION.SDK_INT >= 26 && b.config == Bitmap.Config.HARDWARE) b.copy(Bitmap.Config.ARGB_8888, false) else b
        val px = IntArray(bm.width * bm.height)
        bm.getPixels(px, 0, bm.width, 0, 0, bm.width, bm.height)
        Cover.of(px, bm.width, bm.height)
    } catch (e: Exception) {
        Log.w(TAG, "cover: ${e.message}")
        null
    }

    private companion object {
        const val TAG = "MediaWatch"
        val PLAYING_STATES = setOf(PlaybackState.STATE_PLAYING, PlaybackState.STATE_BUFFERING, PlaybackState.STATE_CONNECTING,
            PlaybackState.STATE_FAST_FORWARDING, PlaybackState.STATE_REWINDING,
            PlaybackState.STATE_SKIPPING_TO_NEXT, PlaybackState.STATE_SKIPPING_TO_PREVIOUS, PlaybackState.STATE_SKIPPING_TO_QUEUE_ITEM)
    }
}

/**
 * Only there for the access: Android lets an app read the media sessions
 * while it holds a notification listener the user switched on. It reads no
 * notification; it tells [MediaWatch] when the access comes and goes.
 */
class MediaListener : NotificationListenerService() {
    override fun onListenerConnected() { (application as App).graph.media.refresh() }
    override fun onListenerDisconnected() { (application as App).graph.media.refresh() }
}
