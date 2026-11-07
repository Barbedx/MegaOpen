package ua.megaopen.companion.link

import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import kotlinx.coroutines.flow.MutableSharedFlow
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.runBlocking
import kotlinx.coroutines.withTimeout
import org.json.JSONObject
import org.junit.After
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test
import java.io.ByteArrayOutputStream
import java.util.concurrent.Executors
import kotlin.random.Random

/**
 * The session over a fake board: the board's bytes come back cut at random
 * places (as BLE notifications or USB reads may cut them), on another thread.
 */
class LinkSessionTest {
    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.Default)

    @After fun done() = scope.cancel()

    private class FakeBoard(private val answerHello: Boolean = true) : Pipe {
        private val out = Executors.newSingleThreadExecutor()
        private val rnd = Random(7)
        private val parser = Frames.Parser()
        private var rx: ((ByteArray, Int) -> Unit)? = null
        private val req = HashMap<Int, ByteArrayOutputStream>()
        val got = ArrayList<Int>()
        @Volatile var closed = false

        override fun start(rx: (ByteArray, Int) -> Unit, gone: (String) -> Unit) { this.rx = rx }

        override fun write(b: ByteArray): Boolean {
            if (closed) return false
            // The app's bytes, cut too.
            var at = 0
            while (at < b.size) {
                val n = minOf(b.size - at, 1 + rnd.nextInt(20))
                parser.feed(b.copyOfRange(at, at + n), n) { onFrame(it) }
                at += n
            }
            return true
        }

        override fun close() { closed = true; out.shutdown() }

        private fun onFrame(f: Frames.Frame) {
            synchronized(got) { got += f.type }
            when (f.type) {
                Frames.HELLO -> if (answerHello) {
                    reply(Frames.HELLO_ACK, 0, JSONObject().put("proto", 1).put("fw", "1.9.9").put("name", "MegaOpen-TEST")
                        .put("chans", 2).put("via", "ble").toString().toByteArray())
                    reply(Frames.EVENT, 0, """{"t":"alert","seq":1,"sig":"fuel"}""".toByteArray())
                }
                Frames.HTTP_REQ -> {
                    req.getOrPut(f.chan) { ByteArrayOutputStream() }.write(f.payload)
                    if (f.fin) {
                        val text = req.remove(f.chan)!!.toString(Charsets.ISO_8859_1.name())
                        val path = text.substringAfter(' ').substringBefore(' ')
                        val body = (if (text.startsWith("POST")) "posted " + text.substringAfter("\r\n\r\n") else "x".repeat(3000) + path)
                            .toByteArray()
                        val resp = "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: ${body.size}\r\n\r\n".toByteArray() + body
                        var at = 0
                        while (at < resp.size) {
                            val n = minOf(Frames.MAX_PAYLOAD, resp.size - at)
                            reply(Frames.HTTP_RESP, f.chan, resp.copyOfRange(at, at + n), fin = false)   // keep-alive: the server stays open
                            at += n
                        }
                    }
                }
            }
        }

        // One frame back, in pieces of 1..60 bytes, as notifications would come.
        private fun reply(type: Int, chan: Int, p: ByteArray, fin: Boolean = true) {
            val frame = Frames.encode(type, if (fin) Frames.FIN else 0, chan, p)
            out.execute {
                var at = 0
                while (at < frame.size) {
                    val n = minOf(frame.size - at, 1 + rnd.nextInt(60))
                    rx?.invoke(frame.copyOfRange(at, at + n), n)
                    at += n
                }
            }
        }
    }

    private fun session(board: Pipe, ended: (String) -> Unit = {}) =
        LinkSession(scope, board, MutableStateFlow(LinkStats()), events, ended)

    private val events = MutableSharedFlow<JSONObject>(replay = 4)

    @Test fun helloRequestsAndEvents() = runBlocking {
        val board = FakeBoard()
        val s = session(board)
        val hello = s.open(3_000)
        assertNotNull(hello)
        assertEquals("MegaOpen-TEST", hello!!.getString("name"))
        assertTrue(s.up)
        assertEquals("fuel", withTimeout(3_000) { events.first() }.getString("sig"))

        val r = s.request("GET", "/api/status")
        assertEquals(200, r.code)
        assertTrue(r.text().endsWith("/api/status"))
        val p = s.request("POST", "/api/app/media", mapOf("Content-Type" to "application/json"), """{"state":"none"}""".toByteArray())
        assertEquals("""posted {"state":"none"}""", p.text())
        s.close("test")
        assertTrue(board.closed)
    }

    @Test fun pingsEverySecond() = runBlocking {
        val board = FakeBoard()
        val s = session(board)
        assertNotNull(s.open(3_000))
        Thread.sleep(2_300)
        assertTrue(synchronized(board.got) { board.got.count { it == Frames.PING } } >= 2)
        s.close("test")
    }

    @Test fun noHelloEndsTheSession() = runBlocking {
        var why = ""
        val board = FakeBoard(answerHello = false)
        val s = session(board) { why = it }
        assertNull(s.open(300))
        assertEquals("no HELLO from the board", why)
        assertTrue(board.closed)
    }
}
