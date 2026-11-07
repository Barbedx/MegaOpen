package ua.megaopen.companion.link

import android.app.PendingIntent
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.hardware.usb.UsbConstants
import android.hardware.usb.UsbDevice
import android.hardware.usb.UsbDeviceConnection
import android.hardware.usb.UsbEndpoint
import android.hardware.usb.UsbInterface
import android.hardware.usb.UsbManager
import android.os.Build
import androidx.core.content.ContextCompat
import kotlinx.coroutines.CompletableDeferred
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import kotlinx.coroutines.withTimeoutOrNull
import org.json.JSONObject
import ua.megaopen.companion.BuildConfig
import kotlin.concurrent.thread

/**
 * The board on the head unit's USB (docs/design/usb-link.md): the vendor
 * interface of its composite device, as a [Pipe] under a [LinkSession].
 * Here: finding the device, Android's permission, the interface and its
 * bulk endpoints. One board at a time.
 */
class UsbLink(private val ctx: Context, private val scope: CoroutineScope) : Link() {
    sealed interface State {
        data object Unplugged : State
        data object NoPermission : State
        data object Connecting : State
        data class Up(val hello: JSONObject) : State
        data class Failed(val why: String) : State
        /** Not used: the connection setting leaves USB out. */
        data object Off : State
    }

    private val _state = MutableStateFlow<State>(State.Unplugged)
    val state: StateFlow<State> = _state

    private val um = ctx.getSystemService(Context.USB_SERVICE) as UsbManager
    private val openLock = Mutex()
    @Volatile private var asked: String? = null

    /** The board, if it is plugged in: our VID/PID and a vendor interface. */
    fun device(): UsbDevice? =
        um.deviceList.values.firstOrNull { it.vendorId == BuildConfig.USB_VID && it.productId == BuildConfig.USB_PID && vendorItf(it) != null }

    private fun vendorItf(d: UsbDevice): UsbInterface? =
        (0 until d.interfaceCount).map { d.getInterface(it) }
            .firstOrNull { it.interfaceClass == UsbConstants.USB_CLASS_VENDOR_SPEC && it.endpointCount >= 2 }

    /**
     * Connect if the board is plugged in. Asks Android's permission the first
     * time for each plug (the dialog; «Always» there remembers it) and waits
     * for the answer. False: no board, no permission, or no HELLO.
     */
    suspend fun connect(): Boolean = openLock.withLock {
        session?.let { if (!it.closed) return@withLock it.up }
        val d = device() ?: run { _state.value = State.Unplugged; return@withLock false }
        if (!um.hasPermission(d)) {
            if (asked == d.deviceName) { _state.value = State.NoPermission; return@withLock false }
            asked = d.deviceName
            if (!askPermission(d)) { _state.value = State.NoPermission; return@withLock false }
        }
        _state.value = State.Connecting
        val f = vendorItf(d) ?: return@withLock fail("no vendor interface")
        val c = um.openDevice(d) ?: return@withLock fail("open")
        if (!c.claimInterface(f, true)) { c.close(); return@withLock fail("claim") }
        val bulk = (0 until f.endpointCount).map { f.getEndpoint(it) }.filter { it.type == UsbConstants.USB_ENDPOINT_XFER_BULK }
        val i = bulk.firstOrNull { it.direction == UsbConstants.USB_DIR_IN }
        val o = bulk.firstOrNull { it.direction == UsbConstants.USB_DIR_OUT }
        if (i == null || o == null) { c.close(); return@withLock fail("endpoints") }
        val s = LinkSession(scope, UsbPipe(c, f, i, o), _stats, _events, ::ended)
        session = s
        val hello = s.open(HELLO_MS) ?: return@withLock false
        _state.value = State.Up(hello)
        true
    }

    private fun fail(why: String): Boolean {
        _state.value = State.Failed(why)
        _stats.update { it.copy(lastError = why) }
        return false
    }

    private fun ended(why: String) {
        _state.value = when (why) {
            UNPLUGGED -> State.Unplugged
            OFF -> State.Off
            else -> State.Failed(why)
        }
        if (why != OFF) _stats.update { it.copy(lastError = why) }
    }

    private suspend fun askPermission(d: UsbDevice): Boolean {
        val answer = CompletableDeferred<Boolean>()
        val r = object : BroadcastReceiver() {
            override fun onReceive(context: Context, intent: Intent) {
                answer.complete(intent.getBooleanExtra(UsbManager.EXTRA_PERMISSION_GRANTED, false))
            }
        }
        ContextCompat.registerReceiver(ctx, r, IntentFilter(ACTION_PERMISSION), ContextCompat.RECEIVER_NOT_EXPORTED)
        try {
            val flags = if (Build.VERSION.SDK_INT >= 31) PendingIntent.FLAG_MUTABLE else 0
            val pi = PendingIntent.getBroadcast(ctx, 0, Intent(ACTION_PERMISSION).setPackage(ctx.packageName), flags)
            um.requestPermission(d, pi)
            return withTimeoutOrNull(60_000) { answer.await() } ?: false
        } finally {
            ctx.unregisterReceiver(r)
        }
    }

    /** Unplugged (Android said so), or not to be used ([OFF]): everything waiting fails. */
    fun close(why: String = UNPLUGGED) {
        if (why == UNPLUGGED) asked = null
        val s = session
        if (s != null && !s.closed) s.close(why) else ended(why)
    }

    /** The vendor interface's two bulk endpoints; the reader is its own thread. */
    private class UsbPipe(private val c: UsbDeviceConnection, private val itf: UsbInterface,
                          private val epIn: UsbEndpoint, private val epOut: UsbEndpoint) : Pipe {
        @Volatile private var open = true

        override fun start(rx: (ByteArray, Int) -> Unit, gone: (String) -> Unit) {
            thread(name = "usb-link-rx", isDaemon = true) {
                val buf = ByteArray(16384)
                while (open) {
                    val n = c.bulkTransfer(epIn, buf, buf.size, 250)
                    if (n > 0 && open) rx(buf, n)
                }
            }
        }

        override fun write(b: ByteArray): Boolean {
            var at = 0
            while (at < b.size) {
                if (!open) return false
                val w = c.bulkTransfer(epOut, b, at, b.size - at, 2000)
                if (w <= 0) return false
                at += w
            }
            return true
        }

        override fun close() {
            if (!open) return
            open = false
            try { c.releaseInterface(itf) } catch (_: Exception) {}
            c.close()
        }
    }

    companion object {
        const val ACTION_PERMISSION = "ua.megaopen.companion.USB_PERMISSION"
        const val UNPLUGGED = "unplugged"
        const val OFF = "off"
        private const val HELLO_MS = 3_000L
    }
}
