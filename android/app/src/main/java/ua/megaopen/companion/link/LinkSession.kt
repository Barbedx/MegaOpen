package ua.megaopen.companion.link

import android.os.SystemClock
import kotlinx.coroutines.CompletableDeferred
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.TimeoutCancellationException
import kotlinx.coroutines.channels.Channel
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableSharedFlow
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.SharedFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import kotlinx.coroutines.withTimeout
import kotlinx.coroutines.withTimeoutOrNull
import org.json.JSONObject
import ua.megaopen.companion.BuildConfig
import java.io.ByteArrayOutputStream
import java.io.IOException
import java.net.URL
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicBoolean
import java.util.concurrent.locks.ReentrantLock
import javax.net.ssl.HttpsURLConnection
import kotlin.concurrent.withLock as locked

/**
 * A byte pipe to the board: the USB vendor interface ([UsbLink]) or the BLE
 * characteristics ([BleLink]). Both carry the same Frames; bytes may arrive
 * cut anywhere, the session's parser puts them together.
 */
interface Pipe {
    /** Start handing the board's bytes to [rx]; [gone] once when the pipe dies under us. */
    fun start(rx: (ByteArray, Int) -> Unit, gone: (String) -> Unit)

    /** All of [b], blocking; false when it could not be written. One writer at a time (the session's lock). */
    fun write(b: ByteArray): Boolean

    /** Idempotent. */
    fun close()
}

/** What a link did, for Diagnostics; kept across its sessions. */
data class LinkStats(
    val rxFrames: Long = 0, val txFrames: Long = 0, val bad: Long = 0,
    val requests: Long = 0, val failures: Long = 0, val netRequests: Long = 0,
    val lastError: String = "",
)

/**
 * One transport to the board and its current [LinkSession]: what [UsbLink] and
 * [BleLink] have in common. The rest of the app sees requests, events and stats.
 */
abstract class Link {
    protected val _stats = MutableStateFlow(LinkStats())
    val stats: StateFlow<LinkStats> = _stats
    protected val _events = MutableSharedFlow<JSONObject>(extraBufferCapacity = 32)
    /** The board's EVENT frames: `{"t":"alert",...}`. */
    val events: SharedFlow<JSONObject> = _events

    @Volatile protected var session: LinkSession? = null

    val up: Boolean get() = session?.up == true
    /** The board's HELLO while up. */
    val hello: JSONObject? get() = session?.takeIf { it.up }?.hello

    suspend fun request(method: String, path: String, headers: Map<String, String> = emptyMap(),
                        body: ByteArray? = null, timeoutMs: Long = 20_000): HttpWire.Response =
        (session?.takeIf { it.up } ?: throw IOException("no link")).request(method, path, headers, body, timeoutMs)
}

/**
 * The link's conversation over one [Pipe] (docs/design/usb-link.md): HELLO,
 * a PING a second, HTTP to the board's web server on keep-alive channels,
 * the board's events, and the board's own GitHub requests carried out here
 * ("net"). One session per connection: a new pipe is a new session.
 *
 * Writes are serialised by one lock (each frame whole); requests suspend on a
 * deferred per channel; frames come in on the pipe's own thread.
 */
