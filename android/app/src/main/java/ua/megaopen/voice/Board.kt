package ua.megaopen.voice

import android.content.Context
import android.net.nsd.NsdManager
import android.net.nsd.NsdServiceInfo
import android.net.wifi.WifiManager
import android.os.SystemClock
import org.json.JSONObject
import java.io.IOException
import java.net.HttpURLConnection
import java.net.Inet4Address
import java.net.NetworkInterface
import java.net.URL
import java.util.Collections
import java.util.concurrent.CountDownLatch
import java.util.concurrent.ExecutorCompletionService
import java.util.concurrent.Executors
import java.util.concurrent.LinkedBlockingQueue
import java.util.concurrent.TimeUnit

/** The board's HTTP API, as MegaOpen Companion uses it. Blocking: call off the main thread. */
object Board {
    class Alert(val seq: Long, val agoMs: Long, val sig: String, val value: Long, val text: String)
    class Alerts(val last: Long, val upMs: Long, val list: List<Alert>)

    private fun open(host: String, path: String, timeoutMs: Int): HttpURLConnection {
        val c = URL("http://$host$path").openConnection() as HttpURLConnection
        c.connectTimeout = timeoutMs
        c.readTimeout = timeoutMs
        c.useCaches = false
        return c
    }

    private fun read(c: HttpURLConnection): String {
        try {
            val code = c.responseCode
            val stream = if (code in 200..299) c.inputStream else c.errorStream
            val body = stream?.bufferedReader()?.use { it.readText() } ?: ""
            if (code !in 200..299) throw IOException("HTTP $code $body".trim())
            return body
        } finally {
            c.disconnect()
        }
    }

    fun get(host: String, path: String, timeoutMs: Int = 2500): String = read(open(host, path, timeoutMs))

    /** Every change on the board needs the X-MegaOpen header (src/net/RequestGate.h). */
    fun post(host: String, path: String, json: String, timeoutMs: Int = 3000): String {
        val c = open(host, path, timeoutMs)
        c.requestMethod = "POST"
        c.doOutput = true
        c.setRequestProperty("Content-Type", "application/json")
        c.setRequestProperty("X-MegaOpen", "1")
        c.outputStream.use { it.write(json.toByteArray()) }
        return read(c)
    }

    /** GET /api/alerts?since=N (src/net/WebUiMap.cpp routesAlerts). */
    fun alerts(host: String, since: Long): Alerts {
        val j = JSONObject(get(host, "/api/alerts?since=$since"))
        val a = j.optJSONArray("alerts")
        val list = ArrayList<Alert>()
        if (a != null) for (i in 0 until a.length()) {
            val o = a.getJSONObject(i)
            list += Alert(o.optLong("seq"), o.optLong("agoMs"), o.optString("sig"),
                o.optLong("value"), o.optString("text"))
        }
        return Alerts(j.optLong("last"), j.optLong("upMs"), list)
    }

    /** Ask the board to raise a test alert: the whole chain, board to voice. */
    fun test(host: String, sig: String) {
        post(host, "/api/alerts/test", JSONObject().put("sig", sig).toString())
    }

    /** The firmware version, or null when `host` is not a MegaOpen board. */
    fun probe(host: String, timeoutMs: Int): String? = try {
        val j = JSONObject(get(host, "/api/status", timeoutMs))
        if (j.has("fw") && j.has("apName")) j.getString("fw") else null
    } catch (e: Exception) {
        null
    }

    /** mDNS first (_megaopen._tcp, src/net/WifiManager.cpp), then every host of the local /24s. */
    fun discover(ctx: Context): String? = nsd(ctx, 5000) ?: scan()

