#if canImport(UIKit)
import UIKit
#if canImport(AsukaOps)
import AsukaOps
#endif
#if canImport(CAsuka)
@_implementationOnly import CAsuka
#endif

/// A list cell, or a section's header: its content view holds a row the
/// core lays out; the row's height comes from the core.
final class AsukaListCell: UICollectionViewListCell {
    var asukaID: UInt32 = 0
    var rowHeight: CGFloat = 44
    weak var list: AsukaList?
    /// The core says its row is selected (shown while the list is selecting).
    var asukaSelected = false
    /// A header's, while it's pinned at the top: iOS 26's scroll edge
    /// effect goes behind it (UIScrollEdgeElementContainerInteraction).
    var edgeInteraction: UIInteraction?

    /// With Reorder, VoiceOver's actions that move its row up and down a
    /// place (where it can go), after its own.
    override var accessibilityCustomActions: [UIAccessibilityCustomAction]? {
        get {
            let own = super.accessibilityCustomActions
            guard let moves = list?.moveActions(self), !moves.isEmpty else { return own }
            return (own ?? []) + moves
        }
        set { super.accessibilityCustomActions = newValue }
    }

    func setRowHeight(_ h: CGFloat) {
        guard h != rowHeight else { return }
        rowHeight = h
        invalidateIntrinsicContentSize()
        list?.heightsChanged()
    }

    override func preferredLayoutAttributesFitting(_ attrs: UICollectionViewLayoutAttributes) -> UICollectionViewLayoutAttributes {
        let a = super.preferredLayoutAttributesFitting(attrs)
        a.size.height = rowHeight
        return a
    }
}

/// The collection view of a List: it tells the list as it lays out, so a
/// list anchored at its bottom can stay there.
final class AsukaListView: UICollectionView {
    weak var owner: AsukaList?

    override func layoutSubviews() {
        owner?.willLayout()
        super.layoutSubviews()
        owner?.didLayout()
    }
}

/// Lets the list's layout configuration reach the list made after it.
final class AsukaWeakList {
    weak var list: AsukaList?
}

/// A List: a UICollectionView with a list layout, fed by a diffable data
/// source from the core's item IDs (in sections, when the core sends
/// them, each with a header the core binds too). Cells bind their rows
/// through asuka_list_bind when they're about to show.
@MainActor final class AsukaList: NSObject, UICollectionViewDelegate {
    let id: UInt32
    weak var runtime: AsukaRuntime?
    let collection: AsukaListView
    var source: UICollectionViewDiffableDataSource<Int64, Int64>!
    var pending: (ids: [Int64], sections: [(start: Int, id: Int64)])?
    var applied = false
    var binding = false
    private var applying = false
    var columns = 1
    var gap: CGFloat = 0
    var appearance = AsukaListAppearance.plain
    var inset: CGFloat = 0
    /// The first item's index of each section shown (one section: [0]).
    private var starts = [0]
    private var hasSections = false
    #if !os(tvOS) // no swipe actions on a TV
    private var swipes: UICollectionLayoutListConfiguration.SwipeActionsConfigurationProvider!
    private var leadingSwipes: UICollectionLayoutListConfiguration.SwipeActionsConfigurationProvider!
    #endif

    // Anchored at the bottom: whether it's at its end (and stays there),
    // the room added above rows that don't fill it, and an animated scroll
    // to the end under way, which layout leaves alone (it ends at the end).
    private var bottom = false
    private var stuck = true
    private var extraTop: CGFloat = 0
    private var scrollingToEnd = false
    // Selecting, and the room its control takes (leading, trailing).
    private(set) var editing = false
    private var rowInset = CGPoint.zero
    /// Reorder: the names of VoiceOver's actions that move a row up and
    /// down (nil: rows don't move).
    private(set) var reorder: (up: String, down: String)?

