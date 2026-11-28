// A TextField's suggestions on a Mac. A Dropdown field is an NSComboBox:
// its button opens the list of the Suggestions, which also opens under it
// as the user types (the core has filtered them), and its arrow keys,
// Return and Escape are its own. A field without one is an NSTextField
// with a suggestion window under it, as Safari's and Spotlight's are: a
// borderless child window of the menus' material, an NSTableView of the
// suggestions, which Up and Down move in (the field keeps the keyboard),
// Return or a click chooses from and Escape closes. Choosing one puts its
// text in the field, the cursor at its end, and reports Suggestion, not
// TextChanged.
import AppKit
#if canImport(AsukaOps)
import AsukaOps
#endif

/// What a TextField with suggestions keeps.
@MainActor final class AsukaSuggest {
    /// The Suggestions as last set: what the list shows, in order.
    var items: [String] = []
    var dropdown = false
    /// The user has typed since the field took the focus or since a
    /// choice: the list may show.
    var typed = false
    /// The host is changing the combo box's list: its selection changing
    /// isn't the user's.
    var quiet = false
    /// A plain field's suggestion window.
    var window: AsukaSuggestWindow?
}

/// A Dropdown TextField's view: it reports its focus, as AsukaTextField.
final class AsukaComboBox: NSComboBox {
    override func becomeFirstResponder() -> Bool {
        let ok = super.becomeFirstResponder()
        if ok, let n = AsukaRuntime.shared.node(of: self) { AsukaRuntime.shared.fieldFocused(n) }
        return ok
    }

    /// Its list shows (NSComboBoxCell's pop-up, as a screen reader's
    /// expanded state says).
    var listShown: Bool {
        get { cell?.isAccessibilityExpanded() ?? false }
        set { if newValue != listShown { cell?.setAccessibilityExpanded(newValue) } }
    }
}

/// The suggestion window's: the panel never takes the keyboard (the field
/// keeps it).
final class AsukaSuggestPanel: NSPanel {
    override var canBecomeKey: Bool { false }
    override var canBecomeMain: Bool { false }
}

/// The rows of a suggestion window: a click chooses the row under it, the
/// pointer moving over one highlights it, as a menu's.
final class AsukaSuggestTable: NSTableView {
    var choose: ((Int) -> Void)?

    override func mouseDown(with e: NSEvent) {
        let r = row(at: convert(e.locationInWindow, from: nil))
        if r >= 0 { choose?(r) }
    }

    override func updateTrackingAreas() {
        super.updateTrackingAreas()
        for t in trackingAreas where t.owner === self { removeTrackingArea(t) }
        addTrackingArea(NSTrackingArea(rect: .zero, options: [.mouseMoved, .activeAlways, .inVisibleRect], owner: self, userInfo: nil))
    }

    override func mouseMoved(with e: NSEvent) {
        let r = row(at: convert(e.locationInWindow, from: nil))
        if r >= 0 && r != selectedRow { selectRowIndexes([r], byExtendingSelection: false) }
    }

    override var acceptsFirstResponder: Bool { false }
}

/// A row that draws its selection as a menu's highlighted item: in the
/// accent color, though its window isn't key.
final class AsukaSuggestRow: NSTableRowView {
    override var isEmphasized: Bool {
        get { true }
        set {}
    }

    /// A menu's highlight: rounded, inset from the menu's sides.
    override func drawSelection(in dirtyRect: NSRect) {
        guard isSelected else { return }
        NSColor.selectedContentBackgroundColor.setFill()
        let r = bounds.insetBy(dx: 5, dy: 0)
        NSBezierPath(roundedRect: r, xRadius: AsukaSuggestWindow.corner / 2, yRadius: AsukaSuggestWindow.corner / 2).fill()
    }
}

/// A plain field's suggestion window: a list under the field.
@MainActor final class AsukaSuggestWindow: NSObject, NSTableViewDataSource, NSTableViewDelegate {
    let panel: AsukaSuggestPanel
    let table = AsukaSuggestTable(frame: .zero)
    let scroll = NSScrollView(frame: .zero)
    var items: [String] = []
    static let rowHeight: CGFloat = 22
    static let pad: CGFloat = 5
    static let maxRows = 8

