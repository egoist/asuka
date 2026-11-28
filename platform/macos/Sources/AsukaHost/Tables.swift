// Tables (Table, Outline): AppKit's own, an NSTableView (view-based, its
// header an NSTableHeaderView, the platform's inset style) and an
// NSOutlineView, each in a scroll view.
//
// Their rows are a List's: the table asks for a row view as a row comes
// into view, and the host binds the row into the row view's cell (a
// host-made node, AK_HOST_ID_BIT set) through asuka_list_bind. Row views
// go back to the table's reuse queue as they scroll out, their cells still
// bound, and bind again to the row they're reused for, so scrolling makes
// nothing new once the rows on screen have their views. A row of text
// (the cell's RowData: a string per column, and a symbol before the first)
// is drawn by the table's own cells, NSTableCellView's text field and image
// view; a row of views (the cell's children, a view per column, in Columns'
// order) is the cell's view across the row, each child at its column's
// cell (frameOfCell), at the width the core gave it.
//
// The core says what's sorted, selected and expanded: a header's click
// reports SortBy and shows Sort as it comes back, the user's selection is
// reported (RowsSelected) and Selection applied without a report, and an
// outline expands and collapses only as Levels says, the user's disclosure
// clicks and arrow keys reported with Expand. The columns as shown (their
// order and widths, the user's to change) go to the core in ColumnLayout.
import AppKit
#if canImport(AsukaOps)
import AsukaOps
#endif
#if canImport(CAsuka)
@_implementationOnly import CAsuka
#endif

/// A Table's column, as Columns says it.
struct AsukaTableColumnSpec: Equatable {
    var id = ""
    var title = ""
    var width: CGFloat = 0
    var min: CGFloat = 0
    var max: CGFloat = 0
    var flags: UInt8 = 0
    var align: UInt8 = 0
    var resizable: Bool { flags & 1 != 0 }
    var sortable: Bool { flags & 2 != 0 }

    /// Columns' bytes: u32 count, per column a u32-length ID and title, f32
    /// width, min and max, u8 flags and u8 TextAlign.
    static func decode(_ b: [UInt8]) -> [AsukaTableColumnSpec] {
        var r = AsukaByteReader(b)
        let n = Int(r.u32())
        var out: [AsukaTableColumnSpec] = []
        for _ in 0..<n {
            var c = AsukaTableColumnSpec()
            c.id = r.string()
            c.title = r.string()
            c.width = CGFloat(r.f32())
            c.min = CGFloat(r.f32())
            c.max = CGFloat(r.f32())
            c.flags = r.u8()
            c.align = r.u8()
            if r.bad { break }
            out.append(c)
        }
        return out
    }
}

/// An outline's row as NSOutlineView knows it: an item of the tree, kept
/// by its ID across updates (the outline's expanded state is its items').
final class AsukaOutlineItem: NSObject {
    let id: Int64
    var row = 0
    var depth = 0
    var flags: UInt8 = 0
    var children: [AsukaOutlineItem] = []
    init(_ id: Int64) { self.id = id }
}

/// A row's cell: a node of the host's the core binds a row into.
@MainActor final class AsukaTableCell: NSObject {
    /// Its row's views go in it (a row of views), across the row.
    let content = AsukaView(frame: .zero)
    var node: AsukaNode!
    weak var table: AsukaTable?
    weak var rowView: AsukaTableRowView?
    /// The row it's bound to (-1: none); it stays bound while its row view
    /// waits for reuse.
    var row = -1
    /// Its RowData: the row's text, per column, and its symbol.
    var strings: [String] = []
    var symbol = ""
    /// Its size as the core lays it out.
    var size = CGSize.zero

    var hasViews: Bool { !(node?.children.isEmpty ?? true) }

    /// RowData's bytes: u32 count, u32-length strings, a u32-length symbol.
    func setRowData(_ b: [UInt8]) {
        var r = AsukaByteReader(b)
        let n = Int(r.u32())
        strings.removeAll(keepingCapacity: true)
        for _ in 0..<n {
            let s = r.string()
            if r.bad { break }
            strings.append(s)
        }
        symbol = r.atEnd ? "" : r.string()
        table?.refresh(self)
    }
}

/// A row's view: it holds its cell's view.
final class AsukaTableRowView: NSTableRowView {
    @MainActor var cell: AsukaTableCell?

    override func layout() {
        super.layout()
        MainActor.assumeIsolated {
            if let c = cell { c.table?.placeContent(c) }
        }
    }
}

/// A row's text, drawn by the table: a label, and a symbol before it.
final class AsukaTableTextCell: NSTableCellView {
    /// Its column's index in Columns.
    var model = 0
    /// The symbol's color when the row isn't selected (nil: the label's).
    var tint: NSColor?

    override init(frame: NSRect) {
        super.init(frame: frame)
        let tf = NSTextField(labelWithString: "")
        tf.lineBreakMode = .byTruncatingTail
        tf.cell?.truncatesLastVisibleLine = true
        tf.font = .systemFont(ofSize: NSFont.systemFontSize)
        tf.isSelectable = false
        addSubview(tf)
        textField = tf
        let iv = NSImageView(frame: .zero)
        iv.imageScaling = .scaleProportionallyDown
        iv.isHidden = true
        addSubview(iv)
        imageView = iv
    }
    required init?(coder: NSCoder) { fatalError() }

    override var backgroundStyle: NSView.BackgroundStyle {
        didSet { imageView?.contentTintColor = backgroundStyle == .emphasized ? nil : tint }
    }

    override func layout() {
        super.layout()
        let b = bounds
        let rtl = userInterfaceLayoutDirection == .rightToLeft
        var lead: CGFloat = 0
        if let iv = imageView, !iv.isHidden {
            let s: CGFloat = 16
            iv.frame = CGRect(x: rtl ? b.width - s : 0, y: ((b.height - s) / 2).rounded(), width: s, height: s)
            lead = s + 5
        }
        if let tf = textField {
            let h = ceil(tf.cell?.cellSize.height ?? 16)
            tf.frame = CGRect(x: rtl ? 0 : lead, y: ((b.height - h) / 2).rounded(), width: max(b.width - lead, 0), height: h)
        }
    }
}

