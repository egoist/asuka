// Screen readers' roles, states and relations on a Mac (RFC 0010): each
// goes to the view's NSAccessibility properties, which VoiceOver and the
// automation read.
//
//   - A11yRole: NSAccessibility roles and subroles, as browsers map ARIA's
//     (a switch is a check box with the switch subrole, a tab a radio
//     button with the tab button subrole, a dialog a group with the
//     application dialog subrole).
//   - A11yChecked: AXValue 0, 1 or 2 (mixed) on a checkable role; a
//     checkable role with none reads unchecked.
//   - A11yRange: AXMinValue, AXMaxValue and AXValue; A11yValue is then its
//     AXValueDescription (else AXValue itself).
//   - A11yExpanded, A11ySelected, A11yModal: AXExpanded, AXSelected,
//     AXModal. While a modal view (or a Modal overlay) shows, the views of
//     the host around it list only what leads to it: the rest of the
//     window is hidden from VoiceOver.
//   - A11yLabelledBy: AXTitleUIElement; A11yControls: AXLinkedUIElements;
//     A11yDescribedBy: AXHelp, the describing view's text (its own hint
//     otherwise). They're resolved again after each batch: a view named
//     may come later, and its text change.
import AppKit
#if canImport(AsukaOps)
import AsukaOps
#endif

/// A node's screen readers' role and states.
@MainActor final class AsukaA11y {
    var role: AsukaRole = .none
    var checked: AsukaA11yChecked = .none
    var expanded: AsukaA11yExpanded = .none
    var selected = false
    var value = ""
    var range: (min: Double, max: Double, now: Double)?
    var modal = false
    var controls: UInt32 = 0
    var labelledBy: UInt32 = 0
    var describedBy: UInt32 = 0
    /// Its A11yHint (its help when nothing describes it).
    var hint = ""
}

/// The nodes with relations or A11yModal.
@MainActor final class AsukaA11yState {
    static let shared = AsukaA11yState()
    var related = Set<UInt32>()
    var modal = Set<UInt32>()
}

@MainActor extension AsukaRuntime {
    func a11y(_ n: AsukaNode) -> AsukaA11y {
        if let a = n.a11y { return a }
        let a = AsukaA11y()
        n.a11y = a
        return a
    }

    /// The common props of screen readers RFC 0010 added, and the role.
    func setA11yProp(_ n: AsukaNode, _ p: UInt16, _ v: AsukaValue) {
        guard let view = n.view else { return }
        let a = a11y(n)
        switch p {
        case AsukaProp.a11yRole:
            a.role = AsukaRole(rawValue: v.enumValue) ?? .none
            applyRole(n)
            applyA11yValue(n)
        case AsukaProp.a11yChecked:
            a.checked = AsukaA11yChecked(rawValue: v.enumValue) ?? .none
            applyA11yValue(n)
        case AsukaProp.a11yExpanded:
            a.expanded = AsukaA11yExpanded(rawValue: v.enumValue) ?? .none
            view.setAccessibilityExpanded(a.expanded == .expanded)
        case AsukaProp.a11ySelected:
            a.selected = v.bool
            view.setAccessibilitySelected(v.bool)
        case AsukaProp.a11yValue:
            a.value = v.string
            applyA11yValue(n)
        case AsukaProp.a11yRange:
            let b = v.data
            a.range = b.count >= 24 ? (Double(bitPattern: b.u64(0)), Double(bitPattern: b.u64(8)), Double(bitPattern: b.u64(16))) : nil
            applyA11yValue(n)
        case AsukaProp.a11yModal:
            a.modal = v.bool
            view.setAccessibilityModal(v.bool)
            if v.bool { AsukaA11yState.shared.modal.insert(n.id) } else { AsukaA11yState.shared.modal.remove(n.id) }
            a11yModalChanged(view.window)
        case AsukaProp.a11yControls:
            a.controls = UInt32(truncatingIfNeeded: v.int)
            relationsChanged(n)
        case AsukaProp.a11yLabelledBy:
            a.labelledBy = UInt32(truncatingIfNeeded: v.int)
            relationsChanged(n)
        case AsukaProp.a11yDescribedBy:
            a.describedBy = UInt32(truncatingIfNeeded: v.int)
            relationsChanged(n)
        case AsukaProp.a11yHint:
            a.hint = v.string
            applyHelp(n)
        default:
            break
        }
    }

