package ua.megaopen.companion.service

import android.Manifest
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.content.Context
import android.content.Intent
import android.content.pm.PackageManager
import android.os.Build
import android.os.SystemClock
import androidx.core.app.NotificationCompat
import androidx.core.app.NotificationManagerCompat
import androidx.core.content.ContextCompat
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Job
import kotlinx.coroutines.delay
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import ua.megaopen.companion.R
import ua.megaopen.companion.alerts.Alert
import ua.megaopen.companion.alerts.Phrases
import ua.megaopen.companion.board.BoardConnection
import ua.megaopen.companion.settings.Settings
import ua.megaopen.companion.ui.MainActivity
import java.io.IOException

/**
 * An alert while its condition lasts. The board raises a reminder once, on
 * its rising edge; here, for an alert with a repeat (Голос › each alert), the
 * board is asked for the signal (`/api/signal`) every [CHECK_MS] and the
 * phrase is said again every N seconds until the signal is 0 again. Each
 * alert also has a notification, there while it lasts.
 *
 * A board without `/api/signal` (before 1.6.0) or one gone for good: the
 * alert is said once, as before.
 */
class Reminders(
    private val ctx: Context,
    private val scope: CoroutineScope,
    private val settings: Settings,
    private val connection: BoardConnection,
    private val say: (String) -> Unit,
) {
    private val jobs = HashMap<String, Job>()

    init {
        if (Build.VERSION.SDK_INT >= 26) {
            val ch = NotificationChannel(CHANNEL, ctx.getString(R.string.channel_alerts), NotificationManager.IMPORTANCE_HIGH)
            ch.setSound(null, null)          // the voice speaks; no chime over it
            ch.enableVibration(false)
            ctx.getSystemService(NotificationManager::class.java).createNotificationChannel(ch)
        }
    }

    /** An alert was said: its notification, and its repeat if it has one. Main thread. */
    fun onAlert(a: Alert, text: String) {
        val s = settings.values.value
        val k = Phrases.kind(a.sig)
        val repeatS = k?.let { s.repeatOf(it.sig, it.repeatS) } ?: 0
        jobs.remove(a.sig)?.cancel()
        show(a.sig, k?.label, text, lasting = repeatS > 0)
        if (repeatS <= 0) return
        jobs[a.sig] = scope.launch {
            var lastSaid = SystemClock.elapsedRealtime()
            var errors = 0
            try {
                while (isActive) {
                    delay(CHECK_MS)
                    when (still(a.sig)) {
                        false -> break
                        null -> if (++errors >= MAX_ERRORS) break else continue
                        true -> errors = 0
                    }
                    if (SystemClock.elapsedRealtime() - lastSaid >= repeatS * 1000L) {
                        val again = Phrases.say(a, settings.values.value) ?: break
                        say(again)
                        show(a.sig, k?.label, again, lasting = true)
                        lastSaid = SystemClock.elapsedRealtime()
                    }
                }
            } finally {
                clear(a.sig)
                if (jobs[a.sig] === coroutineContext[Job]) jobs.remove(a.sig)
            }
        }
    }

    fun stop() {
        jobs.values.forEach { it.cancel() }
        jobs.clear()
    }

    /** The signal still on: true; off: false; can't tell now: null; can't tell ever (no endpoint): false. */
    private suspend fun still(sig: String): Boolean? {
        val b = connection.board.value ?: return null
        return try {
            val r = b.request("GET", "/api/signal?name=$sig", timeoutMs = 5_000)
            when (r.code) {
                200 -> {
                    val j = org.json.JSONObject(r.text())
                    j.optBoolean("has") && j.optLong("value") != 0L
                }
                404 -> false
                else -> null
            }
        } catch (e: CancellationException) {
            throw e
        } catch (e: IOException) {
            null
        }
    }

    private fun show(sig: String, label: Int?, text: String, lasting: Boolean) {
        if (!settings.values.value.notify) return
        if (Build.VERSION.SDK_INT >= 33 &&
            ContextCompat.checkSelfPermission(ctx, Manifest.permission.POST_NOTIFICATIONS) != PackageManager.PERMISSION_GRANTED
        ) return
        val open = PendingIntent.getActivity(ctx, 0, Intent(ctx, MainActivity::class.java),
            PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT)
        val b = NotificationCompat.Builder(ctx, CHANNEL)
            .setSmallIcon(R.drawable.ic_alert)
            .setContentTitle(if (label != null) ctx.getString(label) else ctx.getString(R.string.app_name))
            .setContentText(text)
            .setPriority(NotificationCompat.PRIORITY_HIGH)
            .setCategory(NotificationCompat.CATEGORY_REMINDER)
            .setContentIntent(open)
            .setOnlyAlertOnce(true)
            .setAutoCancel(true)
        // A once-only alert does not stay for ever.
        if (!lasting) b.setTimeoutAfter(ONCE_SHOWN_MS)
        NotificationManagerCompat.from(ctx).notify(idOf(sig), b.build())
    }

    private fun clear(sig: String) = NotificationManagerCompat.from(ctx).cancel(idOf(sig))

    private fun idOf(sig: String) = 1000 + (sig.hashCode() and 0xFFFF)

    private companion object {
        const val CHANNEL = "alerts"
        const val CHECK_MS = 2_000L
        const val MAX_ERRORS = 30           // a minute without an answer: given up
        const val ONCE_SHOWN_MS = 60_000L
    }
}
