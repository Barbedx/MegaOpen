package ua.megaopen.voice

import android.app.PendingIntent
import android.content.Context
import android.content.Intent
import android.content.pm.PackageInstaller
import android.os.Build
import org.json.JSONArray
import org.json.JSONObject
import java.io.File
import java.io.IOException
import java.net.HttpURLConnection
import java.net.URL
import java.security.MessageDigest

/**
 * The app updates itself from the same GitHub releases as the board
 * (release.yml puts megaopen-companion-X.Y.Z.apk next to the firmware, and
 * the same file as megaopen-voice-X.Y.Z.apk, the name 1.5.4 looks for). Off the
 * rc channel: the latest release; on it: the newest of the last ten, an rc
 * included. An rc's APK carries its final's version, so the tag installed
 * last is remembered (Prefs.installedTag), as the board does (NVS fw/tag).
 * Android asks the owner to confirm each install; no root, no silent update.
 */
object Updater {
    private const val API = "https://api.github.com/repos/Barbedx/MegaOpen"

    class Release(val tag: String, val apkUrl: String, val apkName: String, val sumsUrl: String?)

    /** v1.5.4-rc2 → [1, 5, 4, 2]; a final sorts after its rcs. */
    fun order(tag: String): List<Int>? {
        val m = Regex("""^v?(\d+)\.(\d+)\.(\d+)(?:-rc(\d+))?$""").find(tag.trim()) ?: return null
        val (a, b, c, rc) = m.destructured
        return listOf(a.toInt(), b.toInt(), c.toInt(), if (rc.isEmpty()) Int.MAX_VALUE else rc.toInt())
    }

    private fun cmp(x: List<Int>, y: List<Int>): Int {
        for (i in 0 until 4) if (x[i] != y[i]) return x[i].compareTo(y[i])
        return 0
    }

    /** What this install is: the tag it came from, else the version as a final. */
    fun mine(ctx: Context, prefs: Prefs): List<Int>? {
        val name = try {
            ctx.packageManager.getPackageInfo(ctx.packageName, 0).versionName ?: ""
        } catch (_: Exception) {
            ""
        }
        val tagged = order(prefs.installedTag)
        val own = order(name) ?: return tagged
        // A tag of another version is stale (an APK put on by hand since).
        return if (tagged != null && tagged.subList(0, 3) == own.subList(0, 3)) tagged else own
    }

    private fun get(url: String, accept: String): HttpURLConnection {
        val c = URL(url).openConnection() as HttpURLConnection
        c.connectTimeout = 10_000
        c.readTimeout = 20_000
        c.setRequestProperty("User-Agent", "MegaOpen-Companion")
        c.setRequestProperty("Accept", accept)
        c.instanceFollowRedirects = true
        if (c.responseCode !in 200..299) throw IOException("HTTP ${c.responseCode}")
        return c
    }

    private fun text(url: String) = get(url, "application/vnd.github+json").let { c ->
        try {
            c.inputStream.bufferedReader().use { it.readText() }
        } finally {
            c.disconnect()
        }
    }

    private fun release(o: JSONObject): Release? {
        if (o.optBoolean("draft")) return null
        val tag = o.optString("tag_name")
        if (order(tag) == null) return null
        val assets = o.optJSONArray("assets") ?: return null
        var apk: JSONObject? = null
        var sums: String? = null
        for (i in 0 until assets.length()) {
            val a = assets.getJSONObject(i)
            val n = a.optString("name")
            // The new name first; a release with only the old one still updates.
            if (n.startsWith("megaopen-companion-") && n.endsWith(".apk")) apk = a
            else if (apk == null && n.startsWith("megaopen-voice-") && n.endsWith(".apk")) apk = a
            if (n == "SHA256SUMS") sums = a.optString("browser_download_url")
        }
        apk ?: return null
        return Release(tag, apk.optString("browser_download_url"), apk.optString("name"), sums)
    }

    /** The newest release with an APK on the channel, or null. Blocking. */
    fun newest(pre: Boolean): Release? {
        if (!pre) return release(JSONObject(text("$API/releases/latest")))
        val list = JSONArray(text("$API/releases?per_page=10"))
        var best: Release? = null
        for (i in 0 until list.length()) {
            val r = release(list.getJSONObject(i)) ?: continue
            if (best == null || cmp(order(r.tag)!!, order(best.tag)!!) > 0) best = r
        }
        return best
    }

    fun isNewer(r: Release, mine: List<Int>?): Boolean = mine == null || cmp(order(r.tag)!!, mine) > 0

    /**
     * Download, check against SHA256SUMS, hand to the system installer. The
     * system then shows its own «Install?» (InstallReceiver starts it).
     */
    fun install(ctx: Context, prefs: Prefs, r: Release, progress: (Int) -> Unit) {
        val dir = File(ctx.cacheDir, "update").apply { mkdirs() }
        dir.listFiles()?.forEach { it.delete() }
        val f = File(dir, r.apkName)
        val md = MessageDigest.getInstance("SHA-256")
        val c = get(r.apkUrl, "application/octet-stream")
        try {
            val total = c.contentLength
            var got = 0L
            c.inputStream.use { inp ->
                f.outputStream().use { out ->
                    val buf = ByteArray(32 * 1024)
                    while (true) {
                        val n = inp.read(buf)
                        if (n < 0) break
                        out.write(buf, 0, n)
                        md.update(buf, 0, n)
                        got += n
                        if (total > 0) progress((got * 100 / total).toInt())
                    }
                }
            }
        } finally {
            c.disconnect()
        }
        val hex = md.digest().joinToString("") { "%02x".format(it) }
        if (r.sumsUrl != null) {
            val want = get(r.sumsUrl, "application/octet-stream").let { s ->
                try {
                    s.inputStream.bufferedReader().readLines()
                } finally {
                    s.disconnect()
                }
            }.firstOrNull { it.trim().endsWith(r.apkName) }?.substringBefore(' ')?.lowercase()
            if (want != null && want != hex) throw IOException("SHA-256 mismatch")
        }

        val pi = ctx.packageManager.packageInstaller
        val params = PackageInstaller.SessionParams(PackageInstaller.SessionParams.MODE_FULL_INSTALL)
        params.setAppPackageName(ctx.packageName)
        val id = pi.createSession(params)
        pi.openSession(id).use { s ->
            f.inputStream().use { inp ->
                s.openWrite("apk", 0, f.length()).use { out ->
                    inp.copyTo(out)
                    s.fsync(out)
                }
            }
            prefs.pendingTag = r.tag
            val flags = PendingIntent.FLAG_UPDATE_CURRENT or
                (if (Build.VERSION.SDK_INT >= 31) PendingIntent.FLAG_MUTABLE else 0)
            val done = PendingIntent.getBroadcast(ctx, id, Intent(ctx, InstallReceiver::class.java), flags)
            s.commit(done.intentSender)
        }
        f.delete()
    }
}
