package ua.megaopen.voice

import android.annotation.SuppressLint
import android.app.Activity
import android.content.ContentValues
import android.content.Intent
import android.net.Uri
import android.os.Build
import android.os.Bundle
import android.os.Environment
import android.provider.MediaStore
import android.util.Base64
import android.webkit.JavascriptInterface
import android.webkit.ValueCallback
import android.webkit.WebChromeClient
import android.webkit.WebResourceRequest
import android.webkit.WebResourceResponse
import android.webkit.WebView
import android.webkit.WebViewClient
import android.widget.TextView
import android.widget.Toast
import org.json.JSONObject
import java.io.File
import java.util.concurrent.Executors

/**
 * The board's own settings page, over the USB link: no WiFi, no address.
 * The page is loaded from http://megaopen.link/, which never reaches a
 * network: every GET of that host is answered over USB here, and the page's
 * API calls (web/js/10-api.js) go through window.MOLink, since an
 * intercepted request has no body for a POST.
 */
class BoardActivity : Activity() {
    companion object {
        const val HOST = "megaopen.link"
        private const val PICK = 7
    }

    private lateinit var web: WebView
    private val io = Executors.newFixedThreadPool(2)
    private var pick: ValueCallback<Array<Uri>>? = null

    @SuppressLint("SetJavaScriptEnabled")
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        web = WebView(this)
        web.settings.javaScriptEnabled = true
        web.settings.domStorageEnabled = true
        web.addJavascriptInterface(Bridge(), "MOLink")
        web.webViewClient = object : WebViewClient() {
            override fun shouldInterceptRequest(view: WebView, req: WebResourceRequest): WebResourceResponse? {
                if (req.url.host != HOST || req.method != "GET") return null
                val path = req.url.encodedPath.orEmpty().ifEmpty { "/" } +
                    (req.url.encodedQuery?.let { "?$it" } ?: "")
                return try {
                    val r = UsbBoard.request("GET", path)
                    // A WebResourceResponse may not carry a 3xx.
                    val code = if (r.code in 300..399 || r.code < 100) 502 else r.code
                    val mime = r.type.substringBefore(';').trim()
                    val cs = Regex("charset=([^;]+)").find(r.type)?.groupValues?.get(1)
                        ?: if (mime.startsWith("text/") || mime.contains("json")) "utf-8" else null
                    WebResourceResponse(mime, cs, code, reason(code), mapOf("Cache-Control" to "no-store"),
                        r.body.inputStream())
                } catch (e: Exception) {
                    WebResourceResponse("text/plain", "utf-8", 502, "Bad Gateway", emptyMap(),
                        getString(R.string.link_none, e.message ?: "").byteInputStream())
                }
            }

            override fun shouldOverrideUrlLoading(view: WebView, req: WebResourceRequest): Boolean {
                if (req.url.host == HOST) return false
                try { startActivity(Intent(Intent.ACTION_VIEW, req.url)) } catch (_: Exception) {}
                return true
            }
        }
        // <input type=file>: the firmware file for the page's own update.
        web.webChromeClient = object : WebChromeClient() {
            override fun onShowFileChooser(v: WebView, cb: ValueCallback<Array<Uri>>, p: FileChooserParams): Boolean {
                pick?.onReceiveValue(null)
                pick = cb
                return try {
                    startActivityForResult(p.createIntent(), PICK)
                    true
                } catch (_: Exception) {
                    pick = null
                    false
                }
            }
        }
        setContentView(TextView(this).apply {
            setText(R.string.link_opening)
            setPadding(48, 48, 48, 48)
        })
        io.execute {
            val ok = UsbBoard.open(this)
            runOnUiThread {
                if (ok) {
                    setContentView(web)
                    web.loadUrl("http://$HOST/")
                } else {
                    setContentView(TextView(this).apply {
                        text = getString(R.string.link_none, UsbBoard.lastError)
                        setPadding(48, 48, 48, 48)
                    })
                }
            }
        }
    }

    override fun onActivityResult(requestCode: Int, resultCode: Int, data: Intent?) {
        if (requestCode == PICK) {
            pick?.onReceiveValue(WebChromeClient.FileChooserParams.parseResult(resultCode, data))
            pick = null
            return
        }
        super.onActivityResult(requestCode, resultCode, data)
    }

    @Deprecated("Deprecated in Java")
    @Suppress("DEPRECATION")
    override fun onBackPressed() {
        if (web.canGoBack()) web.goBack() else super.onBackPressed()
    }

    override fun onDestroy() {
        io.shutdownNow()
        web.destroy()
        super.onDestroy()
    }

    private fun reason(code: Int) = when (code) {
        200 -> "OK"; 204 -> "No Content"; 400 -> "Bad Request"; 403 -> "Forbidden"; 404 -> "Not Found"
        500 -> "Internal Server Error"; 502 -> "Bad Gateway"; 503 -> "Service Unavailable"
        else -> "Status"
    }

    private fun js(code: String) = runOnUiThread { web.evaluateJavascript(code, null) }

    /** Into Downloads: MediaStore on Android 10+, the app's own folder before. */
    private fun save(name: String, bytes: ByteArray): String {
        if (Build.VERSION.SDK_INT >= 29) {
            val v = ContentValues().apply {
                put(MediaStore.Downloads.DISPLAY_NAME, name)
                put(MediaStore.Downloads.IS_PENDING, 1)
            }
            val uri = contentResolver.insert(MediaStore.Downloads.EXTERNAL_CONTENT_URI, v) ?: error("no Downloads")
            contentResolver.openOutputStream(uri)?.use { it.write(bytes) }
            v.clear()
            v.put(MediaStore.Downloads.IS_PENDING, 0)
            contentResolver.update(uri, v, null, null)
            return "${Environment.DIRECTORY_DOWNLOADS}/$name"
        }
        val dir = getExternalFilesDir(Environment.DIRECTORY_DOWNLOADS) ?: filesDir
        val f = File(dir, name)
        f.writeBytes(bytes)
        return f.absolutePath
    }

    private fun toast(s: String) = runOnUiThread { Toast.makeText(this, s, Toast.LENGTH_LONG).show() }

    /** window.MOLink: what web/js/10-api.js calls instead of fetch. */
    inner class Bridge {
        @JavascriptInterface
        fun request(id: Int, method: String, path: String, headersJson: String, bodyB64: String) {
            io.execute {
                try {
                    val hj = JSONObject(headersJson.ifEmpty { "{}" })
                    val h = HashMap<String, String>()
                    for (k in hj.keys()) h[k] = hj.getString(k)
                    val body = if (bodyB64.isEmpty()) null else Base64.decode(bodyB64, Base64.NO_WRAP)
                    val r = UsbBoard.request(method, path, h, body, if (path.startsWith("/ota/")) 120_000 else 20_000)
                    js("window.__moDone($id,${r.code},${JSONObject.quote(r.type)},'${Base64.encodeToString(r.body, Base64.NO_WRAP)}')")
                } catch (e: Exception) {
                    js("window.__moDone($id,-1,'',${JSONObject.quote(e.message ?: "link")})")
                }
            }
        }

        @JavascriptInterface
        fun download(path: String, name: String) {
            io.execute {
                try {
                    val r = UsbBoard.request("GET", path, timeoutMs = 60_000)
                    toast(getString(R.string.link_saved, save(name, r.body)))
                } catch (e: Exception) {
                    toast(getString(R.string.link_failed, e.message ?: ""))
                }
            }
        }

        @JavascriptInterface
        fun saveText(name: String, text: String) {
            io.execute {
                try {
                    toast(getString(R.string.link_saved, save(name, text.toByteArray())))
                } catch (e: Exception) {
                    toast(getString(R.string.link_failed, e.message ?: ""))
                }
            }
        }
    }
}
