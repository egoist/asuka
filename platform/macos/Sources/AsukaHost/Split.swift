// A SplitView is an NSSplitViewController: a sidebar item (the source
// list's material, collapsible) holding a source-list NSTableView of the
// destinations, their sections as group rows, and a content item holding
// the detail Stack, which fills it.
import AppKit
#if canImport(AsukaOps)
import AsukaOps
#endif

/// A section's title (group) or a destination (index among all the
/// destinations).
struct AsukaSidebarRow: Equatable {
    var title = "", icon = "", badge = ""
    var group = false
    var index = 0
}

@MainActor final class AsukaSplit {
    var ctl: NSSplitViewController?
    var table: NSTableView?
    /// The content item's view, the Stack's superview.
    var detail: AsukaView?
    var rows: [AsukaSidebarRow] = []
    /// The chosen destination, among them all.
    var selected = -1
    var reloading = false
    /// The detail's frame changes, observed.
    var detailObserver: NSObjectProtocol?
}

/// The content item's view: the detail Stack, below the window's toolbar.
/// A window whose content is a split view goes to its top edge (the
/// sidebar under the window's buttons, as AppKit's split view apps have
/// it), and the toolbar is over the detail: the Stack takes the room under
/// it, its safe area. Under a hidden title bar the Stack fills it, and its
/// screens' safe area is the bar's.
final class AsukaSplitDetail: AsukaView {
    override func resizeSubviews(withOldSize oldSize: NSSize) { layoutStack() }

    override func layout() {
        super.layout()
        layoutStack()
    }

    func layoutStack() {
        guard let v = subviews.first else { return }
        var top: CGFloat = 0
        // Under a title bar the app hid, the content is the app's to place
        // (its screens' safe area says where the bar is).
        let hidden = MainActor.assumeIsolated { AsukaRuntime.shared.win(of: window)?.cfg.titleBar == .hidden }
        if #available(macOS 11.0, *), !hidden { top = safeAreaInsets.top }
        let r = CGRect(x: 0, y: top, width: bounds.width, height: max(bounds.height - top, 0))
        if v.frame != r { v.frame = r }
    }
}

/// A row's view: its symbol, title and badge, which it lays out itself as
/// it's sized, from its leading edge.
final class AsukaSidebarCell: NSTableCellView {
    /// The badge, at its trailing end.
    var badge: NSTextField?

    /// Its own layout, not NSTableCellView's (which places its text field
    /// and image view as a row size style has them, over the badge).
    override func resizeSubviews(withOldSize oldSize: NSSize) { layoutRow() }

    override func layout() {
        super.layout()
        layoutRow()
    }

    /// Places its symbol at its leading edge, its badge at its trailing
    /// edge and its title between them, mirrored right to left.
    func layoutRow() {
        let b = bounds
        let w = b.width, ht = b.height
        let rtl = userInterfaceLayoutDirection == .rightToLeft
        func place(_ v: NSView, _ x: CGFloat, _ width: CGFloat, _ height: CGFloat) {
            let x = rtl ? w - x - width : x
            v.frame = CGRect(x: x, y: (ht - height) / 2, width: max(width, 0), height: height)
        }
        var x: CGFloat = 4, end = w - 4
        if let iv = imageView {
            place(iv, 4, 18, 16)
            x = 28
        }
        if let badge {
            let s = badge.fittingSize
            end = w - 8 - s.width
            place(badge, end, s.width, s.height)
            end -= 6
        }
        if let label = textField { place(label, x, end - x, label.intrinsicContentSize.height) }
    }
}

@MainActor extension AsukaRuntime {
    func makeSplit(_ n: AsukaNode) -> NSView {
        let s = AsukaSplit()
        n.split = s
        let table = NSTableView(frame: .zero)
        table.addTableColumn(NSTableColumn(identifier: NSUserInterfaceItemIdentifier("item")))
        table.headerView = nil
        if #available(macOS 11.0, *) { table.style = .sourceList }
        table.selectionHighlightStyle = .sourceList
        table.rowSizeStyle = .medium
        table.dataSource = navTarget
        table.delegate = navTarget
        s.table = table
        let scroll = NSScrollView(frame: .zero)
        scroll.documentView = table
        scroll.drawsBackground = false
        scroll.hasVerticalScroller = true
        scroll.autohidesScrollers = true
        let sideVC = NSViewController()
        sideVC.view = scroll
        let detail = AsukaSplitDetail(frame: .zero)
        s.detail = detail
        let contentVC = NSViewController()
        contentVC.view = detail
        let ctl = NSSplitViewController()
        let side = NSSplitViewItem(sidebarWithViewController: sideVC)
        let content = NSSplitViewItem(viewController: contentVC)
        side.minimumThickness = 180
        side.maximumThickness = 320
        ctl.addSplitViewItem(side)
        ctl.addSplitViewItem(content)
        s.ctl = ctl
        navTarget.splits[ObjectIdentifier(table)] = n
        return ctl.view
    }