/// A Table's table view: Return opens the selected row, a secondary click
/// shows the row's menu.
final class AsukaTableView: NSTableView {
    weak var owner: AsukaTable?
    override func keyDown(with e: NSEvent) {
        if !MainActor.assumeIsolated({ owner?.keyDown(e) ?? false }) { super.keyDown(with: e) }
    }
    override func menu(for e: NSEvent) -> NSMenu? { MainActor.assumeIsolated { owner?.contextMenu(e) } }
    // A click in a window that isn't key selects too, as Finder's lists do.
    override func acceptsFirstMouse(for event: NSEvent?) -> Bool { true }
}

/// An Outline's outline view, likewise.
final class AsukaOutlineView: NSOutlineView {
    weak var owner: AsukaTable?
    override func keyDown(with e: NSEvent) {
        if !MainActor.assumeIsolated({ owner?.keyDown(e) ?? false }) { super.keyDown(with: e) }
    }
    override func menu(for e: NSEvent) -> NSMenu? { MainActor.assumeIsolated { owner?.contextMenu(e) } }
    override func acceptsFirstMouse(for event: NSEvent?) -> Bool { true }
}

/// A Table's header: a click in a window that isn't key sorts too.
final class AsukaTableHeaderView: NSTableHeaderView {
    override func acceptsFirstMouse(for event: NSEvent?) -> Bool { true }
}

/// A Table's or an Outline's state.
@MainActor final class AsukaTable: NSObject {
    weak var n: AsukaNode?
    let outline: Bool
    let scroll = NSScrollView(frame: .zero)
    let table: NSTableView
    /// The data source and delegate: the table's, or the outline's.
    private var source: NSObject?

    static let rowID = NSUserInterfaceItemIdentifier("asuka.row")
    static let textID = NSUserInterfaceItemIdentifier("asuka.text")

    var ids: [Int64] = []
    var pending: [Int64]?
    var depths: [UInt16] = []
    var flags: [UInt8] = []
    var pendingLevels: ([UInt16], [UInt8])?
    var columns: [AsukaTableColumnSpec] = []
    var colIndex: [String: Int] = [:]
    var sort: [(column: Int, ascending: Bool)] = []
    var mode = AsukaSelectionMode.single
    /// The rows selected, as the core last said or the user chose.
    var selection = IndexSet()
    /// The bound cells of the rows on screen, by row.
    var bound: [Int: AsukaTableCell] = [:]
    /// Every cell made (each a row view's).
    var cells: [AsukaTableCell] = []
    /// Cells whose row binds once the core has returned.
    var unbound: [AsukaTableCell] = []
    /// The host applies what the core said (no reports, an outline's
    /// expanding allowed).
    var applying = 0
    /// In a table's callback that binds rows: a reload waits.
    var inCallback = 0
    var dirty = false
    var queued = false
    var bindQueued = false
    var lastLayout: [UInt8] = []
    var layoutQueued = false
    // An outline's tree: its roots, its rows' items, by ID.
    var roots: [AsukaOutlineItem] = []
    var items: [AsukaOutlineItem] = []
    var byID: [Int64: AsukaOutlineItem] = [:]
    var symbols: [String: NSImage] = [:]
    /// The automation's last row clicked: where a shift-click extends from.
    var anchor = -1

    var rt: AsukaRuntime { AsukaRuntime.shared }

    init(_ n: AsukaNode, outline: Bool) {
        self.n = n
        self.outline = outline
        if outline {
            let o = AsukaOutlineView(frame: .zero)
            let col = NSTableColumn(identifier: NSUserInterfaceItemIdentifier("asuka.outline"))
            col.resizingMask = .autoresizingMask
            o.addTableColumn(col)
            o.outlineTableColumn = col
            o.headerView = nil
            o.indentationPerLevel = 16
            o.columnAutoresizingStyle = .uniformColumnAutoresizingStyle
            o.autoresizesOutlineColumn = false
            table = o
        } else {
            let t = AsukaTableView(frame: .zero)
            t.headerView = AsukaTableHeaderView(frame: CGRect(x: 0, y: 0, width: 0, height: 28))
            // The app's widths stay the app's (and the user's): a table
            // wider than its view scrolls across.
            t.columnAutoresizingStyle = .noColumnAutoresizing
            table = t
        }
        super.init()
        (table as? AsukaTableView)?.owner = self
        (table as? AsukaOutlineView)?.owner = self
        table.rowHeight = 24
        table.usesAutomaticRowHeights = false
        table.allowsMultipleSelection = false
        table.allowsEmptySelection = true
        table.allowsColumnReordering = false
        table.allowsColumnSelection = false
        table.target = self
        table.doubleAction = #selector(doubleClicked(_:))
        if outline {
            let s = AsukaOutlineSource(self)
            (table as? NSOutlineView)?.dataSource = s
            (table as? NSOutlineView)?.delegate = s
            source = s
        } else {
            let s = AsukaTableSource(self)
            table.dataSource = s
            table.delegate = s
            source = s
        }
        scroll.documentView = table
        scroll.hasVerticalScroller = true
        scroll.hasHorizontalScroller = !outline
        scroll.autohidesScrollers = true
        scroll.borderType = .noBorder
    }

    // MARK: Props

    func setProp(_ p: UInt16, _ v: AsukaValue) {
        if outline {
            switch p {
            case AsukaProp.Outline.levels: setLevels(v.data)
            case AsukaProp.Outline.selectionMode: setMode(v.enumValue)
            case AsukaProp.Outline.selection: setSelection(v.data)
            case AsukaProp.Outline.rowHeight: setRowHeight(v.float)
            default: break
            }
            return
        }
        switch p {
        case AsukaProp.Table.columns: setColumns(AsukaTableColumnSpec.decode(v.data))
        case AsukaProp.Table.sort: setSort(v.data)
        case AsukaProp.Table.selectionMode: setMode(v.enumValue)
        case AsukaProp.Table.selection: setSelection(v.data)
        case AsukaProp.Table.rowHeight: setRowHeight(v.float)
        case AsukaProp.Table.columnsMovable: table.allowsColumnReordering = v.bool
        default: break
        }
    }

