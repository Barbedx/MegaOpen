package ua.megaopen.companion.link

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test
import java.io.ByteArrayOutputStream
import java.util.zip.GZIPOutputStream

class HttpWireTest {
    private fun parse(s: ByteArray) = HttpWire.parse(s, s.size)

    @Test fun requestHasTheGateHeadersAndLength() {
        val r = String(HttpWire.request("POST", "/api/x", mapOf("Content-Type" to "application/json", "Host" to "evil"),
            "{}".toByteArray()), Charsets.ISO_8859_1)
        assertTrue(r.startsWith("POST /api/x HTTP/1.1\r\nHost: 127.0.0.1\r\nX-MegaOpen: 1\r\n"))
        assertTrue(r.contains("Content-Type: application/json\r\n"))
        assertTrue(r.contains("Content-Length: 2\r\n"))
        assertTrue(!r.contains("evil"))
        assertTrue(r.endsWith("\r\n\r\n{}"))
    }

    @Test fun contentLengthWholeAndShort() {
        val whole = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: 4\r\n\r\n{\"a\"".toByteArray()
        val r = parse(whole)!!
        assertEquals(200, r.code)
        assertEquals("{\"a\"", r.text())
        assertEquals("application/json", r.type)
        assertNull(parse(whole.copyOf(whole.size - 1)))
        assertNull(parse("HTTP/1.1 200 OK\r\nContent-Len".toByteArray()))
    }

    @Test fun chunkedWholeAndShort() {
        val s = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n4\r\nabcd\r\n3\r\nefg\r\n0\r\n\r\n".toByteArray()
        assertEquals("abcdefg", parse(s)!!.text())
        // The last chunk's own CRLF still missing: not done.
        assertNull(parse(s.copyOf(s.size - 2)))
        assertNull(parse(s.copyOf(s.size - 10)))
    }

    @Test(expected = IllegalStateException::class) fun misalignedChunkIsAnError() {
        // Bytes lost mid-stream: what used to be read as a short valid answer.
        parse("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\nzz\r\nab\r\n0\r\n\r\n".toByteArray())
    }

    @Test fun gzipIsUndone() {
        val z = ByteArrayOutputStream().also { o -> GZIPOutputStream(o).use { it.write("<html>hi</html>".toByteArray()) } }.toByteArray()
        val s = ("HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nContent-Encoding: gzip\r\nContent-Length: ${z.size}\r\n\r\n")
            .toByteArray() + z
        val r = parse(s)!!
        assertEquals("<html>hi</html>", r.text())
        assertNull(r.headers["content-encoding"])
    }

    @Test fun noBodyNoLength() {
        assertEquals(0, parse("HTTP/1.1 204 No Content\r\n\r\n".toByteArray())!!.body.size)
    }
}
