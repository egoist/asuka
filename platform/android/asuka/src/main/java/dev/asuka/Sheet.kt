package dev.asuka

import android.graphics.Color
import android.view.View
import android.view.ViewGroup
import android.widget.FrameLayout
import android.widget.LinearLayout
import androidx.coordinatorlayout.widget.CoordinatorLayout
import androidx.core.graphics.Insets
import androidx.core.view.ViewCompat
import androidx.core.view.WindowInsetsAnimationCompat
import androidx.core.view.WindowInsetsCompat
import com.google.android.material.bottomsheet.BottomSheetBehavior
import com.google.android.material.bottomsheet.BottomSheetDialog
import com.google.android.material.bottomsheet.BottomSheetDragHandleView
import com.google.android.material.color.MaterialColors
import com.google.android.material.shape.CornerFamily
import com.google.android.material.shape.MaterialShapeDrawable
import java.nio.ByteBuffer
import java.nio.ByteOrder

// A Sheet (SheetProps): Material's bottom sheet. A modal one is a
// BottomSheetDialog; one with Undimmed is a standard (persistent) bottom
// sheet, a CoordinatorLayout over the activity's content that lets the
// touches it doesn't take through to the app below, as Google Maps' is.
// Either rests at the app's detents, at most three of them: Material's
// BottomSheetBehavior has a collapsed (peek), a half-expanded and an
// expanded height.

/** A Sheet's props as the core sent them, and its content's height (Fit). */
class AsukaSheetProps {
    /** Detents: each one's DetentKind and value (Height's points, Fraction's share). */
    var kinds = IntArray(0)
    var values = FloatArray(0)
    /** 1 + the index of the detent it's at (0: the shortest). */
    var detent = 0
    /** 1 + the index of the tallest detent that doesn't dim what's behind (0: none, modal). */
    var undimmed = 0
    var corner = 0f
    var grabber = false
    var noDismiss = false
    /** Its content's height, in dp, from the core's WindowAction Fit (0: none came). */
    var fit = 0f

    fun setDetents(b: ByteBuffer) {
        val (k, v) = decodeDetents(b)
        kinds = k
        values = v
    }
}

/** Decodes a Sheet's Detents (u32 count, then per detent u8 DetentKind and f32 value); cut short, the detents before the cut. */
fun decodeDetents(src: ByteBuffer): Pair<IntArray, FloatArray> {
    val b = src.duplicate().order(ByteOrder.LITTLE_ENDIAN)
    if (b.remaining() < 4) return IntArray(0) to FloatArray(0)
    val n = minOf(b.int.toLong() and 0xffffffffL, (b.remaining() / 5).toLong()).toInt()
    val kinds = IntArray(n)
    val values = FloatArray(n)
    for (i in 0 until n) {
        kinds[i] = b.get().toInt() and 0xff
        values[i] = b.float
    }
    return kinds to values
}

/**
 * The heights (px, above the navigation bar) a sheet's detents resolve to:
 * large the screen under the status bar, medium half the screen, fit its
 * content's (fitPx, with the grabber over it; large until the core
 * measured it), a height its points, a fraction its share of large; each
 * at most large. No detents: large alone.
 */
fun resolveDetents(kinds: IntArray, values: FloatArray, density: Float, large: Int, medium: Int, fitPx: Int, handle: Int): IntArray {
    if (kinds.isEmpty()) return intArrayOf(large)
    return IntArray(kinds.size) { i ->
        val h = when (kinds[i]) {
            AsukaDetentKind.MEDIUM -> medium
            AsukaDetentKind.FIT -> if (fitPx > 0) fitPx + handle else large
            AsukaDetentKind.HEIGHT -> Math.round(values[i] * density)
            AsukaDetentKind.FRACTION -> Math.round(values[i] * large)
            else -> large
        }
        h.coerceIn(1, maxOf(1, large))
    }
}

/**
 * The detents Material's sheet rests at, shortest first, as indexes into
 * heights: at most three (collapsed, half-expanded, expanded), one of
 * each height. With more, the shortest and the tallest, and between them
 * the one it's at (current) if it's neither, else the next shortest.
 */
