package dev.asuka

import android.animation.ValueAnimator
import android.annotation.SuppressLint
import android.view.GestureDetector
import android.view.MotionEvent
import android.view.VelocityTracker
import android.view.View
import android.view.ViewConfiguration
import android.view.accessibility.AccessibilityNodeInfo
import android.view.animation.DecelerateInterpolator
import android.widget.FrameLayout
import android.widget.OverScroller
import kotlin.math.abs
import kotlin.math.max
import kotlin.math.min
import kotlin.math.roundToInt

/**
 * A zoomable Scroll (MaxZoom over MinZoom). Android has no zoomable scroll
 * view, so this is one, as UIScrollView's zooming is: its content (laid
 * out by the core at zoom 1, at least as big as the Scroll) is drawn
 * scaled (scaleX and scaleY about its top left), and its own scroll offset
 * (scrollX, scrollY) moves over the scaled extent, both ways once that's
 * bigger than it. A pinch zooms about its focus (and pans with it), a
 * fling goes on (OverScroller), a double tap zooms in to twice MinZoom
 * (MaxZoom at most) about where it fell, or back out to MinZoom; a pinch
 * past the limits comes back to them as it ends. Zoomed reports the user's zoom; ZoomTo's isn't. The
 * offsets it reports (Scrolled) are the scaled content's.
 */
