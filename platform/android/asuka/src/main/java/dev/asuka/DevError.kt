package dev.asuka

import android.graphics.Color
import android.graphics.Typeface
import android.graphics.drawable.GradientDrawable
import android.util.TypedValue
import android.view.Gravity
import android.view.View
import android.view.ViewGroup
import android.widget.HorizontalScrollView
import android.widget.LinearLayout
import android.widget.TextView
import androidx.appcompat.view.ContextThemeWrapper
import androidx.core.view.ViewCompat
import androidx.core.view.WindowCompat
import androidx.core.view.WindowInsetsCompat
import androidx.core.widget.NestedScrollView
import com.google.android.material.button.MaterialButton
import com.google.android.material.color.MaterialColors
import com.google.android.material.divider.MaterialDivider
import com.google.android.material.textview.MaterialTextView
import java.nio.ByteBuffer
import java.nio.ByteOrder

/** A DevError command's arguments (ops/deverror.go). */
data class AsukaDevError(
    val flags: Int,
    val title: String,
    val message: String,
    /** The lines of the source where it went wrong, numbered, with a marker and a caret line; may be "". */
    val source: String,
    /** The stack, or the build's other errors; may be "". */
    val details: String,
) {
    /** It offers Reload: the app starts again. */
    val reload get() = flags and RELOAD != 0
    /** A build that failed, not an error of the app's. */
    val build get() = flags and BUILD != 0

    companion object {
        const val RELOAD = 1
        const val BUILD = 2
    }
}

/**
 * Decodes a DevError command's arguments: u8 flags, then the title, the
 * message, the source and the details, u32-length (little-endian) UTF-8
 * each. Null if it's cut short.
 */
fun decodeDevError(buf: ByteBuffer): AsukaDevError? {
    val b = buf.duplicate().order(ByteOrder.LITTLE_ENDIAN)
    if (!b.hasRemaining()) return null
    val flags = b.get().toInt() and 0xff
    fun str(): String? {
        if (b.remaining() < 4) return null
        val n = b.int
        if (n < 0 || n > b.remaining()) return null
        val a = ByteArray(n)
        b.get(a)
        return String(a, Charsets.UTF_8)
    }
    val title = str() ?: return null
    val message = str() ?: return null
    val source = str() ?: return null
    val details = str() ?: return null
    return AsukaDevError(flags, title, message, source, details)
}

/** What decodeDevError reads (ops.AppendDevError). */
fun encodeDevError(e: AsukaDevError): ByteArray =
    AsukaWriter().apply { u8(e.flags); string(e.title); string(e.message); string(e.source); string(e.details) }.bytes()

/**
 * asuka dev's error overlay (the DevError command, to the root): an error
 * nothing in the app caught, a module that didn't load, a build that
 * failed. Its own views, not the app's (the app's tree may be what's
 * broken), in a window of its own as big as the screen: a full-screen
 * dialog of the Activity's, which is above the Activity's window and the
 * dialogs shown before it (sheets, alerts; popovers are panels of the
 * windows they're anchored in). A window the app shows while it's up
 * (present) would go over it: it's shown again then, last, on top.
 *
 * The title, the message, the lines of the source where it went wrong and
 * the details (selectable, scrolling when long), with Dismiss and, when
 * the core offers it, Reload; back is Dismiss. They send DevErrorAction
 * to the root; the core removes it (DevErrorClear) or sends another.
 *
 * It's the process's, as the views are: an Activity that takes them over
 * shows it again, in the app's theme as it changes.
 */
class AsukaDevErrors(private val rt: AsukaRuntime) {
    private var error: AsukaDevError? = null
    /** The node it was sent to: the root. */
    private var root = 0
    private var dialog: androidx.activity.ComponentDialog? = null
    private var owner: AsukaActivity? = null
    /** Its texts and buttons, in order: what the automation reads and taps. */
    private val views = ArrayList<View>()

    /** Whether it shows (in a window of the current Activity's). */
    val showing get() = dialog?.isShowing == true

