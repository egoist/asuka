#if canImport(UIKit)
import UIKit
#if canImport(AsukaOps)
import AsukaOps
#endif

// Screen readers' roles and states (RFC 0010): A11yRole's cases as
// UIAccessibility traits and container types, A11yChecked, A11yExpanded,
// A11ySelected, A11yValue and A11yRange as traits, the value VoiceOver
// reads and the expanded status, A11yModal as accessibilityViewIsModal,
// and the relations UIKit has none of as what they name: a labelledBy's
// text is the label, a describedBy's the hint.

/// A view's accessibility props, as set.
struct AsukaA11yState {
    var role: AsukaRole?
    var checked = AsukaA11yChecked.none
    var expanded = AsukaA11yExpanded.none
    var selected = false
    var value = ""
    var range: (min: Double, max: Double, now: Double)?
    var modal = false
    var controls: UInt32 = 0
    var labelledBy: UInt32 = 0
    var describedBy: UInt32 = 0
    /// A11yLabel and A11yHint as set, which a relation's text replaces.
    var label: String?
    var hint: String?
    /// The traits the role and states gave it, and whether they gave it
    /// its value.
    var traits: UIAccessibilityTraits = []
    var setsValue = false

    /// A role whose view groups others for VoiceOver rather than being
    /// read as one.
    var isContainer: Bool {
        switch role {
        case .radioGroup, .group, .toolbar, .dialog, .alertDialog, .tabPanel, .status, .menu, .menuBar, .listbox, .tabList:
            return true
        default:
            return false
        }
    }

    /// A role checked or not.
    var checkable: Bool {
        switch role {
        case .checkbox, .switch, .menuItemCheckbox, .radio, .menuItemRadio: return true
        default: return false
        }
    }
}

@MainActor extension AsukaRuntime {
    /// Sets one of RFC 0010's accessibility props; false: not one.
    func setA11y(_ n: AsukaNode, _ prop: UInt16, _ v: AsukaValue) -> Bool {
        switch prop {
        case AsukaProp.a11yRole:
            n.a11y.role = AsukaRole(rawValue: v.enumValue)
            if let r = n.view as? AsukaRichTextView { // its own elements: the text, its links
                r.heading = n.a11y.role == .header
                return true
            }
            if n.a11y.role != nil && n.a11y.role != AsukaRole.none && !n.a11y.isContainer && n.a11y.role != .separator {
                n.view.isAccessibilityElement = true
            }
        case AsukaProp.a11yChecked:
            n.a11y.checked = AsukaA11yChecked(rawValue: v.enumValue) ?? .none
        case AsukaProp.a11yExpanded:
            n.a11y.expanded = AsukaA11yExpanded(rawValue: v.enumValue) ?? .none
        case AsukaProp.a11ySelected:
            n.a11y.selected = v.bool
        case AsukaProp.a11yValue:
            n.a11y.value = v.string
        case AsukaProp.a11yRange:
            if case .bytes(let b) = v, b.count >= 24 {
                var r = AsukaReader(b)
                n.a11y.range = (r.f64(), r.f64(), r.f64())
            } else {
                n.a11y.range = nil
            }
        case AsukaProp.a11yModal:
            setA11yModal(n, v.bool)
            return true
        case AsukaProp.a11yControls:
            n.a11y.controls = UInt32(truncatingIfNeeded: v.int) // UIKit has no such relation
            return true
        case AsukaProp.a11yLabelledBy, AsukaProp.a11yDescribedBy:
            let id = UInt32(truncatingIfNeeded: v.int)
            if prop == AsukaProp.a11yLabelledBy { n.a11y.labelledBy = id } else { n.a11y.describedBy = id }
            if n.a11y.labelledBy != 0 || n.a11y.describedBy != 0 { related.insert(n.id) } else { related.remove(n.id) }
            relate(n)
            return true
        case AsukaProp.a11yLabel:
            n.a11y.label = v.string
            if !n.a11y.isContainer { n.view.isAccessibilityElement = true }
            if n.a11y.labelledBy == 0 { n.view.accessibilityLabel = v.string }
            return true
        case AsukaProp.a11yHint:
            n.a11y.hint = v.string
            if !n.a11y.isContainer { n.view.isAccessibilityElement = true }
            if n.a11y.describedBy == 0 { n.view.accessibilityHint = v.string }
            return true
        default:
            return false
        }
        applyA11y(n)
        return true
    }

