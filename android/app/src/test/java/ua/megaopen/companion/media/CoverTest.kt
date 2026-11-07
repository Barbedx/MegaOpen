package ua.megaopen.companion.media

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class CoverTest {
    private fun solid(w: Int, h: Int, argb: Int) = IntArray(w * h) { argb }
    private fun grey(v: Int) = (0xFF shl 24) or (v shl 16) or (v shl 8) or v

    @Test fun allWhiteIsAllLit() {
        val c = Cover.of(solid(300, 300, grey(255)), 300, 300)
        assertTrue(c.bits.all { it == 0xFF.toByte() })
        assertEquals("46ec1585", c.hash)
    }

    @Test fun allBlackIsDark() {
        val c = Cover.of(solid(64, 40, grey(0)), 64, 40)
        assertTrue(c.bits.all { it == 0.toByte() })
        assertEquals("ebf26a52", c.hash)
    }

    @Test fun transparentIsDark() {
        val c = Cover.of(solid(48, 48, 0x00FFFFFF), 48, 48)
        assertEquals(0.0, c.density, 0.0)
    }

    @Test fun gradientIsAboutHalf() {
        val w = 200
        val px = IntArray(w * w) { i -> grey((i % w) * 255 / (w - 1)) }
        val c = Cover.of(px, w, w)
        assertEquals(0.5, c.density, 0.05)
        // Darker on the left, brighter on the right.
        val left = (0 until 48).sumOf { y -> (0 until 12).count { x -> c.lit(x, y) } }
        val right = (0 until 48).sumOf { y -> (36 until 48).count { x -> c.lit(x, y) } }
        assertTrue("left $left right $right", left < right / 4)
    }

    @Test fun checkerScalesToQuarters() {
        val w = grey(255)
        val b = grey(0)
        val c = Cover.of(intArrayOf(w, b, b, w), 2, 2)
        for (y in 0 until 48) for (x in 0 until 48) {
            assertEquals("($x,$y)", (x < 24) == (y < 24), c.lit(x, y))
        }
    }

    @Test fun centreCropsAWidePicture() {
        // 3:1, white in the middle third only: the crop is all white.
        val px = IntArray(90 * 30) { i -> if (i % 90 in 30 until 60) grey(255) else grey(0) }
        assertEquals(1.0, Cover.of(px, 90, 30).density, 0.0)
    }

    @Test fun autoContrastStretchesADullPicture() {
        // Only 100..140: stretched, the bright half lights and the dark one does not.
        val px = IntArray(96 * 96) { i -> grey(if (i % 96 < 48) 100 else 140) }
        val c = Cover.of(px, 96, 96)
        assertFalse(c.lit(5, 20))
        assertTrue(c.lit(40, 20))
    }

    @Test fun bitLayoutAndHex() {
        val bits = ByteArray(Cover.BYTES)
        bits[0] = 0x80.toByte()          // top-left pixel
        bits[Cover.BYTES - 1] = 0x01     // bottom-right pixel
        val c = Cover(bits)
        assertTrue(c.lit(0, 0))
        assertFalse(c.lit(1, 0))
        assertTrue(c.lit(47, 47))
        assertEquals(576, c.hex.length)
        assertTrue(c.hex.startsWith("80000000"))
        assertTrue(c.hex.endsWith("0001"))
        val only = ByteArray(Cover.BYTES).also { it[0] = 0x80.toByte() }
        assertEquals("aefe330d", Cover(only).hash)
    }
}
