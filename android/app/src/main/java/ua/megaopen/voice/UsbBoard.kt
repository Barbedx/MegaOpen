package ua.megaopen.voice

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
import org.json.JSONObject
import java.io.ByteArrayOutputStream
import java.io.IOException
import java.util.concurrent.ArrayBlockingQueue
import java.util.concurrent.CopyOnWriteArrayList
import java.util.concurrent.TimeUnit

/**
 * The board on the head unit's USB (docs/design/usb-link.md): the vendor
 * interface of the board's composite device, frames from Link, HTTP piped to
 * the board's web server, alerts pushed as events. One board at a time.
 *
 * Threads: a reader (bulk IN, dispatches frames) and a pinger; request() and
 * the senders take the write lock. Every call here may come from any thread.
 */
object UsbBoard {
    const val VID = 0x303A
    const val ACTION_PERMISSION = "ua.megaopen.voice.USB_PERMISSION"

    /** HELLO's answer: firmware, build, AP name, channels. Null until it came. */
    @Volatile var hello: JSONObject? = null
        private set
    @Volatile var lastError: String = ""
        private set
    val connected: Boolean get() = hello != null && conn != null

    private val events = CopyOnWriteArrayList<(JSONObject) -> Unit>()
    fun onEvent(f: (JSONObject) -> Unit) { events += f }
    fun offEvent(f: (JSONObject) -> Unit) { events -= f }

    private var conn: UsbDeviceConnection? = null
    private var itf: UsbInterface? = null
    private var epIn: UsbEndpoint? = null
    private var epOut: UsbEndpoint? = null
    private var reader: Thread? = null
    private var pinger: Thread? = null
    private val writeLock = Any()
    @Volatile private var asked: String? = null
    private var helloWait: java.util.concurrent.CountDownLatch? = null

    // One waiter per channel; the pool hands channels out, request() gives them back.
    private class Chan(val n: Int) {
        val buf = ByteArrayOutputStream()
        @Volatile var waiting = false
        @Volatile var result: Http.Response? = null
        @Volatile var error: String? = null
        val lock = Object()
    }
    private var chans: Array<Chan> = emptyArray()
    private var free = ArrayBlockingQueue<Chan>(1)

    // ---- the device ---------------------------------------------------------

    fun find(ctx: Context): UsbDevice? {
        val um = ctx.getSystemService(Context.USB_SERVICE) as? UsbManager ?: return null
        return um.deviceList.values.firstOrNull { d -> d.vendorId == VID && vendorItf(d) != null }
    }

    private fun vendorItf(d: UsbDevice): UsbInterface? {
        for (i in 0 until d.interfaceCount) {
            val f = d.getInterface(i)
            if (f.interfaceClass == UsbConstants.USB_CLASS_VENDOR_SPEC && f.endpointCount >= 2) return f
        }
        return null
    }

    /**
     * Connect to the board if one is plugged in: asks Android's permission
     * the first time (the dialog; «Always» there remembers it). False when
     * there is no board or no permission yet; the permission's answer comes
     * back as ACTION_PERMISSION and the caller tries again.
     */
    @Synchronized
    fun open(ctx: Context): Boolean {
        if (conn != null) return true
        val um = ctx.getSystemService(Context.USB_SERVICE) as? UsbManager ?: return false
        val d = find(ctx) ?: return false
        if (!um.hasPermission(d)) {
            // Once per plug: the dialog again on every retry would never let go.
            if (asked == d.deviceName) return false
            asked = d.deviceName
            val flags = if (Build.VERSION.SDK_INT >= 31) PendingIntent.FLAG_MUTABLE else 0
            val pi = PendingIntent.getBroadcast(ctx, 0, Intent(ACTION_PERMISSION).setPackage(ctx.packageName), flags)
            um.requestPermission(d, pi)
            lastError = "permission"
            return false
        }
        val f = vendorItf(d) ?: return false
        val c = um.openDevice(d) ?: run { lastError = "open"; return false }
        if (!c.claimInterface(f, true)) {
            c.close()
            lastError = "claim"
            return false
        }
        var i: UsbEndpoint? = null
        var o: UsbEndpoint? = null
        for (k in 0 until f.endpointCount) {
            val e = f.getEndpoint(k)
            if (e.type != UsbConstants.USB_ENDPOINT_XFER_BULK) continue
            if (e.direction == UsbConstants.USB_DIR_IN) i = e else o = e
        }
        if (i == null || o == null) {
            c.close()
            return false
        }
        conn = c; itf = f; epIn = i; epOut = o
        hello = null
        val latch = java.util.concurrent.CountDownLatch(1)
        helloWait = latch
        reader = Thread({ readLoop(c, i) }, "usb-reader").also { it.start() }
        // "net": this app carries the board's own GitHub requests (log push, updates).
        send(Link.HELLO, Link.FIN, 0, JSONObject().put("app", BuildConfigVersion.app(ctx)).put("net", 1).toString().toByteArray())
        if (!latch.await(3, TimeUnit.SECONDS)) {
            lastError = "no hello"
            close()
            return false
        }
        pinger = Thread({ pingLoop() }, "usb-ping").also { it.start() }
        lastError = ""
        return true
    }

