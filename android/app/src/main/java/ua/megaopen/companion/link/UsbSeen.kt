package ua.megaopen.companion.link

import android.content.Context
import android.hardware.usb.UsbDevice
import android.hardware.usb.UsbManager
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import ua.megaopen.companion.BuildConfig

/**
 * Every USB device the head unit sees, not only the board: the diagnostics'
 * USB tester. Another device in the same cable and port (a phone, a flash
 * drive) tells a port that carries no data from a board the head unit will
 * not take. Android tells an app when a device goes (the service passes
 * ACTION_USB_DEVICE_DETACHED on) but not when one comes, so the list is read
 * once a second; a device that came and went between two readings is still
 * logged by its detach.
 */
class UsbSeen(ctx: Context, private val scope: CoroutineScope) {
    /** One device as Android lists it; `key` is its bus path, new on every plug. */
    data class Dev(val key: String, val vid: Int, val pid: Int, val name: String, val classes: List<Int>) {
        /** The board as UsbLink looks for it. */
        val ours get() = vid == BuildConfig.USB_VID && pid == BuildConfig.USB_PID
        /** An ESP32-S3 on its built-in USB (the ROM's serial/JTAG): the firmware's own USB never came up. */
        val rom get() = vid == ESPRESSIF && pid == ESP_ROM_PID
        val id get() = "%04X:%04X".format(vid, pid)
        val classText get() = classes.joinToString(" ") { "%02X".format(it) }
    }

    data class Event(val atMs: Long, val came: Boolean, val dev: Dev)

    private val um = ctx.getSystemService(UsbManager::class.java)
    private val _devices = MutableStateFlow<List<Dev>>(emptyList())
    val devices: StateFlow<List<Dev>> = _devices
    private val _log = MutableStateFlow<List<Event>>(emptyList())
    /** The last LOG_MAX comings and goings, oldest first. */
    val log: StateFlow<List<Event>> = _log
    private var started = false

    fun start() {
        if (started) return
        started = true
        scope.launch {
            while (isActive) {
                read()
                delay(1000)
            }
        }
    }

    /** Android said this one went. */
    fun detached(d: UsbDevice) {
        val x = dev(d)
        if (_devices.value.none { it.key == x.key }) add(listOf(Event(System.currentTimeMillis(), false, x)))
        read()
    }

    private fun read() {
        val now = um.deviceList.values.map(::dev).sortedBy { it.key }
        val ev = diff(_devices.value, now, System.currentTimeMillis())
        _devices.value = now
        if (ev.isNotEmpty()) add(ev)
    }

    private fun add(ev: List<Event>) {
        _log.value = (_log.value + ev).takeLast(LOG_MAX)
    }

    private fun dev(d: UsbDevice): Dev {
        val name = listOfNotNull(text { d.manufacturerName }, text { d.productName }).joinToString(" ")
        val classes = ((if (d.deviceClass != 0) listOf(d.deviceClass) else emptyList()) +
            (0 until d.interfaceCount).map { d.getInterface(it).interfaceClass }).distinct()
        return Dev(d.deviceName, d.vendorId, d.productId, name, classes)
    }

    // A name the device did not give, or one Android will not read without the permission.
    private inline fun text(f: () -> String?): String? =
        try { f()?.trim()?.takeIf { it.isNotEmpty() } } catch (_: Exception) { null }

    companion object {
        const val ESPRESSIF = 0x303A
        const val ESP_ROM_PID = 0x1001
        const val LOG_MAX = 20

        /** What came and what went between two readings of the list. */
        fun diff(before: List<Dev>, now: List<Dev>, atMs: Long): List<Event> {
            val was = before.map { it.key }.toSet()
            val isNow = now.map { it.key }.toSet()
            return before.filter { it.key !in isNow }.map { Event(atMs, false, it) } +
                now.filter { it.key !in was }.map { Event(atMs, true, it) }
        }
    }
}
