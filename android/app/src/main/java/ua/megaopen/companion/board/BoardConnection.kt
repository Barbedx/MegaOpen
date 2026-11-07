package ua.megaopen.companion.board

import android.content.Context
import android.os.SystemClock
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Job
import kotlinx.coroutines.async
import kotlinx.coroutines.coroutineScope
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableSharedFlow
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.SharedFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.distinctUntilChanged
import kotlinx.coroutines.flow.drop
import kotlinx.coroutines.flow.filter
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.flow.map
import kotlinx.coroutines.flow.merge
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import kotlinx.coroutines.selects.select
import kotlinx.coroutines.withTimeoutOrNull
import org.json.JSONObject
import ua.megaopen.companion.alerts.Alert
import ua.megaopen.companion.link.BleLink
import ua.megaopen.companion.link.Link
import ua.megaopen.companion.link.UsbLink
import ua.megaopen.companion.settings.ConnMode
import ua.megaopen.companion.settings.Settings

/**
 * Which way in, now, in the order the connection setting allows: the board on
 * USB, else on Bluetooth (both are links: frames, the alerts come as events),
 * else WiFi (an address typed in, or found), polled for its alerts. The
 * screens and the voice read [state], [board] and [alerts]; nothing else
 * picks a way in. While no way is up, [state] says what is tried and when
 * the next try is; the links' own states say more ([UsbLink.state],
 * [BleLink.state]).
 *
 * Alerts the board raised before we were listening are not said: the first
 * answer after a (re)connect only sets the mark. The board numbers its alerts
 * from 1 after each boot; `upMs` going down, or `last` below the mark, is a
 * reboot, and the mark starts again from 0.
 */
