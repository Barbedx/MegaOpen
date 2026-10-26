package ua.megaopen.companion.ui

import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.darkColorScheme
import androidx.compose.runtime.Composable
import androidx.compose.ui.graphics.Color

// Dark always: a head unit in a car, at night as much as by day.
private val Dark = darkColorScheme(
    primary = Color(0xFF64B5F6), onPrimary = Color(0xFF002F55),
    secondary = Color(0xFF80CBC4),
    background = Color(0xFF111418), surface = Color(0xFF181C21), surfaceVariant = Color(0xFF232830),
    onBackground = Color(0xFFE3E6EA), onSurface = Color(0xFFE3E6EA), onSurfaceVariant = Color(0xFFB4BAC2),
    error = Color(0xFFEF9A9A),
)

@Composable
fun CompanionTheme(content: @Composable () -> Unit) {
    MaterialTheme(colorScheme = Dark, content = content)
}
