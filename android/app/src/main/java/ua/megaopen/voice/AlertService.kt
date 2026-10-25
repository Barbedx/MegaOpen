package ua.megaopen.voice

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.Context
import android.content.Intent
import android.os.Build
import android.os.IBinder

/**
 * Polls the board's alert list and says each new alert once.
 *
 * The board numbers its alerts (seq) from 1 after each boot; `upMs` going
 * down, or `last` below what we have seen, is a reboot and we start again
 * from 0. The first answer after a (re)connect only sets the mark: what the
 * board showed before the app was listening is not said.
 */
class AlertService : Service() {
    companion object {
        @Volatile var status: String = ""
            private set
        @Volatile var host: String? = null
            private set
        @Volatile var lastSaid: String = ""
            private set

        /** An alert older than this (the app was away) is not said. */
        private const val STALE_MS = 30_000L
        private const val CHANNEL = "voice"
        private const val NOTE_ID = 1

        fun start(ctx: Context) {
            val i = Intent(ctx, AlertService::class.java)
            if (Build.VERSION.SDK_INT >= 26) ctx.startForegroundService(i) else ctx.startService(i)
        }

        fun stop(ctx: Context) {
            ctx.stopService(Intent(ctx, AlertService::class.java))
        }
    }

    private lateinit var prefs: Prefs
    private lateinit var speaker: Speaker
    private var worker: Thread? = null
    @Volatile private var quit = false
    private var shown = ""

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onCreate() {
        super.onCreate()
        prefs = Prefs(this)
        speaker = Speaker(this, prefs)
        if (Build.VERSION.SDK_INT >= 26) {
            val nm = getSystemService(NotificationManager::class.java)
            nm.createNotificationChannel(
                NotificationChannel(CHANNEL, getString(R.string.channel), NotificationManager.IMPORTANCE_LOW))
        }
        shown = getString(R.string.st_starting)
        startForeground(NOTE_ID, note(shown))
        registerReceiver(UsbBoard.detach, UsbBoard.filter)
        UsbBoard.onEvent(usbEvent)
    }

    // The board on USB pushes its alerts: nothing to poll.
    private val usbEvent: (org.json.JSONObject) -> Unit = { j ->
        if (j.optString("t") == "alert") {
            say(Board.Alert(j.optLong("seq"), 0, j.optString("sig"), j.optLong("value"), j.optString("text")))
        }
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        if (worker == null) {
            quit = false
            worker = Thread({ loop() }, "megaopen-alerts").also { it.start() }
        }
        return START_STICKY
    }

    override fun onDestroy() {
        quit = true
        UsbBoard.offEvent(usbEvent)
        try {
            unregisterReceiver(UsbBoard.detach)
        } catch (_: Exception) {
        }
        worker?.interrupt()
        worker = null
        speaker.shutdown()
        status = ""
        host = null
        super.onDestroy()
    }

    private fun loop() {
        var h: String? = null
        var since = -1L
        var lastUp = 0L
        var fails = 0
        while (!quit) {
            try {
                // The board on USB first (docs/design/usb-link.md): its alerts
                // come as events, WiFi is not asked.
                if (UsbBoard.connected || UsbBoard.open(this)) {
                    host = null
                    since = -1      // back on WiFi later: from the board's mark then
                    setStatus(getString(R.string.st_usb, UsbBoard.hello?.optString("name") ?: "MegaOpen"))
                    Thread.sleep(1000)
                    continue
                }
                val want = prefs.host
                if (want.isNotEmpty() && want != h) {
                    h = want
                    since = -1
                }
                if (h == null) {
                    setStatus(getString(R.string.st_searching))
                    h = Board.discover(this)
                    if (h == null) {
                        setStatus(getString(R.string.st_not_found))
                        Thread.sleep(10_000)
                        continue
                    }
                    since = -1
                }
                host = h
                var r = Board.alerts(h, if (since < 0) 0 else since)
                fails = 0
                if (since < 0) {
                    since = r.last
                } else {
                    if (r.upMs < lastUp || r.last < since) {
                        since = 0
                        r = Board.alerts(h, 0)
                    }
                    for (a in r.list) if (a.seq > since && a.agoMs < STALE_MS) say(a)
                    if (r.last > since) since = r.last
                }
                lastUp = r.upMs
                setStatus(getString(R.string.st_connected, h))
                Thread.sleep(prefs.intervalMs)
            } catch (e: InterruptedException) {
                break
            } catch (e: Exception) {
                fails++
                setStatus(getString(R.string.st_lost, h ?: "?"))
                // A found address may have moved (the hotspot's DHCP): look again.
                if (fails >= 3 && prefs.host.isEmpty()) {
                    h = null
                    fails = 0
                }
                try {
                    Thread.sleep(3_000)
                } catch (_: InterruptedException) {
                    break
                }
            }
        }
    }

    private fun say(a: Board.Alert) {
        val k = Phrases.kind(a.sig)
        val text = when {
            k != null -> if (prefs.alertOn(k.sig)) prefs.phrase(k) else return
            prefs.others -> a.text
            else -> return
        }.replace("{v}", a.value.toString())
        lastSaid = text
        speaker.say(text)
    }

    private fun setStatus(s: String) {
        status = s
        if (s == shown) return
        shown = s
        val nm = getSystemService(Context.NOTIFICATION_SERVICE) as NotificationManager
        nm.notify(NOTE_ID, note(s))
    }

    private fun note(text: String): Notification {
        val open = PendingIntent.getActivity(this, 0, Intent(this, MainActivity::class.java),
            PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT)
        val b = builder()
        return b.setSmallIcon(R.drawable.ic_stat)
            .setContentTitle(getString(R.string.app_name))
            .setContentText(text)
            .setContentIntent(open)
            .setOngoing(true)
            .build()
    }

    @Suppress("DEPRECATION")
    private fun builder(): Notification.Builder =
        if (Build.VERSION.SDK_INT >= 26) Notification.Builder(this, CHANNEL) else Notification.Builder(this)
}
