// Moving a List's rows (Reorder), as a Mac's lists move theirs (Finder's
// sidebar, Reminders): a drag of a row with the mouse, past the drag
// distance, lifts it (a shadow over the rows, which it follows the mouse
// over), the others make way, animated, and dragged near the list's top or
// bottom it scrolls; dropped, the host moves its item (its IDs in the new
// order, its row view kept: never bound again) and reports Moved. Each
// row offers screen readers the two actions (NSAccessibilityCustomAction):
// up one place and down one, as there is room. Single-column lists only:
// a grid's rows would bind other items.
//
// The core's ListUpdate that follows a move it took is in the host's order
// (it sends none); one that puts the old order back moves the row views
// back, animated: an update keeps each row view on its item
// (Lists.swift's take).
import AppKit
#if canImport(AsukaOps)
import AsukaOps
#endif

/// A row lifted and dragged.
@MainActor final class AsukaRowMove {
    let rv: AsukaRowView
    /// The row it was at, and the one it would drop at.
    let from: Int
    var to: Int
    let height: CGFloat
    /// Where the mouse holds it, from its top.
    let grab: CGFloat
    /// Its top now, in the document view.
    var y: CGFloat
    /// The mouse, in the window.
    var loc: CGPoint
    var timer: Timer?

    init(_ rv: AsukaRowView, from: Int, height: CGFloat, grab: CGFloat, y: CGFloat, loc: CGPoint) {
        self.rv = rv
        self.from = from
        to = from
        self.height = height
        self.grab = grab
        self.y = y
        self.loc = loc
    }

    /// How far row r moves to make way.
    func shift(_ r: Int) -> CGFloat {
        if r > from && r <= to { return -height }
        if r >= to && r < from { return height }
        return 0
    }
}

/// How close to the list's edge, at most, a dragged row scrolls it, and
/// how fast (points a frame) at the edge itself.
private let edgeBand: CGFloat = 44
private let edgeSpeed: CGFloat = 16

@MainActor extension AsukaList {
    /// Whether the user can move its rows.
    var reorders: Bool { reorder.count >= 2 && columns <= 1 && !selecting }

    /// Reorder's names changed (or it's on or off): the rows' actions
    /// again.
    func setReorder(_ names: [String]) {
        guard names != reorder else { return }
        reorder = names
        if !reorders { cancelRowDrag() }
        for c in allCells { c.moveActions = 0x80 } // made again as they're placed
        queue()
    }

    // MARK: Dragging

    /// Lifts c's row, the mouse having gone down at down (in the window):
    /// false if it can't move now.
    func liftRow(_ c: AsukaListCell, _ down: CGPoint) -> Bool {
        guard reorders, move == nil, !updated, let rv = c.rowView, rv.row >= 0, rv.row < rows.count, !rows[rv.row].header,
              tops.count == rows.count + 1, shown[rv.row] === rv else { return false }
        let r = rv.row
        let p = doc.convert(down, from: nil)
        let m = AsukaRowMove(rv, from: r, height: rowHeight(r), grab: p.y - tops[r], y: tops[r], loc: down)
        move = m
        // Over the other rows (a pinned header's over it still), with the
        // system's lifted look: a shadow, an opaque background.
        rv.view.layer?.zPosition = 1
        rv.sep?.isHidden = true
        if let l = rv.view.layer {
            rt.withAppearance(n) { l.backgroundColor = NSColor.controlBackgroundColor.cgColor }
            l.shadowColor = NSColor.black.cgColor
            l.shadowOpacity = 0.25
            l.shadowRadius = 8
            l.shadowOffset = CGSize(width: 0, height: 3) // below it (the layer's y is up)
        }
        NSAccessibility.post(element: c.content, notification: .layoutChanged)
        return true
    }

    /// The mouse moved to loc (in the window) holding the lifted row.
    func dragRow(_ loc: CGPoint) {
        guard let m = move, tops.count == rows.count + 1 else { return }
        m.loc = loc
        let p = doc.convert(loc, from: nil)
        let total = tops.last ?? 0
        m.y = min(max(p.y - m.grab, tops.first ?? 0), max(total - pad - m.height, 0))
        let f = m.rv.view.frame
        m.rv.view.setFrameOrigin(CGPoint(x: f.minX, y: m.y))
        // The slot it would drop in: after the rows whose middle is above
        // its middle (the others but it), below the first header.
        let mid = m.y + m.height / 2
        var to = asukaSearch(rows.count) { self.tops[$0] + self.rowHeight($0) / 2 >= mid }
        if m.from < to { to -= 1 }
        if rows.first?.header == true { to = max(to, 1) }
        to = min(max(to, 0), rows.count - 1)
        if to != m.to {
            m.to = to
            placeShown(animated: true)
        }
        autoscroll()
    }

