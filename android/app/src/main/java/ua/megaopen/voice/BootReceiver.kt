package ua.megaopen.voice

import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent

/** The head unit starts: listen again, if the owner left it on. */
class BootReceiver : BroadcastReceiver() {
    override fun onReceive(context: Context, intent: Intent) {
        val p = Prefs(context)
        if (p.enabled && p.autostart) {
            try {
                AlertService.start(context)
            } catch (_: Exception) {
                // A locked boot or a start the system refuses: the app's next open starts it.
            }
        }
    }
}
