package ua.megaopen.companion.board

import android.content.Context
import android.net.nsd.NsdManager
import android.net.nsd.NsdServiceInfo
import android.net.wifi.WifiManager
import kotlinx.coroutines.CompletableDeferred
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.async
import kotlinx.coroutines.awaitAll
import kotlinx.coroutines.channels.Channel
import kotlinx.coroutines.coroutineScope
import kotlinx.coroutines.sync.Semaphore
import kotlinx.coroutines.sync.withPermit
import kotlinx.coroutines.withContext
import kotlinx.coroutines.withTimeoutOrNull
import java.net.Inet4Address
import java.net.NetworkInterface
import java.util.concurrent.atomic.AtomicReference

/**
 * Finding the board on WiFi with no address typed in: mDNS first
 * (`_megaopen._tcp`, src/net/WifiManager.cpp), then every address of this
 * device's own /24s (a head unit's hotspot hands out no names), the board's
 * own AP address among them.
 */
class Discovery(private val ctx: Context) {
    suspend fun find(): String? = nsd(5_000) ?: scan()

    private suspend fun nsd(waitMs: Long): String? {
        val nsd = ctx.getSystemService(Context.NSD_SERVICE) as? NsdManager ?: return null
        val wifi = ctx.applicationContext.getSystemService(Context.WIFI_SERVICE) as? WifiManager
        val lock = wifi?.createMulticastLock("megaopen")?.apply { setReferenceCounted(false) }
        val found = Channel<NsdServiceInfo>(Channel.UNLIMITED)
        val listener = object : NsdManager.DiscoveryListener {
            override fun onDiscoveryStarted(serviceType: String) {}
            override fun onDiscoveryStopped(serviceType: String) {}
            override fun onStartDiscoveryFailed(serviceType: String, errorCode: Int) { found.close() }
            override fun onStopDiscoveryFailed(serviceType: String, errorCode: Int) {}
            override fun onServiceFound(serviceInfo: NsdServiceInfo) { found.trySend(serviceInfo) }
            override fun onServiceLost(serviceInfo: NsdServiceInfo) {}
        }
        try {
            lock?.acquire()
            nsd.discoverServices(SERVICE, NsdManager.PROTOCOL_DNS_SD, listener)
        } catch (e: Exception) {
            lock?.release()
            return null
        }
        try {
            return withTimeoutOrNull(waitMs) {
                for (s in found) {
                    val host = resolve(nsd, s) ?: continue
                    if (WifiBoard(host).probe(1_500) != null) return@withTimeoutOrNull host
                }
                null
            }
        } finally {
            try { nsd.stopServiceDiscovery(listener) } catch (_: Exception) {}
            lock?.release()
        }
    }

    @Suppress("DEPRECATION")
    private suspend fun resolve(nsd: NsdManager, s: NsdServiceInfo): String? {
        val done = CompletableDeferred<String?>()
        try {
            nsd.resolveService(s, object : NsdManager.ResolveListener {
                override fun onResolveFailed(serviceInfo: NsdServiceInfo, errorCode: Int) { done.complete(null) }
                override fun onServiceResolved(serviceInfo: NsdServiceInfo) {
                    val a = serviceInfo.host
                    done.complete(if (a is Inet4Address) a.hostAddress + if (serviceInfo.port == 80) "" else ":${serviceInfo.port}" else null)
                }
            })
        } catch (e: Exception) {
            return null
        }
        return withTimeoutOrNull(4_000) { done.await() }
    }

    private suspend fun scan(): String? = withContext(Dispatchers.IO) {
        val hosts = LinkedHashSet<String>()
        hosts += AP_ADDRESS
        val ifs = try { NetworkInterface.getNetworkInterfaces()?.toList() } catch (e: Exception) { null }
        for (ni in ifs.orEmpty()) {
            if (!ni.isUp || ni.isLoopback) continue
            if (SKIP.any { (ni.name ?: "").startsWith(it) }) continue    // mobile data, VPN
            for (ia in ni.interfaceAddresses) {
                val a = ia.address as? Inet4Address ?: continue
                if (!a.isSiteLocalAddress) continue
                val b = a.address
                val net = "${b[0].toInt() and 255}.${b[1].toInt() and 255}.${b[2].toInt() and 255}."
                val self = b[3].toInt() and 255
                for (i in 1..254) if (i != self) hosts += net + i
            }
        }
        val gate = Semaphore(48)
        val hit = AtomicReference<String?>(null)
        coroutineScope {
            hosts.map { h ->
                async { gate.withPermit { if (hit.get() == null && WifiBoard(h).probe(700) != null) hit.compareAndSet(null, h) } }
            }.awaitAll()
        }
        hit.get()
    }

    private companion object {
        const val SERVICE = "_megaopen._tcp"
        const val AP_ADDRESS = "192.168.4.1"     // the board's own access point
        val SKIP = listOf("rmnet", "ccmni", "pdp", "v4-", "tun", "ppp", "dummy", "ifb", "clat")
    }
}