    init(id: UInt32, runtime: AsukaRuntime) {
        self.id = id
        self.runtime = runtime
        let box = AsukaWeakList()
        collection = AsukaListView(frame: .zero, collectionViewLayout: UICollectionViewFlowLayout())
        super.init()
        #if !os(tvOS)
        // A row's swipe actions are its cell's (the core sends them as it binds it).
        swipes = { [weak runtime] path in
            guard let runtime, let list = box.list else { return nil }
            return list.swipeConfiguration(runtime, path, leading: false)
        }
        leadingSwipes = { [weak runtime] path in
            guard let runtime, let list = box.list else { return nil }
            return list.swipeConfiguration(runtime, path, leading: true)
        }
        #endif
        box.list = self
        collection.owner = self
        collection.delegate = self
        collection.keyboardDismissMode = .interactive
        // Rows handle their own presses; a cell selection would stick.
        // Selecting (edit mode), a tap selects its row.
        collection.allowsSelection = false
        collection.allowsSelectionDuringEditing = true
        collection.allowsMultipleSelectionDuringEditing = true
        collection.selfSizingInvalidation = .enabled
        relayout()
        let reg = UICollectionView.CellRegistration<AsukaListCell, Int64> { _, _, _ in }
        source = UICollectionViewDiffableDataSource(collectionView: collection) { [weak self] cv, path, item in
            let cell = cv.dequeueConfiguredReusableCell(using: reg, for: path, item: item)
            guard let self, let runtime = self.runtime else { return cell }
            cell.list = self
            if self.appearance == .bare { cell.backgroundConfiguration = .clear() }
            cell.accessories = [.multiselect(displayed: .whenEditing)]
            cell.contentView.isUserInteractionEnabled = !self.editing
            if cell.asukaID == 0 {
                cell.asukaID = runtime.makeCell(cell)
            }
            self.bind(runtime, self.index(of: path), cell)
            return cell
        }
        let headers = UICollectionView.SupplementaryRegistration<AsukaListCell>(elementKind: UICollectionView.elementKindSectionHeader) { [weak self] cell, _, path in
            guard let self, let runtime = self.runtime else { return }
            cell.list = self
            cell.backgroundConfiguration = switch self.appearance {
            case .bare: .clear()
            case .insetGrouped: .listGroupedHeaderFooter()
            default: .listPlainHeaderFooter()
            }
            if cell.asukaID == 0 {
                cell.asukaID = runtime.makeCell(cell)
            }
            self.bind(runtime, -1 - path.section, cell)
        }
        source.supplementaryViewProvider = { cv, kind, path in
            cv.dequeueConfiguredReusableSupplementary(using: headers, for: path)
        }
        // Reorder: a row dragged to another place is moved by the data
        // source, its cell kept (the collection view's drag and drop).
        source.reorderingHandlers.canReorderItem = { [weak self] _ in self?.canReorder ?? false }
        source.reorderingHandlers.didReorder = { [weak self] t in self?.reordered(t) }
    }

    private func bind(_ runtime: AsukaRuntime, _ index: Int, _ cell: AsukaListCell) {
        binding = true
        runtime.enter { asuka_list_bind(self.id, Int64(index), cell.asukaID) }
        binding = false
    }

    /// Item path's index among all the list's items.
    private func index(of path: IndexPath) -> Int {
        (path.section < starts.count ? starts[path.section] : 0) + path.item
    }

    /// The path of item i, among the sections shown.
    private func path(of i: Int) -> IndexPath? {
        guard i >= 0, i < collection.numberOfItemsTotal else { return nil }
        var s = 0
        while s + 1 < starts.count && starts[s + 1] <= i { s += 1 }
        return IndexPath(item: i - starts[s], section: s)
    }

    #if !os(tvOS)
    private func swipeConfiguration(_ runtime: AsukaRuntime, _ path: IndexPath, leading: Bool) -> UISwipeActionsConfiguration? {
        guard !editing, let cell = collection.cellForItem(at: path) as? AsukaListCell,
              let n = runtime.nodes[cell.asukaID] else { return nil }
        let acts = leading ? n.leadingSwipeActions : n.swipeActions
        guard !acts.isEmpty else { return nil }
        let id = cell.asukaID
        let actions = acts.enumerated().map { i, act in // flat: numbered by their place
            let a = UIContextualAction(style: act.role == AsukaAlertButtonRole.destructive.rawValue ? .destructive : .normal,
                                       title: act.title) { [weak runtime] _, _, done in
                runtime?.emit(id, .action) { AsukaEvents.action(id, leading ? Int64(-1 - i) : Int64(i)) }
                done(true)
            }
            a.image = act.image
            // A leading action that isn't destructive is tinted, as Mail's are.
            if leading && act.role != AsukaAlertButtonRole.destructive.rawValue { a.backgroundColor = .tintColor }
            return a
        }
        return UISwipeActionsConfiguration(actions: actions)
    }
    #endif

    func update(_ ids: [Int64], sections: [(start: Int, id: Int64)]) { pending = (ids, sections) }

    // MARK: Layout

