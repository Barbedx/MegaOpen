package ua.megaopen.companion.board

import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import org.json.JSONObject
import ua.megaopen.companion.link.HttpWire
import ua.megaopen.companion.link.UsbLink
import java.io.IOException
import java.net.HttpURLConnection
import java.net.URL
import java.util.zip.GZIPInputStream

/** The board's web API, whichever way in: the rest of the app does not care. */
interface Board {
    enum class Via { Usb, Wifi }

    val via: Via
    /** "MegaOpen-8428" on USB, the address on WiFi. */
    val label: String

    suspend fun request(method: String, path: String, headers: Map<String, String> = emptyMap(),
                        body: ByteArray? = null, timeoutMs: Long = 20_000): HttpWire.Response

    suspend fun json(path: String): JSONObject {
        val r = request("GET", path)
        if (r.code != 200) throw IOException("HTTP ${r.code}")
        return JSONObject(r.text())
    }

    suspend fun postJson(path: String, body: JSONObject): HttpWire.Response =
        request("POST", path, mapOf("Content-Type" to "application/json"), body.toString().toByteArray())
}

class UsbBoard(private val link: UsbLink, override val label: String) : Board {
    override val via = Board.Via.Usb
    override suspend fun request(method: String, path: String, headers: Map<String, String>,
                                 body: ByteArray?, timeoutMs: Long) = link.request(method, path, headers, body, timeoutMs)
}

class WifiBoard(val host: String) : Board {
    override val via = Board.Via.Wifi
    override val label = host

    override suspend fun request(method: String, path: String, headers: Map<String, String>,
                                 body: ByteArray?, timeoutMs: Long): HttpWire.Response = withContext(Dispatchers.IO) {
        val c = URL("http://$host$path").openConnection() as HttpURLConnection
        try {
            c.requestMethod = method
            c.connectTimeout = minOf(timeoutMs, 5_000L).toInt()
            c.readTimeout = timeoutMs.toInt()
            c.useCaches = false
            // Every change on the board needs it (src/net/RequestGate.h); a GET does not mind.
            c.setRequestProperty("X-MegaOpen", "1")
            for ((k, v) in headers) c.setRequestProperty(k, v)
            if (body != null) {
                c.doOutput = true
                c.setFixedLengthStreamingMode(body.size)
                c.outputStream.use { it.write(body) }
            }
            val code = c.responseCode
            val h = HashMap<String, String>()
            for ((k, v) in c.headerFields) if (k != null && v.isNotEmpty()) h[k.lowercase()] = v.last()
            var b = (if (code >= 400) c.errorStream else c.inputStream)?.use { it.readBytes() } ?: ByteArray(0)
            // The page is stored gzipped and sent so even unasked.
            if (h["content-encoding"]?.lowercase() == "gzip") {
                b = GZIPInputStream(b.inputStream()).use { it.readBytes() }
                h.remove("content-encoding")
            }
            HttpWire.Response(code, h, b)
        } finally {
            c.disconnect()
        }
    }

    /** The firmware version, or null when `host` is not a MegaOpen board. */
    suspend fun probe(timeoutMs: Long): String? = try {
        val r = request("GET", "/api/status", timeoutMs = timeoutMs)
        val j = JSONObject(r.text())
        if (r.code == 200 && j.has("fw") && j.has("apName")) j.getString("fw") else null
    } catch (e: Exception) {
        null
    }
}
