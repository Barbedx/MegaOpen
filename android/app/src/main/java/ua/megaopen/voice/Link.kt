package ua.megaopen.voice

/**
 * The USB link's frames, as the board's src/link/LinkFrame.* writes them
 * (docs/design/usb-link.md): "MO", type, flags, chan, len, payload, CRC-16.
 */
object Link {
    const val HEAD = 8
    const val MAX_PAYLOAD = 1024
    const val FIN = 0x01

    const val HELLO = 0x01
    const val PING = 0x02
    const val HTTP_REQ = 0x10
    const val HTTP_CLOSE = 0x11
    const val HELLO_ACK = 0x81
    const val PONG = 0x82
    const val HTTP_RESP = 0x90
    const val HTTP_FAIL = 0x91
    const val EVENT = 0xA0

    /** CRC-16/CCITT-FALSE, the board's crc16(). */
    fun crc16(d: ByteArray, from: Int, n: Int): Int {
        var crc = 0xFFFF
        for (i in from until from + n) {
            crc = crc xor ((d[i].toInt() and 0xFF) shl 8)
            for (b in 0 until 8) crc = if (crc and 0x8000 != 0) ((crc shl 1) xor 0x1021) and 0xFFFF else (crc shl 1) and 0xFFFF
        }
        return crc
    }

    fun encode(type: Int, flags: Int, chan: Int, p: ByteArray = ByteArray(0), off: Int = 0, len: Int = p.size): ByteArray {
        require(len <= MAX_PAYLOAD)
        val f = ByteArray(HEAD + len + 2)
        f[0] = 0x4D; f[1] = 0x4F
        f[2] = type.toByte(); f[3] = flags.toByte()
        f[4] = chan.toByte(); f[5] = (chan shr 8).toByte()
        f[6] = len.toByte(); f[7] = (len shr 8).toByte()
        System.arraycopy(p, off, f, HEAD, len)
        val c = crc16(f, 2, HEAD - 2 + len)
        f[HEAD + len] = c.toByte(); f[HEAD + len + 1] = (c shr 8).toByte()
        return f
    }

    class Frame(val type: Int, val flags: Int, val chan: Int, val payload: ByteArray)

    /** Bytes in, frames out; a bad magic, length or CRC drops a byte and resyncs. */
    class Parser {
        private var buf = ByteArray(4 * (HEAD + MAX_PAYLOAD + 2))
        private var n = 0
        var bad = 0
            private set

        fun feed(d: ByteArray, len: Int, out: (Frame) -> Unit) {
            if (n + len > buf.size) buf = buf.copyOf(maxOf(buf.size * 2, n + len))
            System.arraycopy(d, 0, buf, n, len)
            n += len
            var at = 0
            while (true) {
                while (at < n && buf[at] != 0x4D.toByte()) { at++; bad++ }
                if (n - at < 2) break
                if (buf[at + 1] != 0x4F.toByte()) { at++; bad++; continue }
                if (n - at < HEAD) break
                val plen = (buf[at + 6].toInt() and 0xFF) or ((buf[at + 7].toInt() and 0xFF) shl 8)
                if (plen > MAX_PAYLOAD) { at++; bad++; continue }
                if (n - at < HEAD + plen + 2) break
                val want = (buf[at + HEAD + plen].toInt() and 0xFF) or ((buf[at + HEAD + plen + 1].toInt() and 0xFF) shl 8)
                if (crc16(buf, at + 2, HEAD - 2 + plen) != want) { at++; bad++; continue }
                out(Frame(buf[at + 2].toInt() and 0xFF, buf[at + 3].toInt() and 0xFF,
                    (buf[at + 4].toInt() and 0xFF) or ((buf[at + 5].toInt() and 0xFF) shl 8),
                    buf.copyOfRange(at + HEAD, at + HEAD + plen)))
                at += HEAD + plen + 2
            }
            if (at > 0) {
                System.arraycopy(buf, at, buf, 0, n - at)
                n -= at
            }
        }

        fun reset() { n = 0 }
    }
}
