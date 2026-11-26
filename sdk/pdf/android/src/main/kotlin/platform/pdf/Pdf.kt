// platform/pdf on Android: the platform has no view that shows a PDF, so
// this is one made as apps make theirs. Android's PdfRenderer draws the
// pages, one at a time on one background thread, into a RecyclerView that
// lists them one under the other at the view's width (their sizes read
// when the document opens), each drawn as it comes into view and kept in
// a cache bounded by memory. The list pinches and double-taps to zoom (1×
// to 4×) and pans while zoomed; once a zoom or a pan settles, the part of
// each page in view is drawn again at that zoom, so text stays sharp.
package platform.pdf

import android.animation.Animator
import android.annotation.SuppressLint
import android.animation.AnimatorListenerAdapter
import android.animation.ValueAnimator
import android.content.Context
import android.graphics.Bitmap
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Matrix
import android.graphics.Paint
import android.graphics.Rect
import android.graphics.RectF
import android.graphics.pdf.PdfRenderer
import android.os.Handler
import android.os.Looper
import android.os.ParcelFileDescriptor
import android.util.LruCache
import android.view.GestureDetector
import android.view.MotionEvent
import android.view.ScaleGestureDetector
import android.view.VelocityTracker
import android.view.ViewConfiguration
import android.view.ViewGroup
import android.view.animation.DecelerateInterpolator
import android.widget.OverScroller
import androidx.core.graphics.createBitmap
import androidx.core.graphics.withTranslation
import androidx.core.net.toUri
import androidx.recyclerview.widget.LinearLayoutManager
import androidx.recyclerview.widget.RecyclerView
import com.google.android.material.color.MaterialColors
import dev.asuka.AsukaModules
import java.io.File
import java.io.IOException
import java.util.UUID
import java.util.concurrent.ConcurrentHashMap
import java.util.concurrent.LinkedBlockingQueue
import java.util.concurrent.ThreadPoolExecutor
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicInteger
import kotlin.math.hypot
import kotlin.math.max
import kotlin.math.min
import kotlin.math.roundToInt
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import android.view.View as AndroidView

private const val MAX_ZOOM = 4f
private const val TAP_ZOOM = 2f

/**
 * The thread every PdfRenderer is used on, for all the views: one, as
 * PdfRenderer (and PDFium under it) isn't thread-safe and has one page
 * open at a time. It ends when it's been idle a while.
 */
private val pool = ThreadPoolExecutor(1, 1, 10, TimeUnit.SECONDS, LinkedBlockingQueue()) { r ->
    Thread(r, "asuka-pdf").apply { isDaemon = true }
}.apply { allowCoreThreadTimeOut(true) }

private val main = Handler(Looper.getMainLooper())

/**
 * Where the copies of assets and the downloads the renderer reads are
 * kept, each a file of its own, deleted when its view lets it go; what an
 * earlier process left is deleted the first time one's needed.
 */
private val folder: File by lazy {
    File(AsukaModules.context.cacheDir, "asuka-pdf").apply { deleteRecursively(); mkdirs() }
}

/** A failure the app is told of, its message after "platform/pdf: ". */
private class Failure(message: String) : Exception(message)

/** A source's local file; temp when it's this view's copy, to delete with it. */
private class Local(val file: File, val temp: Boolean) {
    fun drop() { if (temp) file.delete() }
}

/** A page's size, in points. */
private class PageSize(val width: Int, val height: Int)

/**
 * An open document: its renderer, used on the render thread alone, and
 * its pages' sizes. Closed is set on the main thread as it closes, so
 * what's queued for it then draws nothing.
 */
private class Doc(val pdf: PdfRenderer, val sizes: List<PageSize>) {
    @Volatile var closed = false

    /** Closes it on the render thread, after what's queued there. */
    fun close() {
        closed = true
        pool.execute { runCatching { pdf.close() } }
    }

