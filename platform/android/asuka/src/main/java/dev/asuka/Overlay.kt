package dev.asuka

import android.content.Context
import android.graphics.Matrix
import android.view.MotionEvent
import android.view.View
import android.view.ViewGroup
import android.view.ViewTreeObserver
import android.widget.FrameLayout

/*
 * Overlay (RFC 0010): a layer of the core's views over a layout root, its
 * children placed by the core in the root's coordinates. Over the root
 * view it's the last child of the activity's frame, which holds the
 * root's view; over a sheet or a popover the last child (in drawing and
 * touch order: AsukaContainer keeps overlays over its other children) of
 * the sheet's or popover's screen, in its window. Either way it's the
 * root's size and at its origin, so the core's frames need nothing done
 * to them.
 *
 * Presses: the overlay's own view takes none unless it's modal; a press
 * on any view of it (a menu's surface too, not only its buttons) stays
 * with it. A press that begins in its root on no view of it is reported
 * as OutsidePress, before it reaches its target, by the window's
 * dispatchTouchEvent (the activity's, or the sheet's or popover's
 * screen's). Where its anchor is goes to the core as AnchorFrame, checked
 * before each frame is drawn (after a layout, a scroll, a resize, an
 * animation): what changed, once a frame.
 */

/** Where an overlay's anchor was last reported: AnchorFrame's payload when it changed, else null. */
class AsukaAnchorReport {
    private var known = false
    private val last = FloatArray(4)
    private var lastVisible = false

    /** The AnchorFrame payload for this frame and visibility, or null when it's what was reported last. */
    fun changed(x: Float, y: Float, w: Float, h: Float, visible: Boolean): ByteArray? {
        if (known && last[0] == x && last[1] == y && last[2] == w && last[3] == h && lastVisible == visible) return null
        known = true
        last[0] = x; last[1] = y; last[2] = w; last[3] = h
        lastVisible = visible
        return encodeAnchorFrame(x, y, w, h, visible)
    }

    /** Its anchor changed: the next is reported whatever it is. */
    fun forget() { known = false }

    /** The frame reported last (an anchor gone keeps it, not visible). */
    fun lastFrame(): FloatArray = last.copyOf()
}

/** AnchorFrame's payload (ops.AppendAnchorFrame): f32 x, y, w, h, then u8 1 if any of the anchor is visible. */
fun encodeAnchorFrame(x: Float, y: Float, w: Float, h: Float, visible: Boolean): ByteArray =
    AsukaWriter().apply { f32(x); f32(y); f32(w); f32(h); u8(if (visible) 1 else 0) }.bytes()

/** The layer's view. */
class AsukaOverlay(context: Context, private val rt: AsukaRuntime, val nodeID: Int) : AsukaContainer(context) {
    /** Modal: presses anywhere in the root reach only its views. */
    var modal = false
    /** The node its anchor is (0: none). */
    var anchor = 0
    val anchorReport = AsukaAnchorReport()
    /** Its InterceptBack's callback, while it has one. */
    var back: androidx.activity.OnBackPressedCallback? = null
    private var lastEnv: AsukaEnvironment? = null
    /** Whether the touch going on began on one of its views. */
    private var onView = false

    init {
        isClickable = false
        isFocusable = false
    }

    private val preDraw = ViewTreeObserver.OnPreDrawListener {
        rt.nodes[nodeID]?.let { rt.reportAnchor(it) }
        true
    }

    override fun onAttachedToWindow() {
        super.onAttachedToWindow()
        viewTreeObserver.addOnPreDrawListener(preDraw)
    }

    override fun onDetachedFromWindow() {
        viewTreeObserver.removeOnPreDrawListener(preDraw)
        super.onDetachedFromWindow()
    }

    override fun onLayout(changed: Boolean, l: Int, t: Int, r: Int, b: Int) {
        super.onLayout(changed, l, t, r, b)
        report()
    }

