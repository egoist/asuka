// A List: virtualized as on the other hosts (DESIGN.md's "Lists"). Its rows
// are cells the host makes (IDs with the high bit set) as SwiftUI's lazy
// stack shows them, bound with asuka_list_bind: the core makes the row
// under the cell, lays it out at the list's width and sends the cell's
// frame, whose height is the row's. Rows leaving the screen are unbound
// and their cells kept for the next rows. The Digital Crown scrolls it, as
// it does any ScrollView.

import Foundation
import SwiftUI
#if canImport(AsukaOps)
import AsukaOps
#endif
#if canImport(CAsuka)
@_implementationOnly import CAsuka
#endif

@MainActor final class AsukaWatchList {
    unowned let node: AsukaNode
    /// The items' IDs, in order (ListUpdate's).
    private(set) var ids: [Int64] = []
    /// The cells bound, by the item they show.
    private var cells: [Int64: AsukaNode] = [:]
    /// Cells unbound, for the next rows.
    private var free: [AsukaNode] = []
    /// A row to scroll to (ScrollTo), and whether animated; or an offset.
    var scrollTarget: (item: Int64, animated: Bool)?
    var scrollToTop = false

    /// Reorder: the names of the screen readers' actions that move a row
    /// up and down a place (nil: rows don't move). A watch has no drag to
    /// reorder with: VoiceOver's actions move them.
    var reorder: (up: String, down: String)?

    init(node: AsukaNode) { self.node = node }

    /// Reorder's bytes: u32 count, then the names.
    func setReorder(_ b: UnsafeRawBufferPointer) {
        var r = AsukaReader(b)
        guard b.count >= 4, r.u32() >= 2 else { reorder = nil; return }
        let up = r.string(), down = r.string()
        reorder = (up, down)
    }

    /// Moves the row at index a place up (-1) or down (1): the IDs are in
    /// the new order (the core's update in it changes nothing), and the
    /// core hears of it with Moved.
    func move(_ index: Int, by d: Int) {
        let to = index + d
        guard reorder != nil, index >= 0, index < ids.count, to >= 0, to < ids.count else { return }
        ids.insert(ids.remove(at: index), at: to)
        let rt = AsukaRuntime.shared, id = node.id
        rt.emit(id, .moved) { AsukaEvents.moved(id, x: Double(index), y: Double(to)) }
        rt.touch(node)
    }

    func update(_ ids: [Int64]) {
        self.ids = ids
        let live = Set(ids)
        for (item, cell) in cells where !live.contains(item) {
            unbind(item, cell)
        }
    }

    func scroll(to index: Int, animated: Bool) {
        guard index >= 0, index < ids.count else { return }
        scrollTarget = (ids[index], animated)
    }

    /// The row of item at index came on screen: the cell that shows it,
    /// bound now if it wasn't.
    func appear(_ item: Int64, _ index: Int) -> AsukaNode? {
        if let c = cells[item] { return c }
        let rt = AsukaRuntime.shared
        guard rt.nodes[node.id] === node, index < ids.count else { return nil }
        let cell = free.popLast() ?? rt.makeCell(self)
        cell.cellIndex = index
        cells[item] = cell
        rt.enter { asuka_list_bind(node.id, Int64(index), cell.id) }
        return cell
    }

    /// Its row left the screen: its cell is unbound, for another row.
    func disappear(_ item: Int64) {
        guard let c = cells[item] else { return }
        unbind(item, c)
    }

    private func unbind(_ item: Int64, _ cell: AsukaNode) {
        cells[item] = nil
        let rt = AsukaRuntime.shared
        if rt.nodes[node.id] === node { rt.enter { asuka_list_unbind(node.id, cell.id) } }
        cell.cellIndex = -1
        free.append(cell)
    }

    func rowResized(_ cell: AsukaNode) {}

    func cellDestroyed(_ cell: AsukaNode) {
        free.removeAll { $0 === cell }
        for (k, v) in cells where v === cell { cells[k] = nil }
    }

    /// The list went: its cells with it.
    func destroyed() {
        let rt = AsukaRuntime.shared
        for c in Array(cells.values) + free { rt.nodes[c.id] = nil }
        cells = [:]
        free = []
    }
}

/// A List's view: a lazy stack of its rows in a ScrollView (the Digital
/// Crown scrolls it), each row on the watch's platter, as SwiftUI's lists
/// are, at the height the core laid it out at.
struct AsukaListView: View {
    let node: AsukaNode

    var body: some View {
        let _ = node.rev
        let list = node.list
        let ids = list?.ids ?? []
        // The watch's lists set each row on a platter, a few points apart
        // (a bare List's rows are as the app draws them).
        let platter = AsukaListAppearance(rawValue: node.mode) != .bare
        ScrollViewReader { proxy in
            ScrollView(.vertical) {
                LazyVStack(alignment: .leading, spacing: platter ? 4 : 0) {
                    ForEach(Array(ids.enumerated()), id: \.element) { i, item in
                        if let list {
                            AsukaListRow(list: list, item: item, index: i)
                                .background { if platter { RoundedRectangle(cornerRadius: 14, style: .continuous).fill(Color(white: 0.13)) } }
                                .accessibilityActions {
                                    if let r = list.reorder {
                                        if i > 0 { Button(r.up) { list.move(i, by: -1) } }
                                        if i + 1 < ids.count { Button(r.down) { list.move(i, by: 1) } }
                                    }
                                }
                                .id(item)
                        }
                    }
                }
            }
            .modifier(AsukaScrollOffsets(node: node) { if let first = ids.first { proxy.scrollTo(first, anchor: .top) } })
            .onChange(of: node.rev) {
                guard let t = list?.scrollTarget else { return }
                list?.scrollTarget = nil
                if t.animated {
                    withAnimation { proxy.scrollTo(t.item, anchor: .top) }
                } else {
                    proxy.scrollTo(t.item, anchor: .top)
                }
            }
        }
    }
}

/// A row: its cell, bound as it comes on screen.
struct AsukaListRow: View {
    let list: AsukaWatchList
    let item: Int64
    let index: Int
    @State private var cell: AsukaNode?

    var body: some View {
        Group {
            if let cell {
                AsukaCellView(cell: cell)
            } else {
                // Not bound yet: about a row's height, so the lazy stack
                // doesn't make every row at once.
                Color.clear.frame(height: 44)
            }
        }
        .onAppear { cell = list.appear(item, index) }
        .onDisappear {
            list.disappear(item)
            cell = nil
        }
    }
}

struct AsukaCellView: View {
    let cell: AsukaNode

    var body: some View {
        let _ = cell.rev
        ZStack(alignment: .topLeading) {
            ForEach(cell.shownChildren) { AsukaNodeView(node: $0) }
        }
        .frame(width: cell.placed ? cell.frame.width : nil, height: cell.placed ? cell.frame.height : 44, alignment: .topLeading)
        .frame(maxWidth: .infinity, alignment: .topLeading)
    }
}
