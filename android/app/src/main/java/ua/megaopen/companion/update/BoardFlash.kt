package ua.megaopen.companion.update

import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.delay
import ua.megaopen.companion.board.Board
import ua.megaopen.companion.board.WifiBoard
import java.io.ByteArrayOutputStream
import java.io.IOException
import java.security.MessageDigest

/**
 * An image handed to the board the way its own page does it (web/js/10-api.js
 * `upload`): /ota/start, then /ota/upload as one multipart file. The board
 * reboots into it and confirms it itself after a minute of healthy work
 * (src/app/OtaTrial.*).
 */
object BoardFlash {
    private val GATE = mapOf("X-MegaOpen" to "1")   // src/net/RequestGate.h: every change needs it

    fun md5(data: ByteArray): String = MessageDigest.getInstance("MD5").digest(data).joinToString("") { "%02x".format(it) }

    fun multipart(boundary: String, fileName: String, data: ByteArray): ByteArray {
        val out = ByteArrayOutputStream(data.size + 256)
        out.write(("--$boundary\r\nContent-Disposition: form-data; name=\"file\"; filename=\"$fileName\"\r\n" +
            "Content-Type: application/octet-stream\r\n\r\n").toByteArray(Charsets.ISO_8859_1))
        out.write(data)
        out.write("\r\n--$boundary--\r\n".toByteArray(Charsets.ISO_8859_1))
        return out.toByteArray()
    }

    /**
     * The way to send 1.8 MB: the board's own address when it is on a network
     * this device reaches too (plain HTTP, fast, nothing of the board's
     * Bluetooth memory), else the way the app already has.
     */
    suspend fun direct(board: Board): Board {
        if (board.via == Board.Via.Wifi) return board
        return try {
            val st = board.json("/api/status")
            val ip = st.optString("ip")
            if (st.optString("mode") != "sta" || ip.isEmpty()) return board
            val w = WifiBoard(ip)
            val fw = w.probe(3_000) ?: return board
            if (w.name != st.optString("apName")) return board
            w.fw = fw
            w
        } catch (e: CancellationException) {
            throw e
        } catch (e: Exception) {
            board
        }
    }

    /**
     * Send [data] and wait for the board to come back. Returns the version it
     * then reports: [Releases.Firmware.version] when it took the image, ""
     * when it took the upload and has not answered since.
     */
    suspend fun flash(board: Board, f: Releases.Firmware, data: ByteArray): String {
        val before = try {
            board.json("/api/status").optString("fw")
        } catch (e: CancellationException) {
            throw e
        } catch (e: Exception) {
            ""
        }
        // Three tries: on the bench one /ota/start in a quiet hour got no answer
        // at all (docs/BACKLOG.md, 2026-11-26) and the next one did. Asking
        // twice is harmless: it only begins the update again.
        var start: ua.megaopen.companion.link.HttpWire.Response? = null
        var why: Exception? = null
        for (i in 1..3) {
            try {
                start = board.request("GET", "/ota/start?mode=fr&hash=${md5(data)}", GATE)
                break
            } catch (e: CancellationException) {
                throw e
            } catch (e: Exception) {
                why = e
                delay(2_000)
            }
        }
        if (start == null) throw why ?: IOException("no answer to /ota/start")
        if (start.code != 200) throw IOException("HTTP ${start.code} ${start.text().take(80)}")
        val boundary = "----MegaOpen" + java.lang.Long.toHexString(System.nanoTime())
        // The board reboots right after its answer; on a link the answer may not make it.
        var lost: Exception? = null
        val r = try {
            board.request("POST", "/ota/upload", GATE + ("Content-Type" to "multipart/form-data; boundary=$boundary"),
                multipart(boundary, f.name, data), 180_000)
        } catch (e: CancellationException) {
            throw e
        } catch (e: Exception) {
            lost = e
            null
        }
        if (r != null && r.code !in 200..299) throw IOException("HTTP ${r.code} ${r.text().take(80)}")
        // Without the answer only a version that changed proves the upload.
        val proves = lost == null || before != f.version
        delay(8_000)
        repeat(30) {
            try {
                if (proves && board.json("/api/status").optString("fw") == f.version) return f.version
            } catch (e: CancellationException) {
                throw e
            } catch (e: Exception) {
                // rebooting, or the link is coming back
            }
            delay(3_000)
        }
        lost?.let { throw it }
        return ""
    }
}