    /** DevError: shows e, replacing what it showed. */
    fun show(root: Int, e: AsukaDevError) {
        this.root = root
        error = e
        val d = dialog
        if (d != null && d.isShowing && owner === rt.activity) {
            d.setContentView(content(e), ViewGroup.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT))
            bars(d)
        } else present(animated = true)
    }

    /** DevErrorClear: removes it. */
    fun clear() {
        error = null
        views.clear()
        val d = dialog ?: return
        dialog = null
        owner = null
        d.dismiss()
    }

    /** An Activity took over the views: it shows there. */
    fun attached() {
        if (error != null && (!showing || owner !== rt.activity)) present(animated = false)
    }

    /** act is going: its window goes with it, the error stays for the next. */
    fun detached(act: AsukaActivity) {
        if (owner !== act) return
        dialog?.let { it.window?.setWindowAnimations(0); it.dismiss() }
        dialog = null
        owner = null
    }

    /** The theme changed (dark mode, the accent, the language): made again in it. */
    fun restyle() {
        if (showing) present(animated = false)
    }

    /** The app showed a window of its own: this one goes over it again. */
    fun raise() {
        if (showing) present(animated = false)
    }

    /** Its views while it shows (it covers the app's), else null. */
    fun shownViews(): List<View>? = if (showing) views.filter { it.isShown } else null

    /** Back, as the system's: Dismiss. False if it doesn't show. */
    fun back(): Boolean {
        val d = dialog ?: return false
        if (!d.isShowing) return false
        d.onBackPressedDispatcher.onBackPressed()
        return true
    }

    private fun send(a: Int) {
        if (error == null) return
        rt.send(AsukaEvents.devErrorAction(root, a.toLong()))
    }

    /** Shows it in a new window over the Activity's others, then takes away the one it was in. */
    private fun present(animated: Boolean) {
        val e = error ?: return
        val old = dialog
        val act = rt.activity
        if (act == null || act.isFinishing || act.isDestroyed) {
            detached(owner ?: return)
            return
        }
        val d = androidx.activity.ComponentDialog(rt.dialogContext(act), R.style.ThemeOverlay_Asuka_DevError)
        d.setContentView(content(e), ViewGroup.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT))
        d.setCancelable(false)
        d.onBackPressedDispatcher.addCallback(object : androidx.activity.OnBackPressedCallback(true) {
            override fun handleOnBackPressed() = send(AsukaDevErrorAction.DISMISS)
        })
        rt.directDialog(d)
        d.window?.let { w ->
            w.setLayout(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT)
            WindowCompat.setDecorFitsSystemWindows(w, false)
            @Suppress("DEPRECATION") run {
                w.statusBarColor = Color.TRANSPARENT
                w.navigationBarColor = Color.TRANSPARENT
            }
            w.isNavigationBarContrastEnforced = true
            if (!animated) w.setWindowAnimations(0)
        }
        bars(d)
        dialog = d
        owner = act
        d.show()
        if (old != null) {
            old.window?.setWindowAnimations(0)
            old.dismiss()
        }
    }

    /** The system bars' icons, light or dark as the app is. */
    private fun bars(d: android.app.Dialog) {
        val w = d.window ?: return
        val night = rt.isNight(rt.themed.resources.configuration)
        WindowCompat.getInsetsController(w, w.decorView).run {
            isAppearanceLightStatusBars = !night
            isAppearanceLightNavigationBars = !night
        }
    }

    /** Its views for e, in the app's theme. */
    private fun content(e: AsukaDevError): View {
        val ctx = rt.themed
        val dp = ctx.resources.displayMetrics.density
        fun px(v: Int) = (v * dp).toInt()
        fun color(attr: Int, fallback: Int) = MaterialColors.getColor(ctx, attr, fallback)
        fun appearance(attr: Int): Int {
            val tv = TypedValue()
            ctx.theme.resolveAttribute(attr, tv, true)
            return tv.resourceId
        }
        val night = rt.isNight(ctx.resources.configuration)
        val onSurface = color(com.google.android.material.R.attr.colorOnSurface, Color.BLACK)
        views.clear()

        val body = LinearLayout(ctx).apply { orientation = LinearLayout.VERTICAL }
        fun text(s: String, style: Int, bottom: Int) = MaterialTextView(ctx).apply {
            if (style != 0) setTextAppearance(style)
            text = s
            setTextIsSelectable(true)
            body.addView(this, LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT).apply { bottomMargin = px(bottom) })
            views += this
        }
        fun mono(t: TextView) = t.apply {
            typeface = Typeface.MONOSPACE
            setTextSize(TypedValue.COMPLEX_UNIT_SP, 12f)
            setLineSpacing(0f, 1.25f)
        }

        // The title: small and semibold, the error's red, or a build's orange.
        text(e.title, appearance(com.google.android.material.R.attr.textAppearanceLabelLarge), 8).apply {
            typeface = Typeface.create(Typeface.DEFAULT, 600, false)
            setTextColor(if (e.build) MaterialColors.harmonizeWithPrimary(ctx, if (night) 0xFFFFB86E.toInt() else 0xFF8B5000.toInt())
                else color(androidx.appcompat.R.attr.colorError, Color.RED))
            ViewCompat.setAccessibilityHeading(this, true)
        }
        text(e.message, appearance(com.google.android.material.R.attr.textAppearanceTitleLarge), 16).apply {
            setTextColor(onSurface)
        }
        // The source: monospaced, in a box, its lines as they are (scrolling sideways).
        if (e.source.isNotEmpty()) {
            val src = mono(MaterialTextView(ctx)).apply {
                text = e.source
                setTextColor(onSurface)
                setTextIsSelectable(true)
                setPadding(px(14), px(12), px(14), px(12))
            }
            views += src
            val box = HorizontalScrollView(ctx).apply {
                isFillViewport = true
                isHorizontalScrollBarEnabled = false
                background = GradientDrawable().apply {
                    cornerRadius = 12f * dp
                    setColor(color(com.google.android.material.R.attr.colorSurfaceContainerHigh, Color.LTGRAY))
                }
                clipToOutline = true
                addView(src, ViewGroup.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT))
            }
            body.addView(box, LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT).apply { bottomMargin = px(16) })
        }
        if (e.details.isNotEmpty()) mono(text(e.details, 0, 16)).setTextColor(color(com.google.android.material.R.attr.colorOnSurfaceVariant, Color.DKGRAY))

        val scroll = NestedScrollView(ctx).apply {
            isFillViewport = true
            clipToPadding = false
            addView(body, ViewGroup.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT))
        }

        val buttons = LinearLayout(ctx).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
        }
        fun button(label: String, overlay: Int, a: Int) = MaterialButton(ContextThemeWrapper(ctx, overlay)).apply {
            isAllCaps = false
            text = label
            setOnClickListener { send(a) }
            val lp = LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f)
            if (buttons.childCount > 0) lp.marginStart = px(12)
            buttons.addView(this, lp)
            views += this
        }
        button("Dismiss", R.style.ThemeOverlay_Asuka_TonalButton, AsukaDevErrorAction.DISMISS)
        if (e.reload) button("Reload", R.style.ThemeOverlay_Asuka_FilledButton, AsukaDevErrorAction.RELOAD)

        val root = LinearLayout(ctx).apply {
            orientation = LinearLayout.VERTICAL
            setBackgroundColor(color(com.google.android.material.R.attr.colorSurface, Color.WHITE))
            layoutDirection = if (rt.rtl) View.LAYOUT_DIRECTION_RTL else View.LAYOUT_DIRECTION_LTR
            accessibilityPaneTitle = e.title
            addView(scroll, LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, 0, 1f))
            addView(MaterialDivider(ctx), LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT))
            addView(buttons, LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT))
        }
        // Edge to edge: the bars and the cutout pad it.
        ViewCompat.setOnApplyWindowInsetsListener(root) { _, ins ->
            val i = ins.getInsets(WindowInsetsCompat.Type.systemBars() or WindowInsetsCompat.Type.displayCutout())
            val l = maxOf(px(20), i.left)
            val r = maxOf(px(20), i.right)
            body.setPadding(l, i.top + px(24), r, px(8))
            buttons.setPadding(l, px(12), r, i.bottom + px(12))
            WindowInsetsCompat.CONSUMED
        }
        return root
    }
}
