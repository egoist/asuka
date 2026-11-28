package dev.asuka

import android.view.Gravity
import android.view.View
import android.view.ViewGroup
import android.widget.FrameLayout
import androidx.recyclerview.widget.DefaultItemAnimator
import androidx.recyclerview.widget.DiffUtil
import androidx.recyclerview.widget.GridLayoutManager
import androidx.recyclerview.widget.ItemTouchHelper
import androidx.recyclerview.widget.LinearLayoutManager
import androidx.recyclerview.widget.RecyclerView
import com.google.android.material.color.MaterialColors
import com.google.android.material.divider.MaterialDividerItemDecoration

/**
 * A List: a RecyclerView whose cells bind their rows (and its sections'
 * headers) through AsukaNative.listBind when they're about to show. Item
 * IDs come from the core; changes are diffed here and dispatched as item
 * ranges and moves. The list's view is a frame around the RecyclerView,
 * so a plain list's section header can stay over its top while its rows
 * scroll under it (UITableView's plain style, which Android's lists don't
 * have): a cell of its own, bound to the section at the top.
 */
class AsukaList(val id: Int, val runtime: AsukaRuntime) {
    val recycler = object : RecyclerView(runtime.themed) {
        override fun onLayout(changed: Boolean, l: Int, t: Int, r: Int, b: Int) {
            super.onLayout(changed, l, t, r, b)
            // Rows inserted or removed change their neighbors' places.
            if (grouped) segments()
            pin()
        }
    }
    /** The list's view: the RecyclerView, and over its top the pinned header. */
    val frame = FrameLayout(runtime.themed)
    private var pinned: AsukaCell? = null
    private var pinnedSection = -1

    // The core's items and sections, and the entries the adapter shows
    // (each section's header, then its rows): their IDs, their indexes as
    // the core counts them (an item's, or -1 - a section's), and sections.
    private var ids = LongArray(0)
    private var starts = IntArray(0)
    private var entries = LongArray(0)
    private var entryIndex = IntArray(0)
    private var entrySection = IntArray(0)
    private var headerPos = IntArray(0)
    private class Update(val ids: LongArray, val starts: IntArray, val sections: LongArray)
    private var pending: Update? = null
    private val adapter = Adapter()
    var appearance = AsukaListAppearance.PLAIN
        private set
    var inset = 0f
        private set
    /** Material 3 Expressive's segmented list: an inset grouped list's look. */
    private val grouped get() = appearance == AsukaListAppearance.INSET_GROUPED && columns == 1
    private val hasSections get() = starts.isNotEmpty()
    private var separators = makeSeparators()

    // Anchored at the bottom, and whether it's at its end (and stays
    // there): only the user's scrolling moves it off, or back.
    private var bottom = false
    private var stuck = true
    private var dragged = false
    private var settling = false // a scroll the core asked for
    // Selecting, and the room the checkbox takes at the rows' end.
    var selecting = false
        private set
    private var rowInsetSent = 0f

    private fun isHeader(position: Int) = position in entryIndex.indices && entryIndex[position] < 0

    /** A list's dividers, none around headers; a segmented list's gaps, 2dp (Material's catalog). */
    private fun makeSeparators(): RecyclerView.ItemDecoration {
        if (appearance == AsukaListAppearance.BARE) return object : RecyclerView.ItemDecoration() {}
        if (!grouped) return object : MaterialDividerItemDecoration(runtime.themed, LinearLayoutManager.VERTICAL) {
            override fun shouldDrawDivider(position: Int, adapter: RecyclerView.Adapter<*>?): Boolean =
                !isHeader(position) && !isHeader(position + 1)
        }.apply { isLastItemDecorated = false }
        val g = Math.round(2 * runtime.density)
        val ends = Math.round(8 * runtime.density)
        return object : RecyclerView.ItemDecoration() {
            override fun getItemOffsets(out: android.graphics.Rect, view: View, parent: RecyclerView, state: RecyclerView.State) {
                val p = parent.getChildAdapterPosition(view)
                if (p == RecyclerView.NO_POSITION) return
                // Each section is a group of its own: its header, then its rows 2dp apart.
                if (isHeader(p)) { if (p > 0) out.top = ends; return }
                if (p > 0 && !isHeader(p - 1)) out.top = g
            }
        }
    }

