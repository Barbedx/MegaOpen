package ua.megaopen.companion.media

import org.json.JSONObject

/**
 * What plays on the head unit now, as one media session tells it. The
 * position is the session's: [positionMs] at [updatedAt] (elapsedRealtime),
 * moving at [speed] while [playing]. Pure: unit-tested in NowPlayingTest.
 */
data class NowPlaying(
    /** The player's name as the launcher shows it ("Spotify"). */
    val app: String,
    val pkg: String,
    val title: String,
    val artist: String,
    val album: String,
    val playing: Boolean,
    val positionMs: Long,
    val updatedAt: Long,
    val speed: Float,
    /** 0 when the session does not say. */
    val durationMs: Long,
    val cover: Cover?,
) {
    /** The position at [now] (elapsedRealtime), within 0..duration. */
    fun positionAt(now: Long): Long = position(positionMs, updatedAt, speed, playing, durationMs, now)

    /**
     * Worth telling the board, against what it was told last: any field, or
     * the position off by more than [SEEK_MS] from where [last] would be now
     * (a seek). A player that reports its position every second is not news.
     */
    fun differs(last: NowPlaying?, now: Long): Boolean {
        if (last == null) return true
        if (copy(positionMs = 0, updatedAt = 0) != last.copy(positionMs = 0, updatedAt = 0)) return true
        return kotlin.math.abs(positionAt(now) - last.positionAt(now)) > SEEK_MS
    }

    /** /api/app/media's body. */
    fun json(now: Long): JSONObject = JSONObject()
        .put("app", app).put("pkg", pkg)
        .put("title", title).put("artist", artist).put("album", album)
        .put("state", if (playing) "playing" else "paused")
        .put("pos", positionAt(now)).put("dur", durationMs)
        .put("rate", if (playing) speed.toDouble() else 0.0)
        .put("art", cover?.hash.orEmpty())

    companion object {
        const val SEEK_MS = 2_000L

        /** /api/app/media's body when no session is left. */
        fun none(): JSONObject = JSONObject()
            .put("app", "").put("pkg", "").put("title", "").put("artist", "").put("album", "")
            .put("state", "none").put("pos", 0).put("dur", 0).put("rate", 0.0).put("art", "")

        fun position(pos: Long, updatedAt: Long, speed: Float, playing: Boolean, dur: Long, now: Long): Long {
            if (pos < 0) return 0
            val p = if (playing && updatedAt > 0 && now > updatedAt) pos + ((now - updatedAt) * speed).toLong() else pos
            return if (dur > 0) p.coerceIn(0, dur) else maxOf(0, p)
        }
    }
}