    private fun nsd(ctx: Context, waitMs: Long): String? {
        val nsd = ctx.getSystemService(Context.NSD_SERVICE) as? NsdManager ?: return null
        val wifi = ctx.applicationContext.getSystemService(Context.WIFI_SERVICE) as? WifiManager
        val lock = wifi?.createMulticastLock("megaopen")?.apply { setReferenceCounted(false) }
        val found = LinkedBlockingQueue<NsdServiceInfo>()
        val failed = CountDownLatch(1)
        val listener = object : NsdManager.DiscoveryListener {
            override fun onDiscoveryStarted(serviceType: String) {}
            override fun onDiscoveryStopped(serviceType: String) {}
            override fun onStartDiscoveryFailed(serviceType: String, errorCode: Int) = failed.countDown()
            override fun onStopDiscoveryFailed(serviceType: String, errorCode: Int) {}
            override fun onServiceFound(serviceInfo: NsdServiceInfo) {
                found.offer(serviceInfo)
            }
            override fun onServiceLost(serviceInfo: NsdServiceInfo) {}
        }
        try {
            lock?.acquire()
            nsd.discoverServices("_megaopen._tcp", NsdManager.PROTOCOL_DNS_SD, listener)
        } catch (e: Exception) {
            lock?.release()
            return null
        }
        try {
            val deadline = SystemClock.elapsedRealtime() + waitMs
            while (failed.count > 0) {
                val left = deadline - SystemClock.elapsedRealtime()
                if (left <= 0) break
                val s = found.poll(left, TimeUnit.MILLISECONDS) ?: break
                val host = resolve(nsd, s) ?: continue
                if (probe(host, 1500) != null) return host
            }
        } finally {
            try {
                nsd.stopServiceDiscovery(listener)
            } catch (_: Exception) {
            }
            lock?.release()
        }
        return null
    }

    @Suppress("DEPRECATION")
    private fun resolve(nsd: NsdManager, s: NsdServiceInfo): String? {
        val done = CountDownLatch(1)
        var out: String? = null
        try {
            nsd.resolveService(s, object : NsdManager.ResolveListener {
                override fun onResolveFailed(serviceInfo: NsdServiceInfo, errorCode: Int) = done.countDown()
                override fun onServiceResolved(serviceInfo: NsdServiceInfo) {
                    val a = serviceInfo.host
                    if (a is Inet4Address) {
                        out = a.hostAddress + if (serviceInfo.port == 80) "" else ":${serviceInfo.port}"
                    }
                    done.countDown()
                }
            })
            done.await(4, TimeUnit.SECONDS)
        } catch (_: Exception) {
        }
        return out
    }

    /** The head unit's hotspot gives no names: try each address of our own /24s. */
    private fun scan(): String? {
        val hosts = LinkedHashSet<String>()
        hosts += "192.168.4.1"                     // the board's own access point
        val ifs = try {
            NetworkInterface.getNetworkInterfaces()
        } catch (_: Exception) {
            null
        }
        if (ifs != null) for (ni in Collections.list(ifs)) {
            if (!ni.isUp || ni.isLoopback) continue
            val name = ni.name ?: ""
            if (SKIP.any { name.startsWith(it) }) continue   // mobile data, VPN
            for (ia in ni.interfaceAddresses) {
                val a = ia.address as? Inet4Address ?: continue
                if (!a.isSiteLocalAddress) continue
                val b = a.address
                val net = "${b[0].toInt() and 255}.${b[1].toInt() and 255}.${b[2].toInt() and 255}."
                val self = b[3].toInt() and 255
                for (i in 1..254) if (i != self) hosts += net + i
            }
        }
        val pool = Executors.newFixedThreadPool(48)
        try {
            val cs = ExecutorCompletionService<String?>(pool)
            for (h in hosts) cs.submit { if (probe(h, 700) != null) h else null }
            repeat(hosts.size) {
                val h = cs.take().get()
                if (h != null) return h
            }
        } finally {
            pool.shutdownNow()
        }
        return null
    }

    private val SKIP = listOf("rmnet", "ccmni", "pdp", "v4-", "tun", "ppp", "dummy", "ifb", "clat")
}
