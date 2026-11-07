package ua.megaopen.companion.media

import java.util.zip.CRC32

/**
 * A cover as the panel's 48×48 picture pane shows it: one bit a pixel, row
 * major, 6 bytes a row, the most significant bit first (bit 7 of byte 0 is
 * the top-left pixel), 1 = lit. [hash] (CRC-32 of the 288 bytes, 8 lowercase
 * hex) is how the board asks for a cover it does not have.
 */
class Cover(val bits: ByteArray) {
    init { require(bits.size == BYTES) { "cover ${bits.size} bytes" } }

    val hash: String by lazy {
        val c = CRC32()
        c.update(bits)
        "%08x".format(c.value)
    }

    /** The 576 lowercase hex characters of /api/app/art's `bits`. */
    val hex: String by lazy {
        val sb = StringBuilder(BYTES * 2)
        for (b in bits) sb.append(HEX[(b.toInt() shr 4) and 15]).append(HEX[b.toInt() and 15])
        sb.toString()
    }

    fun lit(x: Int, y: Int): Boolean = (bits[y * ROW + x / 8].toInt() shr (7 - x % 8)) and 1 == 1

    /** Lit pixels over all, 0..1. */
    val density: Double get() = bits.sumOf { Integer.bitCount(it.toInt() and 0xFF) } / (SIZE * SIZE).toDouble()

    override fun equals(other: Any?) = other is Cover && other.bits.contentEquals(bits)
    override fun hashCode() = bits.contentHashCode()

    companion object {
        const val SIZE = 48
        const val ROW = SIZE / 8
        const val BYTES = ROW * SIZE
        private const val HEX = "0123456789abcdef"

        /**
         * A picture (ARGB, row major, [w]×[h]) to the pane: centre-crop to a
         * square, area-average to 48×48, grey (luma, transparency as black),
         * the 2nd–98th percentile stretched to full range, Floyd–Steinberg to
         * one bit. Pure: unit-tested in CoverTest.
         */
        fun of(argb: IntArray, w: Int, h: Int): Cover {
            require(w > 0 && h > 0 && argb.size >= w * h) { "picture ${w}x$h" }
            val side = minOf(w, h)
            val x0 = (w - side) / 2
            val y0 = (h - side) / 2
            val grey = scale(argb, w, x0, y0, side)
            stretch(grey)
            return Cover(dither(grey))
        }

        // Each output pixel the average of the source area it covers (fractional
        // edges weighted): a filter down, blocks up.
        private fun scale(argb: IntArray, w: Int, x0: Int, y0: Int, side: Int): FloatArray {
            val out = FloatArray(SIZE * SIZE)
            val step = side.toDouble() / SIZE
            for (oy in 0 until SIZE) {
                val sy0 = oy * step
                val sy1 = sy0 + step
                for (ox in 0 until SIZE) {
                    val sx0 = ox * step
                    val sx1 = sx0 + step
                    var sum = 0.0
                    var area = 0.0
                    var y = sy0.toInt()
                    while (y < sy1 && y < side) {
                        val wy = minOf(sy1, y + 1.0) - maxOf(sy0, y.toDouble())
                        var x = sx0.toInt()
                        while (x < sx1 && x < side) {
                            val wx = minOf(sx1, x + 1.0) - maxOf(sx0, x.toDouble())
                            val a = wx * wy
                            if (a > 0) {
                                sum += luma(argb[(y0 + y) * w + x0 + x]) * a
                                area += a
                            }
                            x++
                        }
                        y++
                    }
                    out[oy * SIZE + ox] = if (area > 0) (sum / area).toFloat() else 0f
                }
            }
            return out
        }

        /** 0..255, Rec. 601, premultiplied by alpha (transparent = black). */
        fun luma(p: Int): Double {
            val a = (p ushr 24) and 255
            val r = (p shr 16) and 255
            val g = (p shr 8) and 255
            val b = p and 255
            return (0.299 * r + 0.587 * g + 0.114 * b) * a / 255.0
        }

        // Auto-contrast: the 2nd percentile to 0, the 98th to 255. A flat picture
        // (one shade, white or black) is left as it is.
        private fun stretch(g: FloatArray) {
            val sorted = g.copyOf().also { it.sort() }
            val lo = sorted[(sorted.size - 1) * 2 / 100]
            val hi = sorted[(sorted.size - 1) * 98 / 100]
            if (hi - lo < MIN_RANGE) return
            val k = 255f / (hi - lo)
            for (i in g.indices) g[i] = ((g[i] - lo) * k).coerceIn(0f, 255f)
        }

        private fun dither(g: FloatArray): ByteArray {
            val bits = ByteArray(BYTES)
            for (y in 0 until SIZE) for (x in 0 until SIZE) {
                val i = y * SIZE + x
                val old = g[i]
                val on = old >= 128f
                if (on) bits[y * ROW + x / 8] = (bits[y * ROW + x / 8].toInt() or (0x80 ushr (x % 8))).toByte()
                val err = old - if (on) 255f else 0f
                if (x + 1 < SIZE) g[i + 1] += err * 7 / 16
                if (y + 1 < SIZE) {
                    if (x > 0) g[i + SIZE - 1] += err * 3 / 16
                    g[i + SIZE] += err * 5 / 16
                    if (x + 1 < SIZE) g[i + SIZE + 1] += err * 1 / 16
                }
            }
            return bits
        }

        /** Below this spread (of 255) a picture is one shade: no stretch. */
        private const val MIN_RANGE = 8f
    }
}