    @Synchronized
    fun close() {
        val c = conn ?: return
        conn = null
        hello = null
        reader?.interrupt(); pinger?.interrupt()
        reader = null; pinger = null
        try { itf?.let { c.releaseInterface(it) } } catch (_: Exception) {}
        c.close()
        for (ch in chans) fail(ch, "unplugged")
    }

    /** Unplugged: Android says so, the service closes. */
    val detach = object : BroadcastReceiver() {
        override fun onReceive(context: Context, intent: Intent) {
            if (intent.action == UsbManager.ACTION_USB_DEVICE_DETACHED) {
                asked = null
                close()
            }
        }
    }
    val filter = IntentFilter().apply {
        addAction(UsbManager.ACTION_USB_DEVICE_DETACHED)
        addAction(ACTION_PERMISSION)
    }

    // ---- frames -------------------------------------------------------------

    private fun send(type: Int, flags: Int, chan: Int, p: ByteArray = ByteArray(0), off: Int = 0, len: Int = p.size): Boolean {
        val c = conn ?: return false
        val o = epOut ?: return false
        val f = Link.encode(type, flags, chan, p, off, len)
        synchronized(writeLock) {
            var at = 0
            while (at < f.size) {
                val w = c.bulkTransfer(o, f, at, f.size - at, 2000)
                if (w <= 0) return false
                at += w
            }
        }
        return true
    }

    private fun readLoop(c: UsbDeviceConnection, ep: UsbEndpoint) {
        val parser = Link.Parser()
        val buf = ByteArray(16384)
        while (!Thread.currentThread().isInterrupted && conn === c) {
            val n = c.bulkTransfer(ep, buf, buf.size, 250)
            if (n > 0) parser.feed(buf, n) { onFrame(it) }
        }
    }

    private fun pingLoop() {
        try {
            while (!Thread.currentThread().isInterrupted) {
                Thread.sleep(1000)
                if (!send(Link.PING, Link.FIN, 0)) {
                    lastError = "write"
                    close()
                    return
                }
            }
        } catch (_: InterruptedException) {
        }
    }

    private fun onFrame(f: Link.Frame) {
        when (f.type) {
            Link.HELLO_ACK -> {
                val j = try { JSONObject(String(f.payload)) } catch (e: Exception) { JSONObject() }
                val n = j.optInt("chans", 1).coerceIn(1, 8)
                chans = Array(n) { Chan(it) }
                free = ArrayBlockingQueue<Chan>(n).apply { chans.forEach { offer(it) } }
                hello = j
                helloWait?.countDown()
            }
            Link.HTTP_RESP -> {
                val ch = chans.getOrNull(f.chan) ?: return
                synchronized(ch.lock) {
                    if (!ch.waiting) return            // a stale close (idle) on a free channel
                    ch.buf.write(f.payload)
                    val r = Http.parse(ch.buf.toByteArray(), ch.buf.size())
                    if (r != null) done(ch, r)
                    else if (f.flags and Link.FIN != 0 && ch.buf.size() > 0) fail(ch, "closed")
                }
            }
            Link.HTTP_FAIL -> chans.getOrNull(f.chan)?.let { fail(it, String(f.payload)) }
            Link.EVENT -> {
                val j = try { JSONObject(String(f.payload)) } catch (e: Exception) { return }
                for (e in events) e(j)
            }
            Link.NET_REQ -> {
                val head = try { JSONObject(String(f.payload)) } catch (e: Exception) { return }
                netBody.reset()
                netHead = head
                netId = f.chan
                if (f.flags and Link.FIN != 0) netStart(f.chan, head, null)
            }
            Link.NET_BODY -> {
                if (f.chan != netId || netHead == null) return
                netBody.write(f.payload)
                if (f.flags and Link.FIN != 0) netStart(f.chan, netHead!!, netBody.toByteArray())
            }
            Link.NET_ACK -> {
                val j = netJob ?: return
                if (j.id != f.chan || f.payload.size < 4) return
                val v = (f.payload[0].toLong() and 0xFF) or ((f.payload[1].toLong() and 0xFF) shl 8) or
                    ((f.payload[2].toLong() and 0xFF) shl 16) or ((f.payload[3].toLong() and 0xFF) shl 24)
                synchronized(j.lock) { j.acked = v; j.lock.notifyAll() }
            }
            Link.NET_CANCEL -> {
                val j = netJob ?: return
                if (j.id != f.chan) return
                synchronized(j.lock) { j.cancel = true; j.lock.notifyAll() }
            }
        }
    }

