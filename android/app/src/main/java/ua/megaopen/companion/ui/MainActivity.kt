package ua.megaopen.companion.ui

import android.Manifest
import android.content.Intent
import android.content.pm.PackageManager
import android.os.Build
import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.ui.Modifier
import androidx.core.content.ContextCompat
import ua.megaopen.companion.App
import ua.megaopen.companion.service.CompanionService

class MainActivity : ComponentActivity() {
    private val notifications = registerForActivityResult(ActivityResultContracts.RequestPermission()) { }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        if (Build.VERSION.SDK_INT >= 33 &&
            ContextCompat.checkSelfPermission(this, Manifest.permission.POST_NOTIFICATIONS) != PackageManager.PERMISSION_GRANTED
        ) notifications.launch(Manifest.permission.POST_NOTIFICATIONS)
        CompanionService.start(this)
        val graph = (application as App).graph
        setContent {
            CompanionTheme {
                Surface(Modifier.fillMaxSize(), color = MaterialTheme.colorScheme.background) { CompanionApp(graph) }
            }
        }
    }

    // The board plugged into USB opened us (USB_DEVICE_ATTACHED): the service connects.
    override fun onNewIntent(intent: Intent) {
        super.onNewIntent(intent)
        CompanionService.start(this)
    }
}