    func splitOf(_ t: NSTableView) -> AsukaSplit? { navTarget.splits[ObjectIdentifier(t)]?.split }

    func setSplitProp(_ n: AsukaNode, _ p: UInt16, _ v: AsukaValue) {
        guard let s = n.split, let table = s.table else { return }
        switch p {
        case AsukaProp.Split.items:
            var rows: [AsukaSidebarRow] = []
            if case .bytes(let b) = v {
                var flat = 0
                for sec in AsukaSidebar.decode(b) {
                    if !sec.title.isEmpty { rows.append(AsukaSidebarRow(title: sec.title, group: true)) }
                    for it in sec.items {
                        rows.append(AsukaSidebarRow(title: it.title, icon: it.icon, badge: it.badge, index: flat))
                        flat += 1
                    }
                }
            }
            if sameRows(s.rows, rows) {
                // As many sections and items: the rows that changed change in
                // place, keeping the selection, the scroll and VoiceOver's
                // focus.
                let rtl = isRTL(n)
                for r in rows.indices where rows[r] != s.rows[r] {
                    s.rows[r] = rows[r]
                    if let cell = table.view(atColumn: 0, row: r, makeIfNecessary: false) as? AsukaSidebarCell {
                        fillSidebarCell(cell, rows[r], rtl)
                    }
                }
                return
            }
            s.rows = rows
            s.reloading = true
            table.reloadData()
            s.reloading = false
            showSidebarSelection(s)
        case AsukaProp.Split.selected:
            s.selected = Int(v.int)
            showSidebarSelection(s)
        case AsukaProp.Split.title:
            s.ctl?.title = v.string
        default:
            break
        }
    }

    /// Whether a and b are as many rows, each a group or a destination as
    /// before: they change in place.
    func sameRows(_ a: [AsukaSidebarRow], _ b: [AsukaSidebarRow]) -> Bool {
        guard a.count == b.count else { return false }
        return zip(a, b).allSatisfy { $0.group == $1.group && $0.index == $1.index }
    }

    /// Shows a row in its view (a new one, or one shown already, in
    /// place): a destination's symbol, title and badge, or a section's
    /// title in the group row's style.
    func fillSidebarCell(_ cell: AsukaSidebarCell, _ r: AsukaSidebarRow, _ rtl: Bool) {
        cell.userInterfaceLayoutDirection = rtl ? .rightToLeft : .leftToRight
        let label: NSTextField
        if let l = cell.textField {
            label = l
        } else {
            label = NSTextField(labelWithString: "")
            label.lineBreakMode = .byTruncatingTail
            cell.addSubview(label)
            cell.textField = label
        }
        label.stringValue = r.title
        label.alignment = .natural // the way its text reads
        if r.group {
            label.font = .systemFont(ofSize: 11, weight: NSFont.Weight(0.3))
            label.textColor = .secondaryLabelColor
        }
        if !r.group, let img = symbolImage(r.icon) {
            let iv: NSImageView
            if let x = cell.imageView {
                iv = x
            } else {
                iv = NSImageView(frame: .zero)
                iv.contentTintColor = .controlAccentColor
                cell.addSubview(iv)
                cell.imageView = iv
            }
            iv.image = img
        } else if let iv = cell.imageView {
            iv.removeFromSuperview()
            cell.imageView = nil
        }
        // The badge: a count at the trailing end in the secondary color, as
        // Mail counts a mailbox's unread messages; VoiceOver reads it after
        // the title, a count as so many items.
        if !r.badge.isEmpty && !r.group {
            let badge: NSTextField
            if let b = cell.badge {
                badge = b
            } else {
                badge = NSTextField(labelWithString: "")
                badge.identifier = NSUserInterfaceItemIdentifier("asuka.badge")
                badge.font = .monospacedDigitSystemFont(ofSize: NSFont.systemFontSize, weight: .regular)
                badge.textColor = .secondaryLabelColor
                cell.addSubview(badge)
                cell.badge = badge
            }
            badge.stringValue = r.badge
            badge.setAccessibilityLabel(asukaBadge(r.badge))
        } else if let b = cell.badge {
            b.removeFromSuperview()
            cell.badge = nil
        }
        cell.layoutRow()
    }

    /// Selects the chosen destination's row.
    func showSidebarSelection(_ s: AsukaSplit) {
        guard let table = s.table else { return }
        for (r, row) in s.rows.enumerated() where !row.group && row.index == s.selected {
            if table.selectedRow != r {
                s.reloading = true
                table.selectRowIndexes(IndexSet(integer: r), byExtendingSelection: false)
                s.reloading = false
            }
            return
        }
    }