    // ---- the board's internet through this app ------------------------------
    // The board composes its GitHub requests (src/net/GhOta.cpp, LogPush.cpp)
    // as over WiFi; here they are carried out on the head unit's network. Only
    // GitHub over https, the board's own list (GhRelease::trustedUrl).
    private val NET_HOSTS = setOf("api.github.com", "github.com", "objects.githubusercontent.com",
        "release-assets.githubusercontent.com")
    private class NetJob(val id: Int) {
        val lock = Object()
        @Volatile var acked = 0L
        @Volatile var cancel = false
    }
    @Volatile private var netJob: NetJob? = null
    private var netHead: JSONObject? = null
    private var netId = -1
    private val netBody = ByteArrayOutputStream()
    private val netExec = java.util.concurrent.Executors.newSingleThreadExecutor()

    private fun netStart(id: Int, head: JSONObject, body: ByteArray?) {
        netHead = null
        val job = NetJob(id)
        netJob?.let { synchronized(it.lock) { it.cancel = true; it.lock.notifyAll() } }
        netJob = job
        netExec.execute { netRun(job, head, body) }
    }

    private fun netRun(job: NetJob, head: JSONObject, body: ByteArray?) {
        var c: javax.net.ssl.HttpsURLConnection? = null
        try {
            val url = java.net.URL(head.getString("url"))
            if (url.protocol != "https" || url.host !in NET_HOSTS) {
                send(Link.NET_FAIL, Link.FIN, job.id, "host".toByteArray())
                return
            }
            c = url.openConnection() as javax.net.ssl.HttpsURLConnection
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
            if (final.host !in NET_HOSTS) {
                send(Link.NET_FAIL, Link.FIN, job.id, "host".toByteArray())
                return
            }
            val len = c.getHeaderField("Content-Length")?.toLongOrNull() ?: -1L
            send(Link.NET_HEAD, Link.FIN, job.id,
                JSONObject().put("status", code).put("len", len).put("url", final.toString()).toString().toByteArray())
            val input = (if (code >= 400) c.errorStream else c.inputStream)
            var sent = 0L
            val buf = ByteArray(Link.MAX_PAYLOAD)
            input?.use { inp ->
                while (true) {
                    val n = inp.read(buf)
                    if (n < 0) break
                    synchronized(job.lock) {
                        val until = SystemClock.elapsedRealtime() + 30_000
                        while (!job.cancel && sent + n - job.acked > Link.NET_WINDOW) {
                            val left = until - SystemClock.elapsedRealtime()
                            if (left <= 0) throw IOException("stalled")
                            job.lock.wait(left)
                        }
                    }
                    if (job.cancel) return
                    if (!send(Link.NET_DATA, 0, job.id, buf, 0, n)) return
                    sent += n
                }
            }
            send(Link.NET_DATA, Link.FIN, job.id)
        } catch (e: Exception) {
            if (!job.cancel) send(Link.NET_FAIL, Link.FIN, job.id, (e.javaClass.simpleName + " " + (e.message ?: "")).take(36).toByteArray())
        } finally {
            c?.disconnect()
        }
    }

    private fun done(ch: Chan, r: Http.Response) {
        synchronized(ch.lock) {
            ch.result = r
            ch.waiting = false
            ch.lock.notifyAll()
        }
    }

    private fun fail(ch: Chan, why: String) {
        synchronized(ch.lock) {
            if (!ch.waiting) return
            ch.error = why
            ch.waiting = false
            ch.lock.notifyAll()
        }
    }

    // ---- HTTP -----------------------------------------------------------------

    /** One request to the board's web server over the link. Blocking; any thread but the reader. */
    fun request(method: String, path: String, headers: Map<String, String> = emptyMap(),
                body: ByteArray? = null, timeoutMs: Long = 20_000): Http.Response {
        if (!connected) throw IOException("no board on USB")
        val ch = free.poll(timeoutMs, TimeUnit.MILLISECONDS) ?: throw IOException("link busy")
        try {
            synchronized(ch.lock) {
                ch.buf.reset(); ch.result = null; ch.error = null; ch.waiting = true
            }
            val raw = Http.request(method, path, headers, body)
            var at = 0
            while (at < raw.size) {
                val n = minOf(Link.MAX_PAYLOAD, raw.size - at)
                val last = at + n >= raw.size
                if (!send(Link.HTTP_REQ, if (last) Link.FIN else 0, ch.n, raw, at, n)) throw IOException("write")
                at += n
            }
            val end = SystemClock.elapsedRealtime() + timeoutMs
            synchronized(ch.lock) {
                while (ch.waiting) {
                    val left = end - SystemClock.elapsedRealtime()
                    if (left <= 0) break
                    ch.lock.wait(left)
                }
                ch.result?.let { return it }
                val why = ch.error ?: "timeout"
                ch.waiting = false
                send(Link.HTTP_CLOSE, Link.FIN, ch.n)   // the board drops that socket
                throw IOException(why)
            }
        } finally {
            free.offer(ch)
        }
    }
}
