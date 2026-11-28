// Chips and disclosures: AppKit's buttons in the shapes a Mac gives them.
// A Chip is a recessed NSButton (a capsule; a filter chip a push-on-push-off
// one, the accent fill while it's on, with Material's checkmark in place of
// its icon), its leading icon an SF Symbol; a removable chip has an
// xmark.circle.fill at its end, a button of its own, and Delete or
// Backspace while it has the keyboard's focus removes it too. A
// Disclosure's header is NSButton's disclosure triangle (a chevron since
// macOS 26) before its title, as Finder's and System Settings' disclosures
// are: a click anywhere on it turns the triangle, and the core shows the
// content under it.
import AppKit
#if canImport(AsukaOps)
import AsukaOps
#endif

/// A view of the host's own made of AppKit's controls (a date range's
/// pickers, a disclosure's triangle and title): what the common props set on
/// a control it passes to them.
@MainActor protocol AsukaCompound: NSView {
    /// Disabled: its controls take no clicks and look it.
    func setEnabled(_ on: Bool)
    /// Font: its text's.
    func setFont(_ f: NSFont)
    /// Color: its text's, or its tint (nil: the platform's).
    func setColor(_ c: NSColor?)
}

// MARK: Chips

/// A Chip node's props.
@MainActor final class AsukaChipState {
    var icon = ""
    var filter = false
    var selected = false
    var removable = false
}

/// A Chip: a recessed button, its remove button (a removable chip's) a
/// subview at its end, which its cell leaves room for.
final class AsukaChipButton: NSButton {
    /// The remove button, while it's removable.
    private(set) var remover: NSButton?
    /// Asked to remove it: the remove button, Delete or Backspace, a screen
    /// reader's remove action.
    var onRemove: (() -> Void)?

    var chipCell: AsukaChipCell? { cell as? AsukaChipCell }

    /// Shows or hides the remove button, named name for a screen reader.
    func setRemovable(_ on: Bool, name: String) {
        if !on {
            remover?.removeFromSuperview()
            remover = nil
            chipCell?.trailingRoom = 0
            invalidateIntrinsicContentSize()
            return
        }
        if remover == nil {
            let b = NSButton(frame: .zero)
            b.isBordered = false
            b.setButtonType(.momentaryChange)
            b.imagePosition = .imageOnly
            if #available(macOS 11.0, *) {
                let conf = NSImage.SymbolConfiguration(pointSize: 12, weight: .regular)
                b.image = NSImage(systemSymbolName: "xmark.circle.fill", accessibilityDescription: nil)?.withSymbolConfiguration(conf)
            }
            b.contentTintColor = .secondaryLabelColor
            b.target = self
            b.action = #selector(removePressed(_:))
            b.refusesFirstResponder = true
            b.isEnabled = isEnabled
            addSubview(b)
            remover = b
            chipCell?.trailingRoom = AsukaChipCell.removerRoom
            invalidateIntrinsicContentSize()
            needsLayout = true
        }
        remover?.setAccessibilityLabel(name)
    }

    @objc func removePressed(_ sender: Any?) { onRemove?() }

    override var isEnabled: Bool {
        didSet { remover?.isEnabled = isEnabled }
    }

    override var userInterfaceLayoutDirection: NSUserInterfaceLayoutDirection {
        didSet {
            remover?.userInterfaceLayoutDirection = userInterfaceLayoutDirection
            needsLayout = true
            needsDisplay = true
        }
    }

    /// The remove button at its end: the right, or the left reading right
    /// to left (its cell leaves the room there).
    override func layout() {
        super.layout()
        guard let r = remover else { return }
        let s: CGFloat = 16, pad: CGFloat = 5
        let x = userInterfaceLayoutDirection == .rightToLeft ? pad : bounds.width - pad - s
        r.frame = CGRect(x: x, y: ((bounds.height - s) / 2).rounded(), width: s, height: s)
    }

    /// Delete and Backspace remove a removable chip with the keyboard's
    /// focus.
    override func keyDown(with e: NSEvent) {
        if remover != nil, isEnabled, e.keyCode == 51 || e.keyCode == 117 {
            onRemove?()
            return
        }
        super.keyDown(with: e)
    }
}

/// A chip's cell: its title and icon drawn short of the remove button's
/// room at its end, which it measures with.
final class AsukaChipCell: NSButtonCell {
    /// The room the remove button takes.
    static let removerRoom: CGFloat = 12
    var trailingRoom: CGFloat = 0

    /// Its size with the room (cellSize asks it too).
    override func cellSize(forBounds rect: NSRect) -> NSSize {
        var s = super.cellSize(forBounds: rect)
        s.width += trailingRoom
        return s
    }