    /**
     * Page i drawn into a white bitmap of w by h: the whole page fitted
     * to it, or, with m, the page through m (points to pixels). Null if
     * there's no memory for it. On the render thread.
     */
    fun draw(i: Int, w: Int, h: Int, m: Matrix?): Bitmap? {
        if (closed) return null
        val b = try {
            createBitmap(w, h)
        } catch (_: OutOfMemoryError) {
            return null
        }
        b.eraseColor(Color.WHITE)
        return try {
            val page = pdf.openPage(i)
            try { page.render(b, null, m, PdfRenderer.Page.RENDER_MODE_FOR_DISPLAY) } finally { page.close() }
            b
        } catch (_: Exception) {
            null
        }
    }

    companion object {
        /** The document in file, its pages' sizes read one page at a time. On the render thread. */
        fun open(file: File): Doc {
            val fd = ParcelFileDescriptor.open(file, ParcelFileDescriptor.MODE_READ_ONLY)
            // The renderer closes the descriptor as it closes, but not if it never opened.
            val pdf = try { PdfRenderer(fd) } catch (t: Throwable) { fd.close(); throw t }
            try {
                val sizes = List(pdf.pageCount) { i ->
                    val p = pdf.openPage(i)
                    try { PageSize(max(p.width, 1), max(p.height, 1)) } finally { p.close() }
                }
                return Doc(pdf, sizes)
            } catch (t: Throwable) {
                pdf.close()
                throw t
            }
        }
    }
}

/**
 * The file source names, off the main thread. A path or a file: URL is
 * read as it is; an asset (pushed by asuka dev, else in the APK) and an
 * http(s) URL (downloaded) become a copy of the view's own in the caches,
 * as the renderer reads the file for as long as it shows it, and another
 * view's copy of the same asset mustn't change it underneath.
 */
private suspend fun resolve(source: String): Local = withContext(Dispatchers.IO) {
    val app = AsukaModules.context
    val local = when {
        source.startsWith("http://") || source.startsWith("https://") -> {
            val f = try {
                AsukaModules.localFile(source, "application/pdf")
            } catch (e: Exception) {
                val m = e.message.orEmpty()
                throw Failure(if (m.startsWith("couldn't download")) m else "couldn't download $source: ${m.ifEmpty { e.javaClass.simpleName }}")
            }
            // Into the view's folder, out of the one the download made for it.
            val own = File(folder, UUID.randomUUID().toString() + ".pdf")
            if (f.renameTo(own)) {
                f.parentFile?.delete()
                Local(own, temp = true)
            } else {
                Local(f, temp = true)
            }
        }
        source.startsWith("file://") -> Local(File(source.toUri().path.orEmpty()), temp = false)
        source.startsWith("/") -> Local(File(source), temp = false)
        else -> {
            val rel = source.removePrefix("./")
            val pushed = File(app.filesDir, "asuka/assets/$rel")
            if (pushed.isFile) Local(pushed, temp = false) else {
                val own = File(folder, UUID.randomUUID().toString() + ".pdf")
                try {
                    app.assets.open(rel).use { i -> own.outputStream().use { o -> i.copyTo(o) } }
                } catch (_: IOException) {
                    own.delete()
                    throw Failure("no file at $source")
                }
                Local(own, temp = true)
            }
        }
    }
    if (!local.file.isFile) throw Failure("no file at $source")
    local
}

/**
 * A page: its bitmap at the view's width, stretched to its size, and over
 * it, once a zoom settles, the part of it in view drawn at that zoom.
 */
private class PageView(context: Context) : AndroidView(context) {
    var base: Bitmap? = null
        set(v) { field = v; invalidate() }
    private var detail: Bitmap? = null
    private val detailAt = RectF()
    private val bounds = Rect()
    private val paint = Paint(Paint.FILTER_BITMAP_FLAG or Paint.ANTI_ALIAS_FLAG)

    init { setBackgroundColor(Color.WHITE) }

    /** The part at (in this view's pixels) drawn at a zoom, or none. */
    fun setDetail(b: Bitmap?, at: RectF?) {
        if (b == null && detail == null) return
        detail = b
        if (at != null) detailAt.set(at)
        invalidate()
    }

    override fun onDraw(c: Canvas) {
        base?.let {
            bounds.set(0, 0, width, height)
            c.drawBitmap(it, null, bounds, paint)
        }
        detail?.let { c.drawBitmap(it, null, detailAt, paint) }
    }
}