    func setRowHeight(_ h: Double) {
        guard h > 0 else { return }
        table.rowHeight = max(CGFloat(h) - table.intercellSpacing.height, 1)
    }

    func setMode(_ m: Int32) {
        mode = AsukaSelectionMode(rawValue: m) ?? .single
        table.allowsMultipleSelection = mode == .multiple
        if mode == .none {
            applying += 1
            table.deselectAll(nil)
            applying -= 1
        }
    }

    /// The app's columns: in its order with its widths, or, while their IDs
    /// are the ones shown, as the user has them (moved, resized).
    func setColumns(_ specs: [AsukaTableColumnSpec]) {
        let same = specs.count == columns.count && Set(specs.map(\.id)) == Set(columns.map(\.id)) && specs.count == table.tableColumns.count
        columns = specs
        colIndex.removeAll(keepingCapacity: true)
        for (i, c) in specs.enumerated() { colIndex[c.id] = i }
        applying += 1
        if same {
            for col in table.tableColumns {
                if let i = colIndex[col.identifier.rawValue] { configure(col, specs[i], width: false) }
            }
        } else {
            for col in table.tableColumns { table.removeTableColumn(col) }
            for s in specs {
                let col = NSTableColumn(identifier: NSUserInterfaceItemIdentifier(s.id))
                configure(col, s, width: true)
                table.addTableColumn(col)
            }
        }
        applying -= 1
        applySort()
        reloadShown()
        scheduleLayout()
    }

    func configure(_ col: NSTableColumn, _ s: AsukaTableColumnSpec, width: Bool) {
        col.title = s.title
        col.headerCell.alignment = alignment(s.align)
        col.minWidth = s.min > 0 ? s.min : 20
        col.maxWidth = s.max > 0 ? Swift.max(s.max, col.minWidth) : CGFloat.greatestFiniteMagnitude
        col.resizingMask = s.resizable ? .userResizingMask : []
        col.sortDescriptorPrototype = s.sortable ? NSSortDescriptor(key: s.id, ascending: true) : nil
        if width { col.width = s.width > 0 ? s.width : 100 }
    }

    /// A TextAlign as the table draws it, the way the table reads.
    func alignment(_ a: UInt8) -> NSTextAlignment {
        let rtl = n.map { rt.isRTL($0) } ?? false
        switch a {
        case 1: return .center
        case 2: return rtl ? .left : .right
        default: return rtl ? .right : .left
        }
    }

    func setSort(_ b: [UInt8]) {
        var r = AsukaByteReader(b)
        let count = Int(r.u32())
        sort.removeAll(keepingCapacity: true)
        for _ in 0..<count {
            let c = Int(r.u32()), asc = r.u8() != 0
            if r.bad { break }
            sort.append((c, asc))
        }
        applySort()
    }

    /// Shows the sort the core says: the primary's indicator in its header.
    func applySort() {
        applying += 1
        defer { applying -= 1 }
        var ds: [NSSortDescriptor] = []
        for k in sort where k.column >= 0 && k.column < columns.count {
            ds.append(NSSortDescriptor(key: columns[k.column].id, ascending: k.ascending))
        }
        if table.sortDescriptors != ds { table.sortDescriptors = ds }
        let first = sort.first.flatMap { $0.column < columns.count ? $0 : nil }
        var highlighted: NSTableColumn?
        for col in table.tableColumns {
            var img: NSImage?
            if let f = first, columns[f.column].id == col.identifier.rawValue {
                img = NSImage(named: f.ascending ? "NSAscendingSortIndicator" : "NSDescendingSortIndicator")
                highlighted = col
            }
            table.setIndicatorImage(img, in: col)
        }
        table.highlightedTableColumn = highlighted
    }

    func setSelection(_ b: [UInt8]) {
        selection = asukaRunsSet(b)
        if !dirty { applySelection() }
    }

    /// Selects the rows the core said, and none else, without reporting it.
    func applySelection() {
        applying += 1
        defer { applying -= 1 }
        var s = IndexSet()
        let count = table.numberOfRows
        for r in selection.rangeView where r.lowerBound < count { s.insert(integersIn: r.lowerBound..<Swift.min(r.upperBound, count)) }
        if mode == .none { s = IndexSet() }
        if table.selectedRowIndexes != s { table.selectRowIndexes(s, byExtendingSelection: false) }
    }

    func setLevels(_ b: [UInt8]) {
        var r = AsukaByteReader(b)
        let count = Int(r.u32())
        var d: [UInt16] = [], f: [UInt8] = []
        d.reserveCapacity(Swift.min(count, b.count / 3))
        f.reserveCapacity(Swift.min(count, b.count / 3))
        for _ in 0..<count {
            let depth = r.u16(), fl = r.u8()
            if r.bad { break }
            d.append(depth)
            f.append(fl)
        }
        pendingLevels = (d, f)
        dirty = true
        queue()
    }

    // MARK: Rows

    /// ListUpdate: the rows' item IDs; shown once the core has returned.
    func update(_ args: UnsafeRawBufferPointer) {
        guard args.count >= 4 else { return }
        var r = AsukaByteReader(Array(args))
        let count = Int(r.u32())
        var ids: [Int64] = []
        ids.reserveCapacity(Swift.min(count, args.count / 8))
        for _ in 0..<count {
            let v = r.u64()
            if r.bad { break }
            ids.append(Int64(bitPattern: v))
        }
        pending = ids
        dirty = true
        queue()
    }

    func queue() {
        guard !queued else { return }
        queued = true
        rt.afterCore { [weak self] in self?.flush() }
    }