    override init() {
        panel = AsukaSuggestPanel(contentRect: .zero, styleMask: [.borderless, .nonactivatingPanel], backing: .buffered, defer: true)
        super.init()
        panel.isOpaque = false
        panel.backgroundColor = .clear
        panel.hasShadow = true
        panel.isReleasedWhenClosed = false
        panel.hidesOnDeactivate = true
        panel.becomesKeyOnlyIfNeeded = true
        let fx = NSVisualEffectView(frame: .zero)
        fx.material = .menu
        fx.blendingMode = .behindWindow
        fx.state = .active
        fx.wantsLayer = true
        fx.layer?.cornerRadius = Self.corner
        fx.layer?.masksToBounds = true
        panel.contentView = fx
        let col = NSTableColumn(identifier: NSUserInterfaceItemIdentifier("s"))
        table.addTableColumn(col)
        table.headerView = nil
        table.rowHeight = Self.rowHeight
        table.intercellSpacing = .zero
        table.backgroundColor = .clear
        table.style = .plain
        table.selectionHighlightStyle = .regular
        table.dataSource = self
        table.delegate = self
        table.focusRingType = .none
        scroll.documentView = table
        scroll.drawsBackground = false
        scroll.hasVerticalScroller = true
        scroll.autohidesScrollers = true
        scroll.borderType = .noBorder
        scroll.automaticallyAdjustsContentInsets = false
        fx.addSubview(scroll)
    }

    /// A menu's corners: macOS 26's rounder ones.
    static var corner: CGFloat {
        if #available(macOS 26.0, *) { return 12 }
        return 6
    }

    var shown: Bool { panel.isVisible }
    var selected: Int { table.selectedRow }

    func numberOfRows(in tableView: NSTableView) -> Int { items.count }

    func tableView(_ tableView: NSTableView, viewFor tableColumn: NSTableColumn?, row: Int) -> NSView? {
        let id = NSUserInterfaceItemIdentifier("asuka.suggestion")
        let cell: NSTableCellView
        if let c = tableView.makeView(withIdentifier: id, owner: nil) as? NSTableCellView {
            cell = c
        } else {
            cell = NSTableCellView(frame: .zero)
            cell.identifier = id
            let t = NSTextField(labelWithString: "")
            t.lineBreakMode = .byTruncatingTail
            t.translatesAutoresizingMaskIntoConstraints = false
            cell.addSubview(t)
            cell.textField = t
            NSLayoutConstraint.activate([
                t.leadingAnchor.constraint(equalTo: cell.leadingAnchor, constant: 14),
                t.trailingAnchor.constraint(equalTo: cell.trailingAnchor, constant: -14),
                t.centerYAnchor.constraint(equalTo: cell.centerYAnchor),
            ])
        }
        cell.textField?.stringValue = row < items.count ? items[row] : ""
        cell.textField?.font = font
        return cell
    }

    func tableView(_ tableView: NSTableView, rowViewForRow row: Int) -> NSTableRowView? { AsukaSuggestRow() }

    var font: NSFont = styleFont(0)

    /// Shows items under field (above it where the screen ends below),
    /// as wide as it, keeping the row selected if it's still there.
    func show(_ items: [String], under field: NSView, rtl: Bool) {
        let keep = selected >= 0 && selected < self.items.count ? self.items[selected] : nil
        self.items = items
        table.reloadData()
        if let k = keep, let i = items.firstIndex(of: k) { table.selectRowIndexes([i], byExtendingSelection: false) } else { table.deselectAll(nil) }
        table.userInterfaceLayoutDirection = rtl ? .rightToLeft : .leftToRight
        scroll.userInterfaceLayoutDirection = table.userInterfaceLayoutDirection
        guard let win = field.window else { return }
        place(under: field, in: win)
        if panel.parent !== win {
            panel.parent?.removeChildWindow(panel)
            win.addChildWindow(panel, ordered: .above)
        }
        panel.orderFront(nil)
    }

    func place(under field: NSView, in win: NSWindow) {
        let rows = min(items.count, Self.maxRows)
        let h = CGFloat(rows) * Self.rowHeight + 2 * Self.pad
        let r = win.convertToScreen(field.convert(field.bounds, to: nil))
        var frame = CGRect(x: r.minX, y: r.minY - h - 3, width: max(r.width, 120), height: h)
        if let vis = (win.screen ?? NSScreen.main)?.visibleFrame, frame.minY < vis.minY { frame.origin.y = r.maxY + 3 } // above it
        panel.setFrame(frame, display: false)
        // The rows between the menu's top and bottom padding, from the
        // first.
        scroll.frame = (panel.contentView?.bounds ?? .zero).insetBy(dx: 0, dy: Self.pad)
        table.tableColumns.first?.width = scroll.contentSize.width
        table.sizeToFit()
        scroll.contentView.scroll(to: .zero)
        scroll.reflectScrolledClipView(scroll.contentView)
    }

