package ua.megaopen.companion.link

import android.app.PendingIntent
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.hardware.usb.UsbConstants
import android.hardware.usb.UsbDevice
import android.hardware.usb.UsbDeviceConnection
import android.hardware.usb.UsbEndpoint
import android.hardware.usb.UsbInterface
import android.hardware.usb.UsbManager
import android.os.Build
import android.os.SystemClock
import androidx.core.content.ContextCompat
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
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import kotlinx.coroutines.withContext
import kotlinx.coroutines.withTimeout
import kotlinx.coroutines.withTimeoutOrNull
import org.json.JSONObject
import ua.megaopen.companion.BuildConfig
import java.io.ByteArrayOutputStream
import java.io.IOException
import java.net.URL
import java.util.concurrent.TimeUnit
import java.util.concurrent.locks.ReentrantLock
import kotlin.concurrent.withLock as locked
import javax.net.ssl.HttpsURLConnection

/**
 * The board on the head unit's USB (docs/design/usb-link.md): the vendor
 * interface of its composite device, Frames on it, HTTP piped to the board's
 * web server on two keep-alive channels, the alerts pushed as events, and the
 * board's own GitHub requests carried out here ("net").
 *
 * One board at a time. The reader is one blocking loop on Dispatchers.IO; the
 * writes are serialised by one lock; requests suspend on a deferred per channel.
 */
class UsbLink(private val ctx: Context, private val scope: CoroutineScope) {
    sealed interface State {
        data object Unplugged : State
        data object NoPermission : State
        data object Connecting : State
        data class Up(val hello: JSONObject) : State
        data class Failed(val why: String) : State
    }

    data class Stats(
        val rxFrames: Long = 0, val txFrames: Long = 0, val bad: Long = 0,
        val requests: Long = 0, val failures: Long = 0, val netRequests: Long = 0,
        val lastError: String = "",
    )

    private val _state = MutableStateFlow<State>(State.Unplugged)
    val state: StateFlow<State> = _state
    private val _stats = MutableStateFlow(Stats())
    val stats: StateFlow<Stats> = _stats
    private val _events = MutableSharedFlow<JSONObject>(extraBufferCapacity = 32)
    /** The board's EVENT frames: `{"t":"alert",...}`. */
    val events: SharedFlow<JSONObject> = _events

    val up: Boolean get() = _state.value is State.Up

    private val um = ctx.getSystemService(Context.USB_SERVICE) as UsbManager
    private val openLock = Mutex()
    private val writeLock = Any()
    @Volatile private var conn: UsbDeviceConnection? = null
    private var itf: UsbInterface? = null
    private var epOut: UsbEndpoint? = null
    private var jobs = ArrayList<Job>()
    @Volatile private var hello: CompletableDeferred<JSONObject>? = null
    @Volatile private var asked: String? = null

    private class Chan(val n: Int) {
        val buf = ByteArrayOutputStream()
        var waiter: CompletableDeferred<HttpWire.Response>? = null
        /** The board holds a socket for this channel (a request since the last close). */
        var open = false
        var lastUse = 0L
    }
    @Volatile private var chans: Array<Chan> = emptyArray()
    private var free = Channel<Chan>(Channel.UNLIMITED)

    // ---- the device -----------------------------------------------------------

    /** The board, if it is plugged in: our VID/PID and a vendor interface. */
    fun device(): UsbDevice? =
        um.deviceList.values.firstOrNull { it.vendorId == BuildConfig.USB_VID && it.productId == BuildConfig.USB_PID && vendorItf(it) != null }

    private fun vendorItf(d: UsbDevice): UsbInterface? =
        (0 until d.interfaceCount).map { d.getInterface(it) }
            .firstOrNull { it.interfaceClass == UsbConstants.USB_CLASS_VENDOR_SPEC && it.endpointCount >= 2 }

    /**
     * Connect if the board is plugged in. Asks Android's permission the first
     * time for each plug (the dialog; «Always» there remembers it) and waits
     * for the answer. False: no board, no permission, or no HELLO.
     */
    suspend fun connect(): Boolean = openLock.withLock {
        if (conn != null) return@withLock up
        val d = device() ?: run { _state.value = State.Unplugged; return@withLock false }
        if (!um.hasPermission(d)) {
            if (asked == d.deviceName) { _state.value = State.NoPermission; return@withLock false }
            asked = d.deviceName
            if (!askPermission(d)) { _state.value = State.NoPermission; return@withLock false }
        }
        _state.value = State.Connecting
        val f = vendorItf(d) ?: return@withLock fail("no vendor interface")
        val c = um.openDevice(d) ?: return@withLock fail("open")
        if (!c.claimInterface(f, true)) { c.close(); return@withLock fail("claim") }
        val bulk = (0 until f.endpointCount).map { f.getEndpoint(it) }.filter { it.type == UsbConstants.USB_ENDPOINT_XFER_BULK }
        val i = bulk.firstOrNull { it.direction == UsbConstants.USB_DIR_IN }
        val o = bulk.firstOrNull { it.direction == UsbConstants.USB_DIR_OUT }
        if (i == null || o == null) { c.close(); return@withLock fail("endpoints") }
        conn = c; itf = f; epOut = o
        val h = CompletableDeferred<JSONObject>()
        hello = h
        jobs += scope.launch(Dispatchers.IO) { readLoop(c, i) }
        // "net": this app carries the board's own GitHub requests (updates, log push).
        val me = JSONObject().put("app", "companion " + BuildConfig.VERSION_NAME).put("net", 1)
        if (!send(Frames.HELLO, Frames.FIN, 0, me.toString().toByteArray())) { closeNow("write"); return@withLock false }
        val j = withTimeoutOrNull(3000) { h.await() } ?: run { closeNow("no HELLO from the board"); return@withLock false }
        val n = j.optInt("chans", 1).coerceIn(1, 8)
        chans = Array(n) { Chan(it) }
        free = Channel(Channel.UNLIMITED)
        chans.forEach { free.trySend(it) }
        jobs += scope.launch { pingLoop() }
        jobs += scope.launch { idleLoop() }
        _state.value = State.Up(j)
        true
    }