    /// Drops the lifted row where it is: its item moves there.
    func dropRow() {
        guard let m = move else { return }
        endLift(m)
        guard m.to != m.from, m.from < rows.count, m.to < rows.count else {
            placeShown(animated: true)
            return
        }
        // The rows in their new order: the item's new index, and each
        // section's first item's.
        var order = Array(rows.indices)
        order.remove(at: m.from)
        order.insert(m.from, at: m.to)
        var items = 0, toItem = 0
        var starts: [Int] = []
        for r in order {
            if rows[r].header {
                starts.append(items)
            } else {
                if r == m.from { toItem = items }
                items += rows[r].count
            }
        }
        moveItem(rows[m.from].first, toItem, starts.isEmpty ? nil : starts)
    }

    /// Puts the lifted row back where it was.
    func cancelRowDrag() {
        guard let m = move else { return }
        endLift(m)
        placeShown(animated: true)
    }

    private func endLift(_ m: AsukaRowMove) {
        m.timer?.invalidate()
        m.timer = nil
        move = nil
        if let l = m.rv.view.layer {
            l.zPosition = 0
            l.shadowOpacity = 0
            l.backgroundColor = nil
        }
        headerBackground(m.rv)
    }

    /// Scrolls the list while the lifted row is near its top or bottom
    /// edge, faster the nearer, a frame at a time.
    func autoscroll() {
        guard let m = move else { return }
        if edgeStep() == 0 {
            m.timer?.invalidate()
            m.timer = nil
            return
        }
        guard m.timer == nil else { return }
        let t = Timer(timeInterval: 1.0 / 60, repeats: true) { [weak self] _ in
            MainActor.assumeIsolated { self?.autoscrollTick() }
        }
        RunLoop.main.add(t, forMode: .common)
        m.timer = t
    }

    /// The scroll a frame of autoscrolling makes (0: none).
    private func edgeStep() -> CGFloat {
        guard let m = move else { return 0 }
        let vb = visible
        let p = doc.convert(m.loc, from: nil)
        let band = min(edgeBand, vb.height / 4)
        let least = -scroll.contentInsets.top, most = endY
        guard band > 0 else { return 0 }
        if p.y < vb.minY + band && vb.minY > least + 0.5 {
            return -edgeSpeed * min((vb.minY + band - p.y) / band, 1.5)
        }
        if p.y > vb.maxY - band && vb.minY < most - 0.5 {
            return edgeSpeed * min((p.y - (vb.maxY - band)) / band, 1.5)
        }
        return 0
    }

    private func autoscrollTick() {
        guard let m = move, let n, rt.alive(n) else {
            move?.timer?.invalidate()
            move?.timer = nil
            return
        }
        let d = edgeStep()
        guard d != 0 else {
            m.timer?.invalidate()
            m.timer = nil
            return
        }
        let vb = visible
        let y = min(max(vb.minY + d, -scroll.contentInsets.top), endY)
        scroll.contentView.scroll(to: CGPoint(x: vb.minX, y: y))
        scroll.reflectScrolledClipView(scroll.contentView)
        dragRow(m.loc) // the row stays under the mouse
    }

    // MARK: Moving

    /// Moves the item at from to to (its IDs, its row view with it),
    /// with the sections' new starts if a drag moved it past a header,
    /// and reports Moved.
    func moveItem(_ from: Int, _ to: Int, _ starts: [Int]?) {
        guard from != to, from >= 0, to >= 0, from < ids.count, to < ids.count, !updated else { return }
        let keep = shownKeys()
        let id = ids.remove(at: from)
        ids.insert(id, at: to)
        for i in min(from, to)...max(from, to) { rowOf[ids[i]] = i }
        if let starts {
            for i in sections.indices where i < starts.count { sections[i].start = starts[i] }
        }
        makeRows()
        rekey(keep)
        measureTops()
        doc.setFrameSize(CGSize(width: width, height: tops.last ?? 0))
        placeShown(animated: true)
        placeGroup()
        guard let n else { return }
        // A ListUpdate putting the old order back (the app didn't take the
        // move) comes as the core takes the event: it animates too.
        animateNext = true
        rt.emit(n, .moved) { AsukaEvents.moved($0, x: Double(from), y: Double(to)) }
        if !updated { animateNext = false }
    }