    /// Shows an update waiting: the rows again (a reload keeps where the
    /// table is scrolled to), an outline's tree and what's expanded, the
    /// selection.
    func flush() {
        queued = false
        guard dirty, let n, rt.alive(n) else { return }
        if inCallback > 0 || rt.depth > 0 {
            // Not inside the table's own layout, nor a call into the core.
            queued = true
            DispatchQueue.main.async { [weak self] in self?.flush() }
            return
        }
        dirty = false
        if let p = pending {
            ids = p
            pending = nil
        }
        if let l = pendingLevels {
            depths = l.0
            flags = l.1
            pendingLevels = nil
        }
        applying += 1
        if outline { buildTree() }
        table.reloadData()
        if outline { syncExpansion() }
        applying -= 1
        applySelection()
    }

    /// The outline's tree from the rows and their depths: a row's parent
    /// is the nearest row before it a level up.
    func buildTree() {
        var next: [Int64: AsukaOutlineItem] = [:]
        next.reserveCapacity(ids.count)
        roots.removeAll(keepingCapacity: true)
        items.removeAll(keepingCapacity: true)
        var stack: [AsukaOutlineItem] = []
        for (i, id) in ids.enumerated() {
            let it = byID[id] ?? AsukaOutlineItem(id)
            next[id] = it
            it.row = i
            it.depth = i < depths.count ? Int(depths[i]) : 0
            it.flags = i < flags.count ? flags[i] : 0
            it.children.removeAll(keepingCapacity: true)
            if stack.count > it.depth { stack.removeSubrange(it.depth...) }
            if it.depth > 0, let p = stack.last { p.children.append(it) } else { roots.append(it) }
            stack.append(it)
            items.append(it)
        }
        byID = next
    }

    /// Expands and collapses the outline's items as Levels says, parents
    /// first.
    func syncExpansion() {
        guard let o = table as? NSOutlineView else { return }
        for it in items {
            let want = it.flags & 2 != 0
            let has = o.isItemExpanded(it)
            if want && !has { o.expandItem(it) } else if !want && has { o.collapseItem(it) }
        }
    }

    /// The row view for a row coming into view: a reused one (its cell
    /// still bound to the row it showed) or a new one, its cell bound to
    /// the row.
    func rowView(_ row: Int) -> NSTableRowView? {
        inCallback += 1
        defer { inCallback -= 1 }
        let rv = (table.makeView(withIdentifier: Self.rowID, owner: nil) as? AsukaTableRowView) ?? newRowView()
        bind(rv, row)
        return rv
    }

    func newRowView() -> AsukaTableRowView {
        let rv = AsukaTableRowView(frame: .zero)
        rv.identifier = Self.rowID
        let c = AsukaTableCell()
        c.table = self
        c.rowView = rv
        c.content.wantsLayer = true
        c.node = rt.makeTableCell(c)
        rv.cell = c
        rv.addSubview(c.content)
        cells.append(c)
        return rv
    }

    func bind(_ rv: AsukaTableRowView, _ row: Int) {
        guard let c = rv.cell, let n, row >= 0, row < ids.count else { return }
        if c.row >= 0, bound[c.row] === c { bound.removeValue(forKey: c.row) }
        c.row = row
        bound[row] = c
        if rt.depth > 0 {
            // In a call into the core: once it has returned.
            unbound.append(c)
            if !bindQueued {
                bindQueued = true
                DispatchQueue.main.async { [weak self] in self?.bindWaiting() }
            }
            return
        }
        let list = n.id, cell = c.node.id
        rt.enter { asuka_list_bind(list, Int64(row), cell) }
        c.content.isHidden = !c.hasViews
    }

    /// Binds the cells that waited for the core to return, and shows them.
    func bindWaiting() {
        bindQueued = false
        guard let n, rt.alive(n) else { return }
        let cs = unbound
        unbound.removeAll()
        var rows = IndexSet()
        for c in cs where c.row >= 0 && c.row < ids.count && bound[c.row] === c {
            let list = n.id, cell = c.node.id, row = c.row
            rt.enter { asuka_list_bind(list, Int64(row), cell) }
            c.content.isHidden = !c.hasViews
            rows.insert(row)
        }
        if !rows.isEmpty, table.numberOfColumns > 0 {
            table.reloadData(forRowIndexes: rows, columnIndexes: IndexSet(integersIn: 0..<table.numberOfColumns))
        }
    }

    func removed(_ rv: NSTableRowView) {
        guard let c = (rv as? AsukaTableRowView)?.cell else { return }
        if c.row >= 0, bound[c.row] === c { bound.removeValue(forKey: c.row) }
    }

    /// A column's view of a row of text: the table's cell, its text from
    /// the row's RowData (a row of views has none: its cell's view shows).
    func cellView(_ col: NSTableColumn?, _ row: Int) -> NSView? {
        guard let col, let c = bound[row], !c.hasViews else { return nil }
        let v = (table.makeView(withIdentifier: Self.textID, owner: nil) as? AsukaTableTextCell) ?? {
            let v = AsukaTableTextCell(frame: .zero)
            v.identifier = Self.textID
            return v
        }()
        v.model = outline ? 0 : colIndex[col.identifier.rawValue] ?? 0
        fill(v, c)
        return v
    }

    func fill(_ v: AsukaTableTextCell, _ c: AsukaTableCell) {
        let i = v.model
        let s = i < c.strings.count ? c.strings[i] : ""
        if v.textField?.stringValue != s { v.textField?.stringValue = s }
        v.textField?.alignment = outline ? alignment(0) : (i < columns.count ? alignment(columns[i].align) : alignment(0))
        let name = i == 0 ? c.symbol : ""
        let img = name.isEmpty ? nil : symbolImage(name)
        v.tint = outline ? accentNSColor : nil
        if v.imageView?.image !== img { v.imageView?.image = img }
        v.imageView?.contentTintColor = v.backgroundStyle == .emphasized ? nil : v.tint
        if v.imageView?.isHidden != (img == nil) {
            v.imageView?.isHidden = img == nil
            v.needsLayout = true
        }
    }