private class PageHolder(val page: PageView) : RecyclerView.ViewHolder(page) {
    var index = -1
}

/**
 * The list of pages, zoomed by drawing them scaled: by scale from the
 * list's top left, then moved by (tx, ty), each within what keeps the
 * pages covering the list. A pan moves tx and ty, and what of a vertical
 * one ty can't take becomes the list's own scroll; a zoom keeps the point
 * under the fingers where it is. At 1× the list scrolls and flings as
 * any RecyclerView; zoomed, a drag or a fling is a pan.
 */
private class PageList(context: Context) : RecyclerView(context) {
    var scale = 1f
        private set
    var tx = 0f
        private set
    var ty = 0f
        private set

    /** Called as what's in view moves (a pan, a zoom; the list's scroll listeners hear its scrolls). */
    var onMove: () -> Unit = {}
    /** Called as a zoom, a pan or a fling ends. */
    var onSettle: () -> Unit = {}
    /** Called as the list's width changes, zoomed back to 1×. */
    var onResize: () -> Unit = {}

    private val config = ViewConfiguration.get(context)
    /** Whether this gesture is the zoom's and the pan's, not the list's. */
    private var owning = false
    private var lastX = 0f
    private var lastY = 0f
    /** What's left of the list's scroll under a pixel, so slow pans add up. */
    private var carry = 0f
    private var velocity: VelocityTracker? = null
    private val fling = OverScroller(context)
    private var flingX = 0
    private var flingY = 0
    private var zooming: ValueAnimator? = null

    private val scaler = ScaleGestureDetector(context, object : ScaleGestureDetector.SimpleOnScaleGestureListener() {
        override fun onScale(d: ScaleGestureDetector): Boolean {
            zoomTo(scale * d.scaleFactor, d.focusX, d.focusY)
            return true
        }
    }).apply { isQuickScaleEnabled = false }

    private val taps = GestureDetector(context, object : GestureDetector.SimpleOnGestureListener() {
        override fun onDoubleTap(e: MotionEvent): Boolean {
            animateZoom(if (scale > 1.01f) 1f else TAP_ZOOM, e.x, e.y)
            return true
        }
    })

    private val flingStep = object : Runnable {
        override fun run() {
            if (!fling.computeScrollOffset()) { onSettle(); return }
            val dx = fling.currX - flingX
            val dy = fling.currY - flingY
            flingX = fling.currX
            flingY = fling.currY
            // Stopped at the edges: done.
            if (!panBy(dx.toFloat(), dy.toFloat()) && (dx != 0 || dy != 0)) {
                fling.forceFinished(true)
                onSettle()
                return
            }
            postOnAnimation(this)
        }
    }

    override fun dispatchTouchEvent(ev: MotionEvent): Boolean {
        val action = ev.actionMasked
        if (action == MotionEvent.ACTION_DOWN) {
            removeCallbacks(flingStep)
            fling.forceFinished(true)
            zooming?.cancel()
            owning = scale > 1.001f
            if (owning) {
                stopScroll()
                parent?.requestDisallowInterceptTouchEvent(true)
            }
        }
        // The focus's move pans before the zoom about where it is now.
        if (owning) follow(ev)
        scaler.onTouchEvent(ev)
        taps.onTouchEvent(ev)
        if (!owning && (scaler.isInProgress || zooming != null)) {
            // The list's drag ends: the gesture is the zoom's now.
            owning = true
            val cancel = MotionEvent.obtain(ev)
            cancel.action = MotionEvent.ACTION_CANCEL
            super.dispatchTouchEvent(cancel)
            cancel.recycle()
            stopScroll()
            parent?.requestDisallowInterceptTouchEvent(true)
            follow(ev, start = true)
        }
        if (!owning) return super.dispatchTouchEvent(ev)
        if (action == MotionEvent.ACTION_UP || action == MotionEvent.ACTION_CANCEL) end(ev)
        return true
    }