    /// The list layout's configuration, in an appearance.
    private func config(_ appearance: UICollectionLayoutListConfiguration.Appearance) -> UICollectionLayoutListConfiguration {
        var config = UICollectionLayoutListConfiguration(appearance: appearance)
        if self.appearance == .bare { config.backgroundColor = .clear }
        config.headerMode = hasSections ? .supplementary : .none
        #if !os(tvOS)
        config.showsSeparators = self.appearance != .bare
        config.trailingSwipeActionsConfigurationProvider = swipes
        config.leadingSwipeActionsConfigurationProvider = leadingSwipes
        #endif
        return config
    }

    /// Lays the list out again for its look: a list (plain, bare, or
    /// inset grouped, its rows inset from each side by exactly the core's
    /// inset, which lays them out at the list's width less it, not by the
    /// collection view's layout margins), or a grid of columns, gap
    /// apart (as LazyVGrid's: each cell an equal share of the width, a
    /// row as tall as its tallest). A plain or bare list's headers stick
    /// to its top, as UITableView's plain style's do.
    private func relayout() {
        let pin = appearance != .insetGrouped
        let layout: UICollectionViewLayout
        if columns > 1 {
            let columns = columns, gap = gap, headers = hasSections
            layout = UICollectionViewCompositionalLayout { _, env in
                let w = (env.container.effectiveContentSize.width - gap * CGFloat(columns - 1)) / CGFloat(columns)
                let item = NSCollectionLayoutItem(layoutSize: .init(widthDimension: .absolute(max(w, 1)), heightDimension: .estimated(100)))
                let group = NSCollectionLayoutGroup.horizontal(
                    layoutSize: .init(widthDimension: .fractionalWidth(1), heightDimension: .estimated(100)),
                    repeatingSubitem: item, count: columns)
                group.interItemSpacing = .fixed(gap)
                let section = NSCollectionLayoutSection(group: group)
                section.interGroupSpacing = gap
                if headers {
                    let h = NSCollectionLayoutBoundarySupplementaryItem(
                        layoutSize: .init(widthDimension: .fractionalWidth(1), heightDimension: .estimated(44)),
                        elementKind: UICollectionView.elementKindSectionHeader, alignment: .top)
                    h.pinToVisibleBounds = pin
                    h.zIndex = 2
                    section.boundarySupplementaryItems = [h]
                    section.contentInsets.bottom = gap
                }
                return section
            }
        } else {
            #if os(tvOS)
            let config = config(appearance == .insetGrouped ? .grouped : .plain) // tvOS's grouped rows are inset
            #else
            let config = config(appearance == .insetGrouped ? .insetGrouped : .plain)
            #endif
            let grouped = appearance == .insetGrouped, inset = inset
            layout = UICollectionViewCompositionalLayout { _, env in
                let section = NSCollectionLayoutSection.list(using: config, layoutEnvironment: env)
                if grouped {
                    section.contentInsetsReference = .none
                    section.contentInsets.leading = inset
                    section.contentInsets.trailing = inset
                }
                for h in section.boundarySupplementaryItems {
                    h.pinToVisibleBounds = pin
                    h.zIndex = 2
                }
                return section
            }
        }
        collection.backgroundColor = appearance == .bare ? .clear : nil
        collection.setCollectionViewLayout(layout, animated: false)
    }

    func setGrid(columns: Int, gap: CGFloat) {
        let columns = max(columns, 1)
        guard columns != self.columns || gap != self.gap else { return }
        self.columns = columns
        self.gap = gap
        relayout()
    }

    func setAppearance(_ appearance: AsukaListAppearance, inset: CGFloat) {
        guard appearance != self.appearance || inset != self.inset else { return }
        self.appearance = appearance
        self.inset = inset
        relayout()
    }

    /// Anchored at the bottom: it opens at its end and stays there while
    /// the user leaves it there.
    func setAnchor(_ a: AsukaScrollAnchor) {
        bottom = a == .bottom
        stuck = true
        collection.setNeedsLayout()
    }

    // MARK: Scrolling

    private var maxOffsetY: CGFloat {
        let i = collection.adjustedContentInset
        return max(-i.top, collection.contentSize.height - collection.bounds.height + i.bottom)
    }

    private var atEnd: Bool { collection.contentOffset.y >= maxOffsetY - 1 }
    private var atStart: Bool { collection.contentOffset.y <= -collection.adjustedContentInset.top + 1 }

