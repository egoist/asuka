package dev.asuka

import android.graphics.Color
import android.graphics.drawable.GradientDrawable
import android.view.View
import java.lang.ref.WeakReference
import java.nio.ByteBuffer

// asuka dev's inspector's highlight (DevHighlight): a translucent box over
// the view the inspector selected, drawn in its window's ViewOverlay, in
// front of everything in the window and taking no touches.

/** The highlight showing, and the view whose overlay it's in. */
private var shownHighlight: Pair<WeakReference<View>, GradientDrawable>? = null

/** DevHighlight: a highlight over a rect of v (points from its top left;
 *  a width below 0: all of it), instead of the one showing. */
internal fun showDevHighlight(v: View?, args: ByteBuffer, density: Float) {
    clearDevHighlight()
    if (v == null || args.remaining() < 16 || !v.isAttachedToWindow) return
    var x = args.float * density
    var y = args.float * density
    var w = args.float * density
    var h = args.float * density
    if (w < 0 || h < 0) {
        x = 0f; y = 0f; w = v.width.toFloat(); h = v.height.toFloat()
    }
    val root = v.rootView
    val at = IntArray(2).also { v.getLocationInWindow(it) }
    val rootAt = IntArray(2).also { root.getLocationInWindow(it) }
    val left = at[0] - rootAt[0] + x
    val top = at[1] - rootAt[1] + y
    val d = GradientDrawable().apply {
        setColor(Color.argb(46, 10, 132, 255))
        setStroke(maxOf(1, density.toInt()), Color.rgb(10, 132, 255))
        setBounds(left.toInt(), top.toInt(), (left + w).toInt(), (top + h).toInt())
    }
    root.overlay.add(d)
    shownHighlight = WeakReference(root) to d
}

/** DevHighlightClear: removes the highlight showing. */
internal fun clearDevHighlight() {
    val (root, d) = shownHighlight ?: return
    root.get()?.overlay?.remove(d)
    shownHighlight = null
}