    func symbolImage(_ name: String) -> NSImage? {
        if let i = symbols[name] { return i }
        guard let i = rt.symbolImage(name) else { return nil }
        symbols[name] = i
        return i
    }

    /// A cell's RowData changed: its row's cells on screen show it.
    func refresh(_ c: AsukaTableCell) {
        guard c.row >= 0, bound[c.row] === c, c.row < table.numberOfRows else { return }
        for ci in 0..<table.numberOfColumns {
            if let v = table.view(atColumn: ci, row: c.row, makeIfNecessary: false) as? AsukaTableTextCell { fill(v, c) }
        }
    }

    /// The core laid a cell out: its view across its row.
    func cellPlaced(_ c: AsukaTableCell, _ r: CGRect) {
        c.size = r.size
        placeContent(c)
    }

    /// Puts a cell's view in its row view: across a table's row, or at an
    /// outline's cell (after its disclosure and indent), in the middle of
    /// the row's height.
    func placeContent(_ c: AsukaTableCell) {
        guard let rv = c.rowView else { return }
        let h = c.size.height > 0 ? c.size.height : table.rowHeight
        var x: CGFloat = 0
        var w = Swift.max(c.size.width, rv.bounds.width)
        if outline, c.row >= 0, c.row < table.numberOfRows {
            let f = table.frameOfCell(atColumn: 0, row: c.row)
            w = c.size.width > 0 ? c.size.width : f.width
            x = rv.userInterfaceLayoutDirection == .rightToLeft ? f.maxX - w : f.minX
        }
        let to = CGRect(x: x, y: ((rv.bounds.height - h) / 2).rounded(), width: w, height: h)
        if c.content.frame != to { c.content.frame = to }
    }

    /// Where a row's view in a column goes: at the column's cell, the way
    /// the table shows its columns (moved, resized), at the core's width.
    func childFrame(_ c: AsukaTableCell, _ child: AsukaNode, _ r: CGRect) -> CGRect {
        guard !outline, let p = c.node, let i = p.children.firstIndex(where: { $0 === child }), i < columns.count else { return r }
        let di = table.column(withIdentifier: NSUserInterfaceItemIdentifier(columns[i].id))
        guard di >= 0 else { return r }
        let f = table.numberOfRows > 0 ? table.frameOfCell(atColumn: di, row: Swift.max(Swift.min(c.row, table.numberOfRows - 1), 0)) : table.rect(ofColumn: di)
        return CGRect(x: f.minX, y: r.minY, width: r.width, height: r.height)
    }

    /// The rows' views go to their columns again (one moved or resized).
    func placeChildren() {
        for c in cells where c.hasViews {
            for k in c.node.children {
                guard let v = k.view else { continue }
                let f = childFrame(c, k, k.frame)
                if v.frame.origin.x != f.minX { v.frame.origin.x = f.minX }
            }
        }
    }

    /// The text rows on screen take their cells again (the columns'
    /// alignment changed).
    func reloadShown() {
        let vis = table.rows(in: table.visibleRect)
        guard vis.length > 0, table.numberOfColumns > 0 else { return }
        table.reloadData(forRowIndexes: IndexSet(integersIn: vis.location..<vis.location + vis.length),
                         columnIndexes: IndexSet(integersIn: 0..<table.numberOfColumns))
    }

    // MARK: Columns as shown

    /// ColumnLayout, once a turn: each column shown, from the start, its
    /// index in Columns and its width.
    func scheduleLayout() {
        guard !outline, !layoutQueued else { return }
        layoutQueued = true
        DispatchQueue.main.async { [weak self] in
            guard let self else { return }
            self.layoutQueued = false
            self.sendLayout()
        }
    }

    func sendLayout() {
        guard let n, rt.alive(n), !columns.isEmpty else { return }
        var w = AsukaByteWriter()
        w.u32(UInt32(table.tableColumns.count))
        for col in table.tableColumns {
            w.u32(UInt32(colIndex[col.identifier.rawValue] ?? 0))
            w.f32(Float(col.width))
        }
        placeChildren()
        guard w.bytes != lastLayout else { return }
        lastLayout = w.bytes
        let b = w.bytes
        rt.emit(n, .columnLayout) { AsukaEvents.columnLayout($0, b) }
    }

    func columnsChanged() {
        guard applying == 0 else { return }
        placeChildren()
        scheduleLayout()
    }

    // MARK: The user's

    /// A click on a sortable header: the sort stays as the core says
    /// (SortBy asks it, and it says the new one).
    func sortClicked() {
        guard applying == 0 else { return }
        let key = table.sortDescriptors.first?.key
        applySort()
        guard let key, let i = colIndex[key], i < columns.count, columns[i].sortable else { return }
        DispatchQueue.main.async { [weak self] in
            guard let self, let n = self.n, self.rt.alive(n) else { return }
            self.rt.emit(n, .sortBy) { AsukaEvents.sortBy($0, Int64(i)) }
        }
    }

    func selectionChanged() {
        guard applying == 0, let n else { return }
        selection = table.selectedRowIndexes
        let b = asukaRuns(selection)
        rt.emit(n, .rowsSelected) { AsukaEvents.rowsSelected($0, b) }
    }

    func proposed(_ s: IndexSet) -> IndexSet { mode == .none ? IndexSet() : s }

    @objc func doubleClicked(_ sender: Any?) {
        let r = table.clickedRow
        guard r >= 0 else { return }
        open(r)
    }

    func open(_ row: Int) {
        guard let n else { return }
        rt.emit(n, .open) { AsukaEvents.open($0, Int64(row)) }
    }

    /// Return or Enter opens the first row selected.
    func keyDown(_ e: NSEvent) -> Bool {
        guard e.keyCode == 36 || e.keyCode == 76, e.modifierFlags.intersection([.command, .option, .control, .shift]).isEmpty else { return false }
        guard let r = table.selectedRowIndexes.first else { return false }
        open(r)
        return true
    }

