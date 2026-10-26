package ua.megaopen.companion.update

import android.app.PendingIntent
import android.content.Context
import android.content.Intent
import android.content.pm.PackageInstaller
import android.os.Build
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import org.json.JSONArray
import ua.megaopen.companion.BuildConfig
import java.io.File
import java.io.IOException
import java.net.HttpURLConnection
import java.net.URL
import java.security.MessageDigest

/**
 * The app updates itself from the same GitHub releases as the board. Android
 * asks the owner to confirm each install (InstallReceiver starts that
 * screen); no root, no silent update.
 */
class Updater(private val ctx: Context) {
    private val api = "https://api.github.com/repos/${BuildConfig.RELEASES_REPO}"

    /** The newest release with our APK; with [pre], an rc counts. */
    suspend fun newest(pre: Boolean): Releases.Release? = withContext(Dispatchers.IO) {
        Releases.newest(JSONArray(text("$api/releases?per_page=10")), pre)
    }

    /**
     * Download, check against SHA256SUMS, hand to the system installer.
     * [progress] gets 0..100. Returns when the system has taken it.
     */
    suspend fun install(r: Releases.Release, progress: (Int) -> Unit) = withContext(Dispatchers.IO) {
        val dir = File(ctx.cacheDir, "update").apply { mkdirs() }
        dir.listFiles()?.forEach { it.delete() }
        val f = File(dir, r.apkName)
        val md = MessageDigest.getInstance("SHA-256")
        val c = open(r.apkUrl, "application/octet-stream")
        try {
            val total = c.contentLengthLong
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
        val have = md.digest().joinToString("") { "%02x".format(it) }
        val sumsUrl = r.sumsUrl ?: throw IOException("no SHA256SUMS in the release")
        val want = Releases.sumFor(text(sumsUrl), r.apkName) ?: throw IOException("SHA256SUMS has no line for ${r.apkName}")
        if (want != have) throw IOException("SHA-256 does not match")

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
            val flags = PendingIntent.FLAG_UPDATE_CURRENT or (if (Build.VERSION.SDK_INT >= 31) PendingIntent.FLAG_MUTABLE else 0)
            val done = PendingIntent.getBroadcast(ctx, id, Intent(ctx, InstallReceiver::class.java), flags)
            s.commit(done.intentSender)
        }
        f.delete()
    }

    private fun open(url: String, accept: String): HttpURLConnection {
        val c = URL(url).openConnection() as HttpURLConnection
        c.connectTimeout = 10_000
        c.readTimeout = 20_000
        c.setRequestProperty("User-Agent", "MegaOpen-Companion/" + BuildConfig.VERSION_NAME)
        c.setRequestProperty("Accept", accept)
        c.instanceFollowRedirects = true
        if (c.responseCode !in 200..299) {
            val code = c.responseCode
            c.disconnect()
            throw IOException("GitHub said $code")
        }
        return c
    }

    private fun text(url: String): String {
        val c = open(url, "application/vnd.github+json")
        try {
            return c.inputStream.bufferedReader().use { it.readText() }
        } finally {
            c.disconnect()
        }
    }
}