    func hide() {
        guard panel.isVisible || panel.parent != nil else { return }
        panel.parent?.removeChildWindow(panel)
        panel.orderOut(nil)
        table.deselectAll(nil)
    }

    /// Moves the highlight by delta, from none to the first or the last.
    func move(_ delta: Int) {
        guard !items.isEmpty else { return }
        var r = selected
        if r < 0 { r = delta > 0 ? 0 : items.count - 1 } else { r = min(max(r + delta, 0), items.count - 1) }
        table.selectRowIndexes([r], byExtendingSelection: false)
        table.scrollRowToVisible(r)
    }
}

@MainActor extension AsukaRuntime {
    func suggest(_ n: AsukaNode) -> AsukaSuggest {
        if let s = n.suggest { return s }
        let s = AsukaSuggest()
        n.suggest = s
        return s
    }

    /// Suggestions: the list shows them as they are (the core's filtered).
    func setSuggestions(_ n: AsukaNode, _ b: [UInt8]) {
        let s = suggest(n)
        s.items = asukaStrings(b)
        if let c = n.view as? AsukaComboBox {
            s.quiet = true
            c.reloadData()
            if c.indexOfSelectedItem >= 0 { c.deselectItem(at: c.indexOfSelectedItem) }
            s.quiet = false
        }
        showSuggestions(n)
    }

    /// Dropdown: the field becomes a combo box, whose button opens the
    /// list.
    func setDropdown(_ n: AsukaNode, _ on: Bool) {
        let s = suggest(n)
        s.dropdown = on
        guard on, n.textView == nil, !(n.view is AsukaComboBox), let old = n.view as? NSTextField else { return }
        s.window?.hide()
        s.window = nil
        let c = AsukaComboBox(frame: .zero)
        c.usesDataSource = true
        c.dataSource = self
        c.delegate = self
        c.completes = false
        c.hasVerticalScroller = true
        c.numberOfVisibleItems = 8
        c.isEditable = true
        c.isSelectable = true
        c.cell?.isScrollable = true
        c.cell?.usesSingleLineMode = true
        c.font = old.font ?? styleFont(0)
        c.stringValue = n.text
        c.placeholderString = n.placeholder
        c.isEnabled = old.isEnabled
        replaceView(n, c)
    }

    /// Shows the list while the field has the keyboard's focus, the user
    /// has typed and there's something in it; hides it otherwise.
    func showSuggestions(_ n: AsukaNode) {
        guard let s = n.suggest, let f = n.view as? NSTextField else { return }
        let show = s.typed && !s.items.isEmpty && editing(n)?.window?.firstResponder === editing(n) && f.window?.isVisible == true
        if let c = f as? AsukaComboBox {
            c.listShown = show
            return
        }
        guard show else {
            s.window?.hide()
            return
        }
        let w = s.window ?? AsukaSuggestWindow()
        s.window = w
        w.font = f.font ?? styleFont(0)
        let id = n.id
        w.table.choose = { i in MainActor.assumeIsolated { if let n = AsukaRuntime.shared.nodes[id] { AsukaRuntime.shared.chooseSuggestion(n, i) } } }
        w.show(s.items, under: f, rtl: isRTL(n))
    }

    func hideSuggestions(_ n: AsukaNode) {
        guard let s = n.suggest else { return }
        s.window?.hide()
        (n.view as? AsukaComboBox)?.listShown = false
    }

    /// The user typed in a field with suggestions (the core has answered
    /// with them filtered).
    func suggestTyped(_ n: AsukaNode) {
        guard let s = n.suggest else { return }
        s.typed = true
        showSuggestions(n)
    }

    /// The user chose suggestion i: its text in the field, the cursor at
    /// its end, reported with Suggestion; the list closes until they type
    /// again.
    func chooseSuggestion(_ n: AsukaNode, _ i: Int) {
        guard let s = n.suggest, i >= 0, i < s.items.count, let f = n.view as? NSTextField else { return }
        let t = s.items[i]
        n.text = t
        if let ed = f.currentEditor() {
            ed.string = t
            ed.selectedRange = NSRange(location: (t as NSString).length, length: 0)
        } else {
            f.stringValue = t
        }
        s.typed = false
        hideSuggestions(n)
        let x = Int64(i)
        emit(n, .suggestion) { AsukaEvents.suggestion($0, x) }
    }

