package ua.megaopen.companion.link

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test
import ua.megaopen.companion.BuildConfig

class UsbSeenTest {
    private val board = UsbSeen.Dev("/dev/bus/usb/001/004", BuildConfig.USB_VID, BuildConfig.USB_PID, "MegaOpen", listOf(0xEF, 0x02, 0x0A, 0xFF))
    private val phone = UsbSeen.Dev("/dev/bus/usb/001/005", 0x05AC, 0x12A8, "Apple Inc. iPhone", listOf(0x06, 0xFF))
    private val rom = UsbSeen.Dev("/dev/bus/usb/001/006", 0x303A, 0x1001, "Espressif USB JTAG/serial debug unit", listOf(0xEF, 0x02))

    @Test fun cameAndWent() {
        val ev = UsbSeen.diff(listOf(board), listOf(phone), 7)
        assertEquals(2, ev.size)
        assertFalse(ev[0].came); assertEquals(board, ev[0].dev)
        assertTrue(ev[1].came); assertEquals(phone, ev[1].dev)
        assertTrue(ev.all { it.atMs == 7L })
    }

    @Test fun nothingNewNoEvents() {
        assertTrue(UsbSeen.diff(listOf(board, phone), listOf(board, phone), 1).isEmpty())
    }

    @Test fun replugIsANewDevice() {
        val again = board.copy(key = "/dev/bus/usb/001/007")
        val ev = UsbSeen.diff(listOf(board), listOf(again), 1)
        assertEquals(listOf(false, true), ev.map { it.came })
    }

    @Test fun theBoardAndTheRomAreTold() {
        assertTrue(board.ours); assertFalse(board.rom)
        assertTrue(rom.rom); assertFalse(rom.ours)
        assertFalse(phone.ours || phone.rom)
        assertEquals("05AC:12A8", phone.id)
        assertEquals("06 FF", phone.classText)
    }
}
