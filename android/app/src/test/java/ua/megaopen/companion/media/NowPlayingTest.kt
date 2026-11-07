package ua.megaopen.companion.media

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class NowPlayingTest {
    private val np = NowPlaying("Spotify", "com.spotify.music", "Song", "Artist", "Album", playing = true,
        positionMs = 10_000, updatedAt = 1_000, speed = 1f, durationMs = 200_000, cover = null)

    @Test fun positionMovesWhilePlaying() {
        assertEquals(15_000, np.positionAt(6_000))
        assertEquals(20_000, np.copy(speed = 2f).positionAt(6_000))
    }

    @Test fun positionStandsWhilePaused() {
        assertEquals(10_000, np.copy(playing = false).positionAt(60_000))
    }

    @Test fun positionStaysWithinTheTrack() {
        assertEquals(200_000, np.positionAt(10_000_000))
        assertEquals(0, NowPlaying.position(-1, 0, 1f, true, 0, 5_000))
    }

    @Test fun jsonAsTheBoardReadsIt() {
        val bits = ByteArray(Cover.BYTES)
        val j = np.copy(cover = Cover(bits)).json(3_000)
        assertEquals("Spotify", j.getString("app"))
        assertEquals("com.spotify.music", j.getString("pkg"))
        assertEquals("playing", j.getString("state"))
        assertEquals(12_000, j.getLong("pos"))
        assertEquals(200_000, j.getLong("dur"))
        assertEquals(1.0, j.getDouble("rate"), 0.0)
        assertEquals("ebf26a52", j.getString("art"))
        val p = np.copy(playing = false).json(3_000)
        assertEquals("paused", p.getString("state"))
        assertEquals(0.0, p.getDouble("rate"), 0.0)
        assertEquals("", np.json(0).getString("art"))
        assertEquals("none", NowPlaying.none().getString("state"))
    }

    @Test fun onlyNewsIsWorthSending() {
        assertTrue(np.differs(null, 2_000))
        // The same track, its position reported again where it would be anyway.
        assertFalse(np.copy(positionMs = 11_000, updatedAt = 2_000).differs(np, 2_000))
        // A seek.
        assertTrue(np.copy(positionMs = 90_000, updatedAt = 2_000).differs(np, 2_000))
        assertTrue(np.copy(title = "Other").differs(np, 2_000))
        assertTrue(np.copy(playing = false).differs(np, 1_000))
    }
}