    /// A secondary click on a row: the row selected (unless it is), then
    /// its cell's menu.
    func contextMenu(_ e: NSEvent) -> NSMenu? {
        let p = table.convert(e.locationInWindow, from: nil)
        let r = table.row(at: p)
        return menuFor(r)
    }

    func menuFor(_ r: Int) -> NSMenu? {
        guard r >= 0, let c = bound[r] else { return nil }
        if mode != .none && !table.isRowSelected(r) { table.selectRowIndexes(IndexSet(integer: r), byExtendingSelection: false) }
        return c.node.menu
    }

    /// The user expanding or collapsing an outline's row: asked of the
    /// core, which says so in Levels (true: the host is applying it).
    func shouldExpand(_ item: Any, _ on: Bool) -> Bool {
        if applying > 0 { return true }
        guard let it = item as? AsukaOutlineItem else { return false }
        let row = it.row
        DispatchQueue.main.async { [weak self] in
            guard let self, let n = self.n, self.rt.alive(n) else { return }
            self.rt.emit(n, .expand) { AsukaEvents.expand($0, x: Double(row), y: on ? 1 : 0) }
        }
        return false
    }

    // MARK: Scrolling

    func scrollTo(_ row: Int, _ animated: Bool) {
        flush()
        guard row >= 0, row < table.numberOfRows else { return }
        if !animated {
            table.scrollRowToVisible(row)
            return
        }
        NSAnimationContext.runAnimationGroup { ctx in
            ctx.duration = 0.25
            ctx.allowsImplicitAnimation = true
            table.scrollRowToVisible(row)
        }
    }

    /// The offset from the rows' top (under the header).
    func scrollToOffset(_ y: CGFloat, _ animated: Bool) {
        flush()
        let clip = scroll.contentView
        let top = -clip.contentInsets.top
        let end = Swift.max(table.frame.height - clip.bounds.height, top)
        let to = CGPoint(x: clip.bounds.minX, y: Swift.min(Swift.max(top + y, top), end))
        if animated { clip.animator().setBoundsOrigin(to) } else { clip.scroll(to: to) }
        scroll.reflectScrolledClipView(clip)
    }

    // MARK: The automation's

    /// The rows bound on screen, and their cells' nodes.
    func boundRows() -> [(Int, [AsukaNode])] {
        bound.keys.sorted().compactMap { r in bound[r].map { (r, [$0.node]) } }
    }

    func json() -> [String: Any] {
        let vis = table.rows(in: table.visibleRect)
        var j: [String: Any] = [
            "rows": table.numberOfRows, "items": ids.count, "cells": cells.count, "bound": bound.count,
            "visibleRows": [vis.location, vis.length], "rowHeight": Double(table.rowHeight),
            "style": table.effectiveStyle.rawValue, "multiple": table.allowsMultipleSelection,
            "selected": Array(table.selectedRowIndexes), "movable": table.allowsColumnReordering,
            "header": table.headerView != nil, "alternating": table.usesAlternatingRowBackgroundColors,
            "mode": Int(mode.rawValue), "class": String(describing: type(of: table)),
            "sortDescriptors": table.sortDescriptors.map { ["key": $0.key ?? "", "ascending": $0.ascending] },
        ]
        if !outline {
            j["columns"] = table.tableColumns.map { col -> [String: Any] in
                let img = table.indicatorImage(in: col)
                var ind = ""
                if let img, img === NSImage(named: "NSAscendingSortIndicator") { ind = "ascending" }
                if let img, img === NSImage(named: "NSDescendingSortIndicator") { ind = "descending" }
                return ["id": col.identifier.rawValue, "title": col.title, "width": Double(col.width), "min": Double(col.minWidth),
                        "max": Double(Swift.min(col.maxWidth, 1e9)), "resizable": col.resizingMask.contains(.userResizingMask),
                        "sortable": col.sortDescriptorPrototype != nil, "indicator": ind,
                        "headerAlignment": asukaAlignName(col.headerCell.alignment)]
            }
            j["highlighted"] = table.highlightedTableColumn?.identifier.rawValue ?? ""
            j["layout"] = Array(lastLayout)
        }
        // The rows on screen as the table draws them.
        var shown: [[String: Any]] = []
        if vis.length > 0 {
            for r in vis.location..<vis.location + vis.length {
                var row: [String: Any] = ["row": r]
                var texts: [String] = []
                var symbol = false
                for ci in 0..<table.numberOfColumns {
                    guard let v = table.view(atColumn: ci, row: r, makeIfNecessary: false) as? AsukaTableTextCell else { continue }
                    texts.append(v.textField?.stringValue ?? "")
                    if v.imageView?.isHidden == false && v.imageView?.image != nil { symbol = true }
                    if ci == 0 { row["alignment"] = asukaAlignName(v.textField?.alignment ?? .natural) }
                }
                row["texts"] = texts
                row["symbol"] = symbol
                if let c = bound[r] { row["cell"] = c.node.id }
                if let o = table as? NSOutlineView, let it = o.item(atRow: r) as? AsukaOutlineItem {
                    row["level"] = o.level(forRow: r)
                    row["expanded"] = o.isItemExpanded(it)
                    row["expandable"] = o.isExpandable(it)
                    row["id"] = it.id
                    row["depth"] = it.depth
                    row["inSync"] = it.row == r
                }
                shown.append(row)
            }
        }
        j["shown"] = shown
        return j
    }
}

/// A Table's data source and delegate.
@MainActor final class AsukaTableSource: NSObject, NSTableViewDataSource, NSTableViewDelegate {
    weak var t: AsukaTable?
    init(_ t: AsukaTable) { self.t = t }