    /// Its content's frame: short of the remove button's room.
    func content(_ frame: NSRect, _ v: NSView) -> NSRect {
        guard trailingRoom > 0 else { return frame }
        // Its title where it would be without the button: the bezel's end
        // padding, then the button.
        var r = frame
        r.size.width -= trailingRoom
        if v.userInterfaceLayoutDirection == .rightToLeft { r.origin.x += trailingRoom }
        return r
    }

    override func drawInterior(withFrame frame: NSRect, in v: NSView) {
        super.drawInterior(withFrame: content(frame, v), in: v)
    }

    override func titleRect(forBounds rect: NSRect) -> NSRect {
        guard let v = controlView else { return super.titleRect(forBounds: rect) }
        return super.titleRect(forBounds: content(rect, v))
    }
}

@MainActor extension AsukaRuntime {
    func makeChip(_ n: AsukaNode) -> NSView {
        let b = AsukaChipButton(frame: .zero)
        b.cell = AsukaChipCell(textCell: "")
        b.bezelStyle = .recessed
        b.setButtonType(.momentaryPushIn)
        b.showsBorderOnlyWhileMouseInside = false
        b.imagePosition = .imageLeading
        b.font = styleFont(0)
        b.title = ""
        b.target = self
        b.action = #selector(chipPressed(_:))
        let id = n.id
        b.onRemove = { MainActor.assumeIsolated { AsukaRuntime.shared.chipRemove(id) } }
        n.chip = AsukaChipState()
        return b
    }

    func setChipProp(_ n: AsukaNode, _ p: UInt16, _ v: AsukaValue) {
        guard let c = n.chip, let b = n.view as? AsukaChipButton else { return }
        switch p {
        case AsukaProp.Chip.title:
            n.title = v.string
            b.title = v.string
            chipNames(n)
        case AsukaProp.Chip.icon:
            c.icon = v.string
            chipImage(n)
        case AsukaProp.Chip.filter:
            c.filter = v.bool
            // A filter chip stays pressed while it's on: its state is
            // Selected; an action chip springs back.
            b.setButtonType(v.bool ? .pushOnPushOff : .momentaryPushIn)
            b.showsBorderOnlyWhileMouseInside = false
            b.state = v.bool && c.selected ? .on : .off
            chipImage(n)
        case AsukaProp.Chip.selected:
            c.selected = v.bool
            if c.filter { b.state = v.bool ? .on : .off }
            chipImage(n)
        case AsukaProp.Chip.removable:
            c.removable = v.bool
            chipNames(n)
        default:
            break
        }
    }

    /// A chip's image: a selected filter chip's checkmark, else its icon.
    func chipImage(_ n: AsukaNode) {
        guard let c = n.chip, let b = n.view as? NSButton else { return }
        let name = c.filter && c.selected ? "checkmark" : c.icon
        b.image = chipGap(symbolImage(name), rtl: b.userInterfaceLayoutDirection == .rightToLeft)
        b.imagePosition = b.image == nil ? .noImage : .imageLeading
        b.invalidateIntrinsicContentSize()
    }

    /// A chip's icon with room after it, before the title (NSButton sets
    /// them edge to edge): a template still, so the button tints it.
    func chipGap(_ img: NSImage?, rtl: Bool) -> NSImage? {
        guard let img else { return nil }
        let gap: CGFloat = 4
        let out = NSImage(size: NSSize(width: img.size.width + gap, height: img.size.height), flipped: false) { r in
            img.draw(in: NSRect(x: rtl ? gap : 0, y: 0, width: img.size.width, height: r.height))
            return true
        }
        out.isTemplate = img.isTemplate
        out.accessibilityDescription = img.accessibilityDescription
        return out
    }

    /// The remove button and the remove action, named with Asuka's word
    /// and the title.
    func chipNames(_ n: AsukaNode) {
        guard let c = n.chip, let b = n.view as? AsukaChipButton else { return }
        let word = asukaWord("remove")
        b.setRemovable(c.removable, name: n.title.isEmpty ? word : word + " " + n.title)
        if c.removable {
            let id = n.id
            let action = NSAccessibilityCustomAction(name: word) {
                MainActor.assumeIsolated { AsukaRuntime.shared.chipRemove(id) }
                return true
            }
            b.setAccessibilityCustomActions([action])
        } else {
            b.setAccessibilityCustomActions(nil)
        }
    }

    /// A chip's click: a filter chip's toggle (AppKit has turned it), an
    /// action chip's press.
    @objc func chipPressed(_ sender: Any?) {
        guard let b = sender as? NSButton, let n = node(of: b), let c = n.chip else { return }
        if c.filter {
            c.selected = b.state == .on
            chipImage(n)
            let on = c.selected
            emit(n, .toggled) { AsukaEvents.toggled($0, on) }
        } else {
            b.state = .off // a click turns any button's state: an action chip has none
            emit(n, .press) { AsukaEvents.press($0) }
        }
    }