    /// Before the collection view lays out: rows that don't fill a list
    /// anchored at its bottom sit at its bottom, and one at its end goes
    /// to its end (its new end: rows added, rows grown, the list shorter).
    func willLayout() {
        guard bottom, collection.bounds.height > 0 else { return }
        let content = collection.collectionViewLayout.collectionViewContentSize.height
        let i = collection.adjustedContentInset
        let free = max(0, collection.bounds.height - (i.top - extraTop) - i.bottom - content)
        if abs(free - extraTop) > 0.5 {
            collection.contentInset.top += free - extraTop
            extraTop = free
        }
        guard stuck, !scrollingToEnd, !collection.isTracking, !collection.isDecelerating else { return }
        let a = collection.adjustedContentInset
        let end = max(-a.top, content - collection.bounds.height + a.bottom)
        if abs(collection.contentOffset.y - end) > 0.5 {
            collection.contentOffset.y = end
        }
    }

    /// After: rows sized as they showed may have moved the end.
    func didLayout() {
        pinEdgeElements()
        guard bottom, stuck, !scrollingToEnd, !collection.isTracking, !collection.isDecelerating else { return }
        if abs(collection.contentOffset.y - maxOffsetY) > 0.5 { collection.setNeedsLayout() }
    }

    /// The header pinned at the top is an element over the content: iOS
    /// 26's scroll edge effect goes behind it, not over it (it would blur
    /// it away). Only that one: the effect's region (which takes touches)
    /// reaches as far down as its elements do.
    private func pinEdgeElements() {
        guard #available(iOS 26.0, tvOS 26.0, *), hasSections else { return }
        let top = collection.contentOffset.y + collection.adjustedContentInset.top
        let pin = appearance != .insetGrouped
        for case let c as AsukaListCell in collection.visibleSupplementaryViews(ofKind: UICollectionView.elementKindSectionHeader) {
            let pinned = pin && abs(c.frame.minY - top) < 0.5
            if pinned, c.edgeInteraction == nil {
                let i = UIScrollEdgeElementContainerInteraction()
                i.scrollView = collection
                i.edge = .top
                c.addInteraction(i)
                c.edgeInteraction = i
            } else if !pinned, let i = c.edgeInteraction {
                c.removeInteraction(i)
                c.edgeInteraction = nil
            }
        }
    }

    func scrollViewDidScroll(_ s: UIScrollView) {
        // Only the user's scrolling moves a list off its end, or back.
        if bottom && (s.isTracking || s.isDecelerating) && !scrollingToEnd { stuck = atEnd }
        runtime?.nodes[id]?.scrolls?.scrollViewDidScroll(s)
    }

    func scrollViewDidEndDecelerating(_ s: UIScrollView) {
        if bottom { stuck = atEnd }
    }

    func scrollViewDidEndDragging(_ s: UIScrollView, willDecelerate: Bool) {
        if bottom && !willDecelerate { stuck = atEnd }
    }

    func scrollViewWillBeginDragging(_ s: UIScrollView) {
        // The user takes over: UIKit stops a scroll under way (and doesn't
        // say it ended), and where they leave the list decides.
        scrollingToEnd = false
    }

    func scrollViewDidEndScrollingAnimation(_ s: UIScrollView) {
        let toEnd = scrollingToEnd
        scrollingToEnd = false
        guard bottom else { return }
        if toEnd {
            // A scroll to the end ends at the end as it is now: rows
            // measured as they came into view, or the list shorter (the
            // keyboard came up), while it ran moved it. Layout goes there.
            stuck = true
            collection.setNeedsLayout()
        } else {
            stuck = atEnd
        }
    }

    func scrollViewDidScrollToTop(_ s: UIScrollView) {
        if bottom { stuck = atEnd }
    }

    /// Scrolls item i into view, at the top; a list anchored at its
    /// bottom, to its last item: to its end.
    func scroll(to i: Int, animated: Bool) {
        applyPending()
        guard let p = path(of: i) else { return }
        if bottom && i == collection.numberOfItemsTotal - 1 {
            collection.layoutIfNeeded()
            scroll(to: CGPoint(x: collection.contentOffset.x, y: maxOffsetY), animated: animated)
            return
        }
        if bottom { stuck = false }
        scrollingToEnd = false
        collection.scrollToItem(at: p, at: .top, animated: animated)
    }