    func numberOfRows(in tableView: NSTableView) -> Int { t?.ids.count ?? 0 }
    func tableView(_ tv: NSTableView, rowViewForRow row: Int) -> NSTableRowView? { t?.rowView(row) }
    func tableView(_ tv: NSTableView, viewFor col: NSTableColumn?, row: Int) -> NSView? { t?.cellView(col, row) }
    func tableView(_ tv: NSTableView, didRemove rv: NSTableRowView, forRow row: Int) { t?.removed(rv) }
    func tableView(_ tv: NSTableView, didAdd rv: NSTableRowView, forRow row: Int) {
        if let c = (rv as? AsukaTableRowView)?.cell { t?.placeContent(c) }
    }
    func tableView(_ tv: NSTableView, sortDescriptorsDidChange old: [NSSortDescriptor]) { t?.sortClicked() }
    func tableView(_ tv: NSTableView, selectionIndexesForProposedSelection p: IndexSet) -> IndexSet { t?.proposed(p) ?? p }
    func tableViewSelectionDidChange(_ note: Notification) { t?.selectionChanged() }
    func tableViewColumnDidResize(_ note: Notification) { t?.columnsChanged() }
    func tableViewColumnDidMove(_ note: Notification) { t?.columnsChanged() }
}

/// An Outline's data source and delegate.
@MainActor final class AsukaOutlineSource: NSObject, NSOutlineViewDataSource, NSOutlineViewDelegate {
    weak var t: AsukaTable?
    init(_ t: AsukaTable) { self.t = t }

    func outlineView(_ ov: NSOutlineView, numberOfChildrenOfItem item: Any?) -> Int {
        guard let it = item as? AsukaOutlineItem else { return t?.roots.count ?? 0 }
        return it.children.count
    }
    func outlineView(_ ov: NSOutlineView, child index: Int, ofItem item: Any?) -> Any {
        guard let it = item as? AsukaOutlineItem else { return t?.roots[index] ?? NSNull() }
        return it.children[index]
    }
    func outlineView(_ ov: NSOutlineView, isItemExpandable item: Any) -> Bool {
        guard let it = item as? AsukaOutlineItem else { return false }
        return it.flags & 3 != 0
    }
    func outlineView(_ ov: NSOutlineView, rowViewForItem item: Any) -> NSTableRowView? {
        t?.rowView((item as? AsukaOutlineItem)?.row ?? -1)
    }
    func outlineView(_ ov: NSOutlineView, viewFor col: NSTableColumn?, item: Any) -> NSView? {
        t?.cellView(col, (item as? AsukaOutlineItem)?.row ?? -1)
    }
    func outlineView(_ ov: NSOutlineView, didRemove rv: NSTableRowView, forRow row: Int) { t?.removed(rv) }
    func outlineView(_ ov: NSOutlineView, didAdd rv: NSTableRowView, forRow row: Int) {
        if let c = (rv as? AsukaTableRowView)?.cell { t?.placeContent(c) }
    }
    func outlineView(_ ov: NSOutlineView, shouldExpandItem item: Any) -> Bool { t?.shouldExpand(item, true) ?? false }
    func outlineView(_ ov: NSOutlineView, shouldCollapseItem item: Any) -> Bool { t?.shouldExpand(item, false) ?? false }
    func outlineView(_ ov: NSOutlineView, selectionIndexesForProposedSelection p: IndexSet) -> IndexSet { t?.proposed(p) ?? p }
    func outlineViewSelectionDidChange(_ note: Notification) { t?.selectionChanged() }
}

/// Selection's runs (u32 count, per run u32 first and u32 length) as rows.
func asukaRunsSet(_ b: [UInt8]) -> IndexSet {
    var r = AsukaByteReader(b)
    let n = Int(r.u32())
    var s = IndexSet()
    for _ in 0..<n {
        let first = Int(r.u32()), len = Int(r.u32())
        if r.bad { break }
        if len > 0 { s.insert(integersIn: first..<first + len) }
    }
    return s
}

/// Rows as Selection's runs.
func asukaRuns(_ s: IndexSet) -> [UInt8] {
    var w = AsukaByteWriter()
    w.u32(UInt32(s.rangeView.count))
    for r in s.rangeView {
        w.u32(UInt32(r.lowerBound))
        w.u32(UInt32(r.count))
    }
    return w.bytes
}

@MainActor extension AsukaRuntime {
    func makeTable(_ n: AsukaNode, outline: Bool) -> NSView {
        let t = AsukaTable(n, outline: outline)
        n.table = t
        return t.scroll
    }

    /// A node for a row's cell the host made: its id the host's own
    /// (0x80000000 and up), which the core binds.
    func makeTableCell(_ c: AsukaTableCell) -> AsukaNode {
        nextCell += 1
        let n = AsukaNode(id: 0x8000_0000 | nextCell, kind: nil, view: c.content)
        n.tableCell = c
        nodes[n.id] = n
        byView[ObjectIdentifier(c.content)] = n
        applyDirection(n)
        return n
    }

    func tableScrollTo(_ n: AsukaNode, _ index: Int, _ animated: Bool) {
        guard let t = n.table else { return }
        afterCore { [weak self] in
            guard let self, self.alive(n) else { return }
            t.scrollTo(index, animated)
        }
    }

    func tableScrollToOffset(_ n: AsukaNode, _ y: CGFloat, _ animated: Bool) {
        guard let t = n.table else { return }
        afterCore { [weak self] in
            guard let self, self.alive(n) else { return }
            t.scrollToOffset(y, animated)
        }
    }

    /// Where the core's frame for a view of a table's row puts it.
    func tableChildFrame(_ n: AsukaNode, _ r: CGRect) -> CGRect {
        guard let c = n.parent?.tableCell, let t = c.table else { return r }
        return t.childFrame(c, n, r)
    }

    // Hooks (Parts.swift).
    func tableReleased(_ n: AsukaNode) {
        guard let t = n.table else { return }
        // Its cells are the host's: they go with it.
        for c in t.cells {
            guard let cn = c.node else { continue }
            if let m = cn.menu { byMenu.removeValue(forKey: ObjectIdentifier(m)) }
            if nodes[cn.id] === cn { nodes.removeValue(forKey: cn.id) }
            byView.removeValue(forKey: ObjectIdentifier(c.content))
        }
        t.bound = [:]
        t.cells = []
    }

