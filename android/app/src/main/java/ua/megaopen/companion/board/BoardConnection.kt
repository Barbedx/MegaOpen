package ua.megaopen.companion.board

import android.content.Context
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Job
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableSharedFlow
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.SharedFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.isActive
import kotlinx.coroutines.async
import kotlinx.coroutines.coroutineScope
import kotlinx.coroutines.launch
import kotlinx.coroutines.selects.select
import kotlinx.coroutines.withTimeoutOrNull
import ua.megaopen.companion.alerts.Alert
import ua.megaopen.companion.link.UsbLink
import ua.megaopen.companion.settings.Settings

/**
 * Which way in, now: the board on USB first (its alerts come as events), else
 * WiFi (an address typed in, or found), polled for its alerts. The screens and
 * the voice read [state], [board] and [alerts]; nothing else picks a way in.
 *
 * Alerts the board raised before we were listening are not said: the first
 * answer after a (re)connect only sets the mark. The board numbers its alerts
 * from 1 after each boot; `upMs` going down, or `last` below the mark, is a
 * reboot, and the mark starts again from 0.
 */
class BoardConnection(ctx: Context, private val scope: CoroutineScope,
                      private val settings: Settings, private val usb: UsbLink) {
    sealed interface State {
        data object Starting : State
        data object Searching : State
        data object NotFound : State
        data class Usb(val name: String, val fw: String) : State
        data class Wifi(val host: String, val fw: String) : State
        data class Lost(val host: String) : State
    }

    private val _state = MutableStateFlow<State>(State.Starting)
    val state: StateFlow<State> = _state
    private val _board = MutableStateFlow<Board?>(null)
    val board: StateFlow<Board?> = _board
    private val _alerts = MutableSharedFlow<Alert>(extraBufferCapacity = 16)
    val alerts: SharedFlow<Alert> = _alerts

    private val discovery = Discovery(ctx)
    private var job: Job? = null

    fun start() {
        if (job != null) return
        job = scope.launch {
            launch { usb.events.collect { j -> if (j.optString("t") == "alert") _alerts.emit(alertOf(j, 0)) } }
            loop()
        }
    }

    fun stop() {
        job?.cancel()
        job = null
        _board.value = null
        _state.value = State.Starting
    }

    private fun alertOf(j: org.json.JSONObject, ago: Long) =
        Alert(j.optLong("seq"), j.optString("sig"), j.optLong("value"), j.optString("text"), ago)

    private suspend fun loop() {
        var host: String? = null
        var since = -1L
        var lastUp = 0L
        var fails = 0
        var fw = ""
        var usbTries = 0
        while (scope.isActive) {
            // ---- USB first
            val plugged = usb.device() != null
            if (usb.up || (plugged && usb.connect())) {
                usbTries = 0
                val hello = (usb.state.value as? UsbLink.State.Up)?.hello
                val name = hello?.optString("name").orEmpty().ifEmpty { "MegaOpen" }
                if (_board.value !is UsbBoard) _board.value = UsbBoard(usb, name, hello?.optString("fw").orEmpty())
                _state.value = State.Usb(name, hello?.optString("fw").orEmpty())
                since = -1         // back on WiFi later: from the board's mark then
                delay(1_000)
                continue
            }
            // Plugged in but not answering: a passing hold (the head unit's own
            // USB scanner takes the interface for a moment, the board is busy)
            // clears in a second or two; try again before the WiFi search.
            if (plugged && ++usbTries <= USB_QUICK_TRIES) {
                delay(2_000)
                continue
            }
            usbTries = 0
            // ---- WiFi
            val typed = settings.values.value.host
            if (typed.isNotEmpty() && typed != host) { host = typed; since = -1 }
            if (host == null) {
                _board.value = null
                _state.value = State.Searching
                host = findOrTyped()
                if (host == null) {
                    _state.value = State.NotFound
                    retryOrTyped(10_000, null)
                    continue
                }
                since = -1
            }
            val b = (_board.value as? WifiBoard)?.takeIf { it.host == host } ?: WifiBoard(host).also {
                _board.value = it
                fw = it.probe(3_000).orEmpty()
                it.fw = fw
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
                _state.value = State.Wifi(host, fw)
                delay(POLL_MS)
            } catch (e: CancellationException) {
                throw e
            } catch (e: Exception) {
                fails++
                _state.value = State.Lost(host)
                // A found address may have moved (the hotspot's DHCP): look again.
                if (fails >= 3 && settings.values.value.host.isEmpty()) {
                    host = null
                    fails = 0
                }
                retryOrTyped(3_000, host)
            }
        }
    }

    // The search, or an address typed in while it runs: whichever comes first.
    private suspend fun findOrTyped(): String? = coroutineScope {
        val typed = async { settings.values.first { it.host.isNotEmpty() }.host }
        val found = async { discovery.find() }
        select { typed.onAwait { it }; found.onAwait { it } }.also { typed.cancel(); found.cancel() }
    }

    // A pause before trying again, cut short by a newly typed address: the
    // owner who types one sees it tried at once, not after the retry.
    private suspend fun retryOrTyped(ms: Long, current: String?) {
        withTimeoutOrNull(ms) { settings.values.first { it.host.isNotEmpty() && it.host != current } }
    }

    private companion object {
        const val POLL_MS = 1_500L
        /** USB attempts 2 s apart while the board is plugged in, before a round on WiFi. */
        const val USB_QUICK_TRIES = 3
        /** An alert older than this (we were away) is not said. */
        const val STALE_MS = 30_000L
    }
}
