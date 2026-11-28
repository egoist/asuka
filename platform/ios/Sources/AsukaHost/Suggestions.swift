#if canImport(UIKit)
import UIKit
#if canImport(AsukaOps)
import AsukaOps
#endif

/// A TextField's Suggestions: UIKit has no autocomplete list for a
/// UITextField, so it's a list under the field, in a rounded, shadowed
/// material as the system's own suggestion menus are, over the window
/// (above the field when the keyboard leaves no room under it). It shows
/// while the field is being edited, the user has typed since it took the
/// focus or since a choice, and the core's list isn't empty; the core
/// filters it. A tap chooses; with a hardware keyboard the arrow keys move
/// in it, Return chooses and Escape closes it. Dropdown: a chevron at the
/// field's end opens UIKit's own menu of the suggestions as they are.
@MainActor final class AsukaSuggestions: NSObject, UITableViewDataSource, UITableViewDelegate, UIGestureRecognizerDelegate {
    weak var field: AsukaTextField?
    private(set) var items: [String] = []
    /// The user has typed since the focus came or the last choice.
    private(set) var armed = false
    /// The row the arrow keys are on (-1: none).
    private(set) var highlighted = -1
    var chose: ((Int) -> Void)?

    let popup = UIView()
    private let material = UIVisualEffectView(effect: UIBlurEffect(style: .systemMaterial))
    let table = UITableView(frame: .zero, style: .plain)
    private var scrolls: [NSKeyValueObservation] = []
    private var keyboardTop: CGFloat?
    private(set) var dropdownButton: AsukaDropdownButton?

    static let rowHeight: CGFloat = 44
    static let mostRows: CGFloat = 5.5 // a half row shows there's more