    func tablePlaced(_ n: AsukaNode) {
        guard let t = n.table else { return }
        if t.outline { t.table.sizeLastColumnToFit() }
    }

    func tableDirection(_ n: AsukaNode, _ changed: Bool) {
        guard let t = n.table else { return }
        let d: NSUserInterfaceLayoutDirection = isRTL(n) ? .rightToLeft : .leftToRight
        t.table.userInterfaceLayoutDirection = d
        t.table.headerView?.userInterfaceLayoutDirection = d
        if changed {
            for (i, col) in t.table.tableColumns.enumerated() where i < t.columns.count {
                if let k = t.colIndex[col.identifier.rawValue] { col.headerCell.alignment = t.alignment(t.columns[k].align) }
            }
            t.reloadShown()
        }
    }

    func tableJSON(_ n: AsukaNode, _ d: inout [String: Any]) {
        if let t = n.table { d["table"] = t.json() }
        if let c = n.tableCell {
            d["cell"] = ["row": c.row, "rowData": c.strings, "symbol": c.symbol, "bound": c.table?.bound[c.row] === c,
                         "contentFrame": asukaRect(c.content.frame), "contentHidden": c.content.isHidden]
        }
    }

    // MARK: The automation's steps

    /// The automation's steps of tables (Automation.swift): nil if verb
    /// isn't one of them, else its error (nil: none).
    ///   - tableheader <title>: a click on the Table's column header
    ///   - tablerow <row>[,cmd|,shift], outlinerow: a click on a row (the
    ///     table the first responder, its selection as the click makes it)
    ///   - tabledouble <row>, outlinedouble: a double click on a row
    ///   - tablemenu <row>|<title>, outlinemenu: a secondary click on a row,
    ///     and its menu's item chosen
    ///   - outlinedisclose <row>: a click on the row's disclosure
    func tableStep(_ verb: String, _ arg: String) -> String?? {
        let isOutline = verb.hasPrefix("outline")
        guard isOutline || verb.hasPrefix("table") else { return nil }
        let what = verb.dropFirst(isOutline ? 7 : 5)
        guard ["header", "row", "double", "menu", "disclose"].contains(what) else { return nil }
        let a = AsukaAutomation.shared
        var found: AsukaNode?
        for n in nodes.values where n.table != nil && n.table!.outline == isOutline && a.showing(n) {
            if found == nil || n.id > found!.id { found = n }
        }
        guard let n = found, let t = n.table, let w = t.table.window else { return .some("no \(isOutline ? "Outline" : "Table") shows") }
        t.flush()
        // A click as the window server sends it: the up waits in the queue,
        // for the control's tracking loop.
        func click(_ v: NSView, _ p: CGPoint) {
            let wp = v.convert(p, to: nil)
            func ev(_ type: NSEvent.EventType) -> NSEvent? {
                NSEvent.mouseEvent(with: type, location: wp, modifierFlags: [], timestamp: ProcessInfo.processInfo.systemUptime,
                                   windowNumber: w.windowNumber, context: nil, eventNumber: 0, clickCount: 1, pressure: 1)
            }
            guard let down = ev(.leftMouseDown), let up = ev(.leftMouseUp) else { return }
            NSApp.postEvent(up, atStart: false)
            w.sendEvent(down)
        }
        switch what {
        case "header":
            guard let h = t.table.headerView else { return .some("the table has no header") }
            guard let i = t.table.tableColumns.firstIndex(where: { $0.title == arg }) else { return .some("no column \(arg)") }
            let r = h.headerRect(ofColumn: i)
            click(h, CGPoint(x: r.midX, y: r.midY))
        case "disclose":
            guard let o = t.table as? NSOutlineView, let row = Int(arg), row >= 0, row < o.numberOfRows else { return .some("no row \(arg)") }
            o.scrollRowToVisible(row)
            o.layoutSubtreeIfNeeded()
            let r = o.frameOfOutlineCell(atRow: row)
            guard r.width > 0 else { return .some("row \(row) has no disclosure") }
            click(o, CGPoint(x: r.midX, y: r.midY))
        case "row", "double":
            // As the table's click makes the selection: a click in a window
            // of an app in the background (as under tests) is AppKit's
            // activation's, which a table's rows don't take from synthetic
            // events, so this goes through the table's selection and its
            // delegate (the header and the disclosure take the events).
            let p = arg.split(separator: ",").map { $0.trimmingCharacters(in: .whitespaces) }
            guard let row = p.first.flatMap({ Int($0) }), row >= 0, row < t.table.numberOfRows else { return .some("no row \(arg)") }
            t.table.scrollRowToVisible(row)
            w.makeFirstResponder(t.table)
            var s = t.table.selectedRowIndexes
            if p.contains("cmd") && t.table.allowsMultipleSelection {
                if s.contains(row) { s.remove(row) } else { s.insert(row) }
                t.anchor = row
            } else if p.contains("shift") && t.table.allowsMultipleSelection {
                // From the row clicked last (the selection's anchor).
                let from = t.anchor >= 0 && t.anchor < t.table.numberOfRows ? t.anchor : row
                s.insert(integersIn: Swift.min(from, row)...Swift.max(from, row))
            } else {
                s = IndexSet(integer: row)
                t.anchor = row
            }
            t.table.selectRowIndexes(t.proposed(s), byExtendingSelection: false)
            if what == "double" { t.open(row) }
        case "menu":
            let p = arg.split(separator: "|", maxSplits: 1).map(String.init)
            guard p.count == 2, let row = Int(p[0]) else { return .some("want \(verb) <row>|<title>") }
            guard let m = t.menuFor(row) else { return .some("row \(row) has no menu") }
            guard let i = m.items.firstIndex(where: { $0.title == p[1] }) else { return .some("no \"\(p[1])\" in row \(row)'s menu") }
            m.performActionForItem(at: i)
        default:
            return nil
        }
        return .some(nil)
    }
}