    /// A field's editor's command while its list shows: Up and Down move
    /// in the list, Return chooses, Escape closes. nil: not the list's;
    /// else whether the field took it (false: the combo box's own).
    func suggestCommand(_ n: AsukaNode, _ cmd: Selector) -> Bool? {
        guard let s = n.suggest else { return nil }
        if let c = n.view as? AsukaComboBox {
            // The combo box's own keys, while its list shows.
            guard c.listShown else { return nil }
            if cmd == #selector(NSResponder.cancelOperation(_:)) {
                s.typed = false
                c.listShown = false
                return true
            }
            if cmd == #selector(NSResponder.moveUp(_:)) || cmd == #selector(NSResponder.moveDown(_:)) || cmd == #selector(NSResponder.insertNewline(_:)) {
                return false
            }
            return nil
        }
        guard let w = s.window, w.shown else { return nil }
        switch cmd {
        case #selector(NSResponder.moveDown(_:)):
            w.move(1)
            return true
        case #selector(NSResponder.moveUp(_:)):
            w.move(-1)
            return true
        case #selector(NSResponder.insertNewline(_:)):
            guard w.selected >= 0 else {
                s.typed = false
                w.hide()
                return nil // Return without a choice submits
            }
            chooseSuggestion(n, w.selected)
            return true
        case #selector(NSResponder.cancelOperation(_:)):
            s.typed = false
            w.hide()
            return true
        default:
            return nil
        }
    }

    // Hooks.

    func suggestPlaced(_ n: AsukaNode) {
        guard let w = n.suggest?.window, w.shown, let f = n.view, let win = f.window else { return }
        w.place(under: f, in: win)
    }

    func suggestReleased(_ n: AsukaNode) {
        n.suggest?.window?.hide()
        n.suggest?.window = nil
    }

    /// The field lost the keyboard's focus: its list closes, and shows
    /// again once the user types.
    func suggestBlurred(_ n: AsukaNode) {
        guard let s = n.suggest else { return }
        s.typed = false
        hideSuggestions(n)
    }

    func suggestJSON(_ n: AsukaNode, _ d: inout [String: Any]) {
        guard let s = n.suggest else { return }
        var j: [String: Any] = ["items": s.items, "dropdown": s.dropdown, "typed": s.typed]
        if let c = n.view as? AsukaComboBox {
            j["shown"] = c.listShown
            j["combo"] = true
            j["selected"] = c.indexOfSelectedItem
            j["count"] = c.numberOfItems
        } else if let w = s.window {
            j["shown"] = w.shown
            j["selected"] = w.selected
            j["rows"] = w.table.numberOfRows
            j["frame"] = [Double(w.panel.frame.minX), Double(w.panel.frame.minY), Double(w.panel.frame.width), Double(w.panel.frame.height)]
        } else {
            j["shown"] = false
        }
        d["suggestions"] = j
    }

    /// The automation's: the user chooses suggestion i with the mouse
    /// (the window's row, the combo box's item).
    func suggestStep(_ n: AsukaNode, _ i: Int) -> String? {
        guard let s = n.suggest, i >= 0, i < s.items.count else { return "no suggestion \(i)" }
        if let c = n.view as? AsukaComboBox {
            c.selectItem(at: i) // as its list's click: it reports the choice
            return nil
        }
        guard let w = s.window, w.shown else { return "the suggestions don't show" }
        w.table.choose?(i)
        return nil
    }
}

extension AsukaRuntime: NSComboBoxDataSource, NSComboBoxDelegate {
    public func numberOfItems(in comboBox: NSComboBox) -> Int {
        node(of: comboBox)?.suggest?.items.count ?? 0
    }

    public func comboBox(_ comboBox: NSComboBox, objectValueForItemAt index: Int) -> Any? {
        guard let items = node(of: comboBox)?.suggest?.items, index >= 0, index < items.count else { return nil }
        return items[index]
    }

    /// The user chose an item (a click in the list, or the arrow keys,
    /// which put each in the field as they go: the core hears of each).
    public func comboBoxSelectionDidChange(_ note: Notification) {
        guard let c = note.object as? AsukaComboBox, let n = node(of: c), let s = n.suggest, !s.quiet else { return }
        let i = c.indexOfSelectedItem
        guard i >= 0, i < s.items.count else { return }
        let t = s.items[i]
        n.text = t
        if let ed = c.currentEditor() {
            ed.string = t
            ed.selectedRange = NSRange(location: (t as NSString).length, length: 0)
        } else {
            c.stringValue = t
        }
        s.typed = false
        let x = Int64(i)
        emit(n, .suggestion) { AsukaEvents.suggestion($0, x) }
    }
}