    init(_ field: AsukaTextField) {
        self.field = field
        super.init()
        popup.layer.shadowColor = UIColor.black.cgColor
        popup.layer.shadowOpacity = 0.18
        popup.layer.shadowRadius = 24
        popup.layer.shadowOffset = CGSize(width: 0, height: 8)
        material.layer.cornerRadius = 13
        material.layer.cornerCurve = .continuous
        material.clipsToBounds = true
        material.autoresizingMask = [.flexibleWidth, .flexibleHeight]
        popup.addSubview(material)
        table.backgroundColor = .clear
        table.rowHeight = UITableView.automaticDimension
        table.estimatedRowHeight = Self.rowHeight
        table.contentInsetAdjustmentBehavior = .never
        table.dataSource = self
        table.delegate = self
        table.register(UITableViewCell.self, forCellReuseIdentifier: "s")
        table.autoresizingMask = [.flexibleWidth, .flexibleHeight]
        #if !os(tvOS)
        table.keyboardDismissMode = .none
        #endif
        material.contentView.addSubview(table)
        NotificationCenter.default.addObserver(self, selector: #selector(keyboardChanged(_:)), name: UIResponder.keyboardWillChangeFrameNotification, object: nil)
        NotificationCenter.default.addObserver(self, selector: #selector(keyboardHidden(_:)), name: UIResponder.keyboardWillHideNotification, object: nil)
    }

    var isShowing: Bool { popup.superview != nil }

    // MARK: What the core and the field say

    /// The core's list (filtered already): shown in place, hidden when empty.
    func set(_ list: [String]) {
        guard list != items else { return }
        items = list
        highlighted = -1
        dropdownButton?.isEnabled = !items.isEmpty
        if isShowing { table.reloadData() }
        refresh()
    }

    /// Dropdown: a chevron at the field's end opening the list's menu.
    func setDropdown(_ on: Bool) {
        guard let f = field else { return }
        if !on {
            if f.rightView === dropdownButton { f.rightView = nil }
            dropdownButton = nil
            return
        }
        guard dropdownButton == nil else { return }
        let b = AsukaDropdownButton.make(self)
        b.isEnabled = !items.isEmpty
        dropdownButton = b
        f.rightView = b
        f.rightViewMode = .always
    }

    /// The user typed.
    func typed() {
        armed = true
        refresh()
    }

    /// The field took the focus: nothing shows until the user types.
    func began() {
        armed = false
        hide()
    }

    func ended() {
        armed = false
        hide()
    }

    private func refresh() {
        if armed, !items.isEmpty, field?.isFirstResponder == true, field?.window != nil { show() } else { hide() }
    }

    // MARK: Choosing

    /// Puts suggestion i in the field (not reported as typed), its caret
    /// at the end, closes the list and reports the choice.
    func choose(_ i: Int) {
        guard let f = field, i >= 0, i < items.count else { return }
        let s = items[i]
        f.text = s
        let end = f.endOfDocument
        f.selectedTextRange = f.textRange(from: end, to: end)
        armed = false
        hide()
        chose?(i)
    }

    /// Return with a row highlighted chooses it: whether it did.
    func chooseHighlighted() -> Bool {
        guard isShowing, highlighted >= 0 else { return false }
        choose(highlighted)
        return true
    }

    /// The arrow keys: the highlight moves, from the top or the bottom.
    func move(_ by: Int) {
        guard isShowing, !items.isEmpty else { return }
        var i = highlighted < 0 ? (by > 0 ? 0 : items.count - 1) : highlighted + by
        i = min(max(i, 0), items.count - 1)
        highlighted = i
        let p = IndexPath(row: i, section: 0)
        table.selectRow(at: p, animated: false, scrollPosition: .none)
        table.scrollToRow(at: p, at: .none, animated: false)
    }

    /// Escape: the list closes until the user types again.
    func dismiss() {
        armed = false
        hide()
    }

    // MARK: Showing

    private func show() {
        #if os(tvOS)
        return // the TV's keyboard covers the screen: its own suggestions show there
        #else
        guard let f = field, let w = f.window else { return }
        if popup.superview !== w {
            popup.removeFromSuperview()
            w.addSubview(popup)
            watchOutside(w)
            follow(f)
            table.reloadData()
        } else {
            w.bringSubviewToFront(popup)
        }
        place()
        #endif
    }

    func hide() {
        guard isShowing else { return }
        popup.removeFromSuperview()
        if let g = outsideTap { g.view?.removeGestureRecognizer(g) }
        outsideTap = nil
        scrolls = []
        highlighted = -1
    }

    /// A tap elsewhere closes the list (as a menu closes), and goes on to
    /// what it tapped.
    private var outsideTap: UITapGestureRecognizer?

    private func watchOutside(_ w: UIWindow) {
        guard outsideTap == nil else { return }
        let g = UITapGestureRecognizer(target: self, action: #selector(tappedOutside(_:)))
        g.cancelsTouchesInView = false
        g.delaysTouchesEnded = false
        g.delegate = self
        w.addGestureRecognizer(g)
        outsideTap = g
    }

    @objc private func tappedOutside(_ g: UITapGestureRecognizer) {
        guard let w = g.view, let f = field else { return }
        let p = g.location(in: w)
        if popup.frame.contains(p) || f.convert(f.bounds, to: w).contains(p) { return }
        dismiss()
    }

    func gestureRecognizer(_ g: UIGestureRecognizer, shouldRecognizeSimultaneouslyWith other: UIGestureRecognizer) -> Bool { true }

    /// Placed again as the scroll views holding the field scroll.
    private func follow(_ f: UIView) {
        scrolls = []
        var v = f.superview
        while let s = v {
            if let sv = s as? UIScrollView {
                scrolls.append(sv.observe(\.contentOffset, options: []) { [weak self] _, _ in
                    MainActor.assumeIsolated { self?.place() }
                })
            }
            v = s.superview
        }
    }

    /// Under the field, as wide, as tall as its rows (five and a half at
    /// most), within the room above the keyboard; above it when there's
    /// more room there and too little under it.
    func place() {
        guard let f = field, let w = popup.superview else { return }
        let r = f.convert(f.bounds, to: w)
        let safe = w.safeAreaInsets
        let gap: CGFloat = 6, margin: CGFloat = 8
        // Its rows as tall as their text makes them (a list's, at the text size).
        if table.bounds.width != r.width {
            table.frame.size.width = r.width
            table.reloadData()
        }
        table.layoutIfNeeded()
        let rows = table.contentSize.height > 0 ? table.contentSize.height : CGFloat(items.count) * Self.rowHeight
        let row = items.isEmpty ? Self.rowHeight : rows / CGFloat(items.count)
        let want = min(rows, Self.mostRows * row)
        let bottom = min(w.bounds.maxY - safe.bottom, keyboardTop ?? .greatestFiniteMagnitude) - margin
        let below = bottom - (r.maxY + gap)
        let above = r.minY - gap - (safe.top + margin)
        let frame = Self.frame(field: r, want: want, below: below, above: above, gap: gap)
        popup.frame = frame
        material.frame = popup.bounds
        popup.layer.shadowPath = UIBezierPath(roundedRect: popup.bounds, cornerRadius: 13).cgPath
        table.isScrollEnabled = want > frame.height
    }

    /// Where the list goes for a field's frame: under it unless it's
    /// short of two and a half rows there and there's more room above.
    static func frame(field r: CGRect, want: CGFloat, below: CGFloat, above: CGFloat, gap: CGFloat) -> CGRect {
        let least = min(want, rowHeight * 2.5)
        if below >= least || below >= above {
            return CGRect(x: r.minX, y: r.maxY + gap, width: r.width, height: max(min(want, below), 0))
        }
        let h = max(min(want, above), 0)
        return CGRect(x: r.minX, y: r.minY - gap - h, width: r.width, height: h)
    }

    @objc private func keyboardChanged(_ n: Notification) {
        guard let end = (n.userInfo?[UIResponder.keyboardFrameEndUserInfoKey] as? NSValue)?.cgRectValue,
              let w = field?.window else { return }
        let r = w.convert(end, from: w.screen.coordinateSpace)
        keyboardTop = r.minY < w.bounds.maxY ? r.minY : nil
        if isShowing { place() }
    }

    @objc private func keyboardHidden(_ n: Notification) {
        keyboardTop = nil
        if isShowing { place() }
    }

    // MARK: The list

    func tableView(_ tableView: UITableView, numberOfRowsInSection section: Int) -> Int { items.count }

    func tableView(_ tableView: UITableView, cellForRowAt indexPath: IndexPath) -> UITableViewCell {
        let c = tableView.dequeueReusableCell(withIdentifier: "s", for: indexPath)
        var cfg = UIListContentConfiguration.cell()
        cfg.text = indexPath.row < items.count ? items[indexPath.row] : ""
        cfg.textProperties.numberOfLines = 1
        c.contentConfiguration = cfg
        // Lines between the rows, none under the last (the material's edge is).
        c.separatorInset = indexPath.row == items.count - 1 ? UIEdgeInsets(top: 0, left: 10_000, bottom: 0, right: 0) : UIEdgeInsets(top: 0, left: 20, bottom: 0, right: 0)
        // On the material: clear, but for the row pressed or the arrow keys are on.
        c.configurationUpdateHandler = { cell, state in
            var bg = UIBackgroundConfiguration.listPlainCell().updated(for: state)
            if !state.isHighlighted && !state.isSelected { bg.backgroundColor = .clear }
            cell.backgroundConfiguration = bg
        }
        return c
    }

    func tableView(_ tableView: UITableView, didSelectRowAt indexPath: IndexPath) {
        choose(indexPath.row)
    }
}

/// A combo box's chevron at a field's end: UIKit's menu of the
/// suggestions as they are, the one the field shows checked.
final class AsukaDropdownButton: UIButton {
    weak var suggestions: AsukaSuggestions?

    static func make(_ s: AsukaSuggestions) -> AsukaDropdownButton {
        var c = UIButton.Configuration.plain()
        c.image = UIImage(systemName: "chevron.down")
        c.preferredSymbolConfigurationForImage = UIImage.SymbolConfiguration(textStyle: .footnote, scale: .medium)
            .applying(UIImage.SymbolConfiguration(weight: .semibold))
        c.contentInsets = NSDirectionalEdgeInsets(top: 0, leading: 8, bottom: 0, trailing: 8)
        c.baseForegroundColor = .secondaryLabel
        let b = AsukaDropdownButton(configuration: c)
        b.suggestions = s
        b.showsMenuAsPrimaryAction = true
        b.menu = UIMenu(children: [UIDeferredMenuElement.uncached { [weak s] done in
            MainActor.assumeIsolated {
                guard let s else { return done([]) }
                let text = s.field?.text ?? ""
                done(s.items.enumerated().map { i, t in
                    UIAction(title: t, state: t == text ? .on : .off) { [weak s] _ in s?.choose(i) }
                })
            }
        }])
        b.accessibilityLabel = s.field?.placeholder
        return b
    }

    override func contextMenuInteraction(_ interaction: UIContextMenuInteraction, willDisplayMenuFor configuration: UIContextMenuConfiguration, animator: UIContextMenuInteractionAnimating?) {
        super.contextMenuInteraction(interaction, willDisplayMenuFor: configuration, animator: animator)
        suggestions?.dismiss() // the menu, not the list too
    }
}

extension AsukaTextField {
    #if !os(tvOS)
    /// While the suggestions show: the arrow keys and Escape are theirs.
    override var keyCommands: [UIKeyCommand]? {
        guard let s = suggestions, s.isShowing else { return super.keyCommands }
        let cmds = [UIKeyCommand.inputUpArrow, UIKeyCommand.inputDownArrow, UIKeyCommand.inputEscape].map { input in
            let c = UIKeyCommand(title: "", action: #selector(asukaSuggestionKey(_:)), input: input, modifierFlags: [])
            c.wantsPriorityOverSystemBehavior = true
            return c
        }
        return cmds + (super.keyCommands ?? [])
    }

    @objc func asukaSuggestionKey(_ c: UIKeyCommand) {
        guard let s = suggestions else { return }
        switch c.input {
        case UIKeyCommand.inputUpArrow: s.move(-1)
        case UIKeyCommand.inputDownArrow: s.move(1)
        default: s.dismiss()
        }
    }
    #endif
}

@MainActor extension AsukaRuntime {
    /// The field's suggestions, made as the first of their props comes.
    func suggestions(_ n: AsukaNode) -> AsukaSuggestions? {
        guard let t = n.view as? AsukaTextField else { return nil }
        if let s = t.suggestions { return s }
        let s = AsukaSuggestions(t)
        let id = n.id
        s.chose = { [weak self] i in self?.emit(id, .suggestion) { AsukaEvents.suggestion(id, Int64(i)) } }
        t.addAction(UIAction { [weak s] _ in s?.typed() }, for: .editingChanged)
        t.suggestions = s
        return s
    }
}
#endif