    /** Pans by the move of the fingers' focus (their middle). */
    private fun follow(ev: MotionEvent, start: Boolean = false) {
        val action = ev.actionMasked
        val lifted = if (action == MotionEvent.ACTION_POINTER_UP) ev.actionIndex else -1
        var sx = 0f
        var sy = 0f
        var n = 0
        for (i in 0 until ev.pointerCount) {
            if (i == lifted) continue
            sx += ev.getX(i)
            sy += ev.getY(i)
            n++
        }
        if (n == 0) return
        val fx = sx / n
        val fy = sy / n
        if (start || action == MotionEvent.ACTION_DOWN) {
            velocity?.recycle()
            velocity = VelocityTracker.obtain()
        }
        velocity?.addMovement(ev)
        if (start || action != MotionEvent.ACTION_MOVE) {
            lastX = fx
            lastY = fy
            return
        }
        panBy(fx - lastX, fy - lastY)
        lastX = fx
        lastY = fy
    }

    private fun end(ev: MotionEvent) {
        owning = false
        val v = velocity
        velocity = null
        if (ev.actionMasked == MotionEvent.ACTION_UP && v != null && zooming == null && scale > 1.001f) {
            v.computeCurrentVelocity(1000, config.scaledMaximumFlingVelocity.toFloat())
            val id = ev.getPointerId(ev.actionIndex)
            val vx = v.getXVelocity(id)
            val vy = v.getYVelocity(id)
            v.recycle()
            if (hypot(vx, vy) > config.scaledMinimumFlingVelocity) {
                flingX = 0
                flingY = 0
                fling.fling(0, 0, vx.toInt(), vy.toInt(), Int.MIN_VALUE, Int.MAX_VALUE, Int.MIN_VALUE, Int.MAX_VALUE)
                postOnAnimation(flingStep)
                return
            }
        } else {
            v?.recycle()
        }
        if (zooming == null) onSettle()
    }

    /** Moves the pages by (dx, dy) on the screen; whether they moved. */
    fun panBy(dx: Float, dy: Float): Boolean {
        val w = width.toFloat()
        val h = height.toFloat()
        val nx = (tx + dx).coerceIn(w - w * scale, 0f)
        val ny = (ty + dy).coerceIn(h - h * scale, 0f)
        val rest = dy - (ny - ty)
        var moved = nx != tx || ny != ty
        tx = nx
        ty = ny
        if (rest != 0f && scrollList(rest)) moved = true
        if (moved) {
            invalidate()
            onMove()
        }
        return moved
    }

    /** Moves the pages by dy on the screen with the list's scroll (dy over scale of its own); whether it could. */
    private fun scrollList(dy: Float): Boolean {
        if (!canScrollVertically(if (dy < 0) 1 else -1)) {
            carry = 0f
            return false
        }
        carry -= dy / scale
        val n = carry.toInt()
        carry -= n
        if (n != 0) scrollBy(0, n)
        return true
    }

    /** Zooms to s, the point at (fx, fy) staying there. */
    fun zoomTo(target: Float, fx: Float, fy: Float) {
        val s = target.coerceIn(1f, MAX_ZOOM)
        if (s == scale) return
        val w = width.toFloat()
        val h = height.toFloat()
        val px = (fx - tx) / scale
        val py = (fy - ty) / scale
        tx = (fx - px * s).coerceIn(w - w * s, 0f)
        val want = fy - py * s
        val ny = want.coerceIn(h - h * s, 0f)
        scale = s
        ty = ny
        // What ty can't take, the list's scroll does.
        if (want != ny) scrollList(want - ny)
        invalidate()
        onMove()
    }

    private fun animateZoom(target: Float, fx: Float, fy: Float) {
        zooming?.cancel()
        val a = ValueAnimator.ofFloat(scale, target)
        a.duration = 250
        a.interpolator = DecelerateInterpolator()
        a.addUpdateListener { zoomTo(it.animatedValue as Float, fx, fy) }
        a.addListener(object : AnimatorListenerAdapter() {
            private var cancelled = false
            override fun onAnimationCancel(animation: Animator) { cancelled = true }
            override fun onAnimationEnd(animation: Animator) {
                if (zooming === a) zooming = null
                // Cancelled by a touch, the touch settles it.
                if (!cancelled) onSettle()
            }
        })
        zooming = a
        a.start()
    }