    /** Its environment: its root's size and safe area, the keyboard over it. */
    fun report() {
        val n = rt.nodes[nodeID] ?: return
        if (width == 0 || height == 0) return
        var e = rt.environment(this)
        val root = rt.nodes[(n.props[AsukaProp.Overlay.ROOT] as? Long)?.toInt() ?: 0]
        // As its sheet's or popover's screen reports (AsukaScreen.report).
        if (root?.kind == AsukaKind.SHEET && !root.fullScreen) e = e.copy(safeTop = 0f, safeBottom = 0f)
        if (root?.kind == AsukaKind.POPOVER) e = e.copy(safeTop = 0f, safeRight = 0f, safeBottom = 0f, safeLeft = 0f, keyboard = 0f)
        if (e == lastEnv) return
        lastEnv = e
        rt.send(AsukaEvents.environment(nodeID, e))
    }

    /** Whether (x, y), in its coordinates, is on one of its views (shown ones, wherever they draw). */
    fun onView(x: Float, y: Float): Boolean = hitsChild(this, x, y)

    override fun dispatchTouchEvent(e: MotionEvent): Boolean {
        if (e.actionMasked == MotionEvent.ACTION_DOWN) onView = onView(e.x, e.y)
        if (onView) {
            super.dispatchTouchEvent(e)
            return true // its views take a press, whether or not one acts on it
        }
        return modal
    }

    override fun dispatchHoverEvent(e: MotionEvent): Boolean = super.dispatchHoverEvent(e) || modal

    override fun dispatchGenericMotionEvent(e: MotionEvent): Boolean = super.dispatchGenericMotionEvent(e) || modal
}

private val inverse = Matrix()
private val point = FloatArray(2)

/** Whether (x, y), in g's coordinates, is on a shown view inside g (a child's children too: they draw outside it). */
private fun hitsChild(g: ViewGroup, x: Float, y: Float): Boolean {
    for (i in g.childCount - 1 downTo 0) {
        val c = g.getChildAt(i)
        if (c.visibility != View.VISIBLE) continue
        point[0] = x + g.scrollX - c.left
        point[1] = y + g.scrollY - c.top
        if (!c.matrix.isIdentity) {
            c.matrix.invert(inverse)
            inverse.mapPoints(point)
        }
        val lx = point[0]
        val ly = point[1]
        if (lx >= 0 && ly >= 0 && lx < c.width && ly < c.height) return true
        if (c is ViewGroup && hitsChild(c, lx, ly)) return true
    }
    return false
}

/** The view an overlay goes in: its sheet's or popover's screen, or the activity's frame. */
private fun AsukaRuntime.overlayHost(n: AsukaNode): ViewGroup? {
    val r = nodes[(n.props[AsukaProp.Overlay.ROOT] as? Long)?.toInt() ?: 0]
    if (r != null && (r.kind == AsukaKind.SHEET || r.kind == AsukaKind.POPOVER)) return r.view as? AsukaScreen
    return activity?.rootFrame
}

/** Puts an overlay's view over its root (again: the activity changed, or its root did), sized to it. */
fun AsukaRuntime.placeOverlay(n: AsukaNode) {
    val o = n.view as? AsukaOverlay ?: return
    val host = overlayHost(n) ?: return
    if (o.parent !== host) {
        (o.parent as? ViewGroup)?.removeView(o)
        host.addView(o, if (host is AsukaContainer) AsukaParams()
            else FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT))
        if (o.layoutDirection != View.LAYOUT_DIRECTION_LTR) o.layoutDirection = View.LAYOUT_DIRECTION_LTR // frames are physical
    }
    // Its size now, so its environment reaches the core before it lays the overlay out.
    if (host.width > 0 && (o.width != host.width || o.height != host.height)) {
        o.measure(View.MeasureSpec.makeMeasureSpec(host.width, View.MeasureSpec.EXACTLY), View.MeasureSpec.makeMeasureSpec(host.height, View.MeasureSpec.EXACTLY))
        o.layout(0, 0, host.width, host.height)
    }
    o.report()
    refreshModals()
}