    // MARK: Screen readers

    /// Gives row r's cells the move actions there's room for (none when
    /// its rows don't move), when that changed.
    func moveActions(_ rv: AsukaRowView, _ r: Int) {
        let row = rows[r]
        for (i, c) in rv.cells.enumerated() where i < max(row.count, 1) {
            var want: UInt8 = 0
            if reorders && !row.header {
                let at = row.first + i
                want = 4
                if at > 0 { want |= 1 }
                if at < ids.count - 1 { want |= 2 }
            }
            if c.moveActions == want { continue }
            c.moveActions = want
            var acts: [NSAccessibilityCustomAction] = []
            if want & 1 != 0 { acts.append(NSAccessibilityCustomAction(name: reorder[0]) { [weak c] in c?.moveBy(-1) ?? false }) }
            if want & 2 != 0 { acts.append(NSAccessibilityCustomAction(name: reorder[1]) { [weak c] in c?.moveBy(1) ?? false }) }
            c.content.setAccessibilityCustomActions(acts)
            // An element (a group: its views are read in it) for the actions
            // to be offered on.
            c.content.setAccessibilityElement(want != 0)
            if want != 0 { c.content.setAccessibilityRole(.group) }
        }
    }

    // MARK: Row views

    /// The row views shown, by their rows' keys.
    func shownKeys() -> [(key: Int64, header: Bool, rv: AsukaRowView)] {
        shown.compactMap { r, rv in r < rows.count ? (rowKey(r), rows[r].header, rv) : nil }
    }

    /// Shows each kept row view at its item's (or section's) row now:
    /// rows that moved keep their views, bound as they were; those gone go
    /// back to the pool. A single column's rows only.
    func rekey(_ keep: [(key: Int64, header: Bool, rv: AsukaRowView)]) {
        guard columns <= 1 else { return }
        var next: [Int: AsukaRowView] = [:]
        next.reserveCapacity(keep.count)
        for (key, header, rv) in keep {
            var r = -1
            if header {
                if let s = sections.firstIndex(where: { $0.id == key }) { r = min(sections[s].start, ids.count) + s }
                if r >= 0 && (r >= rows.count || !rows[r].header || sections[rows[r].section].id != key) { r = -1 }
            } else if let i = rowOf[key] {
                r = sections.isEmpty ? i : i + asukaSearch(sections.count) { self.sections[$0].start > i }
                if r >= rows.count || rows[r].header || rows[r].first != i { r = -1 }
            }
            if r >= 0 && next[r] == nil {
                next[r] = rv
                rv.row = r
            } else {
                rv.view.isHidden = true
                rv.row = -1
                free.append(rv)
            }
        }
        shown = next
    }

    /// Places the rows shown again (animated: as they make way).
    func placeShown(animated: Bool) {
        guard tops.count == rows.count + 1 else { return }
        func all() {
            for (r, rv) in shown where r < rows.count { place(rv, r) }
        }
        guard animated, !rt.env.reduceMotion, scroll.window?.isVisible == true else {
            all()
            return
        }
        NSAnimationContext.runAnimationGroup { ctx in
            ctx.duration = 0.2
            ctx.allowsImplicitAnimation = true
            animatePlaces = true
            all()
            animatePlaces = false
        }
    }

    func reorderJSON(_ j: inout [String: Any]) {
        guard !reorder.isEmpty else { return }
        var r: [String: Any] = ["names": reorder, "on": reorders]
        if let m = move { r["move"] = ["from": m.from, "to": m.to, "y": Double(m.y), "autoscrolling": m.timer != nil] }
        j["reorder"] = r
    }
}

@MainActor extension AsukaListCell {
    /// A screen reader's move action: its item d places up or down.
    func moveBy(_ d: Int) -> Bool {
        guard let l = list, l.reorders, l.move == nil, !l.updated, let i = l.rowOf[item] else { return false }
        let to = i + d
        guard to >= 0, to < l.ids.count else { return false }
        l.moveItem(i, to, nil)
        return true
    }
}
