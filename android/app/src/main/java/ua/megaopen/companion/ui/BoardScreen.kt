package ua.megaopen.companion.ui

import android.annotation.SuppressLint
import android.content.ContentValues
import android.content.Context
import android.net.Uri
import android.os.Build
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
import androidx.activity.compose.BackHandler
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.viewinterop.AndroidView
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import kotlinx.coroutines.launch
import kotlinx.coroutines.runBlocking
import org.json.JSONObject
import ua.megaopen.companion.AppGraph
import ua.megaopen.companion.R
import ua.megaopen.companion.board.Board
import java.io.File

/**
 * The board's own settings page, whichever way the board is reached. The page
 * is loaded from http://megaopen.board/, which never reaches a network: every
 * GET of that host is answered through [Board] here, and the page's API calls
 * (web/js/10-api.js) go through window.MOLink, since an intercepted request
 * has no body for a POST.
 */
@Composable
fun BoardScreen(graph: AppGraph) {
    val board by graph.connection.board.collectAsStateWithLifecycle()
    val b = board
    if (b == null) {
        Page { Section(stringResource(R.string.tab_board)) { Note(stringResource(R.string.board_none)) } }
        return
    }
    BoardPage(b)
}

@SuppressLint("SetJavaScriptEnabled")
@Composable
private fun BoardPage(board: Board) {
    val ctx = LocalContext.current
    val io = remember(board) { CoroutineScope(SupervisorJob() + Dispatchers.IO) }
    var pick by remember { mutableStateOf<ValueCallback<Array<Uri>>?>(null) }
    val chooser = rememberLauncherForActivityResult(ActivityResultContracts.StartActivityForResult()) { res ->
        pick?.onReceiveValue(WebChromeClient.FileChooserParams.parseResult(res.resultCode, res.data))
        pick = null
    }
    var web by remember { mutableStateOf<WebView?>(null) }
    DisposableEffect(board) { onDispose { io.cancel(); web?.destroy() } }
    BackHandler(enabled = web?.canGoBack() == true) { web?.goBack() }

    AndroidView(modifier = Modifier.fillMaxSize(), factory = { c ->
        WebView(c).apply {
            settings.javaScriptEnabled = true
            settings.domStorageEnabled = true
            addJavascriptInterface(Bridge(c, board, io) { code -> post { evaluateJavascript(code, null) } }, "MOLink")
            webViewClient = PageClient(c, board)
            webChromeClient = object : WebChromeClient() {
                override fun onShowFileChooser(v: WebView, cb: ValueCallback<Array<Uri>>, p: FileChooserParams): Boolean {
                    pick?.onReceiveValue(null)
                    pick = cb
                    return try { chooser.launch(p.createIntent()); true } catch (e: Exception) { pick = null; false }
                }
            }
            loadUrl("http://$HOST/")
            web = this
        }
    })
}

private const val HOST = "megaopen.board"

/**
 * Every GET of the page's host. The page itself ("/", "/i18n/en.json") is the
 * app's own copy of web/ (assets/page, tools/export_page.py) when the board
 * runs the same firmware, or has no page of its own (FEATURE_WEB_PAGE=0);
 * else the board's, which knows its own API. Everything else is the board's.
 */
private class PageClient(private val ctx: Context, private val board: Board) : WebViewClient() {
    private val ownFw: String = try {
        JSONObject(ctx.assets.open("page/page.json").bufferedReader().use { it.readText() }).optString("fw")
    } catch (e: Exception) {
        ""
    }

    private fun own(path: String): WebResourceResponse? {
        val (file, mime) = when (path) {
            "/" -> "page/index.html" to "text/html"
            "/i18n/en.json" -> "page/i18n/en.json" to "application/json"
            else -> return null
        }
        return try {
            WebResourceResponse(mime, "utf-8", 200, "OK", mapOf("Cache-Control" to "no-store"), ctx.assets.open(file))
        } catch (e: Exception) {
            null
        }
    }