    /// Scrolls to an offset the core gives (from the content's start,
    /// clamped to what it scrolls). To the end, a list anchored at its
    /// bottom stays there again, and an animated scroll ends at its end
    /// as it is then (scrollViewDidEndScrollingAnimation).
    func scroll(to p: CGPoint, animated: Bool) {
        let end = bottom && p.y >= maxOffsetY - 1
        if bottom { stuck = end }
        let to = end ? CGPoint(x: p.x, y: maxOffsetY) : p
        let o = collection.contentOffset
        // Already there: nothing to animate. UIKit would still run an
        // animation of no distance, setting the offset it started at as
        // layout keeps the list at its end, and its own end would decide
        // whether the list is.
        guard abs(to.x - o.x) > 0.5 || abs(to.y - o.y) > 0.5 else { return }
        scrollingToEnd = animated && end
        collection.setContentOffset(to, animated: animated)
    }

    // MARK: Updates

    /// Applies what the core last sent. A row being bound can change the list
    /// it's in (its language changing, say), and a data source mustn't be applied
    /// to from inside itself or a cell's making: that waits until it's done.
    ///
    /// Rows added or removed above the ones on screen keep those where
    /// they are, unless the list is at the edge it's anchored to: at its
    /// start, new rows there show (a list read from the top); at its end,
    /// anchored at the bottom, it scrolls to show new rows there.
    func applyPending() {
        guard !applying else { return }
        guard !binding else {
            DispatchQueue.main.async { [weak self] in self?.applyPending() }
            return
        }
        applying = true
        defer { applying = false }
        while let (ids, sections) = pending {
            pending = nil
            var snap = NSDiffableDataSourceSnapshot<Int64, Int64>()
            var next = [0]
            let headers = !sections.isEmpty
            if headers {
                next = []
                for (k, s) in sections.enumerated() {
                    let end = k + 1 < sections.count ? sections[k + 1].start : ids.count
                    snap.appendSections([s.id])
                    snap.appendItems(Array(ids[min(s.start, ids.count)..<min(max(end, s.start), ids.count)]), toSection: s.id)
                    next.append(s.start)
                }
            } else {
                snap.appendSections([0])
                snap.appendItems(ids)
            }
            if headers != hasSections {
                hasSections = headers
                relayout()
            }
            let old = source.snapshot().itemIdentifiers
            let wasApplied = applied
            let atEdge = bottom ? stuck : atStart
            let keep = wasApplied && !atEdge ? visibleAnchor() : nil
            // What's above the row kept in place changed: no animation,
            // and the row back where it was.
            var moved = false
            if let k = keep, let o = old.firstIndex(of: k.id), let n = ids.firstIndex(of: k.id) {
                moved = o != n || old[..<o] != ids[..<n]
            }
            let appendedAtEnd = wasApplied && bottom && stuck && ids.count > old.count && ids.last != old.last
            starts = next
            source.apply(snap, animatingDifferences: wasApplied && !moved)
            applied = true
            if moved, let k = keep, let p = source.indexPath(for: k.id) {
                collection.layoutIfNeeded()
                if let a = collection.layoutAttributesForItem(at: p) {
                    collection.contentOffset.y = a.frame.minY - k.dy
                }
            }
            if appendedAtEnd && collection.window != nil {
                // New rows at the end of a chat the user is at the end of.
                collection.layoutIfNeeded()
                let end = CGPoint(x: collection.contentOffset.x, y: maxOffsetY)
                if abs(end.y - collection.contentOffset.y) > 0.5 {
                    scrollingToEnd = true
                    collection.setContentOffset(end, animated: true)
                }
            }
        }
    }

    /// The first row on screen, and its top's distance below the offset.
    private func visibleAnchor() -> (id: Int64, dy: CGFloat)? {
        let top = collection.contentOffset.y + collection.adjustedContentInset.top
        var best: (IndexPath, CGRect)?
        for p in collection.indexPathsForVisibleItems {
            guard let a = collection.layoutAttributesForItem(at: p), a.frame.maxY > top else { continue }
            if best == nil || a.frame.minY < best!.1.minY { best = (p, a.frame) }
        }
        guard let (p, f) = best, let id = source.itemIdentifier(for: p) else { return nil }
        return (id, f.minY - collection.contentOffset.y)
    }

    /// The layout mirrors its cells for the collection view's direction.
    func directionChanged() {
        collection.collectionViewLayout.invalidateLayout()
        measureRowInset()
    }

    func heightsChanged() {
        guard !binding else { return }
        collection.collectionViewLayout.invalidateLayout()
    }

    // MARK: Reordering

    /// Rows move: with Reorder, while not selecting.
    var canReorder: Bool { reorder != nil && !editing }