fun sheetSlots(heights: IntArray, current: Int): IntArray {
    val order = heights.indices.sortedWith(compareBy({ heights[it] }, { it }))
    val distinct = ArrayList<Int>()
    for (i in order) if (distinct.isEmpty() || heights[distinct.last()] != heights[i]) distinct.add(i)
    if (distinct.size <= 3) return distinct.toIntArray()
    val first = distinct.first()
    val last = distinct.last()
    val mid = distinct.firstOrNull { it == current && it != first && it != last }
        ?: distinct.firstOrNull { current in heights.indices && heights[it] == heights[current] && it != first && it != last }
        ?: distinct[1]
    return intArrayOf(first, mid, last)
}

/** The behavior's state that rests a sheet at slot pos of slots (sheetSlots). */
fun sheetSlotState(pos: Int, slots: Int): Int = when {
    slots <= 1 || pos >= slots - 1 -> BottomSheetBehavior.STATE_EXPANDED
    pos <= 0 -> BottomSheetBehavior.STATE_COLLAPSED
    else -> BottomSheetBehavior.STATE_HALF_EXPANDED
}

/** The slot (a position in slots) a settled state rests at; -1: none (dragging, settling, hidden). */
fun sheetStateSlot(state: Int, slots: Int): Int = when (state) {
    BottomSheetBehavior.STATE_EXPANDED -> slots - 1
    BottomSheetBehavior.STATE_HALF_EXPANDED -> if (slots >= 3) 1 else slots - 1
    BottomSheetBehavior.STATE_COLLAPSED -> 0
    else -> -1
}

/**
 * The scrim's opacity over what's behind a sheet whose top is visible px
 * above the window's bottom: none up to the undimmed detent's height,
 * Material's 32% at the tallest.
 */
fun sheetScrim(visible: Int, undimmed: Int, tallest: Int): Float {
    if (tallest <= undimmed || visible <= undimmed) return 0f
    return 0.32f * ((visible - undimmed).toFloat() / (tallest - undimmed)).coerceIn(0f, 1f)
}

/** A presented Sheet: its dialog or its standard sheet, the behavior that moves it, and what it reported. */
class AsukaSheet(private val rt: AsukaRuntime, private val n: AsukaNode) {
    private val screen = n.view as AsukaScreen
    private val props get() = n.sheetProps
    private var act: AsukaActivity? = null
    private var box: LinearLayout? = null
    private var handle: BottomSheetDragHandleView? = null
    /** A modal sheet's dialog. */
    var dialog: BottomSheetDialog? = null
        private set
    /** A standard sheet's: the layout over the activity's content, the scrim in it, the sheet. */
    private var layer: CoordinatorLayout? = null
    private var scrim: View? = null
    private var sheet: View? = null
    private var behavior: BottomSheetBehavior<*>? = null
    private var back: androidx.activity.OnBackPressedCallback? = null
    private var modal = true
    /** The detent it's at (an index into Detents), and the one Selected last said. */
    private var current = -1
    private var heights = IntArray(0)
    private var slots = IntArray(0)
    private var lastHeight = Float.NaN
    private var closing = false
    private var imeShown = false

    fun present(a: AsukaActivity) {
        act = a
        modal = props.undimmed == 0
        current = props.detent - 1
        if (modal) presentModal(a) else presentStandard(a)
    }

    // ---- What both have ----