class LinkSession(
    private val scope: CoroutineScope,
    private val pipe: Pipe,
    private val stats: MutableStateFlow<LinkStats>,
    private val events: MutableSharedFlow<JSONObject>,
    /** Called once, with why, when the session ends (either side). */
    private val ended: (String) -> Unit,
) {
    private val writeLock = Any()
    private val done = AtomicBoolean(false)
    private val jobs = ArrayList<Job>()
    private val parser = Frames.Parser()
    @Volatile private var helloWait: CompletableDeferred<JSONObject>? = null

    /** The board's HELLO, once it answered. */
    @Volatile var hello: JSONObject? = null
        private set
    val closed: Boolean get() = done.get()
    val up: Boolean get() = hello != null && !done.get()

    private class Chan(val n: Int) {
        val buf = ByteArrayOutputStream()
        var waiter: CompletableDeferred<HttpWire.Response>? = null
        /** The board holds a socket for this channel (a request since the last close). */
        var open = false
        var lastUse = 0L
    }
    @Volatile private var chans: Array<Chan> = emptyArray()
    private val free = Channel<Chan>(Channel.UNLIMITED)

    /**
     * Start the pipe, say HELLO, wait for the board's. The board's HELLO, or
     * null when it did not come (the session is closed then, [ended] called).
     */
    suspend fun open(helloTimeoutMs: Long): JSONObject? {
        val h = CompletableDeferred<JSONObject>()
        helloWait = h
        pipe.start(::feed, ::close)
        // "net": this app carries the board's own GitHub requests (updates, log push).
        val me = JSONObject().put("app", "companion " + BuildConfig.VERSION_NAME).put("net", 1)
            .put("time", System.currentTimeMillis())
        if (!withContext(Dispatchers.IO) { send(Frames.HELLO, Frames.FIN, 0, me.toString().toByteArray()) }) {
            close("write")
            return null
        }
        val j = withTimeoutOrNull(helloTimeoutMs) { h.await() } ?: run { close("no HELLO from the board"); return null }
        val n = j.optInt("chans", 1).coerceIn(1, 8)
        chans = Array(n) { Chan(it) }
        chans.forEach { free.trySend(it) }
        synchronized(jobs) {
            if (done.get()) return null
            jobs += scope.launch { pingLoop() }
            jobs += scope.launch { idleLoop() }
        }
        hello = j
        return j
    }

    /** End the session: the pipe closes, everything waiting fails. Idempotent. */
    fun close(why: String) {
        if (!done.compareAndSet(false, true)) return
        synchronized(jobs) { jobs.forEach { it.cancel() }; jobs.clear() }
        pipe.close()
        free.close()
        for (ch in chans) synchronized(ch) { ch.waiter?.completeExceptionally(IOException(why)); ch.waiter = null }
        helloWait?.cancel()
        netJob?.wake { cancel = true }
        ended(why)
    }

    // ---- frames ---------------------------------------------------------------

    private fun send(type: Int, flags: Int, chan: Int, p: ByteArray = ByteArray(0), off: Int = 0, len: Int = p.size): Boolean {
        if (done.get()) return false
        val f = Frames.encode(type, flags, chan, p, off, len)
        val ok = synchronized(writeLock) { pipe.write(f) }
        if (ok) stats.update { it.copy(txFrames = it.txFrames + 1) }
        return ok
    }

    private fun feed(b: ByteArray, n: Int) {
        parser.feed(b, n) { onFrame(it) }
        if (parser.bad != stats.value.bad) stats.update { it.copy(bad = parser.bad) }
    }

    private suspend fun pingLoop() {
        while (scope.isActive) {
            delay(1000)
            if (!withContext(Dispatchers.IO) { send(Frames.PING, Frames.FIN, 0) }) {
                close("write")
                return
            }
        }
    }

    // Our own idle close at 10 s, before the board's at 15 s: its timeout can
    // then never meet a request we just sent on that channel.
    private suspend fun idleLoop() {
        while (scope.isActive) {
            delay(2000)
            val now = SystemClock.elapsedRealtime()
            for (ch in chans) {
                val close = synchronized(ch) {
                    (ch.open && ch.waiter == null && now - ch.lastUse > IDLE_MS).also { if (it) ch.open = false }
                }
                if (close) withContext(Dispatchers.IO) { send(Frames.HTTP_CLOSE, Frames.FIN, ch.n) }
            }
        }
    }

    private fun onFrame(f: Frames.Frame) {
        stats.update { it.copy(rxFrames = it.rxFrames + 1) }
        when (f.type) {
            Frames.HELLO_ACK -> {
                val j = try { JSONObject(String(f.payload)) } catch (e: Exception) { JSONObject() }
                helloWait?.complete(j)
            }
            Frames.HTTP_RESP -> {
                val ch = chans.getOrNull(f.chan) ?: return
                synchronized(ch) {
                    val w = ch.waiter ?: return       // a close of an idle socket
                    ch.buf.write(f.payload)
                    try {
                        val r = HttpWire.parse(ch.buf.toByteArray(), ch.buf.size())
                        if (r != null) { w.complete(r); ch.waiter = null }
                        else if (f.fin && ch.buf.size() > 0) { w.completeExceptionally(IOException("closed")); ch.waiter = null; ch.open = false }
                    } catch (e: IllegalStateException) {
                        w.completeExceptionally(IOException("bad response"))
                        ch.waiter = null
                    }
                }
            }
            Frames.HTTP_FAIL -> chans.getOrNull(f.chan)?.let { ch ->
                synchronized(ch) {
                    ch.open = false
                    ch.waiter?.completeExceptionally(IOException(String(f.payload)))
                    ch.waiter = null
                }
            }
            Frames.EVENT -> {
                val j = try { JSONObject(String(f.payload)) } catch (e: Exception) { return }
                events.tryEmit(j)
            }
            Frames.NET_REQ -> {
                val head = try { JSONObject(String(f.payload)) } catch (e: Exception) { return }
                netBody.reset()
                netHead = head
                netId = f.chan
                if (f.fin) netStart(f.chan, head, null)
            }
            Frames.NET_BODY -> {
                val h = netHead ?: return
                if (f.chan != netId) return
                netBody.write(f.payload)
                if (f.fin) netStart(f.chan, h, netBody.toByteArray())
            }
            Frames.NET_ACK -> {
                val j = netJob ?: return
                if (j.id != f.chan || f.payload.size < 4) return
                val p = f.payload
                val v = (p[0].toLong() and 0xFF) or ((p[1].toLong() and 0xFF) shl 8) or
                    ((p[2].toLong() and 0xFF) shl 16) or ((p[3].toLong() and 0xFF) shl 24)
                j.wake { acked = v }
            }
            Frames.NET_CANCEL -> {
                val j = netJob ?: return
                if (j.id == f.chan) j.wake { cancel = true }
            }
            else -> {}   // PONG; or a board newer than us: ignored
        }
    }

    // ---- HTTP to the board ----------------------------------------------------

    /**
     * One request to the board's web server over the link. A GET whose
     * channel failed under it (a cut frame, a socket the board just closed) is
     * sent once more; a change never is.
     */
    suspend fun request(method: String, path: String, headers: Map<String, String> = emptyMap(),
                        body: ByteArray? = null, timeoutMs: Long = 20_000): HttpWire.Response {
        try {
            return once(method, path, headers, body, timeoutMs)
        } catch (e: IOException) {
            if (method != "GET" || !up) throw e
            return once(method, path, headers, body, timeoutMs)
        }
    }

    private suspend fun once(method: String, path: String, headers: Map<String, String>,
                             body: ByteArray?, timeoutMs: Long): HttpWire.Response {
        if (!up) throw IOException("no link")
        val ch = withTimeoutOrNull(timeoutMs) { free.receiveCatching() }?.let { it.getOrNull() ?: throw IOException("closed") }
            ?: throw IOException("link busy")
        val d = CompletableDeferred<HttpWire.Response>()
        try {
            synchronized(ch) {
                ch.buf.reset()
                ch.waiter = d
                ch.open = true
                ch.lastUse = SystemClock.elapsedRealtime()
            }
            stats.update { it.copy(requests = it.requests + 1) }
            val raw = HttpWire.request(method, path, headers, body)
            val sent = withContext(Dispatchers.IO) {
                var at = 0
                var ok = true
                while (ok && at < raw.size) {
                    val n = minOf(Frames.MAX_PAYLOAD, raw.size - at)
                    ok = send(Frames.HTTP_REQ, if (at + n >= raw.size) Frames.FIN else 0, ch.n, raw, at, n)
                    at += n
                }
                ok
            }
            if (!sent) throw IOException("write")
            return withTimeout(timeoutMs) { d.await() }
        } catch (e: TimeoutCancellationException) {
            dropChan(ch)
            countFail("timeout")
            throw IOException("timeout")
        } catch (e: IOException) {
            dropChan(ch)
            countFail(e.message ?: "io")
            throw e
        } finally {
            synchronized(ch) {
                ch.waiter = null
                ch.lastUse = SystemClock.elapsedRealtime()
            }
            free.trySend(ch)
        }
    }

    // A channel that failed: the board drops its socket, the next request opens a new one.
    private suspend fun dropChan(ch: Chan) {
        synchronized(ch) { ch.open = false }
        withContext(Dispatchers.IO) { send(Frames.HTTP_CLOSE, Frames.FIN, ch.n) }
    }

    private fun countFail(why: String) = stats.update { it.copy(failures = it.failures + 1, lastError = why) }

    // ---- the board's internet through this app --------------------------------
    // The board composes its GitHub requests (src/net/GhOta.cpp, LogPush.cpp)
    // as over WiFi; here they are carried out on the head unit's network. Only
    // the hosts in BuildConfig.NET_HOSTS (the board's GhRelease::trustedUrl).
    // The body goes out no faster than the board's NET_ACK credit allows,
    // whatever the pipe's speed (USB 100 kB/s, BLE a tenth of that).

    private class NetJob(val id: Int) {
        private val lock = ReentrantLock()
        private val moved = lock.newCondition()
        var acked = 0L
        var cancel = false

        /** Change the credit or cancel, and wake the sender. */
        fun wake(f: NetJob.() -> Unit) = lock.locked { f(); moved.signalAll() }

        /** Wait until `n` more bytes fit the board's credit; false when cancelled. */
        fun room(sent: Long, n: Int): Boolean = lock.locked {
            val until = SystemClock.elapsedRealtime() + 30_000
            while (!cancel && sent + n - acked > Frames.NET_WINDOW) {
                val left = until - SystemClock.elapsedRealtime()
                if (left <= 0) throw IOException("stalled")
                moved.await(left, TimeUnit.MILLISECONDS)
            }
            !cancel
        }

        val cancelled: Boolean get() = lock.locked { cancel }
    }
    @Volatile private var netJob: NetJob? = null
    private var netHead: JSONObject? = null
    private var netId = -1
    private val netBody = ByteArrayOutputStream()

    private fun netStart(id: Int, head: JSONObject, body: ByteArray?) {
        netHead = null
        netJob?.wake { cancel = true }
        val job = NetJob(id)
        netJob = job
        stats.update { it.copy(netRequests = it.netRequests + 1) }
        scope.launch(Dispatchers.IO) { netRun(job, head, body) }
    }

    private fun netFail(job: NetJob, why: String) {
        send(Frames.NET_FAIL, Frames.FIN, job.id, why.take(36).toByteArray())
    }

    private fun netRun(job: NetJob, head: JSONObject, body: ByteArray?) {
        var c: HttpsURLConnection? = null
        try {
            val url = URL(head.getString("url"))
            if (url.protocol != "https" || url.host !in BuildConfig.NET_HOSTS) return netFail(job, "host")
            c = url.openConnection() as HttpsURLConnection
            c.requestMethod = head.optString("m", "GET")
            c.connectTimeout = 15_000
            c.readTimeout = 30_000
            c.instanceFollowRedirects = true
            // The length the board checks (an image against its release) is the one on the wire.
            c.setRequestProperty("Accept-Encoding", "identity")
            head.optJSONObject("h")?.let { h -> for (k in h.keys()) c.setRequestProperty(k, h.getString(k)) }
            if (body != null && body.isNotEmpty()) {
                c.doOutput = true
                c.setFixedLengthStreamingMode(body.size)
                c.outputStream.use { it.write(body) }
            }
            val code = c.responseCode
            val final = c.url
            if (final.host !in BuildConfig.NET_HOSTS) return netFail(job, "host")
            val len = c.getHeaderField("Content-Length")?.toLongOrNull() ?: -1L
            send(Frames.NET_HEAD, Frames.FIN, job.id,
                JSONObject().put("status", code).put("len", len).put("url", final.toString()).toString().toByteArray())
            val input = if (code >= 400) c.errorStream else c.inputStream
            var sent = 0L
            val buf = ByteArray(Frames.MAX_PAYLOAD)
            input?.use { inp ->
                while (true) {
                    val n = inp.read(buf)
                    if (n < 0) break
                    if (!job.room(sent, n)) return
                    if (!send(Frames.NET_DATA, 0, job.id, buf, 0, n)) return
                    sent += n
                }
            }
            send(Frames.NET_DATA, Frames.FIN, job.id)
        } catch (e: Exception) {
            if (!job.cancelled) netFail(job, (e.javaClass.simpleName + " " + (e.message ?: "")).trim())
        } finally {
            c?.disconnect()
        }
    }

    private companion object {
        const val IDLE_MS = 10_000L
    }
}
