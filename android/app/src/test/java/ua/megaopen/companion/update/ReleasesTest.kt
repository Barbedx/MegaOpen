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

    private fun fwRel(tag: String, pre: Boolean, vararg names: String): JSONObject {
        val assets = JSONArray()
        for (n in names) assets.put(JSONObject().put("name", n).put("browser_download_url", "https://github.com/x/$tag/$n"))
        return JSONObject().put("tag_name", tag).put("prerelease", pre).put("assets", assets)
    }

    @Test fun theBoardsImageIsItsBuildsOtaFile() {
        val list = JSONArray()
            .put(fwRel("v1.12.6-rc1", true, "megaopen-t2can-1.12.6.bin", "megaopen-t2can-1.12.6-factory.bin",
                "megaopen-t2can_usbhid-1.12.6.bin", "SHA256SUMS"))
            .put(fwRel("v1.11.0", false, "megaopen-t2can-1.11.0.bin", "SHA256SUMS"))
            .put(fwRel("v1.12.9", false, "megaopen-companion-1.12.9.apk"))
        val rc = Releases.newestFirmware(list, "t2can", pre = true)!!
        assertEquals("v1.12.6-rc1", rc.tag)
        assertEquals("1.12.6", rc.version)
        assertEquals("megaopen-t2can-1.12.6.bin", rc.name)
        assertEquals("https://github.com/x/v1.12.6-rc1/SHA256SUMS", rc.sumsUrl)
        assertEquals("v1.11.0", Releases.newestFirmware(list, "t2can", pre = false)!!.tag)
        assertEquals("megaopen-t2can_usbhid-1.12.6.bin", Releases.newestFirmware(list, "t2can_usbhid", pre = true)!!.name)
        assertNull(Releases.newestFirmware(list, "direct", pre = true))
        assertNull(Releases.newestFirmware(list, "", pre = true))
    }

    @Test fun theUploadIsOneFilePart() {
        val body = String(BoardFlash.multipart("B", "fw.bin", byteArrayOf(65, 66)), Charsets.ISO_8859_1)
        assertEquals("--B\r\nContent-Disposition: form-data; name=\"file\"; filename=\"fw.bin\"\r\n" +
            "Content-Type: application/octet-stream\r\n\r\nAB\r\n--B--\r\n", body)
        assertEquals("b86fc6b051f63d73de262d4c34e3a0a9", BoardFlash.md5(byteArrayOf(65, 66)))
    }

    @Test fun theAppsLineNamesAMissingOne() {
        assertEquals("tablet apps: com.syu.ms 1.0 (190000000), com.syu.canbus absent",
            ua.megaopen.companion.board.DeviceInfo.apps(listOf("com.syu.ms" to "1.0 (190000000)", "com.syu.canbus" to null)))
    }

    @Test fun theInfoLineGoesAsAForm() {
        assertEquals("msg=tablet%3A+a+%26+b", String(ua.megaopen.companion.board.DeviceInfo.form("tablet: a & b")))
    }

    @Test fun sumLine() {
        val sums = "aa11  megaopen-t2can-1.6.0.bin\nBB22  megaopen-companion-1.6.0.apk\n"
        assertEquals("bb22", Releases.sumFor(sums, "megaopen-companion-1.6.0.apk"))
        assertNull(Releases.sumFor(sums, "megaopen-companion-1.6.1.apk"))
    }
}