    private fun fail(why: String): Boolean {
        _state.value = State.Failed(why)
        _stats.update { it.copy(lastError = why) }
        return false
    }

    private suspend fun askPermission(d: UsbDevice): Boolean {
        val answer = CompletableDeferred<Boolean>()
        val r = object : BroadcastReceiver() {
            override fun onReceive(context: Context, intent: Intent) {
                answer.complete(intent.getBooleanExtra(UsbManager.EXTRA_PERMISSION_GRANTED, false))
            }
        }
        ContextCompat.registerReceiver(ctx, r, IntentFilter(ACTION_PERMISSION), ContextCompat.RECEIVER_NOT_EXPORTED)
        try {
            val flags = if (Build.VERSION.SDK_INT >= 31) PendingIntent.FLAG_MUTABLE else 0
            val pi = PendingIntent.getBroadcast(ctx, 0, Intent(ACTION_PERMISSION).setPackage(ctx.packageName), flags)
            um.requestPermission(d, pi)
            return withTimeoutOrNull(60_000) { answer.await() } ?: false
        } finally {
            ctx.unregisterReceiver(r)
        }
    }

    /** Unplugged (Android said so) or given up: everything waiting fails. */
    fun close(why: String = "unplugged") {
        if (why == "unplugged") asked = null
        closeNow(why)
    }

    private fun closeNow(why: String) {
        val c = conn ?: return
        conn = null
        jobs.forEach { it.cancel() }
        jobs = ArrayList()
        try { itf?.let { c.releaseInterface(it) } } catch (_: Exception) {}
        c.close()
        for (ch in chans) synchronized(ch) { ch.waiter?.completeExceptionally(IOException(why)); ch.waiter = null }
        netJob?.wake { cancel = true }
        _state.value = if (why == "unplugged") State.Unplugged else State.Failed(why)
        _stats.update { it.copy(lastError = why) }
    }

    // ---- frames ---------------------------------------------------------------

    private fun send(type: Int, flags: Int, chan: Int, p: ByteArray = ByteArray(0), off: Int = 0, len: Int = p.size): Boolean {
        val c = conn ?: return false
        val o = epOut ?: return false
        val f = Frames.encode(type, flags, chan, p, off, len)
        synchronized(writeLock) {
            var at = 0
            while (at < f.size) {
                val w = c.bulkTransfer(o, f, at, f.size - at, 2000)
                if (w <= 0) return false
                at += w
            }
        }
        _stats.update { it.copy(txFrames = it.txFrames + 1) }
        return true
    }

    private fun readLoop(c: UsbDeviceConnection, ep: UsbEndpoint) {
        val parser = Frames.Parser()
        val buf = ByteArray(16384)
        while (conn === c) {
            val n = c.bulkTransfer(ep, buf, buf.size, 250)
            if (n > 0) {
                parser.feed(buf, n) { onFrame(it) }
                _stats.update { it.copy(bad = parser.bad) }
            }
        }
    }

    private suspend fun pingLoop() {
        while (scope.isActive) {
            delay(1000)
            if (!withContext(Dispatchers.IO) { send(Frames.PING, Frames.FIN, 0) }) {
                closeNow("write")
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
        _stats.update { it.copy(rxFrames = it.rxFrames + 1) }
        when (f.type) {
            Frames.HELLO_ACK -> {
                val j = try { JSONObject(String(f.payload)) } catch (e: Exception) { JSONObject() }
                hello?.complete(j)
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
                _events.tryEmit(j)
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
            else -> {}   // a board newer than us: ignored
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
        if (!up) throw IOException("no board on USB")
        val ch = withTimeoutOrNull(timeoutMs) { free.receive() } ?: throw IOException("link busy")
        val d = CompletableDeferred<HttpWire.Response>()
        try {
            synchronized(ch) {
                ch.buf.reset()
                ch.waiter = d
                ch.open = true
                ch.lastUse = SystemClock.elapsedRealtime()
            }
            _stats.update { it.copy(requests = it.requests + 1) }
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

    private fun countFail(why: String) = _stats.update { it.copy(failures = it.failures + 1, lastError = why) }

    // ---- the board's internet through this app --------------------------------
    // The board composes its GitHub requests (src/net/GhOta.cpp, LogPush.cpp)
    // as over WiFi; here they are carried out on the head unit's network. Only
    // the hosts in BuildConfig.NET_HOSTS (the board's GhRelease::trustedUrl).

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
        _stats.update { it.copy(netRequests = it.netRequests + 1) }
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

    companion object {
        const val ACTION_PERMISSION = "ua.megaopen.companion.USB_PERMISSION"
        private const val IDLE_MS = 10_000L
    }
}
