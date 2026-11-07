package ua.megaopen.companion.link

import android.Manifest
import android.annotation.SuppressLint
import android.bluetooth.BluetoothAdapter
import android.bluetooth.BluetoothDevice
import android.bluetooth.BluetoothGatt
import android.bluetooth.BluetoothGattCallback
import android.bluetooth.BluetoothGattCharacteristic
import android.bluetooth.BluetoothGattDescriptor
import android.bluetooth.BluetoothManager
import android.bluetooth.BluetoothProfile
import android.bluetooth.BluetoothStatusCodes
import android.bluetooth.le.ScanCallback
import android.bluetooth.le.ScanResult
import android.bluetooth.le.ScanSettings
import android.content.Context
import android.content.pm.PackageManager
import android.os.Build
import android.os.SystemClock
import androidx.core.content.ContextCompat
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.CompletableDeferred
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Job
import kotlinx.coroutines.TimeoutCancellationException
import kotlinx.coroutines.channels.awaitClose
import kotlinx.coroutines.currentCoroutineContext
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.callbackFlow
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import kotlinx.coroutines.withTimeout
import org.json.JSONObject
import java.io.IOException
import java.util.concurrent.LinkedBlockingQueue
import java.util.concurrent.TimeUnit
import kotlin.concurrent.thread

/** The runtime permissions Bluetooth needs, by Android version. */
object BlePermissions {
    /** To connect to a board already chosen. Before Android 12 nothing at run time. */
    val connect: Array<String> =
        if (Build.VERSION.SDK_INT >= 31) arrayOf(Manifest.permission.BLUETOOTH_CONNECT) else emptyArray()

    /** To look for one (the picker): before Android 12 a scan needs the location permission. */
    val scan: Array<String> =
        if (Build.VERSION.SDK_INT >= 31) arrayOf(Manifest.permission.BLUETOOTH_SCAN, Manifest.permission.BLUETOOTH_CONNECT)
        else arrayOf(Manifest.permission.ACCESS_FINE_LOCATION)

    fun has(ctx: Context, perms: Array<String>): Boolean =
        perms.all { ContextCompat.checkSelfPermission(ctx, it) == PackageManager.PERMISSION_GRANTED }

    fun adapter(ctx: Context): BluetoothAdapter? = ctx.getSystemService(BluetoothManager::class.java)?.adapter
}

/**
 * The board on Bluetooth LE: [BleBoard]'s service as a [Pipe] under a
 * [LinkSession], to the one board chosen in the settings (its address).
 *
 * While [use]d it keeps trying: the first try a direct connection, then
 * Android's background connection (autoConnect) for a window each, with a
 * pause between tries of 2 s doubling to 30 s, back to 2 s after a
 * connection that came up. [state] says what it is doing and, while it
 * waits, when the next try is.
 */
@SuppressLint("MissingPermission")   // every call is behind BlePermissions.has(connect)
class BleLink(private val ctx: Context, private val scope: CoroutineScope) : Link() {
    sealed interface State {
        /** Not used: the connection setting leaves Bluetooth out. */
        data object Off : State
        /** No board chosen yet (More › Connection › Bluetooth board). */
        data object NoBoard : State
        data object NoPermission : State
        data object AdapterOff : State
        data class Connecting(val name: String, val attempt: Int) : State
        /** The first write asked Android to pair: its dialog may be on screen. */
        data class Pairing(val name: String) : State
        data class Up(val name: String, val hello: JSONObject) : State
        /** The next try at [atMs] (SystemClock.elapsedRealtime). */
        data class Retry(val name: String, val atMs: Long, val why: String) : State
    }

    private val _state = MutableStateFlow<State>(State.Off)
    val state: StateFlow<State> = _state

    private var job: Job? = null
    private var target = ""

    /** Keep a connection to this board (address "" = none chosen). Calling it again with the same is a no-op. */
    fun use(address: String, name: String) {
        if (address.isEmpty()) {
            stop()
            _state.value = State.NoBoard
            return
        }
        if (job?.isActive == true && address == target) return
        stop()
        target = address
        job = scope.launch { keep(address, name.ifEmpty { address }) }
    }