    /** Back to 1×, at once. */
    fun resetZoom() {
        zooming?.cancel()
        removeCallbacks(flingStep)
        fling.forceFinished(true)
        scale = 1f
        tx = 0f
        ty = 0f
        carry = 0f
        invalidate()
    }

    /** Puts the pages' top at the list's top, keeping the zoom and tx. */
    fun alignTop() {
        ty = 0f
        invalidate()
    }

    override fun onSizeChanged(w: Int, h: Int, oldw: Int, oldh: Int) {
        super.onSizeChanged(w, h, oldw, oldh)
        if (w != oldw) {
            resetZoom()
            // Not while the list lays out.
            post { onResize() }
        } else {
            ty = ty.coerceIn(h - h * scale, 0f)
        }
    }

    override fun onDetachedFromWindow() {
        super.onDetachedFromWindow()
        zooming?.cancel()
        removeCallbacks(flingStep)
        fling.forceFinished(true)
    }

    override fun dispatchDraw(c: Canvas) {
        if (scale == 1f && tx == 0f && ty == 0f) {
            super.dispatchDraw(c)
            return
        }
        val s = scale
        c.withTranslation(tx, ty) {
            scale(s, s)
            super.dispatchDraw(this)
        }
    }
}

// The whole document changes at once: a new one, or the width it's drawn at.
@SuppressLint("NotifyDataSetChanged")
private class PdfView(private val events: ViewEvents) : View {
    private val context = events.context
    private val list = PageList(context)
    override val view: AndroidView get() = list

    private var source = ""
    /** Which source's loading is current: a result for an older one is let go. */
    private var gen = 0
    private var local: Local? = null
    private var doc: Doc? = null
    private var opening = false
    private var sizes: List<PageSize> = emptyList()
    /** The page as the app and the view both have it: the one set, or the last reported. */
    private var page = 0

    /** The pages drawn at the list's width, by page and width, bounded by memory (an eighth of the app's). */
    private val cache = object : LruCache<Long, Bitmap>((Runtime.getRuntime().maxMemory() / 8).coerceAtMost(Int.MAX_VALUE.toLong()).toInt()) {
        override fun sizeOf(key: Long, value: Bitmap) = value.byteCount
    }
    /** The cache's keys being drawn. */
    private val drawing = HashSet<Long>()
    /** The pages bound to a view: the render thread draws only those. */
    private val bound: MutableSet<Int> = ConcurrentHashMap.newKeySet()
    /** Which settle the zoomed parts drawn are for: those of an older one are let go. */
    private val settled = AtomicInteger()

    private val adapter = object : RecyclerView.Adapter<PageHolder>() {
        override fun getItemCount() = sizes.size

        override fun onCreateViewHolder(parent: ViewGroup, viewType: Int) =
            PageHolder(PageView(context).apply { layoutParams = RecyclerView.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, 0) })

        override fun onBindViewHolder(holder: PageHolder, position: Int) {
            if (holder.index >= 0 && holder.index != position) bound.remove(holder.index)
            holder.index = position
            bound.add(position)
            val w = list.width
            holder.page.layoutParams.height = pageHeight(w, sizes[position])
            holder.page.contentDescription = "Page ${position + 1} of ${sizes.size}"
            holder.page.base = cache.get(key(position, w))
            holder.page.setDetail(null, null)
            want(holder)
        }

