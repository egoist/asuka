package dev.asuka

import android.graphics.Color
import android.graphics.Typeface
import android.graphics.drawable.GradientDrawable
import android.util.TypedValue
import android.view.Gravity
import android.view.View
import android.view.ViewGroup
import android.widget.FrameLayout
import android.widget.TextView
import androidx.core.view.ViewCompat
import androidx.core.view.WindowInsetsCompat
import java.lang.ref.WeakReference
import java.nio.ByteBuffer

// asuka dev's performance overlay (DevPerf, to the root): the core's
// numbers (frames a second, flushes, the last flush's phases, memory) in a
// small box over the top end corner of the activity's window, below the
// status bar, in front of the app's views, taking no touches and hidden
// from TalkBack. The core sends its text twice a second while it's on,
// and none to remove it.

/** The box showing, and the window's view it's in. */
private var shownPerf: WeakReference<TextView>? = null

/** DevPerf: shows the text in the box, or removes it (no text). */
internal fun showDevPerf(rt: AsukaRuntime, args: ByteBuffer) {
    val bytes = ByteArray(args.remaining()).also { args.get(it) }
    val text = String(bytes, Charsets.UTF_8)
    val old = shownPerf?.get()
    val decor = rt.activity?.window?.decorView as? ViewGroup
    if (text.isEmpty() || decor == null) {
        (old?.parent as? ViewGroup)?.removeView(old)
        shownPerf = null
        return
    }
    val box = if (old != null && old.parent === decor) old else {
        (old?.parent as? ViewGroup)?.removeView(old)
        makePerfBox(decor).also {
            decor.addView(it, FrameLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT, Gravity.TOP or Gravity.END))
            shownPerf = WeakReference(it)
        }
    }
    if (box.text.toString() != text) box.text = text
    // Below the status bar and a cutout, at the end of the display's width.
    val d = decor.resources.displayMetrics.density
    val bars = ViewCompat.getRootWindowInsets(decor)?.getInsets(WindowInsetsCompat.Type.systemBars() or WindowInsetsCompat.Type.displayCutout())
    (box.layoutParams as FrameLayout.LayoutParams).apply {
        topMargin = (bars?.top ?: 0) + (4 * d).toInt()
        marginEnd = ((if (rt.rtl) bars?.left else bars?.right) ?: 0) + (8 * d).toInt()
        box.layoutParams = this
    }
    // In front of what the app added since.
    box.bringToFront()
}

private fun makePerfBox(decor: ViewGroup): TextView {
    val d = decor.resources.displayMetrics.density
    return TextView(decor.context).apply {
        typeface = Typeface.create(Typeface.MONOSPACE, Typeface.NORMAL)
        setTextSize(TypedValue.COMPLEX_UNIT_SP, 10f)
        setTextColor(Color.WHITE)
        setLineSpacing(0f, 1.05f)
        val px = (6 * d).toInt()
        val py = (4 * d).toInt()
        setPadding(px, py, px, py)
        background = GradientDrawable().apply {
            setColor(Color.argb(184, 0, 0, 0))
            cornerRadius = 6 * d
        }
        isClickable = false
        isFocusable = false
        importantForAccessibility = View.IMPORTANT_FOR_ACCESSIBILITY_NO_HIDE_DESCENDANTS
        elevation = 24 * d
        layoutDirection = decor.layoutDirection
    }
}