/** An Overlay's own props; false: not one. */
fun AsukaRuntime.setOverlayProp(n: AsukaNode, prop: Int, v: Any): Boolean {
    val o = n.view as? AsukaOverlay ?: return false
    when (prop) {
        AsukaProp.Overlay.ROOT -> placeOverlay(n)
        AsukaProp.Overlay.ANCHOR -> {
            o.anchor = (v as Long).toInt()
            o.anchorReport.forget()
            o.post { reportAnchor(n) }
        }
        AsukaProp.Overlay.MODAL -> {
            o.modal = v as Boolean
            refreshModals()
        }
        else -> return false
    }
    return true
}

/** Present: over its root's other overlays, and first for Back among them. */
fun AsukaRuntime.presentOverlay(n: AsukaNode) {
    overlays.remove(n)
    overlays.add(n)
    placeOverlay(n)
    val o = n.view as? AsukaOverlay ?: return
    (o.parent as? ViewGroup)?.let { if (it.getChildAt(it.childCount - 1) !== o) it.bringChildToFront(o) }
    overlayBack(n)
    o.post { reportAnchor(n) }
}

/** The root's views changed activity (mount): the overlays over it go to its frame, in their order. */
fun AsukaRuntime.mountOverlays() {
    for (n in ArrayList(overlays)) {
        placeOverlay(n)
        val o = n.view as? AsukaOverlay ?: continue
        (o.parent as? ViewGroup)?.bringChildToFront(o)
        overlayBack(n)
    }
}

/** Destroy: it goes, with what it set (its back callback, what its being modal hid). */
fun AsukaRuntime.destroyOverlay(n: AsukaNode) {
    overlays.remove(n)
    val o = n.view as? AsukaOverlay ?: return
    o.back?.remove()
    o.back = null
    for (c in n.children) c.parent = null
    (o.parent as? ViewGroup)?.removeView(o)
    refreshModals()
}

/**
 * InterceptBack: the window's back (Android's back, its predictive
 * gesture) comes to the overlay as Back while it's on, ahead of the
 * screens' and sheets' (a callback added later goes first: an overlay
 * presented last is first). A popover's window has no dispatcher: its
 * dismiss asks [interceptingOverlay].
 */
fun AsukaRuntime.overlayBack(n: AsukaNode) {
    val o = n.view as? AsukaOverlay ?: return
    o.back?.remove()
    o.back = null
    if (!n.intercepts) return
    val root = nodes[(n.props[AsukaProp.Overlay.ROOT] as? Long)?.toInt() ?: 0]
    val dispatcher = when (root?.kind) {
        AsukaKind.SHEET -> (root.dialog as? androidx.activity.ComponentDialog)?.onBackPressedDispatcher
        AsukaKind.POPOVER -> null
        else -> activity?.onBackPressedDispatcher
    } ?: return
    val id = n.id
    val cb = object : androidx.activity.OnBackPressedCallback(true) {
        override fun handleOnBackPressed() { emit(id, AsukaEventKind.BACK) { AsukaEvents.back(id) } }
    }
    dispatcher.addCallback(cb)
    o.back = cb
}

/** The topmost overlay over root that takes back (InterceptBack), or null. */
fun AsukaRuntime.interceptingOverlay(root: Int): AsukaNode? =
    overlays.lastOrNull { it.intercepts && (it.props[AsukaProp.Overlay.ROOT] as? Long)?.toInt() == root }

