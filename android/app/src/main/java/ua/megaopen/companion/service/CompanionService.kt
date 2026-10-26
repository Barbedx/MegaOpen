package ua.megaopen.companion.service

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.content.pm.ServiceInfo
import android.hardware.usb.UsbManager
import android.os.Build
import androidx.core.app.NotificationCompat
import androidx.core.app.ServiceCompat
import androidx.core.content.ContextCompat
import androidx.lifecycle.LifecycleService
import androidx.lifecycle.lifecycleScope
import kotlinx.coroutines.flow.combine
import kotlinx.coroutines.flow.distinctUntilChanged
import kotlinx.coroutines.launch
import ua.megaopen.companion.App
import ua.megaopen.companion.R
import ua.megaopen.companion.alerts.Phrases
import ua.megaopen.companion.board.BoardConnection
import ua.megaopen.companion.ui.MainActivity

/**
 * The app at work while no screen is open: the connection to the board and
 * the voice. A foreground service of type connectedDevice (the board on USB,
 * or on the network); its notification says which way the board is reached.
 */
class CompanionService : LifecycleService() {
    private val graph get() = (application as App).graph

    // Unplugged: Android says so here, the link closes at once.
    private val detach = object : BroadcastReceiver() {
        override fun onReceive(context: Context, intent: Intent) {
            if (intent.action == UsbManager.ACTION_USB_DEVICE_DETACHED) graph.usb.close()
        }
    }

    override fun onCreate() {
        super.onCreate()
        val nm = getSystemService(NotificationManager::class.java)
        if (Build.VERSION.SDK_INT >= 26) {
            nm.createNotificationChannel(NotificationChannel(CHANNEL, getString(R.string.channel_name), NotificationManager.IMPORTANCE_LOW))
        }
        ServiceCompat.startForeground(this, NOTE_ID, note(getString(R.string.state_starting)),
            if (Build.VERSION.SDK_INT >= 29) ServiceInfo.FOREGROUND_SERVICE_TYPE_CONNECTED_DEVICE else 0)
        ContextCompat.registerReceiver(this, detach, IntentFilter(UsbManager.ACTION_USB_DEVICE_DETACHED),
            ContextCompat.RECEIVER_EXPORTED)

        graph.connection.start()
        reminders = Reminders(this, lifecycleScope, graph.settings, graph.connection) { text -> speak(text) }
        lifecycleScope.launch {
            graph.connection.alerts.collect { a ->
                val text = Phrases.say(a, graph.settings.values.value) ?: return@collect
                speak(text)
                reminders?.onAlert(a, text)
            }
        }
        lifecycleScope.launch {
            graph.connection.state.combine(graph.settings.values) { st, _ -> stateText(st) }
                .distinctUntilChanged()
                .collect { nm.notify(NOTE_ID, note(it)) }
        }
    }

    private var reminders: Reminders? = null

    // Said only with the voice on; the notification goes either way.
    private fun speak(text: String) {
        val s = graph.settings.values.value
        if (!s.voiceOn) return
        graph.said(text)
        graph.speaker.say(text, s.voiceLang, s.volume)
    }

    override fun onDestroy() {
        reminders?.stop()
        try { unregisterReceiver(detach) } catch (_: Exception) {}
        graph.connection.stop()
        super.onDestroy()
    }

    private fun stateText(s: BoardConnection.State): String = when (s) {
        BoardConnection.State.Starting -> getString(R.string.state_starting)
        BoardConnection.State.Searching -> getString(R.string.state_searching)
        BoardConnection.State.NotFound -> getString(R.string.state_not_found)
        is BoardConnection.State.Usb -> getString(R.string.state_usb, s.name)
        is BoardConnection.State.Wifi -> getString(R.string.state_wifi, s.host)
        is BoardConnection.State.Lost -> getString(R.string.state_lost, s.host)
    }

    private fun note(text: String): Notification {
        val open = PendingIntent.getActivity(this, 0, Intent(this, MainActivity::class.java),
            PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT)
        return NotificationCompat.Builder(this, CHANNEL)
            .setSmallIcon(R.drawable.ic_stat)
            .setContentTitle(getString(R.string.app_name))
            .setContentText(text)
            .setContentIntent(open)
            .setOngoing(true)
            .setOnlyAlertOnce(true)
            .build()
    }

    companion object {
        private const val CHANNEL = "board"
        private const val NOTE_ID = 1

        fun start(ctx: Context) = ContextCompat.startForegroundService(ctx, Intent(ctx, CompanionService::class.java))
        fun stop(ctx: Context) = ctx.stopService(Intent(ctx, CompanionService::class.java))
    }
}
