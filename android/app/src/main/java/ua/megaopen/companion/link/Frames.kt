package ua.megaopen.companion.link

/**
 * The USB link's frames, byte for byte as the board's src/link/LinkFrame.*
 * (docs/design/usb-link.md): "MO", type, flags, chan, len, payload, CRC-16.
 * Pure: unit-tested in FramesTest, the same cases as test/test_link.
 */
object Frames {
    const val HEAD = 8
    const val MAX_PAYLOAD = 1024
    const val FIN = 0x01

    // The high bit of the type is the direction: set, board to app.
    const val HELLO = 0x01
    const val PING = 0x02
    const val HTTP_REQ = 0x10
    const val HTTP_CLOSE = 0x11
    const val NET_HEAD = 0x40
    const val NET_DATA = 0x41
    const val NET_FAIL = 0x42
    const val HELLO_ACK = 0x81
    const val PONG = 0x82
    const val HTTP_RESP = 0x90
    const val HTTP_FAIL = 0x91
    const val EVENT = 0xA0
    const val NET_REQ = 0xC0
    const val NET_BODY = 0xC1
    const val NET_ACK = 0xC2
    const val NET_CANCEL = 0xC3

    /** Bytes the app may send ahead of the board's NET_ACK (UsbLink::NET_WINDOW). */
    const val NET_WINDOW = 16384

    /** CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF), the board's crc16(). */
    fun crc16(d: ByteArray, from: Int, n: Int): Int {
        var crc = 0xFFFF
        for (i in from until from + n) {
            crc = crc xor ((d[i].toInt() and 0xFF) shl 8)
            repeat(8) { crc = if (crc and 0x8000 != 0) ((crc shl 1) xor 0x1021) and 0xFFFF else (crc shl 1) and 0xFFFF }
        }
        return crc
    }

    fun encode(type: Int, flags: Int, chan: Int, p: ByteArray = ByteArray(0), off: Int = 0, len: Int = p.size): ByteArray {
        require(len in 0..MAX_PAYLOAD) { "payload $len" }
        val f = ByteArray(HEAD + len + 2)
        f[0] = 0x4D
        f[1] = 0x4F
        f[2] = type.toByte()
        f[3] = flags.toByte()
        f[4] = chan.toByte()
        f[5] = (chan shr 8).toByte()
        f[6] = len.toByte()
        f[7] = (len shr 8).toByte()
        System.arraycopy(p, off, f, HEAD, len)
        val c = crc16(f, 2, HEAD - 2 + len)
        f[HEAD + len] = c.toByte()
        f[HEAD + len + 1] = (c shr 8).toByte()
        return f
    }

    class Frame(val type: Int, val flags: Int, val chan: Int, val payload: ByteArray) {
        val fin: Boolean get() = flags and FIN != 0
    }

    /**
     * Bytes in, frames out. A bad magic, length or CRC drops one byte and
     * looks for the magic again; a frame still short of its length is dropped
     * as soon as a whole CRC-good frame follows it (one cut by a writer that
     * gave up would otherwise swallow the frames after it).
     */
    class Parser {
        private var buf = ByteArray(4 * (HEAD + MAX_PAYLOAD + 2))
        private var n = 0
        var bad = 0L
            private set

        fun feed(d: ByteArray, len: Int, out: (Frame) -> Unit) {
            if (n + len > buf.size) buf = buf.copyOf(maxOf(buf.size * 2, n + len))
            System.arraycopy(d, 0, buf, n, len)
            n += len
            var at = 0
            while (true) {
                while (at < n && buf[at] != M0) { at++; bad++ }
                if (n - at < 2) break
                if (buf[at + 1] != M1) { at++; bad++; continue }
                if (n - at < HEAD) break
                val plen = u8(at + 6) or (u8(at + 7) shl 8)
                if (plen > MAX_PAYLOAD) { at++; bad++; continue }
                if (n - at < HEAD + plen + 2) {
                    val nx = wholeAfter(at + 1)
                    if (nx > 0) { bad += nx - at; at = nx; continue }
                    break
                }
                if (!crcOk(at, plen)) { at++; bad++; continue }
                out(Frame(u8(at + 2), u8(at + 3), u8(at + 4) or (u8(at + 5) shl 8),
                    buf.copyOfRange(at + HEAD, at + HEAD + plen)))
                at += HEAD + plen + 2
            }
            if (at > 0) {
                System.arraycopy(buf, at, buf, 0, n - at)
                n -= at
            }
        }

        fun reset() { n = 0 }

        private fun u8(i: Int) = buf[i].toInt() and 0xFF

        private fun crcOk(at: Int, plen: Int): Boolean =
            crc16(buf, at + 2, HEAD - 2 + plen) == (u8(at + HEAD + plen) or (u8(at + HEAD + plen + 1) shl 8))

        private fun wholeAfter(from: Int): Int {
            var i = from
            while (i + HEAD + 2 <= n) {
                if (buf[i] == M0 && buf[i + 1] == M1) {
                    val plen = u8(i + 6) or (u8(i + 7) shl 8)
                    if (plen <= MAX_PAYLOAD && i + HEAD + plen + 2 <= n && crcOk(i, plen)) return i
                }
                i++
            }
            return 0
        }
    }

    private const val M0: Byte = 0x4D
    private const val M1: Byte = 0x4F
}