    /// The user choosing a row.
    func sidebarChose(_ table: NSTableView) {
        guard let n = navTarget.splits[ObjectIdentifier(table)], let s = n.split, !s.reloading else { return }
        let r = table.selectedRow
        guard r >= 0, r < s.rows.count, !s.rows[r].group, s.rows[r].index != s.selected else { return }
        s.selected = s.rows[r].index
        let i = Int64(s.selected)
        emit(n, .selected) { AsukaEvents.selected($0, i) }
    }

    /// Chooses the sidebar's row titled title, as a click on it would.
    func tapSidebar(_ title: String) -> Bool {
        for n in nodes.values {
            guard let s = n.split, let table = s.table, table.window?.isVisible == true else { continue }
            if let r = s.rows.firstIndex(where: { !$0.group && $0.title == title }) {
                table.selectRowIndexes(IndexSet(integer: r), byExtendingSelection: false)
                return true
            }
        }
        return false
    }

    /// Puts the detail Stack in the content item, filling it.
    func insertSplit(_ p: AsukaNode, _ c: AsukaNode, _ i: Int) -> Bool {
        guard let s = p.split, let detail = s.detail else { return false }
        p.children.insert(c, at: min(i, p.children.count))
        c.parent = p
        guard let v = c.view else { return true }
        detail.addSubview(v)
        (detail as? AsukaSplitDetail)?.layoutStack()
        // The content item sizes the Stack after the window's resize is
        // reported (a full-size content view's split lays out late), so its
        // screens follow its own frame.
        v.postsFrameChangedNotifications = true
        if let o = s.detailObserver { NotificationCenter.default.removeObserver(o) }
        s.detailObserver = NotificationCenter.default.addObserver(forName: NSView.frameDidChangeNotification, object: v, queue: nil) { [weak c] _ in
            MainActor.assumeIsolated {
                guard let c else { return }
                AsukaRuntime.shared.layoutScreens(c)
            }
        }
        return true
    }

    /// The rows lay out from their leading edge.
    func splitDirectionChanged(_ n: AsukaNode) {
        guard let s = n.split, let table = s.table else { return }
        s.reloading = true
        table.reloadData()
        s.reloading = false
        showSidebarSelection(s)
    }

    /// A split view's rows, as its table shows them, for the automation's
    /// tree.
    func splitJSON(_ n: AsukaNode, _ d: inout [String: Any]) {
        guard let s = n.split, let table = s.table else { return }
        d["selectedRow"] = table.selectedRow
        d["sidebarSelected"] = s.selected
        d["title"] = s.ctl?.title ?? ""
        if let item = s.ctl?.splitViewItems.first { d["sidebarIsSidebar"] = item.behavior == .sidebar }
        if let dv = s.detail { d["detailFrame"] = asukaRectJSON(dv.frame) }
        d["rows"] = s.rows.enumerated().map { r, row -> [String: Any] in
            var x: [String: Any] = ["title": row.title, "group": row.group, "badge": row.badge, "icon": row.icon, "index": row.index]
            if let cell = table.view(atColumn: 0, row: r, makeIfNecessary: false) as? AsukaSidebarCell {
                x["cellID"] = ObjectIdentifier(cell).hashValue
                x["shownTitle"] = cell.textField?.stringValue ?? ""
                if let b = cell.badge {
                    x["shownBadge"] = b.stringValue
                    x["badgeLabel"] = b.accessibilityLabel() ?? ""
                    x["badgeFrame"] = asukaRectJSON(b.frame)
                }
                if let t = cell.textField { x["titleFrame"] = asukaRectJSON(t.frame) }
                x["cellWidth"] = Double(cell.bounds.width)
                x["hasImage"] = cell.imageView != nil
            }
            return x
        }
    }
}

extension AsukaNavTarget: NSTableViewDataSource, NSTableViewDelegate {
    func numberOfRows(in tableView: NSTableView) -> Int { rt.splitOf(tableView)?.rows.count ?? 0 }

    func tableView(_ tableView: NSTableView, isGroupRow row: Int) -> Bool {
        guard let s = rt.splitOf(tableView), row < s.rows.count else { return false }
        return s.rows[row].group
    }

    func tableView(_ tableView: NSTableView, shouldSelectRow row: Int) -> Bool {
        guard let s = rt.splitOf(tableView), row < s.rows.count else { return false }
        return !s.rows[row].group
    }

    func tableView(_ tableView: NSTableView, viewFor tableColumn: NSTableColumn?, row: Int) -> NSView? {
        guard let n = rt.navTarget.splits[ObjectIdentifier(tableView)], let s = n.split, row < s.rows.count else { return nil }
        let cell = AsukaSidebarCell(frame: .zero)
        rt.fillSidebarCell(cell, s.rows[row], rt.isRTL(n))
        return cell
    }

    func tableViewSelectionDidChange(_ notification: Notification) {
        guard let t = notification.object as? NSTableView else { return }
        rt.sidebarChose(t)
    }
}
