package ua.megaopen.companion.update

import org.json.JSONArray
import org.json.JSONObject

/**
 * GitHub releases, as the app reads them (release.yml publishes
 * megaopen-companion-X.Y.Z.apk next to the firmware). Pure: unit-tested.
 *
 * An rc's APK carries its final's version, so the tag an install came from
 * is remembered (Settings.installedTag), as the board does (NVS fw/tag).
 */
object Releases {
    const val ASSET_PREFIX = "megaopen-companion-"

    class Release(val tag: String, val apkUrl: String, val apkName: String, val sumsUrl: String?)

    /** v1.6.0-rc2 -> [1, 6, 0, 2]; a final sorts after its rcs. Null: not a release tag. */
    fun order(tag: String): List<Int>? {
        val m = Regex("""^v?(\d+)\.(\d+)\.(\d+)(?:-rc(\d+))?$""").find(tag.trim()) ?: return null
        val (a, b, c, rc) = m.destructured
        return listOf(a.toInt(), b.toInt(), c.toInt(), if (rc.isEmpty()) Int.MAX_VALUE else rc.toInt())
    }

    fun compare(x: List<Int>, y: List<Int>): Int {
        for (i in 0 until 4) if (x[i] != y[i]) return x[i].compareTo(y[i])
        return 0
    }

    /** What this install is: the tag it came from when that is this version, else the version as a final. */
    fun mine(versionName: String, installedTag: String): List<Int>? {
        val own = order(versionName) ?: return order(installedTag)
        val tagged = order(installedTag)
        return if (tagged != null && tagged.subList(0, 3) == own.subList(0, 3)) tagged else own
    }

    fun isNewer(r: Release, mine: List<Int>?): Boolean = mine == null || compare(order(r.tag)!!, mine) > 0

    /** One release from GitHub's JSON, if it is a published release with our APK. */
    fun parse(o: JSONObject): Release? {
        if (o.optBoolean("draft")) return null
        val tag = o.optString("tag_name")
        if (order(tag) == null) return null
        val assets = o.optJSONArray("assets") ?: return null
        var apk: JSONObject? = null
        var sums: String? = null
        for (i in 0 until assets.length()) {
            val a = assets.getJSONObject(i)
            val n = a.optString("name")
            if (n.startsWith(ASSET_PREFIX) && n.endsWith(".apk")) apk = a
            if (n == "SHA256SUMS") sums = a.optString("browser_download_url")
        }
        apk ?: return null
        return Release(tag, apk.optString("browser_download_url"), apk.optString("name"), sums)
    }

    /** The newest release with our APK in GitHub's list; an rc only when [pre]. */
    fun newest(list: JSONArray, pre: Boolean): Release? {
        var best: Release? = null
        for (i in 0 until list.length()) {
            val o = list.getJSONObject(i)
            if (!pre && o.optBoolean("prerelease")) continue
            val r = parse(o) ?: continue
            if (best == null || compare(order(r.tag)!!, order(best.tag)!!) > 0) best = r
        }
        return best
    }

    /** The SHA-256 SHA256SUMS gives for [name], lower-case hex, or null. */
    fun sumFor(sums: String, name: String): String? =
        sums.lineSequence().map { it.trim() }.firstOrNull { it.endsWith(" $name") || it.endsWith("*$name") }
            ?.substringBefore(' ')?.lowercase()
}