        override fun onViewRecycled(holder: PageHolder) {
            bound.remove(holder.index)
            holder.index = -1
            holder.page.base = null
            holder.page.setDetail(null, null)
        }
    }

    init {
        list.layoutManager = LinearLayoutManager(context)
        list.itemAnimator = null
        list.setHasFixedSize(true)
        list.adapter = adapter
        list.setBackgroundColor(MaterialColors.getColor(context, com.google.android.material.R.attr.colorSurfaceContainerHigh, Color.LTGRAY))
        val gap = (8 * context.resources.displayMetrics.density).roundToInt()
        list.addItemDecoration(object : RecyclerView.ItemDecoration() {
            override fun getItemOffsets(out: Rect, v: AndroidView, parent: RecyclerView, state: RecyclerView.State) {
                out.set(0, if (parent.getChildAdapterPosition(v) > 0) gap else 0, 0, 0)
            }
        })
        list.addOnScrollListener(object : RecyclerView.OnScrollListener() {
            override fun onScrolled(rv: RecyclerView, dx: Int, dy: Int) = track()
        })
        list.onMove = { track() }
        list.onSettle = { sharpen() }
        list.onResize = {
            cache.evictAll()
            adapter.notifyDataSetChanged()
        }
        list.addOnAttachStateChangeListener(object : AndroidView.OnAttachStateChangeListener {
            override fun onViewAttachedToWindow(v: AndroidView) {
                // Back after a detach: the same file, opened again where it was.
                val l = local ?: return
                if (doc == null && !opening) open(l, gen, first = false)
            }

            override fun onViewDetachedFromWindow(v: AndroidView) = suspend()
        })
    }

    override fun setSource(value: String) {
        if (value == source) return
        source = value
        gen++
        close()
        local?.drop()
        local = null
        opening = false
        if (sizes.isNotEmpty()) {
            sizes = emptyList()
            adapter.notifyDataSetChanged()
        }
        list.resetZoom()
        if (value.isEmpty()) return
        val g = gen
        opening = true
        AsukaModules.scope.launch {
            val r = try { Result.success(resolve(value)) } catch (e: Exception) { Result.failure(e) }
            main.post {
                if (g != gen) { r.getOrNull()?.drop(); return@post }
                r.fold({ open(it, g, first = true) }, { fail(it) })
            }
        }
    }

    override fun setPage(value: Long) {
        val v = value.coerceIn(0, Int.MAX_VALUE.toLong()).toInt()
        if (v == page) return
        page = v
        // Before the document's loaded: it opens there.
        if (sizes.isEmpty()) return
        val p = v.coerceAtMost(sizes.size - 1)
        list.alignTop()
        (list.layoutManager as LinearLayoutManager).scrollToPositionWithOffset(p, 0)
        if (p != v) report(p)
        if (list.scale > 1.001f) list.post { sharpen() }
    }

    /** Opens the file on the render thread: first, for a new source (loaded is sent), else again after a detach. */
    private fun open(l: Local, g: Int, first: Boolean) {
        opening = true
        val name = source
        pool.execute {
            val r: Result<Doc> = try {
                Result.success(Doc.open(l.file))
            } catch (_: SecurityException) {
                Result.failure(Failure("$name is encrypted"))
            } catch (_: IOException) {
                Result.failure(if (!l.file.isFile) Failure("no file at $name") else Failure("$name isn't a PDF, or is damaged"))
            } catch (e: Exception) {
                Result.failure(Failure("couldn't open $name: ${e.message ?: e.javaClass.simpleName}"))
            }
            main.post { opened(l, g, first, r) }
        }
    }

    private fun opened(l: Local, g: Int, first: Boolean, r: Result<Doc>) {
        if (g != gen) {
            r.getOrNull()?.close()
            if (local !== l) l.drop()
            return
        }
        opening = false
        val d = r.getOrElse {
            local = null
            l.drop()
            fail(it)
            return
        }
        local = l
        doc = d
        if (first) {
            sizes = d.sizes
            list.resetZoom()
            adapter.notifyDataSetChanged()
            val p = page.coerceAtMost(max(sizes.size - 1, 0))
            (list.layoutManager as LinearLayoutManager).scrollToPositionWithOffset(p, 0)
            events.loaded(sizes.size.toLong())
            if (sizes.isNotEmpty() && p != page) report(p)
        } else {
            adapter.notifyDataSetChanged()
        }
        // Opened while away from the window: shut until it's back.
        if (!list.isAttachedToWindow) suspend()
    }

    private fun fail(e: Throwable) {
        opening = false
        val m = if (e is Failure) e.message else "couldn't open $source: ${e.message ?: e.javaClass.simpleName}"
        events.error("platform/pdf: $m")
    }

    /** Closes the document, its file kept for when the view's back in a window; its bitmaps let go. */
    private fun suspend() {
        close()
        for (i in 0 until list.childCount) {
            val p = list.getChildAt(i) as? PageView ?: continue
            p.base = null
            p.setDetail(null, null)
        }
    }

    private fun close() {
        doc?.close()
        doc = null
        cache.evictAll()
        drawing.clear()
        settled.incrementAndGet()
    }

    private fun key(page: Int, width: Int) = (page.toLong() shl 32) or width.toLong()

    private fun pageHeight(w: Int, s: PageSize) = max(1, (w.toLong() * s.height / s.width).toInt())

    private fun holder(page: Int): PageHolder? =
        (list.findViewHolderForAdapterPosition(page) as? PageHolder)?.takeIf { it.index == page }

    /** Draws a bound page at the list's width, unless it's cached or being drawn. */
    private fun want(h: PageHolder) {
        val d = doc ?: return
        val w = list.width
        val i = h.index
        if (w <= 0 || i < 0 || h.page.base != null) return
        val k = key(i, w)
        cache.get(k)?.let { h.page.base = it; return }
        if (!drawing.add(k)) return
        val height = pageHeight(w, d.sizes[i])
        pool.execute {
            // Scrolled away before its turn: not drawn.
            val skipped = !bound.contains(i)
            val b = if (skipped) null else d.draw(i, w, height, null)
            main.post {
                if (doc !== d) return@post
                drawing.remove(k)
                if (b != null) {
                    cache.put(k, b)
                    holder(i)?.page?.base = b
                } else if (skipped) {
                    // Bound again since: drawn after all.
                    holder(i)?.let { want(it) }
                }
            }
        }
    }

    /**
     * Once a zoom or a pan settles: the part of each page in view drawn
     * again at the zoom, over its bitmap at the list's width; at 1×, those
     * parts let go.
     */
    private fun sharpen() {
        val token = settled.incrementAndGet()
        val s = list.scale
        val zoomed = s > 1.001f
        val d = doc
        val w = list.width.toFloat()
        val h = list.height.toFloat()
        for (c in 0 until list.childCount) {
            val v = list.getChildAt(c) as? PageView ?: continue
            if (!zoomed || d == null) { v.setDetail(null, null); continue }
            val i = (list.getChildViewHolder(v) as PageHolder).index
            if (i < 0 || v.width <= 0) continue
            // The part in view, in the page's pixels at the list's width.
            val l = max(0f, -list.tx / s - v.left)
            val t = max(0f, -list.ty / s - v.top)
            val r = min(v.width.toFloat(), (w - list.tx) / s - v.left)
            val b = min(v.height.toFloat(), (h - list.ty) / s - v.top)
            if (r <= l || b <= t) { v.setDetail(null, null); continue }
            val bw = ((r - l) * s).roundToInt()
            val bh = ((b - t) * s).roundToInt()
            if (bw <= 0 || bh <= 0) continue
            val at = RectF(l, t, l + bw / s, t + bh / s)
            val k = v.width.toFloat() / d.sizes[i].width * s
            val m = Matrix().apply {
                setScale(k, k)
                postTranslate(-l * s, -t * s)
            }
            pool.execute {
                if (token != settled.get()) return@execute
                val bmp = d.draw(i, bw, bh, m) ?: return@execute
                main.post {
                    if (token == settled.get() && doc === d) holder(i)?.page?.setDetail(bmp, at)
                }
            }
        }
    }

    /** Reports the page filling most of the view, if it's another than the page. */
    private fun track() {
        if (sizes.isEmpty()) return
        val s = list.scale
        val h = list.height.toFloat()
        var best = -1
        var most = 0f
        for (c in 0 until list.childCount) {
            val v = list.getChildAt(c)
            val i = list.getChildAdapterPosition(v)
            if (i == RecyclerView.NO_POSITION) continue
            val seen = min(v.bottom * s + list.ty, h) - max(v.top * s + list.ty, 0f)
            if (seen > most) { most = seen; best = i }
        }
        if (best >= 0 && best != page) report(best)
    }

    private fun report(p: Int) {
        page = p
        events.pageChange(p.toLong())
    }
}

fun makeView(events: ViewEvents): View = PdfView(events)