    /// Reorder's names (u32 count, the strings): two, rows move; none,
    /// they don't. A long press lifts a row, which the others make way
    /// for as it's dragged (scrolling at the list's edges), and dropping
    /// it moves it (UICollectionView's drag and drop, restricted to the
    /// app, the data source's reordering).
    func setReorder(_ names: [String]) {
        reorder = names.count >= 2 ? (names[0], names[1]) : nil
        #if !os(tvOS) // no drag and drop on a TV: VoiceOver's actions alone
        let on = reorder != nil
        collection.dragDelegate = on ? self : nil
        collection.dropDelegate = on ? self : nil
        collection.dragInteractionEnabled = on // on iPhone too
        collection.reorderingCadence = .immediate
        #endif
    }

    /// The user dropped a row in another place: the data source moved it
    /// (the IDs are in the new order, so the core's update in that order
    /// changes nothing), and the core hears of it with Moved.
    private func reordered(_ t: NSDiffableDataSourceTransaction<Int64, Int64>) {
        starts = Self.starts(t.finalSnapshot, hasSections)
        guard let (from, to) = Self.move(t.initialSnapshot.itemIdentifiers, t.finalSnapshot.itemIdentifiers) else { return }
        let id = self.id
        runtime?.emit(id, .moved) { AsukaEvents.moved(id, x: Double(from), y: Double(to)) }
    }

    /// The first item's index of each of snap's sections.
    private static func starts(_ snap: NSDiffableDataSourceSnapshot<Int64, Int64>, _ sections: Bool) -> [Int] {
        guard sections else { return [0] }
        var at = 0
        return snap.sectionIdentifiers.map { s in
            defer { at += snap.numberOfItems(inSection: s) }
            return at
        }
    }

    /// The one item moved from a to b: its index before and after.
    static func move(_ a: [Int64], _ b: [Int64]) -> (Int, Int)? {
        guard a.count == b.count, let i = a.indices.first(where: { a[$0] != b[$0] }),
              let j = a.indices.last(where: { a[$0] != b[$0] }) else { return nil }
        return a[i] == b[j] ? (i, j) : (j, i)
    }

    /// VoiceOver's actions for cell's row: up a place, unless it's first;
    /// down, unless it's last.
    func moveActions(_ cell: AsukaListCell) -> [UIAccessibilityCustomAction] {
        guard canReorder, let names = reorder, let p = collection.indexPath(for: cell) else { return [] }
        let i = index(of: p), count = collection.numberOfItemsTotal
        var out: [UIAccessibilityCustomAction] = []
        if i > 0 {
            out.append(UIAccessibilityCustomAction(name: names.up) { [weak self, weak cell] _ in self?.move(cell, by: -1) ?? false })
        }
        if i + 1 < count {
            out.append(UIAccessibilityCustomAction(name: names.down) { [weak self, weak cell] _ in self?.move(cell, by: 1) ?? false })
        }
        return out
    }

    /// Moves cell's row a place up (-1) or down (1), animated, its cell
    /// kept, and tells the core with Moved.
    private func move(_ cell: AsukaListCell?, by d: Int) -> Bool {
        applyPending()
        guard canReorder, let cell, let p = collection.indexPath(for: cell), let item = source.itemIdentifier(for: p) else { return false }
        var snap = source.snapshot()
        let ids = snap.itemIdentifiers
        let from = index(of: p), to = from + d
        guard to >= 0, to < ids.count else { return false }
        if d < 0 { snap.moveItem(item, beforeItem: ids[to]) } else { snap.moveItem(item, afterItem: ids[to]) }
        starts = Self.starts(snap, hasSections)
        source.apply(snap, animatingDifferences: collection.window != nil)
        let id = self.id
        runtime?.emit(id, .moved) { AsukaEvents.moved(id, x: Double(from), y: Double(to)) }
        UIAccessibility.post(notification: .layoutChanged, argument: cell) // VoiceOver stays on it
        return true
    }

    // MARK: Selection

    /// Selecting rows (UIKit's edit mode): each row shows its selection
    /// control, a tap selects it, its content doesn't take touches, and
    /// rows don't swipe. The rows the core says are selected show it.
    func setEditing(_ on: Bool) {
        coreEditing = on
        applyEditing(on)
    }

    /// What the core last said of Selecting.
    private var coreEditing = false