    /// The NSAccessibility role and subrole of an Asuka role.
    func macRole(_ r: AsukaRole) -> (NSAccessibility.Role, NSAccessibility.Subrole?)? {
        func sub(_ s: String) -> NSAccessibility.Subrole { NSAccessibility.Subrole(rawValue: s) }
        switch r {
        case .none: return nil
        case .button: return (.button, nil)
        case .header: return (NSAccessibility.Role(rawValue: "AXHeading"), nil)
        case .image: return (.image, nil)
        case .link: return (.link, nil)
        case .text: return (.staticText, nil)
        case .search: return (.textField, .searchField)
        case .checkbox: return (.checkBox, nil)
        case .radio: return (.radioButton, nil)
        case .radioGroup: return (.radioGroup, nil)
        case .switch: return (.checkBox, .switch)
        case .tab: return (.radioButton, sub("AXTabButton"))
        case .tabList: return (.tabGroup, nil)
        case .tabPanel: return (.group, sub("AXTabPanel"))
        case .slider: return (.slider, nil)
        case .spinButton: return (.incrementor, nil)
        case .progressBar: return (.progressIndicator, nil)
        case .meter: return (.levelIndicator, nil)
        case .menu: return (.menu, nil)
        case .menuBar: return (.menuBar, nil)
        case .menuItem, .menuItemCheckbox, .menuItemRadio: return (.menuItem, nil)
        case .listbox: return (.list, nil)
        case .option: return (.staticText, nil) // as WebKit and Chromium show an option
        case .comboBox: return (.comboBox, nil)
        case .dialog: return (.group, sub("AXApplicationDialog"))
        case .alertDialog: return (.group, sub("AXApplicationAlertDialog"))
        case .tooltip: return (.group, sub("AXUserInterfaceTooltip"))
        case .group: return (.group, nil)
        case .separator: return (.splitter, nil)
        case .toolbar: return (.toolbar, nil)
        case .status: return (.group, sub("AXApplicationStatus"))
        }
    }

    /// Gives n's view its role: the app's, else a pressable view's button,
    /// else AppKit's own.
    func applyRole(_ n: AsukaNode) {
        guard let v = n.view else { return }
        let r = n.a11y?.role ?? .none
        if let (role, sub) = macRole(r) {
            v.setAccessibilityRole(role)
            v.setAccessibilitySubrole(sub)
            v.setAccessibilityElement(true)
        } else if n.kind == .view && n.listening(.press) {
            v.setAccessibilityRole(.button)
            v.setAccessibilitySubrole(nil)
        } else if n.adjustable {
            v.setAccessibilityRole(.incrementor)
            v.setAccessibilitySubrole(nil)
        } else {
            v.setAccessibilityRole(nil)
            v.setAccessibilitySubrole(nil)
        }
    }

    /// Whether n's role is checkable (its value is checked or not).
    func checkable(_ a: AsukaA11y) -> Bool {
        switch a.role {
        case .checkbox, .radio, .switch, .menuItemCheckbox, .menuItemRadio: return true
        default: return a.checked != .none
        }
    }

    /// AXValue, AXMinValue, AXMaxValue and AXValueDescription from n's
    /// checked state, range and value.
    func applyA11yValue(_ n: AsukaNode) {
        guard let v = n.view, let a = n.a11y else { return }
        let desc = a.value.isEmpty ? nil : a.value
        if checkable(a) {
            let on = a.checked == .checked ? 1 : a.checked == .mixed ? 2 : 0
            v.setAccessibilityValue(NSNumber(value: on))
            v.setAccessibilityValueDescription(desc)
        } else if let r = a.range {
            v.setAccessibilityMinValue(NSNumber(value: r.min))
            v.setAccessibilityMaxValue(NSNumber(value: r.max))
            v.setAccessibilityValue(NSNumber(value: r.now))
            v.setAccessibilityValueDescription(desc)
        } else {
            v.setAccessibilityValue(desc)
            v.setAccessibilityValueDescription(nil)
        }
        if a.range == nil {
            v.setAccessibilityMinValue(nil)
            v.setAccessibilityMaxValue(nil)
        }
    }

    // MARK: Relations

    func relationsChanged(_ n: AsukaNode) {
        guard let a = n.a11y else { return }
        if a.controls != 0 || a.labelledBy != 0 || a.describedBy != 0 {
            AsukaA11yState.shared.related.insert(n.id)
        } else {
            AsukaA11yState.shared.related.remove(n.id)
        }
        applyRelations(n)
    }

    /// AXTitleUIElement, AXLinkedUIElements and AXHelp from the views n
    /// names.
    func applyRelations(_ n: AsukaNode) {
        guard let v = n.view, let a = n.a11y else { return }
        v.setAccessibilityTitleUIElement(a.labelledBy != 0 ? nodes[a.labelledBy]?.view : nil)
        if a.controls != 0, let c = nodes[a.controls]?.view {
            v.setAccessibilityLinkedUIElements([c])
        } else {
            v.setAccessibilityLinkedUIElements(nil)
        }
        applyHelp(n)
    }