    init {
        frame.tag = this
        recycler.tag = this
        frame.addView(recycler, FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT))
        recycler.layoutManager = LinearLayoutManager(runtime.themed)
        adapter.setHasStableIds(true)
        recycler.adapter = adapter
        recycler.itemAnimator = null
        recycler.addItemDecoration(separators)
        // A row RecyclerView kept aside comes back without being bound
        // again: in the list's selection mode as it is now.
        recycler.addOnChildAttachStateChangeListener(object : RecyclerView.OnChildAttachStateChangeListener {
            override fun onChildViewAttachedToWindow(view: View) { (view as? AsukaSwipeRow)?.setSelecting(selecting) }
            override fun onChildViewDetachedFromWindow(view: View) {}
        })
        val reporter = AsukaScrollReporter(runtime, id, recycler)
        recycler.addOnScrollListener(object : RecyclerView.OnScrollListener() {
            override fun onScrolled(rv: RecyclerView, dx: Int, dy: Int) {
                reporter.scrolled(rv.computeHorizontalScrollOffset(), rv.computeVerticalScrollOffset())
                if (dragged) stuck = !rv.canScrollVertically(1)
                pin()
            }

            override fun onScrollStateChanged(rv: RecyclerView, state: Int) {
                if (state == RecyclerView.SCROLL_STATE_DRAGGING) dragged = true
                if (state == RecyclerView.SCROLL_STATE_IDLE && (dragged || settling)) {
                    dragged = false
                    settling = false
                    stuck = !rv.canScrollVertically(1)
                }
            }
        })
    }

    var columns = 1
        private set
    var gap = 0f
        private set
    private var gapDecoration: RecyclerView.ItemDecoration? = null

    private fun linear(): LinearLayoutManager = LinearLayoutManager(runtime.themed).apply { stackFromEnd = bottom }

    /**
     * A grid of columns, gap points apart (a GridLayoutManager): each
     * cell an equal share of the width, a row as tall as its tallest, a
     * section's header across the whole row; or, columns 1, the list
     * again, with its dividers.
     */
    fun setGrid(columns: Int, gap: Float) {
        val c = maxOf(columns, 1)
        if (c == this.columns && gap == this.gap) return
        this.columns = c
        this.gap = gap
        gapDecoration?.let { recycler.removeItemDecoration(it) }
        gapDecoration = null
        recycler.removeItemDecoration(separators)
        applyAppearance()
        if (c == 1) {
            recycler.layoutManager = linear()
            separators = makeSeparators()
            recycler.addItemDecoration(separators)
            return
        }
        val lm = GridLayoutManager(runtime.themed, c) // a grid reads from its top (the core's anchor for it)
        lm.spanSizeLookup = object : GridLayoutManager.SpanSizeLookup() {
            override fun getSpanSize(position: Int) = if (isHeader(position)) c else 1
        }.apply { isSpanIndexCacheEnabled = false; isSpanGroupIndexCacheEnabled = false }
        recycler.layoutManager = lm
        val g = gap * runtime.density
        if (g > 0) {
            // Equal shares: column i's cell gives up i*g/c on its start and
            // g - (i+1)*g/c on its end, so each is (width - g*(c-1)) / c.
            val d = object : RecyclerView.ItemDecoration() {
                override fun getItemOffsets(out: android.graphics.Rect, view: View, parent: RecyclerView, state: RecyclerView.State) {
                    val pos = parent.getChildAdapterPosition(view)
                    if (pos == RecyclerView.NO_POSITION) return
                    if (isHeader(pos)) { if (pos > 0) out.top = Math.round(g); return }
                    val i = lm.spanSizeLookup.getSpanIndex(pos, c)
                    val start = Math.round(i * g / c)
                    val end = Math.round(g - (i + 1) * g / c)
                    if (parent.layoutDirection == View.LAYOUT_DIRECTION_RTL) out.set(end, 0, start, 0) else out.set(start, 0, end, 0)
                    // Not above the first row of the list or of a section (its header is).
                    val first = if (hasSections) headerPos[entrySection[pos]] + 1 else 0
                    if (lm.spanSizeLookup.getSpanGroupIndex(pos, c) > lm.spanSizeLookup.getSpanGroupIndex(first, c)) out.top = Math.round(g)
                }
            }
            recycler.addItemDecoration(d)
            gapDecoration = d
        }
    }

    /**
     * Anchored at the bottom (a chat's): it opens at its end, rows that
     * don't fill it sit at its bottom (LinearLayoutManager's stackFromEnd),
     * and while it's at its end it stays there, as rows are added there.
     */
    fun setAnchor(anchor: Int) {
        bottom = anchor == AsukaScrollAnchor.BOTTOM
        stuck = true
        // GridLayoutManager can't stack from its end: a grid isn't anchored.
        (recycler.layoutManager as? LinearLayoutManager)?.takeIf { it !is GridLayoutManager }?.stackFromEnd = bottom
    }

    /** The adapter position of item i. */
    private fun position(i: Int): Int {
        if (!hasSections) return i
        var s = 0
        while (s + 1 < starts.size && starts[s + 1] <= i) s++
        return i + s + 1
    }

    /** Scrolls item i into view, at the top. */
    fun scroll(i: Int, animated: Boolean) {
        applyPending()
        if (i < 0 || i >= ids.size) return
        val p = position(i)
        val lm = recycler.layoutManager as LinearLayoutManager
        if (bottom) stuck = i == ids.size - 1
        if (!animated) {
            // Under a pinned header, a section's first row shows below it.
            lm.scrollToPositionWithOffset(if (hasSections && sticky && p == headerPos[entrySection[p]] + 1) p - 1 else p, 0)
            return
        }
        val s = object : androidx.recyclerview.widget.LinearSmoothScroller(runtime.themed) {
            override fun getVerticalSnapPreference() = SNAP_TO_START
        }
        s.targetPosition = p
        lm.startSmoothScroll(s)
    }

    /** Scrolled to an offset by the core: at the end, a list anchored at its bottom stays there again. */
    fun scrolledTo() {
        if (!bottom) return
        if (recycler.scrollState == RecyclerView.SCROLL_STATE_IDLE) stuck = !recycler.canScrollVertically(1)
        else settling = true
    }

    /**
     * The list's look: inset grouped is Material 3 Expressive's segmented
     * list, its rows inset points from each side (the core lays them out
     * at the list's width less that), 8dp from its top and bottom and 2dp
     * apart (Material's catalog), on the surface container; each row's
     * container is rounded as its place in its group is.
     */
    fun setAppearance(appearance: Int, inset: Float) {
        if (appearance == this.appearance && inset == this.inset) return
        this.appearance = appearance
        this.inset = inset
        applyAppearance()
        if (columns == 1) {
            recycler.removeItemDecoration(separators)
            separators = makeSeparators()
            recycler.addItemDecoration(separators)
        }
    }

    private fun applyAppearance() {
        val side = if (grouped) Math.round(inset * runtime.density) else 0
        val ends = if (grouped) Math.round(8 * runtime.density) else 0
        recycler.setPaddingRelative(side, ends, side, ends)
        recycler.clipToPadding = !grouped
        // The app's background, if it has one.
        val n = runtime.nodes[id]
        if (n == null || n.background == 0L && n.gradient == null && n.material == 0) {
            if (grouped) recycler.setBackgroundColor(MaterialColors.getColor(runtime.themed,
                com.google.android.material.R.attr.colorSurfaceContainerHigh, 0))
            else recycler.background = null
        }
        segments()
        pinned?.let { restylePinned(it) }
        pin()
    }

    /** Rounds each row's container as its place in its group is. */
    private fun segments() {
        for (i in 0 until recycler.childCount) {
            val row = recycler.getChildAt(i) as? AsukaSwipeRow ?: continue
            val p = recycler.getChildAdapterPosition(row)
            segment(row, p)
        }
    }

    private fun segment(row: AsukaSwipeRow, p: Int) {
        if (!grouped || p == RecyclerView.NO_POSITION || p !in entryIndex.indices) {
            row.segment(RecyclerView.NO_POSITION, 0)
            return
        }
        val i = entryIndex[p]
        if (!hasSections) { row.segment(i, ids.size); return }
        val s = entrySection[p]
        val start = starts[s]
        val end = if (s + 1 < starts.size) starts[s + 1] else ids.size
        row.segment(i - start, end - start)
    }

    /** Takes the theme's colors again (after a dark mode change). */
    fun restyle() {
        recycler.removeItemDecoration(separators)
        separators = makeSeparators()
        recycler.addItemDecoration(separators)
        applyAppearance()
        for (i in 0 until recycler.childCount) (recycler.getChildAt(i) as? AsukaSwipeRow)?.restyle()
    }

    fun update(next: LongArray, starts: IntArray, sections: LongArray) {
        pending = Update(next, starts, sections)
    }

    /**
     * Applies what the core last sent. Rows added or removed above the
     * ones on screen leave those where they are (RecyclerView keeps its
     * anchor row in place), unless the list is at the edge it's anchored
     * to: at its top, new rows there show; at its end, anchored at the
     * bottom, it scrolls to show new rows there.
     */
    fun applyPending() {
        if (pending == null) return
        // A row lifted: dropped, its move reported first, if it's been
        // let go; else the drag is called off, and the update shows the
        // core's order.
        lifted?.let { h -> if (released) drop(h) else if (!liftCancelled) cancelLift() }
        val next = pending ?: return
        pending = null
        // An update after a move's animations: as without moves, at once.
        if (!animateNext && lifted == null && recycler.itemAnimator === moveAnimator && !moveAnimator.isRunning) recycler.itemAnimator = null
        val old = entries
        val oldLast = ids.lastOrNull()
        val oldCount = ids.size
        ids = next.ids
        starts = next.starts
        build(next.sections)
        if (old.isEmpty()) {
            adapter.notifyDataSetChanged()
            pinnedSection = -1
            return
        }
        val atTop = !recycler.canScrollVertically(-1)
        val now = entries
        diff(old, now)
        pinnedSection = -1 // its section's index may have changed
        if (moveNames != null) refreshMoveActions()
        when {
            !bottom && atTop && now.isNotEmpty() -> recycler.scrollToPosition(0)
            bottom && stuck && ids.size > oldCount && ids.lastOrNull() != oldLast -> {
                val last = now.size - 1
                if (recycler.isAttachedToWindow && recycler.isLaidOut) recycler.smoothScrollToPosition(last) else recycler.scrollToPosition(last)
            }
        }
    }

    /** Shows now in place of old: their changes as item ranges and moves. */
    private fun diff(old: LongArray, now: LongArray) {
        DiffUtil.calculateDiff(object : DiffUtil.Callback() {
            override fun getOldListSize() = old.size
            override fun getNewListSize() = now.size
            override fun areItemsTheSame(o: Int, n: Int) = old[o] == now[n]
            // The core updates bound rows itself.
            override fun areContentsTheSame(o: Int, n: Int) = true
        }, false).dispatchUpdatesTo(adapter)
    }

    /** The entries: each section's header, then its rows. */
    private fun build(sections: LongArray) {
        if (starts.isEmpty()) {
            entries = ids
            entryIndex = IntArray(ids.size) { it }
            entrySection = IntArray(ids.size)
            headerPos = IntArray(0)
            return
        }
        val n = ids.size + starts.size
        val e = LongArray(n)
        val ix = IntArray(n)
        val sec = IntArray(n)
        val hp = IntArray(starts.size)
        var p = 0
        for (s in starts.indices) {
            hp[s] = p
            e[p] = sections[s]
            ix[p] = -1 - s
            sec[p] = s
            p++
            val end = if (s + 1 < starts.size) starts[s + 1] else ids.size
            for (i in starts[s] until end) {
                e[p] = ids[i]
                ix[p] = i
                sec[p] = s
                p++
            }
        }
        entries = e
        entryIndex = ix
        entrySection = sec
        headerPos = hp
    }

    // Pinned headers

    /** A plain or bare list's headers stick to its top; an inset grouped list's scroll with it. */
    private val sticky get() = hasSections && appearance != AsukaListAppearance.INSET_GROUPED

    /**
     * Shows the header of the section at the list's top over it, unless
     * that section's own header is still in view there; the next header
     * pushes it up as it comes.
     */
    private fun pin() {
        val lm = recycler.layoutManager as? LinearLayoutManager
        if (!sticky || lm == null || entries.isEmpty()) {
            pinned?.visibility = View.GONE
            return
        }
        val top = recycler.paddingTop
        var pos = RecyclerView.NO_POSITION
        for (i in 0 until recycler.childCount) {
            val c = recycler.getChildAt(i)
            val p = recycler.getChildAdapterPosition(c)
            if (p != RecyclerView.NO_POSITION && c.bottom > top && (pos == RecyclerView.NO_POSITION || p < pos)) pos = p
        }
        if (pos == RecyclerView.NO_POSITION || pos !in entrySection.indices) {
            pinned?.visibility = View.GONE
            return
        }
        val s = entrySection[pos]
        val own = lm.findViewByPosition(headerPos[s])
        if (own != null && own.top >= top) {
            pinned?.visibility = View.GONE
            return
        }
        val cell = pinned ?: makePinned()
        if (s != pinnedSection) {
            pinnedSection = s
            cell.binding = true
            runtime.enter { AsukaNative.listBind(id, (-1 - s).toLong(), cell.asukaID) }
            cell.binding = false
        }
        cell.visibility = View.VISIBLE
        val h = cell.rowHeight
        // Its size is the row's, the list's width: placed here, at once (a
        // layout asked for inside the list's own would wait a frame).
        val w = recycler.width
        if (cell.width != w || cell.height != h) {
            cell.measure(View.MeasureSpec.makeMeasureSpec(w, View.MeasureSpec.EXACTLY), View.MeasureSpec.makeMeasureSpec(h, View.MeasureSpec.EXACTLY))
            cell.layout(0, 0, w, h)
        }
        val nextHeader = if (s + 1 < headerPos.size) lm.findViewByPosition(headerPos[s + 1]) else null
        cell.translationY = (top + if (nextHeader != null && nextHeader.top < top + h) nextHeader.top - top - h else 0).toFloat()
    }

    private fun makePinned(): AsukaCell {
        val c = AsukaCell(runtime.themed)
        runtime.makeCell(c, id)
        c.importantForAccessibility = View.IMPORTANT_FOR_ACCESSIBILITY_NO_HIDE_DESCENDANTS // the list's own header row is read
        frame.addView(c, FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT, Gravity.TOP))
        restylePinned(c)
        pinned = c
        return c
    }

    private fun restylePinned(c: AsukaCell) {
        // A plain list's is opaque, over the rows, on the surface; a bare
        // list's draws its own, as its rows do.
        if (appearance == AsukaListAppearance.BARE) c.background = null
        else c.setBackgroundColor(MaterialColors.getColor(runtime.themed, com.google.android.material.R.attr.colorSurface, 0))
    }

    // Moving rows (Reorder)

    /** The screen readers' actions' names (up, down); null: rows don't move. */
    private var moveNames: Array<String>? = null
    val reorders get() = moveNames != null
    private var mover: ItemTouchHelper? = null
    /** The row lifted, and its item's index then (the core's). */
    private var lifted: Holder? = null
    private var liftedFrom = -1
    /** Let go: settling where it was dropped (Moved goes as it's there, clearView). */
    private var released = false
    /** An update came while it was held: it goes back with it, not reported. */
    private var liftCancelled = false
    /** The core's answer to a move (its order back, if the app didn't take it) animates too. */
    private var animateNext = false
    /** While rows move: the others make way, animated (the list has no animator otherwise). */
    private val moveAnimator = DefaultItemAnimator().apply { supportsChangeAnimations = false }

    /**
     * Reorder: rows move (empty: they don't). ItemTouchHelper's long press
     * lifts a row (it takes the place of RowSelectBegin's long press), the
     * others make way as it's dragged, the list scrolls near its edges;
     * the host's IDs are in the new order as it's dropped, and Moved
     * reports it. Each row offers TalkBack the move up and down actions.
     */
    fun setReorder(v: java.nio.ByteBuffer) {
        val names = decodeStrings(v)
        moveNames = if (names.size >= 2) arrayOf(names[0], names[1]) else null
        if (moveNames != null && mover == null) mover = ItemTouchHelper(MoveCallback()).also { it.attachToRecyclerView(recycler) }
        else if (moveNames == null && mover != null) {
            mover?.attachToRecyclerView(null)
            mover = null
            if (recycler.itemAnimator === moveAnimator) recycler.itemAnimator = null
        }
        refreshMoveActions()
    }

    private inner class MoveCallback : ItemTouchHelper.Callback() {
        override fun isLongPressDragEnabled() = moveNames != null
        override fun isItemViewSwipeEnabled() = false

        override fun getMovementFlags(rv: RecyclerView, vh: RecyclerView.ViewHolder): Int {
            val p = vh.bindingAdapterPosition
            if (moveNames == null || p == RecyclerView.NO_POSITION || isHeader(p)) return 0
            val dirs = if (columns > 1) ItemTouchHelper.UP or ItemTouchHelper.DOWN or ItemTouchHelper.START or ItemTouchHelper.END
                else ItemTouchHelper.UP or ItemTouchHelper.DOWN
            return makeMovementFlags(dirs, 0)
        }

        // Not above the first section's header.
        override fun canDropOver(rv: RecyclerView, current: RecyclerView.ViewHolder, target: RecyclerView.ViewHolder): Boolean {
            val p = target.bindingAdapterPosition
            return p != RecyclerView.NO_POSITION && !(hasSections && p == 0)
        }

        override fun onMove(rv: RecyclerView, vh: RecyclerView.ViewHolder, target: RecyclerView.ViewHolder): Boolean {
            val from = vh.bindingAdapterPosition
            val to = target.bindingAdapterPosition
            if (from == RecyclerView.NO_POSITION || to == RecyclerView.NO_POSITION || from == to || liftCancelled) return false
            moveEntry(from, to)
            adapter.notifyItemMoved(from, to)
            return true
        }

        override fun onSwiped(vh: RecyclerView.ViewHolder, direction: Int) {}

        override fun onSelectedChanged(vh: RecyclerView.ViewHolder?, actionState: Int) {
            super.onSelectedChanged(vh, actionState)
            if (actionState == ItemTouchHelper.ACTION_STATE_DRAG && vh is Holder) lift(vh)
            else if (actionState == ItemTouchHelper.ACTION_STATE_IDLE && lifted != null) released = true
        }

        override fun clearView(rv: RecyclerView, vh: RecyclerView.ViewHolder) {
            super.clearView(rv, vh)
            if (vh === lifted) drop(vh as Holder)
        }
    }

    private fun lift(h: Holder) {
        val p = h.bindingAdapterPosition
        if (p == RecyclerView.NO_POSITION) return
        lifted = h
        liftedFrom = entryIndex[p]
        released = false
        liftCancelled = false
        recycler.itemAnimator = moveAnimator
        h.row?.lift(true)
        h.itemView.performHapticFeedback(android.view.HapticFeedbackConstants.LONG_PRESS)
    }

    /** An update while a row's held: let go of it, its move not reported. */
    private fun cancelLift() {
        liftCancelled = true
        val now = android.os.SystemClock.uptimeMillis()
        val cancel = android.view.MotionEvent.obtain(now, now, android.view.MotionEvent.ACTION_CANCEL, 0f, 0f, 0)
        recycler.dispatchTouchEvent(cancel)
        cancel.recycle()
    }

    /**
     * The row is where it was dropped: the IDs are the order shown, each
     * item's index its place there (the core's, once it takes the move),
     * and Moved reports it. A row moved only across a section's header
     * (the same index) goes back: the core's sections are by index.
     */
    private fun drop(h: Holder) {
        lifted = null
        released = false
        h.row?.lift(false)
        val from = liftedFrom
        liftedFrom = -1
        if (liftCancelled) {
            liftCancelled = false
            return
        }
        val p = h.bindingAdapterPosition
        var to = -1
        if (p != RecyclerView.NO_POSITION) {
            to = 0
            for (i in 0 until p) if (entryIndex[i] >= 0) to++
        }
        val shown = entries
        val headers = sectionIDs()
        if (to < 0 || to == from) {
            if (hasSections) {
                build(headers) // its ids and starts are as they were
                diff(shown, entries)
            }
        } else {
            if (hasSections) {
                val nids = LongArray(ids.size)
                val nstarts = IntArray(starts.size)
                var k = 0
                var s = 0
                for (i in shown.indices) {
                    if (entryIndex[i] < 0) nstarts[s++] = k else nids[k++] = shown[i]
                }
                ids = nids
                starts = nstarts
            } else ids = shown.copyOf()
            build(headers)
            val f = from.toDouble()
            val t = to.toDouble()
            animateNext = true
            runtime.emit(id, AsukaEventKind.MOVED) { AsukaEvents.moved(id, f, t) }
            animateNext = false
        }
        segments()
        refreshMoveActions()
    }

    /** The sections' IDs, in the order their headers show. */
    private fun sectionIDs(): LongArray {
        if (!hasSections) return LongArray(0)
        val out = LongArray(starts.size)
        var s = 0
        for (i in entries.indices) if (entryIndex[i] < 0 && s < out.size) out[s++] = entries[i]
        return out
    }

    /** Moves an entry shown from one place to another, its item's index (the core's) with it. */
    private fun moveEntry(from: Int, to: Int) {
        val e = entries[from]
        val x = entryIndex[from]
        if (from < to) {
            System.arraycopy(entries, from + 1, entries, from, to - from)
            System.arraycopy(entryIndex, from + 1, entryIndex, from, to - from)
        } else {
            System.arraycopy(entries, to, entries, to + 1, from - to)
            System.arraycopy(entryIndex, to, entryIndex, to + 1, from - to)
        }
        entries[to] = e
        entryIndex[to] = x
        if (!hasSections) return
        // A row past a header is in its section now.
        var s = -1
        for (i in entries.indices) {
            if (entryIndex[i] < 0) { s++; headerPos[s] = i }
            entrySection[i] = maxOf(s, 0)
        }
    }

    /** A screen reader's move: the row one place up (-1) or down (1), animated, and reported. */
    private fun moveBy(h: Holder, delta: Int): Boolean {
        applyPending()
        val p = h.bindingAdapterPosition
        if (moveNames == null || lifted != null || p == RecyclerView.NO_POSITION || p !in entryIndex.indices) return false
        val from = entryIndex[p]
        val to = from + delta
        if (from < 0 || to < 0 || to >= ids.size) return false
        val old = entries
        val headers = sectionIDs()
        val next = ids.copyOf()
        val moving = next[from]
        if (from < to) System.arraycopy(next, from + 1, next, from, to - from) else System.arraycopy(next, to, next, to + 1, from - to)
        next[to] = moving
        ids = next
        build(headers) // the sections start where they did
        recycler.itemAnimator = moveAnimator
        diff(old, entries)
        val f = from.toDouble()
        val t = to.toDouble()
        animateNext = true
        runtime.emit(id, AsukaEventKind.MOVED) { AsukaEvents.moved(id, f, t) }
        animateNext = false
        segments()
        refreshMoveActions()
        return true
    }

    /** The rows' move actions, as their places are now (none up from the first, down from the last). */
    private fun refreshMoveActions() {
        for (i in 0 until recycler.childCount) {
            val row = recycler.getChildAt(i) as? AsukaSwipeRow ?: continue
            moveActions(row, recycler.getChildAdapterPosition(row))
        }
    }

    private fun moveActions(row: AsukaSwipeRow, p: Int) {
        val names = moveNames
        val i = if (names != null && p in entryIndex.indices) entryIndex[p] else -1
        row.setMoveActions(if (i > 0) names!![0] else null, if (i >= 0 && i < ids.size - 1) names!![1] else null)
    }

    // Selection

    /**
     * Selecting rows: each shows a Material checkbox at its end, a tap
     * selects it, and rows don't swipe. The core lays the rows out at
     * the width they have (RowInset).
     */
    fun setSelecting(on: Boolean) {
        if (on == selecting) return
        selecting = on
        for (i in 0 until recycler.childCount) (recycler.getChildAt(i) as? AsukaSwipeRow)?.setSelecting(on)
        val trail = if (on) AsukaSwipeRow.CHECK_WIDTH else 0f
        if (trail != rowInsetSent) {
            rowInsetSent = trail
            runtime.emit(id, AsukaEventKind.ROW_INSET) { AsukaEvents.rowInset(id, 0.0, trail.toDouble()) }
        }
    }

    class Holder(val row: AsukaSwipeRow?, val header: AsukaCell?) : RecyclerView.ViewHolder(row ?: header!!) {
        val cell: AsukaCell get() = row?.cell ?: header!!
    }

    inner class Adapter : RecyclerView.Adapter<Holder>() {
        override fun getItemCount() = entries.size
        override fun getItemId(position: Int) = entries[position]
        override fun getItemViewType(position: Int) = if (entryIndex[position] < 0) 1 else 0

        override fun onCreateViewHolder(parent: ViewGroup, viewType: Int): Holder {
            if (viewType == 1) {
                val h = AsukaCell(runtime.themed)
                h.layoutParams = RecyclerView.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT)
                runtime.makeCell(h, id)
                androidx.core.view.ViewCompat.setAccessibilityHeading(h, true)
                return Holder(null, h)
            }
            val row = AsukaSwipeRow(runtime, AsukaCell(runtime.themed))
            row.layoutParams = RecyclerView.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT)
            runtime.makeCell(row.cell, id)
            val h = Holder(row, null)
            row.onMove = { delta -> moveBy(h, delta) }
            return h
        }

        override fun onBindViewHolder(holder: Holder, position: Int) {
            holder.row?.let {
                it.close(animated = false)
                it.setSelecting(selecting)
                segment(it, position)
            }
            val cell = holder.cell
            cell.binding = true
            runtime.enter { AsukaNative.listBind(id, entryIndex[position].toLong(), cell.asukaID) }
            cell.binding = false
            holder.row?.let { if (moveNames != null || it.hasMoveActions) moveActions(it, position) }
        }
    }
}