@SuppressLint("ViewConstructor", "ClickableViewAccessibility")
class AsukaZoomScroll(private val rt: AsukaRuntime, private val id: Int) :
    FrameLayout(rt.themed, null, android.R.attr.scrollViewStyle) {
    var minZoom = 1f
        private set
    var maxZoom = 1f
        private set
    /** The scale the content is drawn at. */
    var zoom = 1f
        private set

    private val config = ViewConfiguration.get(context)
    private val slop = config.scaledTouchSlop
    private val minFling = config.scaledMinimumFlingVelocity
    private val maxFling = config.scaledMaximumFlingVelocity
    private val scroller = OverScroller(context)
    private var velocity: VelocityTracker? = null
    private var downX = 0f
    private var downY = 0f
    private var lastX = 0f
    private var lastY = 0f
    private var dragging = false
    private var scaling = false
    // The touch pinched (or quick scaled): its second tap isn't a double tap.
    private var scaled = false
    private var lastFocusX = 0f
    private var lastFocusY = 0f
    private var animator: ValueAnimator? = null
    // The zoom animation's: from, to, the anchor (in the view and in the content), and whether it's reported.
    private var animFrom = 1f
    private var animTo = 1f
    private var animX = 0f
    private var animY = 0f
    private var animCX = 0f
    private var animCY = 0f
    private var animReports = false

    init {
        isHorizontalScrollBarEnabled = true
        isVerticalScrollBarEnabled = true
        isScrollContainer = true
        setWillNotDraw(false) // its scroll bars
    }

    private val content: View? get() = if (childCount > 0) getChildAt(0) else null
    private fun extentW() = ((content?.width ?: 0) * zoom).roundToInt()
    private fun extentH() = ((content?.height ?: 0) * zoom).roundToInt()
    private fun rangeX() = max(0, extentW() - width)
    private fun rangeY() = max(0, extentH() - height)
    private val zoomable get() = maxZoom > minZoom

    /** MinZoom and MaxZoom: the zoom shown is clamped to them, silently. */
    fun setRange(lo: Float, hi: Float) {
        minZoom = if (lo > 0f) lo else 1f
        maxZoom = max(minZoom, if (hi > 0f) hi else 1f)
        if (zoom < minZoom || zoom > maxZoom) {
            animator?.cancel()
            setZoom(zoom.coerceIn(minZoom, maxZoom))
            clampScroll()
        }
    }

    // Layout: the content's frame is the core's (its size at zoom 1), kept.

    override fun onMeasure(widthSpec: Int, heightSpec: Int) {
        setMeasuredDimension(getDefaultSize(suggestedMinimumWidth, widthSpec), getDefaultSize(suggestedMinimumHeight, heightSpec))
        for (i in 0 until childCount) {
            val c = getChildAt(i)
            c.measure(MeasureSpec.makeMeasureSpec(c.measuredWidth, MeasureSpec.EXACTLY), MeasureSpec.makeMeasureSpec(c.measuredHeight, MeasureSpec.EXACTLY))
        }
    }

    override fun onLayout(changed: Boolean, l: Int, t: Int, r: Int, b: Int) {
        for (i in 0 until childCount) {
            val c = getChildAt(i)
            c.layout(0, 0, c.measuredWidth, c.measuredHeight)
        }
        applyZoom()
        clampScroll()
    }

    override fun onViewAdded(child: View?) {
        super.onViewAdded(child)
        applyZoom()
    }

    private fun setZoom(z: Float) {
        if (z == zoom) return
        zoom = z
        applyZoom()
        rt.nodes[id]?.let { if (it.view === this) rt.cull(it) }
    }

    /** Scales the content about its top left; a content smaller than the view (zoomed out) is centered in it. */
    private fun applyZoom() {
        val c = content ?: return
        c.pivotX = 0f
        c.pivotY = 0f
        c.scaleX = zoom
        c.scaleY = zoom
        c.translationX = max(0f, (width - c.width * zoom) / 2)
        c.translationY = max(0f, (height - c.height * zoom) / 2)
        awakenScrollBars()
        invalidate()
    }

    private fun clampScroll() {
        val x = scrollX.coerceIn(0, rangeX())
        val y = scrollY.coerceIn(0, rangeY())
        if (x != scrollX || y != scrollY) scrollTo(x, y)
    }

    private fun scrollClamped(x: Float, y: Float) {
        val nx = x.roundToInt().coerceIn(0, rangeX())
        val ny = y.roundToInt().coerceIn(0, rangeY())
        if (nx != scrollX || ny != scrollY) scrollTo(nx, ny)
    }

    /** Zooms to z, the content's point under (fx, fy) staying there, then moved by (px, py). */
    private fun zoomAround(z: Float, fx: Float, fy: Float, px: Float = 0f, py: Float = 0f) {
        val cx = (scrollX + fx) / zoom
        val cy = (scrollY + fy) / zoom
        setZoom(z)
        scrollClamped(cx * z - fx + px, cy * z - fy + py)
    }

    private fun reportZoom() {
        val z = zoom.toDouble()
        rt.emit(id, AsukaEventKind.ZOOMED) { AsukaEvents.zoomed(id, z) }
    }

    // Scrolling

    override fun computeHorizontalScrollRange() = max(width, extentW())
    override fun computeVerticalScrollRange() = max(height, extentH())
    override fun computeHorizontalScrollOffset() = scrollX
    override fun computeVerticalScrollOffset() = scrollY
    override fun computeHorizontalScrollExtent() = width
    override fun computeVerticalScrollExtent() = height

    override fun computeScroll() {
        if (!scroller.computeScrollOffset()) return
        val x = scroller.currX.coerceIn(0, rangeX())
        val y = scroller.currY.coerceIn(0, rangeY())
        if (x != scrollX || y != scrollY) scrollTo(x, y)
        if (!awakenScrollBars()) postInvalidateOnAnimation()
        if (!scroller.isFinished) postInvalidateOnAnimation()
    }

    /** ScrollToOffset: px, clamped to what it scrolls. */
    fun scrollToOffset(x: Int, y: Int, animated: Boolean) {
        scroller.abortAnimation()
        val nx = x.coerceIn(0, rangeX())
        val ny = y.coerceIn(0, rangeY())
        if (!animated) { scrollTo(nx, ny); return }
        scroller.startScroll(scrollX, scrollY, nx - scrollX, ny - scrollY)
        postInvalidateOnAnimation()
    }

    // Zooming

    /** ZoomTo: about the middle of what it shows; not reported. */
    fun zoomTo(scale: Float, animated: Boolean) {
        val z = scale.coerceIn(minZoom, maxZoom)
        scroller.abortAnimation()
        animate(z, width / 2f, height / 2f, report = false, animated = animated)
    }

    private fun doubleTapped(x: Float, y: Float) {
        if (!zoomable) return
        val target = if (zoom > minZoom + 0.01f) minZoom else min(minZoom * 2, maxZoom)
        animate(target, x, y, report = true, animated = true)
    }

    /** Back within MinZoom and MaxZoom, after a pinch past them. */
    private fun settle() {
        if (zoom in minZoom..maxZoom) return
        animate(zoom.coerceIn(minZoom, maxZoom), lastFocusX, lastFocusY, report = true, animated = true)
    }

    private fun animate(to: Float, fx: Float, fy: Float, report: Boolean, animated: Boolean) {
        animator?.cancel()
        if (!animated || !isLaidOut) {
            zoomAround(to, fx, fy)
            if (report) reportZoom()
            return
        }
        animFrom = zoom
        animTo = to
        animX = fx
        animY = fy
        animCX = (scrollX + fx) / zoom
        animCY = (scrollY + fy) / zoom
        animReports = report
        val a = animator ?: ValueAnimator.ofFloat(0f, 1f).apply {
            duration = 300
            interpolator = DecelerateInterpolator(1.5f)
            addUpdateListener { va ->
                val f = va.animatedValue as Float
                val z = animFrom + (animTo - animFrom) * f
                setZoom(z)
                scrollClamped(animCX * z - animX, animCY * z - animY)
                if (animReports) reportZoom()
            }
            animator = this
        }
        a.start()
    }

    // A pinch: two fingers whose span changes by more than the touch slop
    // (ScaleGestureDetector also wants a span of about 27mm, which rules
    // out small pinches, as AsukaGestures' does).
    private var startSpan = 0f
    private var lastSpan = 0f

    /** The span of the touch's first two fingers but one lifting (0: fewer). */
    private fun span(e: MotionEvent): Float {
        val skip = if (e.actionMasked == MotionEvent.ACTION_POINTER_UP) e.actionIndex else -1
        var a = -1
        var b = -1
        for (i in 0 until e.pointerCount) {
            if (i == skip) continue
            if (a < 0) a = i else if (b < 0) { b = i; break }
        }
        if (b < 0) return 0f
        return Math.hypot((e.getX(a) - e.getX(b)).toDouble(), (e.getY(a) - e.getY(b)).toDouble()).toFloat()
    }

    private fun trackPinch(e: MotionEvent) {
        when (e.actionMasked) {
            MotionEvent.ACTION_POINTER_DOWN, MotionEvent.ACTION_POINTER_UP -> {
                val sp = span(e)
                if (sp == 0f) {
                    // Fewer than two fingers: the pinch is over.
                    if (scaling) { scaling = false; settle() }
                    startSpan = 0f
                } else {
                    if (!scaling) startSpan = sp
                    lastSpan = sp
                    lastFocusX = focus(e, true)
                    lastFocusY = focus(e, false)
                }
            }
            MotionEvent.ACTION_MOVE -> {
                if (startSpan <= 0f || e.pointerCount < 2) return
                val sp = span(e)
                val fx = focus(e, true)
                val fy = focus(e, false)
                if (!scaling) {
                    if (abs(sp - startSpan) <= slop) return
                    scaling = true
                    scaled = true
                    animator?.cancel()
                    scroller.abortAnimation()
                    parent?.requestDisallowInterceptTouchEvent(true)
                    lastSpan = sp
                    lastFocusX = fx
                    lastFocusY = fy
                    return
                }
                if (lastSpan <= 0f || sp <= 0f) return
                // Past the limits it gives way less, and comes back as it ends.
                val z = (zoom * sp / lastSpan).coerceIn(minZoom * 0.6f, maxZoom * 1.5f)
                zoomAround(z, fx, fy, lastFocusX - fx, lastFocusY - fy)
                lastSpan = sp
                lastFocusX = fx
                lastFocusY = fy
                reportZoom()
            }
            MotionEvent.ACTION_UP, MotionEvent.ACTION_CANCEL -> startSpan = 0f
        }
    }

    private val taps = GestureDetector(context, object : GestureDetector.SimpleOnGestureListener() {
        override fun onDown(e: MotionEvent) = true
        // On the second tap's release, unless it moved.
        override fun onDoubleTapEvent(e: MotionEvent): Boolean {
            if (e.actionMasked == MotionEvent.ACTION_UP && !scaled && abs(e.x - downX) <= slop && abs(e.y - downY) <= slop) doubleTapped(e.x, e.y)
            return true
        }
    })

    /** The middle of the touch's fingers, but one lifting. */
    private fun focus(e: MotionEvent, horizontal: Boolean): Float {
        val skip = if (e.actionMasked == MotionEvent.ACTION_POINTER_UP) e.actionIndex else -1
        var sum = 0f
        var n = 0
        for (i in 0 until e.pointerCount) {
            if (i == skip) continue
            sum += if (horizontal) e.getX(i) else e.getY(i)
            n++
        }
        return if (n > 0) sum / n else 0f
    }

    override fun dispatchTouchEvent(e: MotionEvent): Boolean {
        track(e)
        super.dispatchTouchEvent(e)
        return true // the whole touch: its drags, pinches and double taps
    }

    override fun onInterceptTouchEvent(e: MotionEvent) = dragging || scaling

    override fun onTouchEvent(e: MotionEvent) = true

    private fun track(e: MotionEvent) {
        if (e.actionMasked == MotionEvent.ACTION_DOWN) {
            // A touch stops a fling, and takes it from the content.
            dragging = !scroller.isFinished
            scroller.abortAnimation()
            if (animator?.isRunning == true && !animReports) animator?.end() // ZoomTo's: there at once
            scaled = false
            downX = e.x
            downY = e.y
            velocity?.recycle()
            velocity = VelocityTracker.obtain()
            if (dragging) parent?.requestDisallowInterceptTouchEvent(true)
        }
        velocity?.addMovement(e)
        if (zoomable) trackPinch(e)
        if (zoomable) taps.onTouchEvent(e)
        when (e.actionMasked) {
            MotionEvent.ACTION_DOWN, MotionEvent.ACTION_POINTER_DOWN, MotionEvent.ACTION_POINTER_UP -> {
                lastX = focus(e, true)
                lastY = focus(e, false)
            }
            MotionEvent.ACTION_MOVE -> {
                val fx = focus(e, true)
                val fy = focus(e, false)
                if (!dragging && !scaling) {
                    val mx = abs(fx - downX)
                    val my = abs(fy - downY)
                    // Only along a way it scrolls: else what's around it scrolls.
                    if ((mx > slop || my > slop) && (if (mx > my) rangeX() > 0 else rangeY() > 0)) {
                        dragging = true
                        parent?.requestDisallowInterceptTouchEvent(true)
                    }
                }
                if (dragging && !scaling) scrollClamped(scrollX + lastX - fx, scrollY + lastY - fy)
                lastX = fx
                lastY = fy
            }
            MotionEvent.ACTION_UP, MotionEvent.ACTION_CANCEL -> {
                val v = velocity
                if (dragging && !scaling && v != null && e.actionMasked == MotionEvent.ACTION_UP) {
                    v.computeCurrentVelocity(1000, maxFling.toFloat())
                    val vx = if (rangeX() > 0) -v.xVelocity.toInt() else 0
                    val vy = if (rangeY() > 0) -v.yVelocity.toInt() else 0
                    if (abs(vx) >= minFling || abs(vy) >= minFling) {
                        scroller.fling(scrollX, scrollY, vx, vy, 0, rangeX(), 0, rangeY())
                        postInvalidateOnAnimation()
                    }
                }
                dragging = false
                scaling = false
                velocity?.recycle()
                velocity = null
                settle()
            }
        }
    }

    override fun onDetachedFromWindow() {
        super.onDetachedFromWindow()
        animator?.end()
        scroller.abortAnimation()
    }

    // TalkBack: a scroll view, a page at a time.

    override fun getAccessibilityClassName(): CharSequence = android.widget.ScrollView::class.java.name

    override fun onInitializeAccessibilityNodeInfo(info: AccessibilityNodeInfo) {
        super.onInitializeAccessibilityNodeInfo(info)
        if (rangeX() == 0 && rangeY() == 0) return
        info.isScrollable = true
        if (scrollY < rangeY() || scrollX < rangeX()) info.addAction(AccessibilityNodeInfo.AccessibilityAction.ACTION_SCROLL_FORWARD)
        if (scrollY > 0 || scrollX > 0) info.addAction(AccessibilityNodeInfo.AccessibilityAction.ACTION_SCROLL_BACKWARD)
        if (scrollY < rangeY()) info.addAction(AccessibilityNodeInfo.AccessibilityAction.ACTION_SCROLL_DOWN)
        if (scrollY > 0) info.addAction(AccessibilityNodeInfo.AccessibilityAction.ACTION_SCROLL_UP)
        if (scrollX < rangeX()) info.addAction(AccessibilityNodeInfo.AccessibilityAction.ACTION_SCROLL_RIGHT)
        if (scrollX > 0) info.addAction(AccessibilityNodeInfo.AccessibilityAction.ACTION_SCROLL_LEFT)
    }

    override fun performAccessibilityAction(action: Int, arguments: android.os.Bundle?): Boolean {
        when (action) {
            android.R.id.accessibilityActionScrollDown -> { scrollToOffset(scrollX, scrollY + height, true); return true }
            android.R.id.accessibilityActionScrollUp -> { scrollToOffset(scrollX, scrollY - height, true); return true }
            android.R.id.accessibilityActionScrollRight -> { scrollToOffset(scrollX + width, scrollY, true); return true }
            android.R.id.accessibilityActionScrollLeft -> { scrollToOffset(scrollX - width, scrollY, true); return true }
        }
        val sign = when (action) {
            AccessibilityNodeInfo.ACTION_SCROLL_FORWARD -> 1
            AccessibilityNodeInfo.ACTION_SCROLL_BACKWARD -> -1
            else -> return super.performAccessibilityAction(action, arguments)
        }
        // Down a page if it scrolls down, else across.
        if (rangeY() > 0) scrollToOffset(scrollX, scrollY + sign * height, true)
        else scrollToOffset(scrollX + sign * width, scrollY, true)
        return true
    }
}