class BoardConnection(ctx: Context, private val scope: CoroutineScope, private val settings: Settings,
                      private val usb: UsbLink, private val ble: BleLink) {
    sealed interface State {
        data object Starting : State
        /** Looking for the board on WiFi. */
        data object Searching : State
        /** Not on WiFi either; the next search at [retryAtMs] (SystemClock.elapsedRealtime). */
        data class NotFound(val retryAtMs: Long) : State
        data class Usb(val name: String, val fw: String) : State
        data class Ble(val name: String, val fw: String) : State
        data class Wifi(val host: String, val fw: String) : State
        data class Lost(val host: String, val retryAtMs: Long) : State
        /** USB only or Bluetooth only, and that way is not up (its link's state says why). */
        data class Waiting(val mode: ConnMode) : State
    }

    private val _state = MutableStateFlow<State>(State.Starting)
    val state: StateFlow<State> = _state
    private val _board = MutableStateFlow<Board?>(null)
    val board: StateFlow<Board?> = _board
    private val _alerts = MutableSharedFlow<Alert>(extraBufferCapacity = 16)
    val alerts: SharedFlow<Alert> = _alerts
    private val _boardName = MutableStateFlow("")
    /** The board's name as last met (HELLO, WiFi status), "" before: a hint for the Bluetooth picker. */
    val boardName: StateFlow<String> = _boardName
    private val kicks = MutableSharedFlow<Unit>(extraBufferCapacity = 1)

    private val discovery = Discovery(ctx)
    private var job: Job? = null

    fun start() {
        if (job != null) return
        job = scope.launch {
            launch { merge(usb.events, ble.events).collect { j -> if (j.optString("t") == "alert") _alerts.emit(alertOf(j, 0)) } }
            loop()
        }
    }

    fun stop() {
        job?.cancel()
        job = null
        ble.stop()
        _board.value = null
        _state.value = State.Starting
    }

    /** Something changed outside (the board plugged in): look again now, not after the pause. */
    fun poke() { kicks.tryEmit(Unit) }

    private fun alertOf(j: JSONObject, ago: Long) =
        Alert(j.optLong("seq"), j.optString("sig"), j.optLong("value"), j.optString("text"), ago)

    private suspend fun loop() {
        var host: String? = null
        var since = -1L
        var lastUp = 0L
        var fails = 0
        var fw = ""
        var usbTries = 0
        while (scope.isActive) {
            val s = settings.values.value
            val mode = s.connMode
            // ---- USB first
            if (mode.usb) {
                val plugged = usb.device() != null
                if (usb.up || (plugged && usb.connect())) {
                    usbTries = 0
                    ble.stop()          // one link at a time: the board counts each as an app
                    onLink(usb, Board.Via.Usb)
                    since = -1          // back on WiFi later: from the board's mark then
                    delay(1_000)
                    continue
                }
                // Plugged in but not answering: a passing hold (the head unit's own
                // USB scanner takes the interface for a moment, the board is busy)
                // clears in a second or two; try again before the other ways.
                if (plugged && ++usbTries <= USB_QUICK_TRIES) {
                    delay(2_000)
                    continue
                }
                usbTries = 0
            } else if (usb.state.value !is UsbLink.State.Off) {
                usb.close(UsbLink.OFF)
            }
            // ---- Bluetooth: its link keeps trying by itself, with its own pauses
            if (mode.ble) {
                ble.use(s.bleAddress, s.bleName)
                if (ble.up) {
                    onLink(ble, Board.Via.Ble)
                    since = -1
                    delay(1_000)
                    continue
                }
            } else if (ble.state.value !is BleLink.State.Off) {
                ble.stop()
            }
            if (!mode.wifi) {
                _board.value = null
                _state.value = State.Waiting(mode)
                pause(1_000, s.host)
                continue
            }
            // ---- WiFi
            val typed = s.host
            if (typed.isNotEmpty() && typed != host) { host = typed; since = -1 }
            if (host == null) {
                _board.value = null
                _state.value = State.Searching
                when (val f = findOrTyped()) {
                    is Find.Host -> { host = f.host; since = -1 }
                    Find.Interrupted -> continue
                    Find.None -> {
                        _state.value = State.NotFound(SystemClock.elapsedRealtime() + SEARCH_PAUSE_MS)
                        pause(SEARCH_PAUSE_MS, null)
                        continue
                    }
                }
            }
            val h = host ?: continue
            val b = (_board.value as? WifiBoard)?.takeIf { it.host == h } ?: WifiBoard(h).also {
                _board.value = it
                fw = it.probe(3_000).orEmpty()
                it.fw = fw
                if (it.name.isNotEmpty()) _boardName.value = it.name
            }
            try {
                // t: this device's clock, the board's only source of the date (its logs, its CAN files).
                var r = b.json("/api/alerts?since=${if (since < 0) 0 else since}&t=${System.currentTimeMillis()}")
                fails = 0
                if (since < 0) {
                    since = r.optLong("last")
                } else {
                    if (r.optLong("upMs") < lastUp || r.optLong("last") < since) {
                        since = 0
                        r = b.json("/api/alerts?since=0")
                    }
                    val list = r.optJSONArray("alerts")
                    for (i in 0 until (list?.length() ?: 0)) {
                        val o = list!!.getJSONObject(i)
                        if (o.optLong("seq") > since && o.optLong("agoMs") < STALE_MS) _alerts.emit(alertOf(o, o.optLong("agoMs")))
                    }
                    since = maxOf(since, r.optLong("last"))
                }
                lastUp = r.optLong("upMs")
                _state.value = State.Wifi(h, fw)
                delay(POLL_MS)
            } catch (e: CancellationException) {
                throw e
            } catch (e: Exception) {
                fails++
                _state.value = State.Lost(h, SystemClock.elapsedRealtime() + LOST_PAUSE_MS)
                // A found address may have moved (the hotspot's DHCP): look again.
                if (fails >= 3 && settings.values.value.host.isEmpty()) {
                    host = null
                    fails = 0
                }
                pause(LOST_PAUSE_MS, host)
            }
        }
    }

    private fun onLink(link: Link, via: Board.Via) {
        val hello = link.hello
        val name = hello?.optString("name").orEmpty().ifEmpty { "MegaOpen" }
        val fw = hello?.optString("fw").orEmpty()
        val cur = _board.value
        if (cur !is LinkBoard || cur.via != via || cur.label != name || cur.fw != fw) _board.value = LinkBoard(link, via, name, fw)
        _state.value = if (via == Board.Via.Usb) State.Usb(name, fw) else State.Ble(name, fw)
        _boardName.value = name
    }

    private sealed interface Find {
        data class Host(val host: String) : Find
        data object None : Find
        /** A link came up, or the setting changed: go round again. */
        data object Interrupted : Find
    }

    // The search, an address typed in, or a better way coming up: whichever first.
    private suspend fun findOrTyped(): Find = coroutineScope {
        val typed = async { Find.Host(settings.values.first { it.host.isNotEmpty() }.host) }
        val found = async { discovery.find()?.let { Find.Host(it) } ?: Find.None }
        val other = async { interruptions(null).first(); Find.Interrupted }
        select { typed.onAwait { it }; found.onAwait { it }; other.onAwait { it } }
            .also { typed.cancel(); found.cancel(); other.cancel() }
    }

    // A pause before trying again, cut short by a newly typed address, the
    // Bluetooth link coming up, the setting changing or a poke: the owner who
    // changes something sees it tried at once, not after the pause.
    private suspend fun pause(ms: Long, current: String?) {
        withTimeoutOrNull(ms) { interruptions(current).first() }
    }

    private fun interruptions(current: String?) = merge(
        settings.values.filter { it.host.isNotEmpty() && it.host != current }.map { },
        settings.values.map { Triple(it.connMode, it.bleAddress, it.host) }.distinctUntilChanged().drop(1).map { },
        ble.state.filter { it is BleLink.State.Up }.map { },
        kicks,
    )

    private companion object {
        const val POLL_MS = 1_500L
        const val SEARCH_PAUSE_MS = 10_000L
        const val LOST_PAUSE_MS = 3_000L
        /** USB attempts 2 s apart while the board is plugged in, before a round on the other ways. */
        const val USB_QUICK_TRIES = 3
        /** An alert older than this (we were away) is not said. */
        const val STALE_MS = 30_000L
    }
}