    /// AXHelp: the describing view's text, else the view's hint.
    func applyHelp(_ n: AsukaNode) {
        guard let v = n.view, let a = n.a11y else { return }
        var help = a.hint
        if a.describedBy != 0, let d = nodes[a.describedBy] {
            let t = a11yText(d)
            if !t.isEmpty { help = t }
        }
        v.setAccessibilityHelp(help.isEmpty ? nil : help)
    }

    /// What a screen reader reads of n: its label, its text, or the texts
    /// in it.
    func a11yText(_ n: AsukaNode) -> String {
        if !n.a11yLabel.isEmpty { return n.a11yLabel }
        if !n.text.isEmpty { return n.text }
        guard let v = n.view else { return "" }
        var parts: [String] = []
        collectLabels(v, &parts)
        return parts.joined(separator: ", ")
    }

    /// After each batch: relations resolved again (a view named may have
    /// come, or its text changed).
    func a11yBatchApplied() {
        let s = AsukaA11yState.shared
        for id in s.related {
            guard let n = nodes[id] else {
                s.related.remove(id)
                continue
            }
            applyRelations(n)
        }
    }

    // MARK: Modal

    /// The view screen readers stay in, in w: the last presented Modal
    /// overlay's, else the newest A11yModal view that shows.
    func a11yModalView(_ w: NSWindow?) -> NSView? {
        guard let w else { return nil }
        if let o = modalOverlays(w).last { return o.view }
        var best: AsukaNode?
        for id in AsukaA11yState.shared.modal {
            guard let n = nodes[id], let v = n.view, v.window === w, !v.isHiddenOrHasHiddenAncestor else { continue }
            if best == nil || n.id > best!.id { best = n }
        }
        return best?.view
    }

    /// An AsukaView's accessibilityChildren while a modal view shows in its
    /// window: only those on the way to it.
    func a11yChildren(_ v: NSView, _ kids: [Any]?) -> [Any]? {
        guard let kids, let m = a11yModalView(v.window), m !== v, m.isDescendant(of: v) else { return kids }
        return kids.filter { k in
            guard let kv = a11yViewOf(k) else { return false }
            return kv === m || m.isDescendant(of: kv) || kv.isDescendant(of: m)
        }
    }

    /// The view an accessibility element is or is of (a label's cell: its
    /// field).
    func a11yViewOf(_ k: Any) -> NSView? {
        var x: Any? = k
        for _ in 0..<8 {
            if let v = x as? NSView { return v }
            if let c = x as? NSCell { return c.controlView }
            guard let e = x as? NSAccessibilityElementProtocol else { return nil }
            x = e.accessibilityParent()
        }
        return nil
    }

    /// The modal view in w changed: VoiceOver reads the window again.
    func a11yModalChanged(_ w: NSWindow?) {
        guard let c = w?.contentView else { return }
        NSAccessibility.post(element: c, notification: .layoutChanged)
    }

    func a11yDestroyed(_ n: AsukaNode) {
        let s = AsukaA11yState.shared
        s.related.remove(n.id)
        if s.modal.remove(n.id) != nil { a11yModalChanged(n.view?.window) }
    }

    /// What the view's accessibility object says, as VoiceOver reads it.
    func a11yJSON(_ n: AsukaNode, _ d: inout [String: Any]) {
        guard n.a11y != nil, let v = n.view else { return }
        var j: [String: Any] = [:]
        if let r = v.accessibilityRole() { j["role"] = r.rawValue }
        if let s = v.accessibilitySubrole() { j["subrole"] = s.rawValue }
        if let x = v.accessibilityValue() { j["value"] = "\(x)" }
        if let x = v.accessibilityValueDescription() { j["valueDescription"] = x }
        if let x = v.accessibilityMinValue() { j["min"] = "\(x)" }
        if let x = v.accessibilityMaxValue() { j["max"] = "\(x)" }
        j["expanded"] = v.isAccessibilityExpanded()
        j["selected"] = v.isAccessibilitySelected()
        j["modal"] = v.isAccessibilityModal()
        if let t = v.accessibilityTitleUIElement() as? NSView, let tn = nodeAt(t) { j["titleElement"] = tn.id }
        if let l = v.accessibilityLinkedUIElements() { j["linked"] = l.compactMap { ($0 as? NSView).flatMap { nodeAt($0)?.id } } }
        if let h = v.accessibilityHelp() { j["help"] = h }
        j["element"] = v.isAccessibilityElement()
        d["ax"] = j
    }
}