    private func applyEditing(_ on: Bool) {
        guard on != editing else { return }
        editing = on
        // Selection is the list's while it's selecting; otherwise rows
        // handle their own presses, and a cell selection would stick.
        collection.allowsSelection = on
        collection.allowsMultipleSelection = on
        collection.isEditing = on
        for case let c as AsukaListCell in collection.visibleCells {
            c.contentView.isUserInteractionEnabled = !on
        }
        if on {
            for case let c as AsukaListCell in collection.visibleCells where c.asukaSelected {
                if let p = collection.indexPath(for: c) { collection.selectItem(at: p, animated: false, scrollPosition: []) }
            }
        } else {
            // Not selecting, no row looks selected.
            for p in collection.indexPathsForSelectedItems ?? [] { collection.deselectItem(at: p, animated: false) }
        }
        measureRowInset()
    }

    /// The core says cell's row is (or isn't) selected.
    func setSelected(_ cell: AsukaListCell, _ v: Bool) {
        cell.asukaSelected = v
        guard editing, !binding, let p = collection.indexPath(for: cell) else { return }
        showSelection(cell, p)
    }

    private func showSelection(_ cell: AsukaListCell, _ p: IndexPath) {
        let shown = collection.indexPathsForSelectedItems?.contains(p) ?? false
        let want = editing && cell.asukaSelected
        if want && !shown { collection.selectItem(at: p, animated: false, scrollPosition: []) }
        if !want && shown { collection.deselectItem(at: p, animated: false) }
    }

    func collectionView(_ cv: UICollectionView, willDisplay cell: UICollectionViewCell, forItemAt p: IndexPath) {
        guard let c = cell as? AsukaListCell else { return }
        c.contentView.isUserInteractionEnabled = !editing
        showSelection(c, p)
        if editing && rowInset == .zero { measureRowInset() }
    }

    func collectionView(_ cv: UICollectionView, didSelectItemAt p: IndexPath) {
        userSelected(p, true)
    }

    func collectionView(_ cv: UICollectionView, didDeselectItemAt p: IndexPath) {
        userSelected(p, false)
    }

    private func userSelected(_ p: IndexPath, _ v: Bool) {
        guard editing, let c = collection.cellForItem(at: p) as? AsukaListCell else { return }
        c.asukaSelected = v
        let id = c.asukaID
        runtime?.emit(id, .rowSelected) { AsukaEvents.rowSelected(id, v) }
    }

    #if !os(tvOS)
    /// A two-finger pan over rows selects them: while selecting, or to
    /// begin selecting when the app asks to hear of it.
    func collectionView(_ cv: UICollectionView, shouldBeginMultipleSelectionInteractionAt p: IndexPath) -> Bool {
        if editing { return true }
        guard let c = cv.cellForItem(at: p) as? AsukaListCell else { return false }
        return runtime?.nodes[c.asukaID]?.listens.contains(AsukaEventKind.rowSelectBegin.rawValue) ?? false
    }

    func collectionView(_ cv: UICollectionView, didBeginMultipleSelectionInteractionAt p: IndexPath) {
        guard !editing, let c = cv.cellForItem(at: p) as? AsukaListCell else { return }
        // At once, so the pan selects as it goes; the core's Selecting
        // follows, or, if the app doesn't turn it on, it ends.
        applyEditing(true)
        let id = c.asukaID
        runtime?.emit(id, .rowSelectBegin) { AsukaEvents.rowSelectBegin(id) }
        DispatchQueue.main.async { [weak self] in
            guard let self, !self.coreEditing else { return }
            self.applyEditing(false)
        }
    }
    #endif

    /// Tells the core how much room the selection control takes from the
    /// rows (the core lays them out at the width they have).
    private func measureRowInset() {
        var next = CGPoint.zero
        if editing {
            collection.layoutIfNeeded()
            guard let c = collection.visibleCells.compactMap({ $0 as? AsukaListCell }).first else { return }
            c.layoutIfNeeded()
            let f = c.contentView.frame
            let left = f.minX, right = c.bounds.width - f.maxX
            next = collection.effectiveUserInterfaceLayoutDirection == .rightToLeft ? CGPoint(x: right, y: left) : CGPoint(x: left, y: right)
        }
        guard next != rowInset else { return }
        rowInset = next
        runtime?.emit(id, .rowInset) { AsukaEvents.rowInset(id, x: Double(next.x), y: Double(next.y)) }
    }
}

