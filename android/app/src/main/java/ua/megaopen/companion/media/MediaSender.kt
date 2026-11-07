package ua.megaopen.companion.media

import android.os.SystemClock
import android.util.Log
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.FlowPreview
import kotlinx.coroutines.Job
import kotlinx.coroutines.channels.Channel
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.debounce
import kotlinx.coroutines.flow.distinctUntilChanged
import kotlinx.coroutines.flow.map
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import kotlinx.coroutines.withTimeoutOrNull
import org.json.JSONObject
import ua.megaopen.companion.board.BoardConnection

/**
 * Now playing to the board (`POST /api/app/media`, then `/api/app/art` when
 * the board asks for the cover), over whichever way the board is reached:
 * on a change (300 ms quiet first), on every (re)connection, and every 20 s
 * while a session lasts (the board forgets the app's source a minute after
 * the last post). "none" once when the session is gone. A refusal (4xx/5xx,
 * a plain-text code) is kept in [status] and logged; the next change or
 * keepalive tries again.
 */
class MediaSender(private val scope: CoroutineScope, private val watch: MediaWatch,
                  private val connection: BoardConnection) {
    /** The last post, for the Media card. [atMs] is the wall clock. */
    data class Status(
        val atMs: Long,
        val sent: NowPlaying?,
        val board: String,
        val ok: Boolean,
        /** The board's error code or the failure, "" when it took it. */
        val error: String,
        val artSent: Boolean,
    )

    private val _status = MutableStateFlow<Status?>(null)
    val status: StateFlow<Status?> = _status

    private var job: Job? = null
    private var told: NowPlaying? = null
    private var toldNone = true
    private var lastPostAt = 0L
    private var resend = false
    private var staleRetried = false
    private val kicks = Channel<Unit>(Channel.CONFLATED)

    @OptIn(FlowPreview::class)
    fun start() {
        if (job != null) return
        job = scope.launch {
            launch { watch.now.debounce(DEBOUNCE_MS).collect { kicks.trySend(Unit) } }
            launch {
                connection.state.map { linkKey(it) }.distinctUntilChanged().collect {
                    if (it != null) { resend = true; kicks.trySend(Unit) }
                }
            }
            while (isActive) {
                val left = KEEPALIVE_MS - (SystemClock.elapsedRealtime() - lastPostAt)
                if (left > 0) withTimeoutOrNull(left) { kicks.receive() }
                val now = SystemClock.elapsedRealtime()
                val np = watch.now.value
                val force = resend || now - lastPostAt >= KEEPALIVE_MS
                resend = false
                when {
                    np != null -> if (force || np.differs(told, now)) post(np, now)
                    !toldNone -> post(null, now)
                    else -> lastPostAt = now      // nothing to keep alive
                }
            }
        }
    }

    fun stop() {
        job?.cancel()
        job = null
    }

    private suspend fun post(np: NowPlaying?, now: Long) {
        lastPostAt = now
        val b = connection.board.value ?: return
        var artSent = false
        val error = try {
            val r = b.postJson("/api/app/media", np?.json(now) ?: NowPlaying.none())
            if (r.code != 200) {
                errorOf(r.code, r.text())
            } else {
                told = np
                toldNone = np == null
                val cover = np?.cover
                if (cover != null && JSONObject(r.text()).optBoolean("needArt")) {
                    val ra = b.postJson("/api/app/art", JSONObject().put("h", cover.hash).put("bits", cover.hex))
                    if (ra.code == 200) {
                        artSent = true
                        staleRetried = false
                        ""
                    } else {
                        // 409 art_stale: the board's last media post named another cover: post it again.
                        // Once: a board that keeps refusing waits for the next change or keepalive.
                        if (ra.code == 409 && !staleRetried) { staleRetried = true; resend = true; kicks.trySend(Unit) }
                        errorOf(ra.code, ra.text())
                    }
                } else ""
            }
        } catch (e: CancellationException) {
            throw e
        } catch (e: Exception) {
            e.message ?: e.javaClass.simpleName
        }
        if (error.isNotEmpty()) Log.w(TAG, "${b.label}: $error")
        _status.value = Status(System.currentTimeMillis(), np, b.label, error.isEmpty(), error, artSent)
    }

    // A refusal is plain text: a code, maybe a space and an argument ("param state", "art_stale").
    private fun errorOf(code: Int, body: String): String {
        val e = body.trim().lineSequence().firstOrNull().orEmpty().take(60)
        return if (e.isNotEmpty()) "$code $e" else "HTTP $code"
    }

    private fun linkKey(s: BoardConnection.State): String? = when (s) {
        is BoardConnection.State.Usb -> "usb:" + s.name
        is BoardConnection.State.Ble -> "ble:" + s.name
        is BoardConnection.State.Wifi -> "wifi:" + s.host
        else -> null
    }

    private companion object {
        const val TAG = "MediaSender"
        const val DEBOUNCE_MS = 300L
        const val KEEPALIVE_MS = 20_000L
    }
}