    /** Disconnect and stop trying. */
    fun stop() {
        job?.cancel()
        job = null
        target = ""
        session?.close(STOPPED)
        _state.value = State.Off
    }

    private suspend fun keep(address: String, label: String) {
        var pause = 0L
        var attempt = 0
        while (currentCoroutineContext().isActive) {
            val adapter = BlePermissions.adapter(ctx)
            if (!BlePermissions.has(ctx, BlePermissions.connect)) { _state.value = State.NoPermission; delay(2_000); continue }
            if (adapter == null || !adapter.isEnabled) { _state.value = State.AdapterOff; delay(2_000); continue }
            attempt++
            _state.value = State.Connecting(label, attempt)
            var wasUp = false
            val why = try {
                once(adapter.getRemoteDevice(address), attempt > 1, label) { wasUp = true }
            } catch (e: IllegalArgumentException) {
                "bad address"
            }
            if (wasUp) { pause = 0; attempt = 0 }
            pause = BleBoard.backoff(pause)
            _stats.update { it.copy(lastError = why) }
            _state.value = State.Retry(label, SystemClock.elapsedRealtime() + pause, why)
            delay(pause)
        }
    }

    /** One connection, from the GATT to the session's end: why it ended. */
    private suspend fun once(dev: BluetoothDevice, auto: Boolean, label: String, upped: () -> Unit): String {
        val pipe = BlePipe(ctx, dev) { _state.value = State.Pairing(label) }
        pipe.open(auto)?.let { return it }
        val end = CompletableDeferred<String>()
        val s = LinkSession(scope, pipe, _stats, _events) { end.complete(it) }
        session = s
        try {
            val hello = s.open(HELLO_MS) ?: return end.await()
            upped()
            _state.value = State.Up(hello.optString("name").ifEmpty { label }, hello)
            return end.await()
        } finally {
            s.close(STOPPED)
            if (session === s) session = null
        }
    }

    /** Boards near by, as found (for the picker); the scan stops when collection does. */
    fun scan(known: String): Flow<Found> = callbackFlow {
        val scanner = BlePermissions.adapter(ctx)?.takeIf { it.isEnabled }?.bluetoothLeScanner
        if (scanner == null) { close(); return@callbackFlow }
        val cb = object : ScanCallback() {
            override fun onScanResult(callbackType: Int, r: ScanResult) {
                val rec = r.scanRecord
                val name = rec?.deviceName ?: try { r.device.name } catch (_: SecurityException) { null }
                val uuids = rec?.serviceUuids.orEmpty().map { it.uuid }
                if (BleBoard.mayBe(name, uuids, known)) {
                    trySend(Found(r.device.address, name.orEmpty(), r.rssi, r.device.bondState == BluetoothDevice.BOND_BONDED))
                }
            }
            override fun onScanFailed(errorCode: Int) { close(IOException("scan $errorCode")) }
        }
        scanner.startScan(null, ScanSettings.Builder().setScanMode(ScanSettings.SCAN_MODE_LOW_LATENCY).build(), cb)
        awaitClose { try { scanner.stopScan(cb) } catch (_: Exception) {} }
    }

    /** Paired devices that may be the board. */
    fun bonded(known: String): List<Found> =
        BlePermissions.adapter(ctx)?.bondedDevices.orEmpty()
            .filter { it.type != BluetoothDevice.DEVICE_TYPE_CLASSIC && BleBoard.mayBe(it.name, emptyList(), known) }
            .map { Found(it.address, it.name.orEmpty(), null, true) }

    data class Found(val address: String, val name: String, val rssi: Int?, val bonded: Boolean)

    companion object {
        const val STOPPED = "stopped"
        /** The board's HELLO after ours (written once the link is encrypted). */
        private const val HELLO_MS = 5_000L
    }
}

/**
 * The GATT side of one connection: connect, MTU, the service, notifications
 * on; then [write] in chunks of MTU-3 with response, one at a time, and the
 * notifications handed to the session. A write the board refuses for want of
 * encryption waits for Android's pairing (Just Works) and goes again.
 */
