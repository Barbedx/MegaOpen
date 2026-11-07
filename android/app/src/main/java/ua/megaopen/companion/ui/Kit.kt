package ua.megaopen.companion.ui

import android.content.Context
import android.os.SystemClock
import android.widget.Toast
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ColumnScope
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Card
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.produceState
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.unit.dp
import kotlinx.coroutines.delay
import ua.megaopen.companion.R
import ua.megaopen.companion.board.BoardConnection
import ua.megaopen.companion.link.BleLink
import ua.megaopen.companion.link.UsbLink

/** A screen: a scrolling column with the page's margins. */
@Composable
fun Page(content: @Composable ColumnScope.() -> Unit) {
    Column(
        Modifier.fillMaxWidth().verticalScroll(rememberScrollState()).padding(16.dp),
        verticalArrangement = Arrangement.spacedBy(12.dp),
        content = content,
    )
}

@Composable
fun Section(title: String, content: @Composable ColumnScope.() -> Unit) {
    Card(Modifier.fillMaxWidth()) {
        Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
            Text(title, style = MaterialTheme.typography.titleMedium)
            content()
        }
    }
}

@Composable
fun Note(text: String) {
    Text(text, style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
}

@Composable
fun SwitchRow(label: String, checked: Boolean, onChange: (Boolean) -> Unit, note: String? = null) {
    Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {
        Column(Modifier.weight(1f)) {
            Text(label, style = MaterialTheme.typography.bodyLarge)
            if (note != null) Note(note)
        }
        Spacer(Modifier.width(12.dp))
        Switch(checked = checked, onCheckedChange = onChange)
    }
}

fun toast(ctx: Context, text: String) = Toast.makeText(ctx, text, Toast.LENGTH_LONG).show()

/** The connection in one line: the headline on Home, the notification, Diagnostics. */
fun stateText(ctx: Context, s: BoardConnection.State): String = when (s) {
    BoardConnection.State.Starting -> ctx.getString(R.string.state_starting)
    BoardConnection.State.Searching -> ctx.getString(R.string.state_searching)
    is BoardConnection.State.NotFound -> ctx.getString(R.string.state_not_found)
    is BoardConnection.State.Usb -> ctx.getString(R.string.state_usb, s.name)
    is BoardConnection.State.Ble -> ctx.getString(R.string.state_ble, s.name)
    is BoardConnection.State.Wifi -> ctx.getString(R.string.state_wifi, s.host)
    is BoardConnection.State.Lost -> ctx.getString(R.string.state_lost, s.host)
    is BoardConnection.State.Waiting -> ctx.getString(if (s.mode.usb) R.string.state_waiting_usb else R.string.state_waiting_ble)
}

/** SystemClock.elapsedRealtime, once a second: for "next try in N s". */
@Composable
fun rememberClock(): Long {
    val now by produceState(SystemClock.elapsedRealtime()) {
        while (true) {
            value = SystemClock.elapsedRealtime()
            delay(1_000)
        }
    }
    return now
}

/** Whole seconds from [now] to [at], rounded up, never below 0. */
fun secondsTo(at: Long, now: Long): Int = ((at - now + 999) / 1000).coerceAtLeast(0).toInt()

fun usbText(ctx: Context, u: UsbLink.State): String = when (u) {
    UsbLink.State.Unplugged -> ctx.getString(R.string.diag_usb_unplugged)
    UsbLink.State.NoPermission -> ctx.getString(R.string.diag_usb_permission)
    UsbLink.State.Connecting -> ctx.getString(R.string.diag_usb_connecting)
    is UsbLink.State.Up -> ctx.getString(R.string.diag_usb_up, u.hello.optString("fw"), u.hello.optString("build"))
    is UsbLink.State.Failed -> ctx.getString(R.string.diag_usb_failed, u.why)
    UsbLink.State.Off -> ctx.getString(R.string.diag_usb_off)
}

fun bleText(ctx: Context, b: BleLink.State, now: Long): String = when (b) {
    BleLink.State.Off -> ctx.getString(R.string.ble_off)
    BleLink.State.NoBoard -> ctx.getString(R.string.ble_no_board)
    BleLink.State.NoPermission -> ctx.getString(R.string.ble_no_permission)
    BleLink.State.AdapterOff -> ctx.getString(R.string.ble_adapter_off)
    is BleLink.State.Connecting -> ctx.getString(R.string.ble_connecting, b.name, b.attempt)
    is BleLink.State.Pairing -> ctx.getString(R.string.ble_pairing, b.name)
    is BleLink.State.Up -> ctx.getString(R.string.ble_up, b.name, b.hello.optString("fw"))
    is BleLink.State.Retry -> ctx.getString(R.string.ble_retry, b.name, b.why, secondsTo(b.atMs, now))
}

/** WiFi's part while no way is up; null when WiFi is not what is being tried. */
fun wifiText(ctx: Context, s: BoardConnection.State, now: Long): String? = when (s) {
    BoardConnection.State.Searching -> ctx.getString(R.string.wifi_searching)
    is BoardConnection.State.NotFound -> ctx.getString(R.string.wifi_not_found, secondsTo(s.retryAtMs, now))
    is BoardConnection.State.Lost -> ctx.getString(R.string.wifi_lost, s.host, secondsTo(s.retryAtMs, now))
    else -> null
}