/** Reports where an overlay's anchor is (AnchorFrame), in its root's coordinates, if that changed. */
fun AsukaRuntime.reportAnchor(n: AsukaNode) {
    val o = n.view as? AsukaOverlay ?: return
    if (o.anchor == 0 || nodes[n.id] !== n || !n.listens(AsukaEventKind.ANCHOR_FRAME) || !o.isAttachedToWindow) return
    val a = nodes[o.anchor]?.view
    val d = density
    val payload = if (a == null || !a.isAttachedToWindow || a.windowToken !== o.windowToken) {
        // Gone, or out of the window (a row scrolled far away): where it was, not visible.
        val f = o.anchorReport.lastFrame()
        o.anchorReport.changed(f[0], f[1], f[2], f[3], false)
    } else {
        val al = IntArray(2)
        val ol = IntArray(2)
        a.getLocationInWindow(al)
        o.getLocationInWindow(ol)
        val visible = a.isShown && a.getGlobalVisibleRect(android.graphics.Rect())
        o.anchorReport.changed((al[0] - ol[0]) / d, (al[1] - ol[1]) / d, a.width / d, a.height / d, visible)
    } ?: return
    send(AsukaEvents.anchorFrame(n.id, payload))
}

/**
 * A press beginning in a window: each overlay over a root there (inScope)
 * that it's in but on none of whose views gets OutsidePress, where it
 * began in the root's coordinates, before the press goes on. Reports
 * whether a modal one was among them: then nothing behind it may get
 * the press, though the overlay closes as it hears of it (its window
 * keeps the touch: [AsukaTouchGate]).
 */
fun AsukaRuntime.outsidePresses(e: MotionEvent, inScope: (AsukaOverlay) -> Boolean): Boolean {
    val action = e.actionMasked
    if (overlays.isEmpty() || (action != MotionEvent.ACTION_DOWN && action != MotionEvent.ACTION_POINTER_DOWN)) return false
    val i = e.actionIndex
    // Before Android 10 there's only the first pointer's raw position:
    // another's is as far from it on the screen as in the event's view.
    val q = android.os.Build.VERSION.SDK_INT >= android.os.Build.VERSION_CODES.Q
    val rx = if (q) e.getRawX(i) else e.rawX + e.getX(i) - e.getX(0)
    val ry = if (q) e.getRawY(i) else e.rawY + e.getY(i) - e.getY(0)
    val d = density
    val at = IntArray(2)
    var modal = false
    // Which, first: what one's OutsidePress does (closing it) mustn't change what the others are told.
    val outside = ArrayList<Triple<Int, Double, Double>>()
    for (n in overlays) {
        val o = n.view as? AsukaOverlay ?: continue
        // An empty layer (closed, its content gone) has nothing to close.
        if (!o.isAttachedToWindow || o.childCount == 0 || !n.listens(AsukaEventKind.OUTSIDE_PRESS) || !inScope(o)) continue
        o.getLocationOnScreen(at)
        val x = rx - at[0]
        val y = ry - at[1]
        if (x < 0 || y < 0 || x >= o.width || y >= o.height) continue // not in its root
        if (o.visibility == View.VISIBLE && o.onView(x, y)) continue
        if (o.modal && o.visibility == View.VISIBLE) modal = true
        outside.add(Triple(n.id, (x / d).toDouble(), (y / d).toDouble()))
    }
    for ((id, x, y) in outside) send(AsukaEvents.outsidePress(id, x, y))
    return modal
}

/**
 * A window's touches while a modal overlay holds them: a gesture that
 * began outside one's views reaches nothing (the overlay's own view
 * takes it, though OutsidePress may close it first), to its end.
 */
class AsukaTouchGate {
    private var held = false

    /**
     * Whether e (in the window) goes no further. began: what
     * [outsidePresses] said of its going down. A gesture is held from its
     * first finger: a second one outside a modal overlay, with the first
     * on its views, goes on with the gesture they have.
     */
    fun holds(e: MotionEvent, began: Boolean): Boolean {
        if (e.actionMasked == MotionEvent.ACTION_DOWN) held = began
        val h = held
        if (e.actionMasked == MotionEvent.ACTION_UP || e.actionMasked == MotionEvent.ACTION_CANCEL) held = false
        return h
    }
}