    func chipRemove(_ id: UInt32) {
        guard let n = nodes[id], let c = n.chip, c.removable, (n.view as? NSControl)?.isEnabled ?? true else { return }
        emit(n, .remove) { AsukaEvents.remove($0) }
    }

    func chipJSON(_ n: AsukaNode, _ d: inout [String: Any]) {
        guard let c = n.chip, let b = n.view as? AsukaChipButton else { return }
        var j: [String: Any] = ["filter": c.filter, "selected": c.selected, "removable": c.removable, "icon": c.icon,
                                "on": b.state == .on]
        if let r = b.remover {
            j["removeFrame"] = [Double(r.frame.minX), Double(r.frame.minY), Double(r.frame.width), Double(r.frame.height)]
            j["removeLabel"] = r.accessibilityLabel() ?? ""
        }
        if let a = b.accessibilityCustomActions() { j["actions"] = a.map { $0.name } }
        d["chip"] = j
    }
}

// MARK: Disclosures

/// A Disclosure's header: the disclosure triangle, then its title; a click
/// anywhere on it turns the triangle.
final class AsukaDisclosureView: NSView, AsukaCompound {
    let triangle = AsukaDisclosureButton(frame: .zero)
    let label = NSTextField(labelWithString: "")
    static let gap: CGFloat = 6

    override init(frame: NSRect) {
        super.init(frame: frame)
        triangle.bezelStyle = .disclosure
        triangle.setButtonType(.onOff)
        triangle.title = ""
        triangle.imagePosition = .imageOnly
        // VoiceOver's disclosure triangle, expanded or not (AppKit's button
        // says none of its own).
        triangle.setAccessibilityRole(.disclosureTriangle)
        addSubview(triangle)
        label.lineBreakMode = .byTruncatingTail
        label.cell?.wraps = false
        label.font = styleFont(0)
        label.textColor = .labelColor
        label.setAccessibilityElement(false) // the triangle says it
        addSubview(label)
        setAccessibilityElement(false)
    }

    required init?(coder: NSCoder) { fatalError("init(coder:)") }

    override var isFlipped: Bool { true }

    override var userInterfaceLayoutDirection: NSUserInterfaceLayoutDirection {
        didSet {
            triangle.userInterfaceLayoutDirection = userInterfaceLayoutDirection
            label.userInterfaceLayoutDirection = userInterfaceLayoutDirection
            label.alignment = userInterfaceLayoutDirection == .rightToLeft ? .right : .left
            needsLayout = true
        }
    }

    var triangleSize: CGSize {
        let s = triangle.intrinsicContentSize
        return CGSize(width: max(s.width, 13), height: max(s.height, 13))
    }

    /// The triangle at its start (the left, or the right reading right to
    /// left), the title after it, both centered down its height.
    override func layout() {
        super.layout()
        let t = triangleSize
        let ls = label.intrinsicContentSize
        let rtl = userInterfaceLayoutDirection == .rightToLeft
        let tx = rtl ? bounds.width - t.width : 0
        triangle.frame = CGRect(x: tx, y: ((bounds.height - t.height) / 2).rounded(), width: t.width, height: t.height)
        let lw = max(bounds.width - t.width - Self.gap, 0)
        let lx = rtl ? 0 : t.width + Self.gap
        label.frame = CGRect(x: lx, y: ((bounds.height - ls.height) / 2).rounded(), width: lw, height: ls.height)
    }

    /// Its size: the triangle and the title in a row a little taller than
    /// the title, as a Mac's disclosure rows are.
    func fit() -> CGSize {
        let t = triangleSize
        let ls = label.intrinsicContentSize
        let w = t.width + (label.stringValue.isEmpty ? 0 : Self.gap + ls.width)
        return CGSize(width: w, height: max(ls.height, t.height) + 6)
    }

    /// A click on the title (or between): the triangle's.
    override func mouseDown(with e: NSEvent) {
        guard triangle.isEnabled else { return }
        triangle.performClick(nil)
    }

    func setEnabled(_ on: Bool) {
        triangle.isEnabled = on
        label.textColor = on ? (labelColor ?? .labelColor) : .disabledControlTextColor
    }

    var labelColor: NSColor?

    func setFont(_ f: NSFont) {
        label.font = f
        needsLayout = true
    }

    func setColor(_ c: NSColor?) {
        labelColor = c
        if triangle.isEnabled { label.textColor = c ?? .labelColor }
    }
}

