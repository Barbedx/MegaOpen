package ua.megaopen.companion

import android.app.Application
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import ua.megaopen.companion.board.BoardConnection
import ua.megaopen.companion.link.BleLink
import ua.megaopen.companion.link.UsbLink
import ua.megaopen.companion.link.UsbSeen
import ua.megaopen.companion.media.MediaSender
import ua.megaopen.companion.media.MediaWatch
import ua.megaopen.companion.settings.Settings
import ua.megaopen.companion.update.Updater
import ua.megaopen.companion.voice.Speaker

class App : Application() {
    lateinit var graph: AppGraph
        private set

    override fun onCreate() {
        super.onCreate()
        graph = AppGraph(this)
    }
}

/** The app's singletons, built once (docs/design/companion-app.md); nothing else creates them. */
class AppGraph(app: Application) {
    val scope = CoroutineScope(SupervisorJob() + Dispatchers.Main.immediate)
    val settings = Settings(app, scope)
    val usb = UsbLink(app, scope)
    val ble = BleLink(app, scope)
    val usbSeen = UsbSeen(app, scope)
    val connection = BoardConnection(app, scope, settings, usb, ble)
    val media = MediaWatch(app, scope)
    val mediaSender = MediaSender(scope, media, connection)
    val updater = Updater(app)
    val speaker by lazy { Speaker(app) }

    private val _lastSaid = MutableStateFlow("")
    /** What the voice said last, for the home screen. */
    val lastSaid: StateFlow<String> = _lastSaid
    fun said(text: String) { _lastSaid.value = text }
}