@SuppressLint("MissingPermission")   // BleLink made sure of BLUETOOTH_CONNECT
private class BlePipe(private val ctx: Context, private val dev: BluetoothDevice,
                      private val pairing: () -> Unit) : Pipe {
    @Volatile private var gatt: BluetoothGatt? = null
    @Volatile private var rxChar: BluetoothGattCharacteristic? = null
    @Volatile private var mtu = 23
    @Volatile private var closed = false
    @Volatile private var rx: ((ByteArray, Int) -> Unit)? = null
    @Volatile private var gone: ((String) -> Unit)? = null
    @Volatile private var discovering = false
    private val connected = CompletableDeferred<Unit>()
    private val ready = CompletableDeferred<Unit>()
    private val written = LinkedBlockingQueue<Int>()

    private val cb = object : BluetoothGattCallback() {
        override fun onConnectionStateChange(g: BluetoothGatt, status: Int, newState: Int) {
            if (newState == BluetoothProfile.STATE_CONNECTED) {
                connected.complete(Unit)
                if (!g.requestMtu(BleBoard.MTU)) discover(g)
            } else if (newState == BluetoothProfile.STATE_DISCONNECTED) {
                val why = "disconnected ($status)"
                connected.completeExceptionally(IOException(why))
                ready.completeExceptionally(IOException(why))
                written.offer(DEAD)
                gone?.invoke(why)
            }
        }

        override fun onMtuChanged(g: BluetoothGatt, mtu: Int, status: Int) {
            if (status == BluetoothGatt.GATT_SUCCESS) this@BlePipe.mtu = mtu
            discover(g)
        }

        override fun onServicesDiscovered(g: BluetoothGatt, status: Int) {
            val s = g.getService(BleBoard.SERVICE)
            val r = s?.getCharacteristic(BleBoard.RX)
            val t = s?.getCharacteristic(BleBoard.TX)
            if (status != BluetoothGatt.GATT_SUCCESS || r == null || t == null) {
                ready.completeExceptionally(IOException("no MegaOpen service"))
                return
            }
            rxChar = r
            g.setCharacteristicNotification(t, true)
            if (!notifyOn(g, t)) ready.completeExceptionally(IOException("notify"))
        }

        override fun onDescriptorWrite(g: BluetoothGatt, d: BluetoothGattDescriptor, status: Int) {
            when {
                status == BluetoothGatt.GATT_SUCCESS -> ready.complete(Unit)
                authError(status) -> thread(isDaemon = true) {
                    if (!(bonded() && notifyOn(g, d.characteristic))) ready.completeExceptionally(IOException("not paired"))
                }
                else -> ready.completeExceptionally(IOException("notify $status"))
            }
        }

        override fun onCharacteristicWrite(g: BluetoothGatt, c: BluetoothGattCharacteristic, status: Int) {
            written.offer(status)
        }

        override fun onCharacteristicChanged(g: BluetoothGatt, c: BluetoothGattCharacteristic, value: ByteArray) {
            if (c.uuid == BleBoard.TX) rx?.invoke(value, value.size)
        }

        @Deprecated("Android 12 and older call this one")
        @Suppress("DEPRECATION")
        override fun onCharacteristicChanged(g: BluetoothGatt, c: BluetoothGattCharacteristic) {
            if (Build.VERSION.SDK_INT < 33 && c.uuid == BleBoard.TX) c.value?.let { rx?.invoke(it, it.size) }
        }
    }

    private fun discover(g: BluetoothGatt) {
        if (discovering) return
        discovering = true
        if (!g.discoverServices()) ready.completeExceptionally(IOException("discover"))
    }

    @Suppress("DEPRECATION")
    private fun notifyOn(g: BluetoothGatt, t: BluetoothGattCharacteristic): Boolean {
        val d = t.getDescriptor(BleBoard.CCCD) ?: return false
        val v = BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE
        return if (Build.VERSION.SDK_INT >= 33) g.writeDescriptor(d, v) == BluetoothStatusCodes.SUCCESS
        else { d.value = v; g.writeDescriptor(d) }
    }

    /** Connect and get the notifications going: null when ready, else why not (closed then). */
    suspend fun open(auto: Boolean): String? {
        val g = dev.connectGatt(ctx, auto, cb, BluetoothDevice.TRANSPORT_LE) ?: return "connectGatt"
        gatt = g
        var ok = false
        try {
            withTimeout(if (auto) AUTO_WINDOW_MS else DIRECT_MS) { connected.await() }
            withTimeout(SETUP_MS) { ready.await() }
            ok = true
            return null
        } catch (e: TimeoutCancellationException) {
            return if (connected.isCompleted) "setup timeout" else "no answer"
        } catch (e: CancellationException) {
            throw e
        } catch (e: Exception) {
            return e.message ?: "gatt"
        } finally {
            if (!ok) close()
        }
    }

    override fun start(rx: (ByteArray, Int) -> Unit, gone: (String) -> Unit) {
        this.rx = rx
        this.gone = gone
        if (closed) gone("closed")
    }

    override fun write(b: ByteArray): Boolean {
        val g = gatt ?: return false
        val c = rxChar ?: return false
        var at = 0
        while (at < b.size) {
            val n = minOf(mtu - 3, b.size - at)
            val chunk = b.copyOfRange(at, at + n)
            var busy = 0
            var auth = 0
            while (true) {
                if (closed) return false
                written.clear()
                if (!startWrite(g, c, chunk)) {
                    // The stack is busy with another request (the CCCD, a pairing): a moment.
                    if (++busy > BUSY_TRIES) return false
                    Thread.sleep(20)
                    continue
                }
                val st = written.poll(WRITE_MS, TimeUnit.MILLISECONDS) ?: return false
                if (st == BluetoothGatt.GATT_SUCCESS) break
                if (authError(st) && auth++ < 2 && bonded()) continue
                return false
            }
            at += n
        }
        return true
    }

    @Suppress("DEPRECATION")
    private fun startWrite(g: BluetoothGatt, c: BluetoothGattCharacteristic, v: ByteArray): Boolean =
        if (Build.VERSION.SDK_INT >= 33) {
            g.writeCharacteristic(c, v, BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT) == BluetoothStatusCodes.SUCCESS
        } else {
            c.writeType = BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT
            c.value = v
            g.writeCharacteristic(c)
        }

    /** Wait for the bond (asking for it if Android has not yet): true once bonded. Blocking. */
    private fun bonded(): Boolean {
        if (dev.bondState == BluetoothDevice.BOND_BONDED) return true
        pairing()
        if (dev.bondState == BluetoothDevice.BOND_NONE && !dev.createBond()) return false
        val until = SystemClock.elapsedRealtime() + PAIR_MS
        var seenBonding = false
        while (!closed && SystemClock.elapsedRealtime() < until) {
            when (dev.bondState) {
                BluetoothDevice.BOND_BONDED -> return true
                BluetoothDevice.BOND_BONDING -> seenBonding = true
                BluetoothDevice.BOND_NONE -> if (seenBonding) return false   // refused or failed
            }
            Thread.sleep(200)
        }
        return false
    }

    override fun close() {
        if (closed) return
        closed = true
        written.offer(DEAD)
        gatt?.let { g ->
            try { g.disconnect() } catch (_: Exception) {}
            try { g.close() } catch (_: Exception) {}
        }
        gatt = null
    }

    private fun authError(status: Int) = status == GATT_INSUFFICIENT_AUTHENTICATION ||
        status == GATT_INSUFFICIENT_ENCRYPTION || status == GATT_AUTH_FAIL || status == GATT_INSUFFICIENT_AUTHORIZATION

    private companion object {
        const val GATT_INSUFFICIENT_AUTHENTICATION = 0x05
        const val GATT_INSUFFICIENT_AUTHORIZATION = 0x08
        const val GATT_INSUFFICIENT_ENCRYPTION = 0x0F
        const val GATT_AUTH_FAIL = 0x89
        const val DEAD = -1
        const val DIRECT_MS = 15_000L
        /** A background connection's window before the pause and the next try. */
        const val AUTO_WINDOW_MS = 30_000L
        /** MTU, services, notifications on; a pairing dialog may be in it. */
        const val SETUP_MS = 60_000L
        const val WRITE_MS = 5_000L
        const val PAIR_MS = 60_000L
        const val BUSY_TRIES = 100
    }
}
