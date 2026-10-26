package ua.megaopen.companion.update

import org.json.JSONArray
import org.json.JSONObject
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

class ReleasesTest {
    private fun rel(tag: String, pre: Boolean = false, apk: Boolean = true, draft: Boolean = false): JSONObject {
        val assets = JSONArray()
        if (apk) assets.put(JSONObject().put("name", "megaopen-companion-${tag.removePrefix("v").substringBefore("-rc")}.apk")
            .put("browser_download_url", "https://github.com/x/$tag.apk"))
        assets.put(JSONObject().put("name", "SHA256SUMS").put("browser_download_url", "https://github.com/x/$tag/SHA256SUMS"))
        return JSONObject().put("tag_name", tag).put("prerelease", pre).put("draft", draft).put("assets", assets)
    }

    @Test fun orderPutsAFinalAfterItsRcs() {
        val rc2 = Releases.order("v1.6.0-rc2")!!
        val fin = Releases.order("v1.6.0")!!
        assertTrue(Releases.compare(fin, rc2) > 0)
        assertTrue(Releases.compare(Releases.order("v1.6.0-rc10")!!, rc2) > 0)
        assertTrue(Releases.compare(Releases.order("1.10.0")!!, Releases.order("v1.9.9")!!) > 0)
        assertNull(Releases.order("latest"))
        assertNull(Releases.order("v1.6"))
    }

    @Test fun newestSkipsRcsDraftsAndReleasesWithoutTheApp() {
        val list = JSONArray().put(rel("v1.6.0-rc1", pre = true)).put(rel("v1.5.4", apk = false))
            .put(rel("v1.5.9", draft = true)).put(rel("v1.5.3"))
        assertEquals("v1.5.3", Releases.newest(list, pre = false)!!.tag)
        assertEquals("v1.6.0-rc1", Releases.newest(list, pre = true)!!.tag)
        assertNull(Releases.newest(JSONArray().put(rel("v1.5.4", apk = false)), pre = true))
    }

    @Test fun anRcInstallSeesItsFinalAndTheNextRcAsNewer() {
        val mine = Releases.mine("1.6.0", "v1.6.0-rc1")
        assertTrue(Releases.isNewer(Releases.parse(rel("v1.6.0-rc2", pre = true))!!, mine))
        assertTrue(Releases.isNewer(Releases.parse(rel("v1.6.0"))!!, mine))
        assertFalse(Releases.isNewer(Releases.parse(rel("v1.6.0-rc1", pre = true))!!, mine))
    }

    @Test fun anApkPutOnByHandKnowsItsRcFromTheBuild() {
        // No tag remembered (installed outside the app): the built-in one counts.
        val mine = Releases.mine("1.6.0", "", "v1.6.0-rc5")
        assertTrue(Releases.isNewer(Releases.parse(rel("v1.6.0-rc6", pre = true))!!, mine))
        assertTrue(Releases.isNewer(Releases.parse(rel("v1.6.0"))!!, mine))
        assertFalse(Releases.isNewer(Releases.parse(rel("v1.6.0-rc5", pre = true))!!, mine))
        // A remembered tag wins over the built-in one.
        val upd = Releases.mine("1.6.0", "v1.6.0", "v1.6.0-rc6")
        assertFalse(Releases.isNewer(Releases.parse(rel("v1.6.0"))!!, upd))
    }

    @Test fun aStaleTagIsIgnored() {
        // Put on by hand since: the tag is of another version, the version wins.
        val mine = Releases.mine("1.6.1", "v1.6.0-rc1")
        assertFalse(Releases.isNewer(Releases.parse(rel("v1.6.0"))!!, mine))
        assertTrue(Releases.isNewer(Releases.parse(rel("v1.6.2"))!!, mine))
    }

    @Test fun sumLine() {
        val sums = "aa11  megaopen-t2can-1.6.0.bin\nBB22  megaopen-companion-1.6.0.apk\n"
        assertEquals("bb22", Releases.sumFor(sums, "megaopen-companion-1.6.0.apk"))
        assertNull(Releases.sumFor(sums, "megaopen-companion-1.6.1.apk"))
    }
}