    /// The traits, the value, the expanded status and the container type
    /// of n's role and states.
    func applyA11y(_ n: AsukaNode) {
        let a = n.a11y
        let view = n.view
        if view is AsukaRichTextView { return }
        var t: UIAccessibilityTraits = []
        var container = UIAccessibilityContainerType.none
        let on = a.checked == .checked
        switch a.role {
        case .button, .menuItem, .option: t = .button
        case .header: t = .header
        case .image: t = .image
        case .link: t = .link
        case .text, .tooltip: t = .staticText
        case .search: t = .searchField
        case .checkbox, .switch, .menuItemCheckbox:
            t = .button
            if #available(iOS 17.0, *) { t.insert(.toggleButton) } else if on { t.insert(.selected) }
        case .radio, .menuItemRadio, .tab:
            t = .button
            if on { t.insert(.selected) }
        case .comboBox:
            if !(view is UITextField || view is UITextView) { t = .button }
        case .slider, .spinButton: t = .adjustable
        case .progressBar, .meter: t = .updatesFrequently
        case .tabList: t = .tabBar
        case .menu, .menuBar, .listbox: container = .list
        case .radioGroup, .group, .toolbar, .dialog, .alertDialog, .tabPanel, .status: container = .semanticGroup
        default: break
        }
        if a.selected { t.insert(.selected) }
        // The traits the props gave before go; the press's button stays.
        var traits = view.accessibilityTraits.subtracting(n.a11y.traits).union(t)
        if (view as? AsukaContainerView)?.pressable == true { traits.insert(.button) }
        if (view as? AsukaContainerView)?.adjustable == true { traits.insert(.adjustable) }
        view.accessibilityTraits = traits
        n.a11y.traits = t
        if a.isContainer || a.role == .separator {
            // Read through: its children are the elements, its label the group's.
            if !((view as? AsukaContainerView)?.pressable ?? false) { view.isAccessibilityElement = false }
        }
        view.accessibilityContainerType = container
        let value = a11yValue(a)
        if let value {
            view.accessibilityValue = value
            n.a11y.setsValue = true
        } else if n.a11y.setsValue {
            view.accessibilityValue = nil
            n.a11y.setsValue = false
        }
        if #available(iOS 18.0, *) {
            switch a.expanded {
            case .expanded: view.accessibilityExpandedStatus = .expanded
            case .collapsed: view.accessibilityExpandedStatus = .collapsed
            default: view.accessibilityExpandedStatus = .unsupported
            }
        }
    }

    /// What VoiceOver reads as the value: A11yValue, else the range's
    /// (a progress bar's and a meter's as a percentage), else a toggle's
    /// state as UIKit's switches give it.
    func a11yValue(_ a: AsukaA11yState) -> String? {
        if !a.value.isEmpty { return a.value }
        if let r = a.range {
            if a.role == .progressBar || a.role == .meter {
                let span = r.max - r.min
                let f = NumberFormatter()
                f.numberStyle = .percent
                f.maximumFractionDigits = 0
                return f.string(from: NSNumber(value: span > 0 ? (r.now - r.min) / span : 0))
            }
            let f = NumberFormatter()
            f.numberStyle = .decimal
            f.maximumFractionDigits = 2
            return f.string(from: NSNumber(value: r.now))
        }
        switch a.role {
        case .checkbox, .switch, .menuItemCheckbox:
            if #available(iOS 17.0, *) {
                // A toggle button's value, as a UISwitch's: VoiceOver reads on or off.
                switch a.checked {
                case .checked: return "1"
                case .mixed: return "mixed"
                default: return "0"
                }
            }
            return a.checked == .mixed ? "mixed" : nil
        default:
            return nil
        }
    }

    /// A11yModal: VoiceOver stays in the view; in an Overlay, the overlay
    /// too, so the rest of its root is hidden from it while it shows.
    func setA11yModal(_ n: AsukaNode, _ on: Bool) {
        n.a11y.modal = on
        n.view.accessibilityViewIsModal = on
        var p = n.parent
        while let x = p {
            if let o = x.overlay {
                if on { o.modalViews.insert(n.id) } else { o.modalViews.remove(n.id) }
                o.updateModal()
                return
            }
            p = x.parent
        }
    }

    /// The text a view reads as: its label's, its title's, its
    /// accessibility label, or its children's, in order.
    func readText(_ v: UIView) -> String? {
        switch v {
        case let l as UILabel: return l.text
        case let b as UIButton: return b.configuration?.title ?? b.accessibilityLabel
        case let f as UITextField: return f.text
        default: break
        }
        if let l = v.accessibilityLabel, !l.isEmpty { return l }
        var parts: [String] = []
        for s in v.subviews where !s.isHidden {
            if let t = readText(s), !t.isEmpty { parts.append(t) }
        }
        return parts.isEmpty ? nil : parts.joined(separator: ", ")
    }

    /// A labelledBy's text as n's label, a describedBy's as its hint (the
    /// views' own, A11yLabel and A11yHint, without one).
    func relate(_ n: AsukaNode) {
        let a = n.a11y
        let label = a.labelledBy != 0 ? nodes[a.labelledBy].flatMap { readText($0.view) } : nil
        let hint = a.describedBy != 0 ? nodes[a.describedBy].flatMap { readText($0.view) } : nil
        let wantLabel = label ?? a.label
        let wantHint = hint ?? a.hint
        if n.view.accessibilityLabel != wantLabel, a.labelledBy != 0 || label != nil || a.label != nil {
            n.view.accessibilityLabel = wantLabel
        }
        if n.view.accessibilityHint != wantHint, a.describedBy != 0 || hint != nil || a.hint != nil {
            n.view.accessibilityHint = wantHint
        }
        if (label != nil || hint != nil), !a.isContainer { n.view.isAccessibilityElement = true }
    }

    /// The relations again after a batch: a view named may have come, or
    /// its text changed.
    func refreshRelations() {
        for id in related {
            if let n = nodes[id] { relate(n) }
        }
    }
}
#endif