    // On WebView's own background thread: a blocking call is in order here.
    override fun shouldInterceptRequest(view: WebView, req: WebResourceRequest): WebResourceResponse? {
        if (req.url.host != HOST || req.method != "GET") return null
        val path = req.url.encodedPath.orEmpty().ifEmpty { "/" } + (req.url.encodedQuery?.let { "?$it" } ?: "")
        val page = path == "/" || path == "/i18n/en.json"
        if (page && ownFw.isNotEmpty() && board.fw == ownFw) own(path)?.let { return it }
        return try {
            val r = runBlocking { board.request("GET", path) }
            // No page on the board: the page's files are a 404, "/" the rescue page.
            if (page && (r.code == 404 || r.headers["x-megaopen-page"] == "rescue")) own(path)?.let { return it }
            // A WebResourceResponse may not carry a 3xx.
            val code = if (r.code in 300..399 || r.code < 100) 502 else r.code
            val mime = r.type.substringBefore(';').trim()
            val cs = Regex("charset=([^;]+)").find(r.type)?.groupValues?.get(1)
                ?: if (mime.startsWith("text/") || mime.contains("json")) "utf-8" else null
            WebResourceResponse(mime, cs, code, reason(code), mapOf("Cache-Control" to "no-store"), r.body.inputStream())
        } catch (e: Exception) {
            WebResourceResponse("text/plain", "utf-8", 502, "Bad Gateway", emptyMap(),
                view.context.getString(R.string.board_unreachable, e.message ?: "").byteInputStream())
        }
    }

    override fun shouldOverrideUrlLoading(view: WebView, req: WebResourceRequest): Boolean = req.url.host != HOST

    private fun reason(code: Int) = when (code) {
        200 -> "OK"; 204 -> "No Content"; 400 -> "Bad Request"; 403 -> "Forbidden"; 404 -> "Not Found"
        409 -> "Conflict"; 500 -> "Internal Server Error"; 502 -> "Bad Gateway"; 503 -> "Service Unavailable"
        else -> "Status"
    }
}

/** window.MOLink: what web/js/10-api.js calls instead of fetch. */
private class Bridge(private val ctx: Context, private val board: Board, private val io: CoroutineScope,
                     private val js: (String) -> Unit) {
    @JavascriptInterface
    fun request(id: Int, method: String, path: String, headersJson: String, bodyB64: String) {
        io.launch {
            try {
                val hj = JSONObject(headersJson.ifEmpty { "{}" })
                val h = hj.keys().asSequence().associateWith { hj.getString(it) }
                val body = if (bodyB64.isEmpty()) null else Base64.decode(bodyB64, Base64.NO_WRAP)
                val r = board.request(method, path, h, body, if (path.startsWith("/ota/")) 180_000 else 20_000)
                js("window.__moDone($id,${r.code},${JSONObject.quote(r.type)},'${Base64.encodeToString(r.body, Base64.NO_WRAP)}')")
            } catch (e: Exception) {
                js("window.__moDone($id,-1,'',${JSONObject.quote(e.message ?: "link")})")
            }
        }
    }

    @JavascriptInterface
    fun download(path: String, name: String) {
        io.launch {
            val msg = try {
                ctx.getString(R.string.board_saved, save(name, board.request("GET", path, timeoutMs = 60_000).body))
            } catch (e: Exception) {
                ctx.getString(R.string.board_not_saved, e.message ?: "")
            }
            launch(Dispatchers.Main) { toast(ctx, msg) }
        }
    }

    @JavascriptInterface
    fun saveText(name: String, text: String) {
        io.launch {
            val msg = try { ctx.getString(R.string.board_saved, save(name, text.toByteArray())) }
            catch (e: Exception) { ctx.getString(R.string.board_not_saved, e.message ?: "") }
            launch(Dispatchers.Main) { toast(ctx, msg) }
        }
    }

    /** Into Downloads: MediaStore on Android 10+, the app's own folder before. */
    private fun save(name: String, bytes: ByteArray): String {
        if (Build.VERSION.SDK_INT >= 29) {
            val v = ContentValues().apply {
                put(MediaStore.Downloads.DISPLAY_NAME, name)
                put(MediaStore.Downloads.IS_PENDING, 1)
            }
            val uri = ctx.contentResolver.insert(MediaStore.Downloads.EXTERNAL_CONTENT_URI, v) ?: error("no Downloads")
            ctx.contentResolver.openOutputStream(uri)?.use { it.write(bytes) }
            v.clear()
            v.put(MediaStore.Downloads.IS_PENDING, 0)
            ctx.contentResolver.update(uri, v, null, null)
            return "${Environment.DIRECTORY_DOWNLOADS}/$name"
        }
        val f = File(ctx.getExternalFilesDir(Environment.DIRECTORY_DOWNLOADS) ?: ctx.filesDir, name)
        f.writeBytes(bytes)
        return f.absolutePath
    }
}
