package ua.megaopen.companion.service

import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.launch
import ua.megaopen.companion.App

/** The head unit started, or this app was updated: back to work, if the owner wants that. */
class BootReceiver : BroadcastReceiver() {
    override fun onReceive(context: Context, intent: Intent) {
        val g = (context.applicationContext as App).graph
        val done = goAsync()
        g.scope.launch {
            try {
                if (intent.action == Intent.ACTION_MY_PACKAGE_REPLACED) g.settings.settleTag()
                val s = g.settings.values.first { it.loaded }
                if (s.autostart) CompanionService.start(context)
            } catch (_: Exception) {
                // A start the system refuses: the app's next open starts it.
            } finally {
                done.finish()
            }
        }
    }
}
