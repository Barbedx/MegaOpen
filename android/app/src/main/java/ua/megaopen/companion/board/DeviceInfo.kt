package ua.megaopen.companion.board

import android.content.Context
import android.os.Build
import androidx.core.content.pm.PackageInfoCompat
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.delay
import kotlinx.coroutines.withContext
import ua.megaopen.companion.BuildConfig
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale

/**
 * What this head unit is, told to the board once per connection (`POST
 * /api/app/info`, one `[app]` line each): the board's log is the only record
 * that leaves the car (its log push), and which firmware the head unit runs
 * decides how it reads the canbox protocol (docs/hu-decoder.md is one build's
 * decoder). A board from before 1.12.9 answers 404; nothing is lost.
 */
object DeviceInfo {
    /** The head unit's own apps: the launcher, the canbox decoder, its screens, the 360 camera. */
    val PACKAGES = listOf("com.dudu.autoui", "com.syu.ms", "com.syu.canbus", "com.syu.fourcamera2")
    const val MAX = 200

    fun device(displayW: Int, displayH: Int): String {
        val built = SimpleDateFormat("yyyy-MM-dd", Locale.ROOT).format(Date(Build.TIME))
        return "tablet: ${Build.MANUFACTURER} ${Build.MODEL} (${Build.DEVICE}), Android ${Build.VERSION.RELEASE} sdk ${Build.VERSION.SDK_INT}, " +
            "build ${Build.DISPLAY} of $built, screen ${displayW}x$displayH, companion ${BuildConfig.VERSION_NAME}"
    }

    fun apps(versions: List<Pair<String, String?>>): String =
        "tablet apps: " + versions.joinToString(", ") { (p, v) -> "$p ${v ?: "absent"}" }

    fun form(msg: String): ByteArray = ("msg=" + java.net.URLEncoder.encode(msg, "UTF-8")).toByteArray(Charsets.US_ASCII)

    fun lines(ctx: Context): List<String> {
        val dm = ctx.resources.displayMetrics
        val pm = ctx.packageManager
        val versions = PACKAGES.map { p ->
            p to try {
                val i = pm.getPackageInfo(p, 0)
                "${i.versionName} (${PackageInfoCompat.getLongVersionCode(i)})"
            } catch (e: Exception) {
                null
            }
        }
        return listOf(device(dm.widthPixels, dm.heightPixels), apps(versions), "tablet fingerprint: ${Build.FINGERPRINT}")
            .map { it.take(MAX) }
    }

    suspend fun send(ctx: Context, board: Board) = withContext(Dispatchers.IO) {
        try {
            for (l in lines(ctx)) {
                // A form, as the page posts to the board's plain routes (web/js/10-api.js formBody).
                val r = board.request("POST", "/api/app/info",
                    mapOf("X-MegaOpen" to "1", "Content-Type" to "application/x-www-form-urlencoded"), form(l))
                if (r.code != 200) break
                delay(400)   // the board takes one line every 300 ms
            }
        } catch (e: CancellationException) {
            throw e
        } catch (e: Exception) {
            // the link went away: the next connection says it again
        }
    }
}