#if !os(tvOS)
/// Reorder's drags: a row's, within the list, dropped between its rows;
/// the data source moves it (reorderingHandlers).
extension AsukaList: UICollectionViewDragDelegate, UICollectionViewDropDelegate {
    func collectionView(_ cv: UICollectionView, itemsForBeginning session: UIDragSession, at p: IndexPath) -> [UIDragItem] {
        guard canReorder, let item = source.itemIdentifier(for: p) else { return [] }
        let d = UIDragItem(itemProvider: NSItemProvider())
        d.localObject = item
        return [d]
    }

    func collectionView(_ cv: UICollectionView, dragSessionIsRestrictedToDraggingApplication session: UIDragSession) -> Bool { true }

    func collectionView(_ cv: UICollectionView, dragSessionWillBegin session: UIDragSession) {
        AsukaPressRecognizer.cancelAll() // the lifted row isn't pressed
    }

    func collectionView(_ cv: UICollectionView, canHandle session: UIDropSession) -> Bool {
        session.localDragSession != nil && cv.hasActiveDrag
    }

    func collectionView(_ cv: UICollectionView, dropSessionDidUpdate session: UIDropSession, withDestinationIndexPath p: IndexPath?) -> UICollectionViewDropProposal {
        guard cv.hasActiveDrag, canReorder else { return UICollectionViewDropProposal(operation: .forbidden) }
        return UICollectionViewDropProposal(operation: .move, intent: .insertAtDestinationIndexPath)
    }

    /// A row dropped in the list is the data source's to move (UIKit
    /// doesn't call this for its reordering): nothing else drops here.
    func collectionView(_ cv: UICollectionView, performDropWith coordinator: UICollectionViewDropCoordinator) {}
}
#endif

extension UICollectionView {
    /// Its items, in all its sections.
    var numberOfItemsTotal: Int {
        (0..<numberOfSections).reduce(0) { $0 + numberOfItems(inSection: $1) }
    }
}
#endif

#if canImport(UIKit)
/// Reports a scroll view's offset (from its content's start) as Scrolled.
/// A list forwards its collection view's scrolling to it.
@MainActor final class AsukaScrollReporter: NSObject, UIScrollViewDelegate {
    let id: UInt32
    weak var runtime: AsukaRuntime?
    private var last = CGPoint(x: CGFloat.nan, y: .nan)

    init(_ id: UInt32, _ rt: AsukaRuntime) {
        self.id = id
        runtime = rt
    }

    func scrollViewDidScroll(_ s: UIScrollView) {
        // Not a view on its way out (its insets change as it leaves the
        // window), whose ID may be another view's by the time it's read.
        guard s.window != nil, runtime?.nodes[id]?.view === s else { return }
        let i = s.adjustedContentInset
        let p = CGPoint(x: s.contentOffset.x + i.left, y: s.contentOffset.y + i.top)
        guard p != last else { return }
        last = p
        let id = self.id
        runtime?.emit(id, .scrolled) { AsukaEvents.scrolled(id, x: Double(p.x), y: Double(p.y)) }
    }

    /// A paging Scroll's swipe: the page it's settling on, from the content's
    /// physical start, as soon as UIKit knows it.
    func scrollViewWillEndDragging(_ s: UIScrollView, withVelocity velocity: CGPoint, targetContentOffset: UnsafeMutablePointer<CGPoint>) {
        guard s.isPagingEnabled, s.bounds.width > 0, s.bounds.height > 0 else { return }
        let i = s.adjustedContentInset
        let t = targetContentOffset.pointee
        let horizontal = (s as? AsukaScrollView)?.horizontal ?? (s.contentSize.width > s.bounds.width)
        let page = horizontal ? ((t.x + i.left) / s.bounds.width).rounded() : ((t.y + i.top) / s.bounds.height).rounded()
        let id = self.id
        runtime?.emit(id, .paged) { AsukaEvents.paged(id, Int64(page)) }
    }

    // A zoomable Scroll's zoom (Scroll.swift).

    func viewForZooming(in s: UIScrollView) -> UIView? {
        guard let z = s as? AsukaScrollView, z.zooms else { return nil }
        return z.zoomView
    }

    func scrollViewWillBeginZooming(_ s: UIScrollView, with view: UIView?) {
        (s as? AsukaScrollView)?.zoomBegan()
    }

    func scrollViewDidZoom(_ s: UIScrollView) {
        (s as? AsukaScrollView)?.zoomChanged(ended: false)
    }

    func scrollViewDidEndZooming(_ s: UIScrollView, with view: UIView?, atScale scale: CGFloat) {
        (s as? AsukaScrollView)?.zoomChanged(ended: true)
    }
}
#endif
