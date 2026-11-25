package ua.megaopen.companion.service

import android.content.Context
import android.graphics.Color
import android.graphics.PixelFormat
import android.graphics.drawable.GradientDrawable
import android.os.Build
import android.os.Handler
import android.os.Looper
import android.provider.Settings
import android.util.TypedValue
import android.view.Gravity
import android.view.WindowManager
import android.widget.TextView

/**
 * An alert as a window over whatever is on the screen (the map, the player):
 * the board's text, large, gone by itself after [SHOWN_MS]. A newer alert
 * takes the window over. It takes no touches and no focus.
 *
 * Needs "display over other apps" (Android asks once, from the Voice page)
 * and Android 8; without either nothing is shown. Main thread only.
 */
class AlertWindow(private val ctx: Context) {
    private val wm = ctx.getSystemService(Context.WINDOW_SERVICE) as WindowManager
    private val main = Handler(Looper.getMainLooper())
    private var view: TextView? = null
    private val hide = Runnable { remove() }

    fun show(text: String) {
        if (!allowed(ctx)) return
        main.removeCallbacks(hide)
        val v = view ?: make()?.also { view = it } ?: return
        v.text = text
        main.postDelayed(hide, SHOWN_MS)
    }

    fun remove() {
        main.removeCallbacks(hide)
        val v = view ?: return
        view = null
        try { wm.removeView(v) } catch (_: Exception) {}
    }

    private fun make(): TextView? {
        if (Build.VERSION.SDK_INT < 26) return null
        val pad = dp(20f)
        val v = TextView(ctx).apply {
            setTextColor(Color.WHITE)
            setTextSize(TypedValue.COMPLEX_UNIT_SP, 34f)
            gravity = Gravity.CENTER
            setPadding(pad * 2, pad, pad * 2, pad)
            background = GradientDrawable().apply {
                setColor(Color.argb(230, 20, 20, 24))
                cornerRadius = dp(18f).toFloat()
            }
        }
        val lp = WindowManager.LayoutParams(
            WindowManager.LayoutParams.WRAP_CONTENT,
            WindowManager.LayoutParams.WRAP_CONTENT,
            WindowManager.LayoutParams.TYPE_APPLICATION_OVERLAY,
            WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE or
                WindowManager.LayoutParams.FLAG_NOT_TOUCHABLE or
                WindowManager.LayoutParams.FLAG_LAYOUT_IN_SCREEN,
            PixelFormat.TRANSLUCENT,
        ).apply {
            gravity = Gravity.TOP or Gravity.CENTER_HORIZONTAL
            y = dp(48f)
        }
        return try {
            wm.addView(v, lp)
            v
        } catch (_: Exception) {
            null      // the permission was taken back between the check and here
        }
    }

    private fun dp(x: Float) =
        TypedValue.applyDimension(TypedValue.COMPLEX_UNIT_DIP, x, ctx.resources.displayMetrics).toInt()

    companion object {
        const val SHOWN_MS = 2_500L

        /** The window can be shown: Android 8+ and "display over other apps" granted. */
        fun allowed(ctx: Context) = Build.VERSION.SDK_INT >= 26 && Settings.canDrawOverlays(ctx)
    }
}
