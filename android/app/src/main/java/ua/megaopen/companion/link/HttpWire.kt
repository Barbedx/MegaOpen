package ua.megaopen.companion.link

import java.io.ByteArrayOutputStream
import java.util.zip.GZIPInputStream

/**
 * HTTP/1.1 as the link carries it: raw bytes to the board's web server and
 * back. esp_http_server keeps the connection open whatever `Connection` says,
 * so a response ends where its framing says (Content-Length, or chunked's
 * last chunk). Pure: unit-tested in HttpWireTest.
 */
object HttpWire {
    class Response(val code: Int, val headers: Map<String, String>, val body: ByteArray) {
        val type: String get() = headers["content-type"] ?: "application/octet-stream"
        fun text(): String = String(body, Charsets.UTF_8)
    }

    fun request(method: String, path: String, headers: Map<String, String>, body: ByteArray?): ByteArray {
        val sb = StringBuilder()
        sb.append(method).append(' ').append(path).append(" HTTP/1.1\r\n")
        // An IP literal passes the board's Host check (src/net/RequestGate.h);
        // X-MegaOpen lets a change through, as the page sends it.
        sb.append("Host: 127.0.0.1\r\nX-MegaOpen: 1\r\n")
        for ((k, v) in headers) {
            when (k.lowercase()) {
                "host", "content-length", "x-megaopen", "connection" -> continue
            }
            sb.append(k).append(": ").append(v).append("\r\n")
        }
        if (body != null || method == "POST" || method == "PUT") sb.append("Content-Length: ").append(body?.size ?: 0).append("\r\n")
        sb.append("\r\n")
        val head = sb.toString().toByteArray(Charsets.ISO_8859_1)
        return if (body == null) head else head + body
    }

    /** The response, when b[0..n) holds all of it; null while it does not. */
    fun parse(b: ByteArray, n: Int): Response? {
        val he = indexOf(b, n, CRLFCRLF, 0)
        if (he < 0) return null
        val lines = String(b, 0, he, Charsets.ISO_8859_1).split("\r\n")
        val code = lines.firstOrNull()?.split(' ')?.getOrNull(1)?.toIntOrNull() ?: 0
        val h = HashMap<String, String>()
        for (l in lines.drop(1)) {
            val i = l.indexOf(':')
            if (i > 0) h[l.substring(0, i).trim().lowercase()] = l.substring(i + 1).trim()
        }
        var at = he + 4
        val body: ByteArray
        if (h["transfer-encoding"]?.lowercase() == "chunked") {
            val out = ByteArrayOutputStream()
            while (true) {
                val eol = indexOf(b, n, CRLF, at)
                if (eol < 0) return null
                val size = String(b, at, eol - at, Charsets.ISO_8859_1).substringBefore(';').trim().toIntOrNull(16)
                    ?: throw IllegalStateException("bad chunk size")
                at = eol + 2
                if (size == 0) {
                    if (n - at < 2) return null
                    break
                }
                if (n - at < size + 2) return null
                out.write(b, at, size)
                at += size + 2
            }
            body = out.toByteArray()
        } else {
            val len = h["content-length"]?.toIntOrNull() ?: 0
            if (n - at < len) return null
            body = b.copyOfRange(at, at + len)
        }
        // The page is stored gzipped; a WebView does not undo that for an
        // intercepted response, so it is undone here.
        return if (h["content-encoding"]?.lowercase() == "gzip") {
            val plain = GZIPInputStream(body.inputStream()).use { it.readBytes() }
            Response(code, h - "content-encoding" - "content-length", plain)
        } else Response(code, h, body)
    }

    private val CRLF = byteArrayOf(13, 10)
    private val CRLFCRLF = byteArrayOf(13, 10, 13, 10)

    private fun indexOf(b: ByteArray, n: Int, pat: ByteArray, from: Int): Int {
        var i = from
        while (i <= n - pat.size) {
            var k = 0
            while (k < pat.size && b[i + k] == pat[k]) k++
            if (k == pat.size) return i
            i++
        }
        return -1
    }
}
