package ua.megaopen.voice

import android.Manifest
import android.app.Activity
import android.content.Intent
import android.content.pm.PackageManager
import android.net.Uri
import android.provider.Settings
import android.graphics.Typeface
import android.os.Build
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.text.Editable
import android.text.InputType
import android.text.TextWatcher
import android.view.View
import android.widget.Button
import android.widget.CheckBox
import android.widget.EditText
import android.widget.LinearLayout
import android.widget.RadioButton
import android.widget.RadioGroup
import android.widget.ScrollView
import android.widget.SeekBar
import android.widget.Switch
import android.widget.TextView
import android.widget.Toast

/** The settings screen. Built in code: one screen, no layout files to keep in step. */
class MainActivity : Activity() {
    private lateinit var prefs: Prefs
    private lateinit var state: TextView
    private val ui = Handler(Looper.getMainLooper())
    private var preview: Speaker? = null
    private val tick = object : Runnable {
        override fun run() {
            state.text = stateText()
            ui.postDelayed(this, 1000)
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        prefs = Prefs(this)
        if (Build.VERSION.SDK_INT >= 33 &&
            checkSelfPermission(Manifest.permission.POST_NOTIFICATIONS) != PackageManager.PERMISSION_GRANTED
        ) {
            requestPermissions(arrayOf(Manifest.permission.POST_NOTIFICATIONS), 1)
        }
        setContentView(build())
        if (prefs.enabled) AlertService.start(this)
    }

    override fun onResume() {
        super.onResume()
        ui.post(tick)
    }

    override fun onPause() {
        ui.removeCallbacks(tick)
        super.onPause()
    }

    override fun onDestroy() {
        preview?.shutdown()
        super.onDestroy()
    }

    private fun speaker(): Speaker = preview ?: Speaker(this, prefs).also { preview = it }

    private fun stateText(): String {
        if (!prefs.enabled) return getString(R.string.st_off)
        val s = AlertService.status.ifEmpty { getString(R.string.st_starting) }
        val said = AlertService.lastSaid
        return if (said.isEmpty()) s else s + "\n" + getString(R.string.st_last, said)
    }

    private fun dp(v: Int) = (v * resources.displayMetrics.density).toInt()

    private fun build(): View {
        val col = LinearLayout(this)
        col.orientation = LinearLayout.VERTICAL
        col.setPadding(dp(16), dp(12), dp(16), dp(24))

        state = TextView(this).apply {
            textSize = 15f
            setPadding(0, 0, 0, dp(8))
        }
        col.addView(state)

        col.addView(Switch(this).apply {
            setText(R.string.voice_on)
            textSize = 18f
            isChecked = prefs.enabled
            setOnCheckedChangeListener { _, on ->
                prefs.enabled = on
                if (on) AlertService.start(this@MainActivity) else AlertService.stop(this@MainActivity)
                state.text = stateText()
            }
        })

        // ---- the board
        col.addView(title(R.string.h_board))
        val host = EditText(this).apply {
            setHint(R.string.host_hint)
            setText(prefs.host)
            isSingleLine = true
            inputType = InputType.TYPE_CLASS_TEXT or InputType.TYPE_TEXT_VARIATION_URI
            onText { prefs.host = it }
        }
        val find = Button(this).apply {
            setText(R.string.find)
            setOnClickListener { findBoard(this) }
        }
        col.addView(row(host, find))
        col.addView(note(R.string.host_note))

        // ---- the voice
        col.addView(title(R.string.h_voice))
        val langs = listOf("uk" to R.string.lang_uk, "en" to R.string.lang_en, "" to R.string.lang_sys)
        val group = RadioGroup(this).apply { orientation = RadioGroup.HORIZONTAL }
        for ((i, l) in langs.withIndex()) {
            group.addView(RadioButton(this).apply {
                id = 100 + i
                setText(l.second)
                isChecked = prefs.lang == l.first
            })
        }
        group.setOnCheckedChangeListener { _, id ->
            prefs.lang = langs[id - 100].first
            recreate()                          // the default phrases follow the voice's language
        }
        col.addView(group)
        col.addView(label(R.string.volume))
        col.addView(SeekBar(this).apply {
            max = 100
            progress = prefs.volume
            setOnSeekBarChangeListener(object : SeekBar.OnSeekBarChangeListener {
                override fun onProgressChanged(s: SeekBar, v: Int, fromUser: Boolean) {
                    if (fromUser) prefs.volume = v
                }
                override fun onStartTrackingTouch(s: SeekBar) {}
                override fun onStopTrackingTouch(s: SeekBar) = speaker().say(getString(R.string.sample))
            })
        })
        col.addView(Button(this).apply {
            setText(R.string.tts_settings)
            setOnClickListener {
                try {
                    startActivity(Intent("com.android.settings.TTS_SETTINGS"))
                } catch (_: Exception) {
                    toast(getString(R.string.no_tts_settings))
                }
            }
        })

        // ---- what is said
        col.addView(title(R.string.h_alerts))
        for (k in Phrases.KNOWN) {
            col.addView(CheckBox(this).apply {
                setText(k.label)
                isChecked = prefs.alertOn(k.sig)
                setOnCheckedChangeListener { _, on -> prefs.setAlertOn(k.sig, on) }
            })
            val phrase = EditText(this).apply {
                hint = k.default(prefs.lang)
                setText(prefs.ownPhrase(k.sig))
                isSingleLine = true
                onText { prefs.setOwnPhrase(k.sig, it) }
            }
            val play = Button(this).apply {
                text = "▶"
                setOnClickListener { speaker().say(prefs.phrase(k).replace("{v}", "1")) }
            }
            col.addView(row(phrase, play))
        }
        col.addView(CheckBox(this).apply {
            setText(R.string.a_other)
            isChecked = prefs.others
            setOnCheckedChangeListener { _, on -> prefs.others = on }
        })
        col.addView(note(R.string.phrase_note))

        // ---- the chain, end to end
        col.addView(title(R.string.h_test))
        col.addView(Button(this).apply {
            setText(R.string.test_board)
            setOnClickListener { testBoard() }
        })
        col.addView(note(R.string.test_note))

        col.addView(title(R.string.h_start))
        col.addView(CheckBox(this).apply {
            setText(R.string.autostart)
            isChecked = prefs.autostart
            setOnCheckedChangeListener { _, on -> prefs.autostart = on }
        })
        col.addView(note(R.string.start_note))

        // ---- updates, from the same GitHub releases as the board
        col.addView(title(R.string.h_update))
        updState = note(version())
        col.addView(updState)
        updButton = Button(this).apply {
            visibility = View.GONE
            setOnClickListener { installUpdate(this) }
        }
        col.addView(CheckBox(this).apply {
            setText(R.string.upd_pre)
            isChecked = prefs.pre
            setOnCheckedChangeListener { _, on ->
                prefs.pre = on
                found = null
                updButton.visibility = View.GONE
            }
        })
        val check = Button(this).apply {
            setText(R.string.upd_check)
            setOnClickListener { checkUpdate(this, quiet = false) }
        }
        col.addView(row(check, updButton))
        checkUpdate(check, quiet = true)

        return ScrollView(this).apply { addView(col) }
    }

    private lateinit var updState: TextView
    private lateinit var updButton: Button
    private var found: Updater.Release? = null

    private fun version(): String = getString(R.string.version, BuildConfigVersion.name(this)) +
        prefs.installedTag.let { if (it.isEmpty()) "" else " ($it)" }

    private fun checkUpdate(button: Button, quiet: Boolean) {
        button.isEnabled = false
        if (!quiet) updState.text = getString(R.string.upd_checking)
        val pre = prefs.pre
        Thread {
            var r: Updater.Release? = null
            var err: String? = null
            try {
                r = Updater.newest(pre)
            } catch (e: Exception) {
                err = e.message ?: e.javaClass.simpleName
            }
            val newer = r != null && Updater.isNewer(r, Updater.mine(this, prefs))
            runOnUiThread {
                button.isEnabled = true
                found = if (newer) r else null
                updState.text = when {
                    err != null -> if (quiet) version() else getString(R.string.upd_error, err)
                    newer -> version() + "\n" + getString(R.string.upd_available, r!!.tag)
                    else -> version() + "\n" + getString(R.string.upd_none)
                }
                updButton.visibility = if (newer) View.VISIBLE else View.GONE
                if (newer) updButton.text = getString(R.string.upd_install, r!!.tag)
            }
        }.start()
    }

    private fun installUpdate(button: Button) {
        val r = found ?: return
        if (Build.VERSION.SDK_INT >= 26 && !packageManager.canRequestPackageInstalls()) {
            toast(getString(R.string.upd_allow))
            try {
                startActivity(Intent(Settings.ACTION_MANAGE_UNKNOWN_APP_SOURCES, Uri.parse("package:$packageName")))
            } catch (_: Exception) {
            }
            return
        }
        button.isEnabled = false
        Thread {
            val err = try {
                Updater.install(this, prefs, r) { pct ->
                    runOnUiThread { updState.text = getString(R.string.upd_loading, pct) }
                }
                null
            } catch (e: Exception) {
                e.message ?: e.javaClass.simpleName
            }
            runOnUiThread {
                button.isEnabled = true
                updState.text = if (err == null) getString(R.string.upd_confirm) else getString(R.string.upd_error, err)
            }
        }.start()
    }

    private fun findBoard(button: Button) {
        button.isEnabled = false
        toast(getString(R.string.searching))
        Thread {
            val h = Board.discover(this)
            val fw = h?.let { Board.probe(it, 2000) }
            runOnUiThread {
                button.isEnabled = true
                toast(if (h != null) getString(R.string.found, h, fw ?: "?") else getString(R.string.not_found))
            }
        }.start()
    }

    private fun testBoard() {
        if (!prefs.enabled) {
            toast(getString(R.string.test_off))
            return
        }
        Thread {
            val msg = try {
                val h = AlertService.host ?: prefs.host.ifEmpty { null } ?: Board.discover(this)
                if (h == null) getString(R.string.not_found)
                else {
                    Board.test(h, "drive_no_lights")
                    getString(R.string.test_sent, h)
                }
            } catch (e: Exception) {
                getString(R.string.test_failed, e.message ?: e.javaClass.simpleName)
            }
            runOnUiThread { toast(msg) }
        }.start()
    }

    private fun toast(s: String) = Toast.makeText(this, s, Toast.LENGTH_LONG).show()

    private fun title(res: Int) = TextView(this).apply {
        setText(res)
        textSize = 19f
        setTypeface(typeface, Typeface.BOLD)
        setPadding(0, dp(20), 0, dp(4))
    }

    private fun label(res: Int) = TextView(this).apply {
        setText(res)
        setPadding(0, dp(8), 0, 0)
    }

    private fun note(res: Int): TextView = note(getString(res))

    private fun note(s: String): TextView = TextView(this).apply {
        text = s
        textSize = 13f
        alpha = 0.7f
        setPadding(0, dp(2), 0, dp(4))
    }

    private fun row(main: View, side: View) = LinearLayout(this).apply {
        orientation = LinearLayout.HORIZONTAL
        addView(main, LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f))
        addView(side, LinearLayout.LayoutParams(LinearLayout.LayoutParams.WRAP_CONTENT, LinearLayout.LayoutParams.WRAP_CONTENT))
    }

    private fun EditText.onText(f: (String) -> Unit) = addTextChangedListener(object : TextWatcher {
        override fun beforeTextChanged(s: CharSequence?, a: Int, b: Int, c: Int) {}
        override fun onTextChanged(s: CharSequence?, a: Int, b: Int, c: Int) {}
        override fun afterTextChanged(s: Editable?) = f(s?.toString() ?: "")
    })
}

/** The installed version, without generating BuildConfig. */
object BuildConfigVersion {
    fun name(a: Activity): String = try {
        a.packageManager.getPackageInfo(a.packageName, 0).versionName ?: "?"
    } catch (_: Exception) {
        "?"
    }
}