/// The disclosure triangle: Return turns it too, as Space does.
final class AsukaDisclosureButton: NSButton {
    override func keyDown(with e: NSEvent) {
        if e.keyCode == 0x24 || e.keyCode == 0x4c { // Return, Enter
            performClick(nil)
            return
        }
        super.keyDown(with: e)
    }
}

@MainActor extension AsukaRuntime {
    func makeDisclosure(_ n: AsukaNode) -> NSView {
        let v = AsukaDisclosureView(frame: .zero)
        v.triangle.target = self
        v.triangle.action = #selector(disclosureToggled(_:))
        return v
    }

    func setDisclosureProp(_ n: AsukaNode, _ p: UInt16, _ v: AsukaValue) {
        guard let d = n.view as? AsukaDisclosureView else { return }
        switch p {
        case AsukaProp.Disclosure.title:
            n.title = v.string
            d.label.stringValue = v.string
            if n.a11yLabel.isEmpty { d.triangle.setAccessibilityLabel(v.string) }
            d.needsLayout = true
        case AsukaProp.Disclosure.expanded:
            // The triangle turns as AppKit animates it.
            d.triangle.state = v.bool ? .on : .off
            d.triangle.setAccessibilityExpanded(v.bool)
        default:
            break
        }
    }

    @objc func disclosureToggled(_ sender: Any?) {
        guard let b = sender as? NSButton, let n = nodeAt(b), n.kind == .disclosure else { return }
        let on = b.state == .on
        b.setAccessibilityExpanded(on)
        emit(n, .toggled) { AsukaEvents.toggled($0, on) }
    }

    func disclosureJSON(_ n: AsukaNode, _ d: inout [String: Any]) {
        guard let v = n.view as? AsukaDisclosureView else { return }
        d["disclosure"] = ["expanded": v.triangle.state == .on, "title": v.label.stringValue,
                           "triangleFrame": [Double(v.triangle.frame.minX), Double(v.triangle.frame.minY),
                                             Double(v.triangle.frame.width), Double(v.triangle.frame.height)],
                           "a11yRole": v.triangle.accessibilityRole()?.rawValue ?? "",
                           "a11yLabel": v.triangle.accessibilityLabel() ?? ""]
    }
}

// MARK: The automation's

@MainActor extension AsukaRuntime {
    /// What the controls of their own files show, for the automation's tree.
    func controlsJSON(_ n: AsukaNode, _ d: inout [String: Any]) {
        switch n.kind {
        case .chip: chipJSON(n, &d)
        case .disclosure: disclosureJSON(n, &d)
        case .dateRangePicker: dateRangeJSON(n, &d)
        default: break
        }
    }

    /// The automation's steps of these controls (Automation.swift): nil if
    /// verb isn't one of them, else its error ("": none).
    ///   - rangeset <id> <0|1> <ms>: the user sets a DateRangePicker's
    ///     first (0) or last (1) day
    ///   - suggestpick <id> <index>: the user clicks a field's suggestion
    ///   - chipremove <id>: a click on a chip's remove button
    ///   - focusview <id>: the keyboard's focus on a view
    func controlsStep(_ verb: String, _ arg: String) -> String? {
        let a = arg.split(separator: " ").map(String.init)
        func node(_ s: String) -> AsukaNode? { UInt32(s).flatMap { nodes[$0] } }
        switch verb {
        case "rangeset":
            guard a.count == 3, let n = node(a[0]), let w = Int(a[1]), let ms = Int64(a[2]) else { return "want rangeset <id> <0|1> <ms>" }
            return rangeStep(n, w, ms) ?? ""
        case "suggestpick":
            guard a.count == 2, let n = node(a[0]), let i = Int(a[1]) else { return "want suggestpick <id> <index>" }
            return suggestStep(n, i) ?? ""
        case "chipremove":
            guard let n = a.first.flatMap(node), let b = n.view as? AsukaChipButton, let r = b.remover else { return "no removable chip" }
            r.performClick(nil)
            return ""
        case "focusview":
            // The keyboard's focus on a view (Tab, with keyboard navigation
            // on, would take it there).
            guard let v = a.first.flatMap(node)?.view, let w = v.window else { return "want focusview <id>" }
            return w.makeFirstResponder(v) ? "" : "it takes no focus"
        default:
            return nil
        }
    }

    /// Presses a chip or a disclosure, as a click on it would; false: n
    /// is neither.
    func pressControl(_ n: AsukaNode) -> Bool {
        switch n.kind {
        case .chip: (n.view as? NSButton)?.performClick(nil)
        case .disclosure: (n.view as? AsukaDisclosureView)?.triangle.performClick(nil)
        default: return false
        }
        return true
    }
}