    private fun makeBox(a: AsukaActivity): LinearLayout {
        (screen.parent as? ViewGroup)?.removeView(screen)
        val b = LinearLayout(a).apply { orientation = LinearLayout.VERTICAL }
        box = b
        if (props.grabber) addHandle(a, b)
        b.addView(screen, LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, 0))
        return b
    }

    private fun addHandle(a: AsukaActivity, b: LinearLayout) {
        val h = BottomSheetDragHandleView(a)
        b.addView(h, 0)
        handle = h
    }

    private fun handlePx(): Int {
        val h = handle ?: return 0
        if (h.measuredHeight == 0) h.measure(View.MeasureSpec.makeMeasureSpec(0, View.MeasureSpec.UNSPECIFIED), View.MeasureSpec.makeMeasureSpec(0, View.MeasureSpec.UNSPECIFIED))
        return h.measuredHeight
    }

    private fun bars(a: AsukaActivity): Insets =
        ViewCompat.getRootWindowInsets(a.window.decorView)?.getInsets(WindowInsetsCompat.Type.systemBars() or WindowInsetsCompat.Type.displayCutout()) ?: Insets.NONE

    /** The parent the sheet moves in: its height, px. */
    private fun parentHeight(a: AsukaActivity): Int {
        val p = sheet?.parent as? View
        return if (p != null && p.height > 0) p.height else a.window.decorView.height
    }

    /** The navigation bar under the sheet, which it pads itself above, px. */
    private fun navPx(a: AsukaActivity): Int = bars(a).bottom

    /** Resolves its detents again and sets the behavior's heights to them. */
    private fun configure() {
        val a = act ?: return
        val b = behavior ?: return
        val decor = a.window.decorView
        val bars = bars(a)
        val hd = handlePx()
        val large = maxOf(1, decor.height - bars.top - bars.bottom)
        val medium = maxOf(1, decor.height / 2 - bars.bottom)
        heights = resolveDetents(props.kinds, props.values, rt.density, large, medium, Math.round(props.fit * rt.density), hd)
        if (current !in heights.indices) current = -1
        slots = sheetSlots(heights, current)
        if (current < 0) current = slots[0]
        val p = parentHeight(a)
        val nav = navPx(a)
        val tallest = heights[slots.last()]
        b.isFitToContents = false
        b.skipCollapsed = false
        b.expandedOffset = maxOf(0, p - tallest - nav)
        b.halfExpandedRatio = ((heights[slots[if (slots.size >= 3) 1 else slots.size - 1]] + nav).toFloat() / maxOf(1, p)).coerceIn(0.01f, 0.99f)
        b.peekHeight = heights[slots[0]] + (if (modal) 0 else nav)
        val st = b.state
        val want = stateFor(current)
        when {
            st == BottomSheetBehavior.STATE_DRAGGING || st == BottomSheetBehavior.STATE_SETTLING -> sizeContent(tallest)
            st == BottomSheetBehavior.STATE_HIDDEN || st == want -> sizeContent(heights[current])
            else -> {
                // Its heights moved under it (its content's height came, or
                // its detents changed): it goes to where the detent it's at
                // is now, as tall as it was on the way.
                sizeContent(maxOf(contentHeight, heights[current]))
                b.state = want
            }
        }
        applyDismissal()
        applyCorners()
        updateScrim()
    }

    /**
     * Sizes its content to a detent's height (px, the grabber in it): the
     * sheet is as tall as the detent it rests at, its content laid out
     * there as iOS's is. While it moves it's as tall as where it goes (a
     * drag: its tallest), so nothing shows under it.
     */
    private fun sizeContent(height: Int) {
        val lp = screen.layoutParams ?: return
        val want = maxOf(0, height - handlePx())
        if (lp.height == want) return
        lp.height = want
        screen.layoutParams = lp
    }

    /** The height its content is laid out at now, px (the grabber in it). */
    private val contentHeight get() = (screen.layoutParams?.height ?: 0) + handlePx()

    /** Whether the user's drag down, or a tap outside, may dismiss it. */
    fun applyDismissal() {
        val b = behavior ?: return
        val hideable = !props.noDismiss && !n.intercepts
        dialog?.let {
            it.setCancelable(!n.intercepts) // sets the behavior's hideable too
            it.setCanceledOnTouchOutside(!props.noDismiss)
        }
        // A standard sheet comes up from hidden: hideable until it's there.
        if (!closing && (modal || b.state != BottomSheetBehavior.STATE_HIDDEN)) b.isHideable = hideable
        rt.sheetBack[n.id]?.isEnabled = n.intercepts
    }

    private fun applyCorners() {
        val s = sheet ?: return
        val bg = s.background as? MaterialShapeDrawable ?: return
        val r = if (props.corner > 0) props.corner * rt.density else if (modal) return else 28 * rt.density
        bg.shapeAppearanceModel = bg.shapeAppearanceModel.toBuilder()
            .setTopLeftCorner(CornerFamily.ROUNDED, r).setTopRightCorner(CornerFamily.ROUNDED, r).build()
    }

    /** The behavior's state for the detent at index. */
    private fun stateFor(index: Int): Int {
        var pos = slots.indexOf(index)
        if (pos < 0) {
            // A detent it doesn't rest at (a fourth): the nearest one it does.
            val h = heights.getOrElse(index) { heights[slots[0]] }
            pos = slots.indices.minByOrNull { Math.abs(heights[slots[it]] - h) } ?: 0
        }
        return sheetSlotState(pos, slots.size)
    }

    /** Props changed while it shows (or its content's height did): its heights and look again. */
    fun update() {
        val a = act ?: return
        if (closing) return
        if ((props.undimmed == 0) != modal) {
            // Modal now, or not: presented again the other way, where it is.
            val at = current
            closeQuietly()
            closing = false
            current = at
            modal = props.undimmed == 0
            if (modal) presentModal(a) else presentStandard(a)
            return
        }
        val b = box
        if (b != null && props.grabber != (handle != null)) {
            if (props.grabber) addHandle(a, b) else { handle?.let { b.removeView(it) }; handle = null }
        }
        configure()
    }

    /** Detent: it moves there, animated. */
    fun moveTo(index: Int) {
        val b = behavior ?: return
        if (closing) return
        if (index !in heights.indices) { configure(); return }
        current = index
        if (index !in slots) configure() // a fourth detent: the middle one now
        sizeContent(maxOf(contentHeight, heights[index])) // nothing shows under it on its way
        b.state = stateFor(index)
    }

    /** The behavior settled: the detent it's at, reported if the user put it there. */
    private fun settled(state: Int) {
        val pos = sheetStateSlot(state, slots.size)
        if (pos < 0) return
        val idx = slots[pos]
        if (idx != current) {
            current = idx
            if (props.kinds.isNotEmpty()) rt.emit(n.id, AsukaEventKind.SELECTED) { AsukaEvents.selected(n.id, idx.toLong()) }
        }
        sizeContent(heights[idx]) // its environment, there (screen reports as it's laid out)
        screen.report()
    }

    /** Its height on the screen, dp, while the core listens (SheetHeight). */
    private fun reportHeight() {
        val a = act ?: return
        val s = sheet ?: return
        if (!n.listens(AsukaEventKind.SHEET_HEIGHT)) return
        val h = if (s.visibility != View.VISIBLE) 0f else maxOf(0, parentHeight(a) - s.top - Math.round(s.translationY)) / rt.density
        if (h == lastHeight) return
        lastHeight = h
        rt.send(AsukaEvents.sheetHeight(n.id, h.toDouble()))
    }

    private fun updateScrim() {
        val sc = scrim ?: return
        val s = sheet ?: return
        val a = act ?: return
        if (heights.isEmpty()) return
        val nav = navPx(a)
        val und = heights.getOrElse(props.undimmed - 1) { 0 } + nav
        val visible = parentHeight(a) - s.top
        val alpha = if (s.visibility != View.VISIBLE) 0f else sheetScrim(visible, und, heights[slots.last()] + nav)
        sc.alpha = alpha
        // It takes touches only while it dims: below it, the app's.
        sc.isClickable = alpha > 0.005f
    }

    private val callback = object : BottomSheetBehavior.BottomSheetCallback() {
        override fun onStateChanged(v: View, state: Int) {
            if (state == BottomSheetBehavior.STATE_HIDDEN) {
                if (!modal) hidden()
                return
            }
        if (state == BottomSheetBehavior.STATE_DRAGGING && slots.isNotEmpty()) {
                sizeContent(heights[slots.last()]) // it may go up to its tallest
                return
            }
            if (!modal && !closing) applyDismissal() // up from hidden: hideable as asked
            settled(state)
            reportHeight()
            updateScrim()
        }
        override fun onSlide(v: View, offset: Float) {
            reportHeight()
            updateScrim()
        }
    }

    /** The keyboard showing takes the sheet to its tallest detent, as on iOS. */
    private fun imeChanged(shown: Boolean) {
        if (shown == imeShown) return
        imeShown = shown
        val b = behavior ?: return
        // Where it settles is reported (Selected), as the user's drag is.
        if (shown && slots.isNotEmpty() && current != slots.last()) {
            sizeContent(heights[slots.last()])
            b.state = BottomSheetBehavior.STATE_EXPANDED
        }
        screen.report()
    }

    // ---- Modal: a BottomSheetDialog ----

    private fun presentModal(a: AsukaActivity) {
        val d = BottomSheetDialog(rt.dialogContext(a))
        rt.directDialog(d)
        dialog = d
        n.dialog = d
        val bx = makeBox(a)
        d.setContentView(bx)
        val s = d.findViewById<View>(com.google.android.material.R.id.design_bottom_sheet)
        sheet = s
        val b = d.behavior
        behavior = b
        configure()
        b.state = stateFor(current)
        b.addBottomSheetCallback(callback)
        // The keyboard: its content follows the keyboard frame by frame.
        val sheetDecor = d.window?.decorView ?: bx
        ViewCompat.setWindowInsetsAnimationCallback(sheetDecor, object : WindowInsetsAnimationCompat.Callback(DISPATCH_MODE_CONTINUE_ON_SUBTREE) {
            override fun onStart(an: WindowInsetsAnimationCompat, bounds: WindowInsetsAnimationCompat.BoundsCompat): WindowInsetsAnimationCompat.BoundsCompat {
                imeChanged(ViewCompat.getRootWindowInsets(bx)?.isVisible(WindowInsetsCompat.Type.ime()) == true)
                return bounds
            }
            override fun onProgress(insets: WindowInsetsCompat, running: MutableList<WindowInsetsAnimationCompat>): WindowInsetsCompat {
                screen.ime = insets.getInsets(WindowInsetsCompat.Type.ime()).bottom
                screen.report()
                return insets
            }
            override fun onEnd(an: WindowInsetsAnimationCompat) {
                screen.ime = -1
                screen.report()
            }
        })
        val id = n.id
        d.setOnDismissListener {
            rt.sheetBack.remove(id)
            if (dialog === d) dialog = null
            if (closing) return@setOnDismissListener
            if (!n.dying) {
                n.dying = true
                rt.emit(id, AsukaEventKind.DISMISSED) { AsukaEvents.dismissed(id) }
            }
        }
        // A keyboard that shows without animating: caught on layout. The
        // sheet's parent sized (a rotation): its heights again.
        bx.viewTreeObserver.addOnGlobalLayoutListener {
            imeChanged(ViewCompat.getRootWindowInsets(bx)?.isVisible(WindowInsetsCompat.Type.ime()) == true)
            reportHeight()
        }
        (s?.parent as? View)?.addOnLayoutChangeListener { _, _, t, _, bm, _, ot, _, ob ->
            if (bm - t != ob - ot) s.post { if (!closing) configure() }
        }
        // ui.OnBack: what would dismiss it asks the core while it intercepts.
        val back = object : androidx.activity.OnBackPressedCallback(false) {
            override fun handleOnBackPressed() { rt.emit(id, AsukaEventKind.BACK) { AsukaEvents.back(id) } }
        }
        d.onBackPressedDispatcher.addCallback(back)
        rt.sheetBack[id] = back
        d.setOnShowListener {
            d.findViewById<View>(com.google.android.material.R.id.touch_outside)?.setOnClickListener {
                if (n.intercepts) rt.emit(id, AsukaEventKind.BACK) { AsukaEvents.back(id) }
                else if (!props.noDismiss && d.isShowing) d.cancel()
            }
            applyCorners() // its background is the behavior's, made as it's laid out
            reportHeight()
        }
        applyDismissal()
        d.show()
    }

    // ---- Not modal: a standard sheet over the activity's content ----

    private fun presentStandard(a: AsukaActivity) {
        val ctx = rt.themed
        val l = CoordinatorLayout(ctx)
        l.layoutDirection = if (rt.rtl) View.LAYOUT_DIRECTION_RTL else View.LAYOUT_DIRECTION_LTR
        layer = l
        val sc = View(ctx).apply {
            setBackgroundColor(Color.BLACK)
            alpha = 0f
            isClickable = false
            importantForAccessibility = View.IMPORTANT_FOR_ACCESSIBILITY_NO
            setOnClickListener { scrimTapped() }
        }
        scrim = sc
        l.addView(sc, CoordinatorLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT))
        val s = AsukaSheetFrame(ctx)
        val elevation = 1 * rt.density
        s.background = MaterialShapeDrawable.createWithElevationOverlay(ctx, elevation).apply {
            fillColor = android.content.res.ColorStateList.valueOf(
                MaterialColors.getColor(ctx, com.google.android.material.R.attr.colorSurfaceContainerLow, Color.WHITE))
        }
        s.elevation = elevation
        s.setPadding(0, 0, 0, navPx(a))
        s.addView(makeBox(a), FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT))
        sheet = s
        val b = BottomSheetBehavior<View>()
        behavior = b
        l.addView(s, CoordinatorLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT).apply { behavior = b })
        a.rootFrame.addView(l, FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT))
        b.isHideable = true
        b.state = BottomSheetBehavior.STATE_HIDDEN
        configure()
        b.addBottomSheetCallback(callback)
        val id = n.id
        val cb = object : androidx.activity.OnBackPressedCallback(true) {
            override fun handleOnBackPressed() {
                if (n.intercepts) rt.emit(id, AsukaEventKind.BACK) { AsukaEvents.back(id) }
                else userDismiss()
            }
        }
        a.onBackPressedDispatcher.addCallback(cb)
        back = cb
        // Its size (a rotation) and the bars' (the navigation bar's)
        // change: its heights again; the keyboard: its tallest.
        l.addOnLayoutChangeListener { _, _, t, _, bm, _, ot, _, ob ->
            if (bm - t != ob - ot) l.post { if (!closing) { s.setPadding(0, 0, 0, navPx(a)); configure() } }
        }
        l.viewTreeObserver.addOnGlobalLayoutListener { imeChanged(a.keyboardPx > 0) }
        // Up from below, once it's laid out where hidden is.
        l.post { if (!closing) b.state = stateFor(current) }
    }

    /** A tap on the scrim, which shows above the undimmed detent: dismissed, or down to that detent if it can't be. */
    private fun scrimTapped() {
        val id = n.id
        when {
            n.intercepts -> rt.emit(id, AsukaEventKind.BACK) { AsukaEvents.back(id) }
            !props.noDismiss -> userDismiss()
            props.undimmed > 0 -> behavior?.state = stateFor(props.undimmed - 1)
        }
    }

    /** The user dismissed a standard sheet (back, the scrim): it slides away, and the core hears of it. */
    private fun userDismiss() {
        val b = behavior ?: return
        b.isHideable = true
        b.state = BottomSheetBehavior.STATE_HIDDEN
    }

    /** A standard sheet is down: gone, and the core told unless it asked. */
    private fun hidden() {
        val wasClosing = closing
        lastHeight = Float.NaN
        reportHeight()
        removeStandard()
        if (wasClosing) return
        closing = true
        if (!n.dying) {
            n.dying = true
            val id = n.id
            rt.emit(id, AsukaEventKind.DISMISSED) { AsukaEvents.dismissed(id) }
        }
    }

    private fun removeStandard() {
        back?.remove()
        back = null
        layer?.let { (it.parent as? ViewGroup)?.removeView(it) }
        layer = null
        scrim = null
    }

    /** The core destroyed it: it goes, a standard sheet sliding down. */
    fun destroy() {
        if (closing) return
        closing = true
        dialog?.let { d ->
            n.dialog = null
            dialog = null
            d.dismiss()
            rt.sheetBack.remove(n.id)
            return
        }
        val b = behavior
        val s = sheet
        if (b == null || s == null || !s.isLaidOut || b.state == BottomSheetBehavior.STATE_HIDDEN) {
            removeStandard()
            return
        }
        b.isHideable = true
        b.state = BottomSheetBehavior.STATE_HIDDEN // hidden() removes it
    }

    /** Takes it down at once, telling the core nothing (presented again the other way). */
    private fun closeQuietly() {
        closing = true
        behavior?.removeBottomSheetCallback(callback)
        dialog?.let { d ->
            n.dialog = null
            dialog = null
            d.setOnDismissListener(null)
            d.dismiss()
            rt.sheetBack.remove(n.id)
        }
        removeStandard()
        (screen.parent as? ViewGroup)?.removeView(screen)
        handle = null
        box = null
        sheet = null
        behavior = null
    }
}

/**
 * A standard sheet's own view: a touch on it is its own, not the app's
 * below, without its being clickable (a click on the sheet would turn
 * Material's drag handle's accessibility toggle).
 */
@android.annotation.SuppressLint("ViewConstructor", "ClickableViewAccessibility")
class AsukaSheetFrame(context: android.content.Context) : FrameLayout(context) {
    override fun onTouchEvent(event: android.view.MotionEvent): Boolean = true
}
