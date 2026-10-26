package ua.megaopen.companion.link

import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

/** The board's test/test_link cases: the two ends must agree byte for byte. */
class FramesTest {
    private val got = ArrayList<Frames.Frame>()
    private fun sink(p: Frames.Parser, b: ByteArray) = p.feed(b, b.size) { got += it }

    @Test fun crcCheckValue() {
        // CRC-16/CCITT-FALSE's check value.
        val s = "123456789".toByteArray()
        assertEquals(0x29B1, Frames.crc16(s, 0, s.size))
    }

    @Test fun encodeLayout() {
        val f = Frames.encode(Frames.HTTP_REQ, Frames.FIN, 0x0102, "GET".toByteArray())
        assertEquals(Frames.HEAD + 3 + 2, f.size)
        assertArrayEquals(byteArrayOf(0x4D, 0x4F, 0x10, 0x01, 0x02, 0x01, 0x03, 0x00, 'G'.code.toByte(),
            'E'.code.toByte(), 'T'.code.toByte()), f.copyOf(11))
        val c = Frames.crc16(f, 2, Frames.HEAD - 2 + 3)
        assertEquals(c and 0xFF, f[11].toInt() and 0xFF)
        assertEquals(c shr 8, f[12].toInt() and 0xFF)
    }

    @Test(expected = IllegalArgumentException::class) fun tooLongIsRefused() {
        Frames.encode(Frames.EVENT, 0, 0, ByteArray(Frames.MAX_PAYLOAD + 1))
    }

    @Test fun roundTripByteByByte() {
        val p = Frames.Parser()
        val s = Frames.encode(Frames.HELLO, 0, 0, "{\"app\":\"x\"}".toByteArray()) + Frames.encode(Frames.PING, 0, 7)
        for (b in s) p.feed(byteArrayOf(b), 1) { got += it }
        assertEquals(2, got.size)
        assertEquals("{\"app\":\"x\"}", String(got[0].payload))
        assertEquals(7, got[1].chan)
        assertEquals(0L, p.bad)
    }

    @Test fun fullPayloadInOneFeed() {
        val p = Frames.Parser()
        sink(p, Frames.encode(Frames.HTTP_RESP, Frames.FIN, 1, ByteArray(Frames.MAX_PAYLOAD) { 'z'.code.toByte() }) +
            Frames.encode(Frames.HTTP_RESP, 0, 2, "tail".toByteArray()))
        assertEquals(2, got.size)
        assertEquals(Frames.MAX_PAYLOAD, got[0].payload.size)
        assertTrue(got[0].fin)
        assertEquals("tail", String(got[1].payload))
    }

    @Test fun garbageAndBadCrcResync() {
        val p = Frames.Parser()
        val bad = Frames.encode(Frames.EVENT, 0, 0, "lost".toByteArray())
        bad[Frames.HEAD] = (bad[Frames.HEAD].toInt() xor 0xFF).toByte()
        sink(p, byteArrayOf(0, 0x4D, 0x11, 0x4D) + bad + Frames.encode(Frames.EVENT, 0, 0, "kept".toByteArray()))
        assertEquals(1, got.size)
        assertEquals("kept", String(got[0].payload))
        assertTrue(p.bad > 0)
    }

    @Test fun cutFrameDoesNotSwallowTheNext() {
        val p = Frames.Parser()
        val a = Frames.encode(Frames.HTTP_RESP, 0, 1, ByteArray(300) { 'a'.code.toByte() })
        sink(p, a.copyOf(100) + Frames.encode(Frames.PONG, 0, 0, "ok".toByteArray()))
        assertEquals(1, got.size)
        assertEquals(Frames.PONG, got[0].type)
        sink(p, Frames.encode(Frames.EVENT, 0, 0, "next".toByteArray()))
        assertEquals("next", String(got[1].payload))
    }

    @Test fun longFrameInPiecesIsNotCut() {
        val p = Frames.Parser()
        val a = Frames.encode(Frames.HTTP_RESP, 0, 1, ByteArray(1000) { 'q'.code.toByte() })
        var i = 0
        while (i < a.size) {
            val k = minOf(64, a.size - i)
            p.feed(a.copyOfRange(i, i + k), k) { got += it }
            i += k
        }
        assertEquals(1, got.size)
        assertEquals(1000, got[0].payload.size)
        assertEquals(0L, p.bad)
    }

    @Test fun oversizeLengthIsGarbage() {
        val p = Frames.Parser()
        sink(p, byteArrayOf(0x4D, 0x4F, 0x90.toByte(), 0, 1, 0, 0xFF.toByte(), 0xFF.toByte()) + Frames.encode(Frames.PONG, 0, 0))
        assertEquals(1, got.size)
    }
}