/**
 * Reports a scroll view's offset (pixels in, points out) as Scrolled: on
 * the main thread's next turn, the latest once. Scrolling happens while
 * the view draws (an animated scroll, a fling), and what the core does
 * with the event (adding, removing views) mustn't happen in the middle
 * of that: it stops the scroll.
 */
class AsukaScrollReporter(private val rt: AsukaRuntime, private val id: Int, private val view: View) {
    private var x = 0
    private var y = 0
    private var posted = false
    private val send = Runnable {
        posted = false
        // Not a view on its way out, whose ID may be another view's by now.
        val n = rt.nodes[id]
        if (!view.isAttachedToWindow || n == null || (n.view !== view && n.list?.recycler !== view)) return@Runnable
        val d = rt.density
        rt.emit(id, AsukaEventKind.SCROLLED) { AsukaEvents.scrolled(id, (x / d).toDouble(), (y / d).toDouble()) }
    }

    fun scrolled(x: Int, y: Int) {
        this.x = x
        this.y = y
        if (!posted) {
            posted = true
            rt.onMain { send.run() }
        }
    }
}

/**
 * Offers a row's swipe actions to TalkBack, as actions on the row (its
 * pressable content, which TalkBack focuses): swiping isn't something a
 * screen reader's user can do. Leading actions' events carry -1 - their
 * index, as the core expects.
 */
fun AsukaRuntime.swipeAccessibility(n: AsukaNode) {
    n.swipeA11y?.let { (v, ids) -> for (i in ids) androidx.core.view.ViewCompat.removeAccessibilityAction(v, i) }
    n.swipeA11y = null
    if (n.swipeActions.isEmpty() && n.leadingSwipeActions.isEmpty()) return
    var v: View = n.view
    while (!v.isClickable && v is ViewGroup && v.childCount == 1) v = v.getChildAt(0)
    val id = n.id
    val all = n.leadingSwipeActions.mapIndexed { i, a -> a to (-1L - i) } + n.swipeActions.mapIndexed { i, a -> a to i.toLong() }
    val ids = all.map { (a, k) ->
        androidx.core.view.ViewCompat.addAccessibilityAction(v, a.title) { _, _ ->
            emit(id, AsukaEventKind.ACTION) { AsukaEvents.action(id, k) }
            true
        }
    }
    n.swipeA11y = v to ids
}
