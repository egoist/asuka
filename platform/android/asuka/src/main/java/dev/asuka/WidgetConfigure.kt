package dev.asuka

import android.app.Activity
import android.appwidget.AppWidgetManager
import android.content.Intent
import android.os.Bundle
import android.text.InputType
import android.util.TypedValue
import android.view.Gravity
import android.view.View
import android.view.ViewGroup
import android.widget.LinearLayout
import android.widget.RadioGroup
import android.widget.ScrollView
import android.widget.TextView
import androidx.activity.enableEdgeToEdge
import androidx.appcompat.app.AppCompatActivity
import androidx.core.view.ViewCompat
import androidx.core.view.WindowInsetsCompat
import com.google.android.material.appbar.MaterialToolbar
import com.google.android.material.button.MaterialButton
import com.google.android.material.materialswitch.MaterialSwitch
import com.google.android.material.radiobutton.MaterialRadioButton
import com.google.android.material.textfield.TextInputEditText
import com.google.android.material.textfield.TextInputLayout

/**
 * A configurable widget's settings (asuka.toml's widgets.params): the
 * launcher opens it as the widget is added (Android 12 lets the user skip
 * it: the params' defaults) and when the user edits the widget. Each
 * param as Material 3 has it: options and entities (platform/widget's
 * setEntities) as radio buttons, a bool as a switch, a string or a number
 * as a text field. Saving keeps the widget's configuration and draws it.
 */
class AsukaWidgetConfigureActivity : AppCompatActivity() {
    private var id = AppWidgetManager.INVALID_APPWIDGET_ID
    private val read = LinkedHashMap<String, () -> String>()

    override fun onCreate(state: Bundle?) {
        enableEdgeToEdge() // the bars over its background, their icons for its light or dark
        super.onCreate(state)
        id = intent.getIntExtra(AppWidgetManager.EXTRA_APPWIDGET_ID, AppWidgetManager.INVALID_APPWIDGET_ID)
        // Backing out leaves the widget unplaced (or as it was).
        setResult(Activity.RESULT_CANCELED, Intent().putExtra(AppWidgetManager.EXTRA_APPWIDGET_ID, id))
        val info = AppWidgetManager.getInstance(this).getAppWidgetInfo(id)
        if (info == null || info.provider.packageName != packageName) {
            finish()
            return
        }
        val params = AsukaWidgetParam.of(this, info.provider)
        val values = AsukaWidgets.config(this, id, params)

        val dp = { v: Float -> TypedValue.applyDimension(TypedValue.COMPLEX_UNIT_DIP, v, resources.displayMetrics).toInt() }
        val root = LinearLayout(this).apply { orientation = LinearLayout.VERTICAL }
        val bar = MaterialToolbar(this).apply {
            title = info.loadLabel(packageManager)
            setNavigationIcon(R.drawable.asuka_sym_close)
            setNavigationContentDescription(android.R.string.cancel)
            setNavigationOnClickListener { finish() }
        }
        root.addView(bar)
        val form = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(dp(16f), dp(8f), dp(16f), dp(16f))
        }
        for (p in params) form.addView(field(p, values[p.name].orEmpty(), dp))
        val save = MaterialButton(this).apply {
            text = getString(android.R.string.ok)
            setOnClickListener { save() }
        }
        form.addView(save, LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT).apply { topMargin = dp(24f) })
        root.addView(ScrollView(this).apply { addView(form) }, LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, 0, 1f))
        ViewCompat.setOnApplyWindowInsetsListener(root) { v, insets ->
            val bars = insets.getInsets(WindowInsetsCompat.Type.systemBars() or WindowInsetsCompat.Type.ime())
            v.setPadding(bars.left, bars.top, bars.right, bars.bottom)
            insets
        }
        setContentView(root)
    }

    private fun heading(text: String, dp: (Float) -> Int) = TextView(this).apply {
        this.text = text
        setTextAppearance(com.google.android.material.R.style.TextAppearance_Material3_TitleSmall)
        setPadding(0, dp(16f), 0, dp(4f))
    }

    /** A param's field, its value read back as text. */
    private fun field(p: AsukaWidgetParam, value: String, dp: (Float) -> Int): View {
        val box = LinearLayout(this).apply { orientation = LinearLayout.VERTICAL }
        when (p.type) {
            "options", "entity" -> {
                box.addView(heading(p.title, dp))
                val choices = if (p.type == "options") p.options
                else AsukaWidgets.entities(this, p.entity).map { e ->
                    val sub = e.optString("subtitle")
                    e.optString("id") to (e.optString("title") + if (sub.isEmpty()) "" else " · $sub")
                }
                val group = RadioGroup(this)
                val ids = HashMap<Int, String>()
                for ((v, title) in choices) {
                    val b = MaterialRadioButton(this).apply {
                        this.id = View.generateViewId()
                        text = title
                    }
                    ids[b.id] = v
                    group.addView(b)
                    if (v == value) group.check(b.id)
                }
                if (choices.isEmpty()) {
                    box.addView(TextView(this).apply {
                        text = "—"
                        setTextAppearance(com.google.android.material.R.style.TextAppearance_Material3_BodyMedium)
                    })
                }
                box.addView(group)
                read[p.name] = { ids[group.checkedRadioButtonId] ?: value }
            }
            "bool" -> {
                val s = MaterialSwitch(this).apply {
                    text = p.title
                    isChecked = value == "true"
                    minHeight = dp(56f)
                }
                box.addView(s, LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT))
                read[p.name] = { s.isChecked.toString() }
            }
            else -> {
                val layout = TextInputLayout(this, null, com.google.android.material.R.attr.textInputOutlinedStyle).apply {
                    hint = p.title
                    if (p.prompt.isNotEmpty()) helperText = p.prompt
                }
                val edit = TextInputEditText(layout.context).apply {
                    setText(value)
                    inputType = when (p.type) {
                        "int" -> InputType.TYPE_CLASS_NUMBER or InputType.TYPE_NUMBER_FLAG_SIGNED
                        "float" -> InputType.TYPE_CLASS_NUMBER or InputType.TYPE_NUMBER_FLAG_SIGNED or InputType.TYPE_NUMBER_FLAG_DECIMAL
                        else -> InputType.TYPE_CLASS_TEXT
                    }
                    gravity = Gravity.START
                }
                layout.addView(edit)
                box.addView(layout, LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT).apply { topMargin = dp(16f) })
                read[p.name] = {
                    val t = edit.text?.toString()?.trim().orEmpty()
                    when (p.type) {
                        "int" -> t.toLongOrNull()?.toString() ?: if (t.isEmpty() && p.optional) "" else p.default
                        "float" -> t.toDoubleOrNull()?.let { if (it == Math.floor(it) && !it.isInfinite()) it.toLong().toString() else it.toString() } ?: if (t.isEmpty() && p.optional) "" else p.default
                        else -> t
                    }
                }
            }
        }
        return box
    }

    private fun save() {
        AsukaWidgets.saveConfig(this, id, read.mapValues { it.value() })
        val info = AppWidgetManager.getInstance(this).getAppWidgetInfo(id)
        if (info != null) {
            // Its provider draws it, configured.
            sendBroadcast(Intent(AppWidgetManager.ACTION_APPWIDGET_UPDATE).setComponent(info.provider)
                .putExtra(AppWidgetManager.EXTRA_APPWIDGET_IDS, intArrayOf(id)))
        }
        setResult(Activity.RESULT_OK, Intent().putExtra(AppWidgetManager.EXTRA_APPWIDGET_ID, id))
        finish()
    }
}
