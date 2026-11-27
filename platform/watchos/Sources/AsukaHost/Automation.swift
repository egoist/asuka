#if DEBUG
import Foundation
import notify
#if canImport(AsukaOps)
import AsukaOps
#endif

/// Drives a debug build (asuka dev's player) from outside, as the iOS
/// host's automation does, with the same commands: write them to <data
/// container>/tmp/asuka-commands.txt, then post dev.asuka.automation:
///
///	xcrun simctl spawn <watch> notifyutil -p dev.asuka.automation
///
/// Commands, one per line: `tap <text>` presses the Button, View or
/// Toggle whose title, text or accessibility label is <text>, as the
/// user's tap does; `switch <n>` flips the n-th Toggle (from 0, in
/// creation order); `scroll <rows>` scrolls the first List down to that
/// row; `alert <title>` presses the alert's button with that title (none:
/// dismisses it); `back` pops the shown stack's top screen, as its back button
/// does; `dump` writes what the screen shows (each text, button title or
/// accessibility label, a line each, in creation order) to
/// tmp/asuka-dump.txt.
@MainActor enum AsukaAutomation {
    static var token: Int32 = 0

    static func start() {
        notify_register_dispatch("dev.asuka.automation", &token, .main) { _ in
            MainActor.assumeIsolated { run() }
        }
    }

    static func run() {
        let tmp = FileManager.default.temporaryDirectory
        guard let text = try? String(contentsOf: tmp.appendingPathComponent("asuka-commands.txt"), encoding: .utf8) else { return }
        let rt = AsukaRuntime.shared
        for line in text.split(separator: "\n") {
            let parts = line.split(separator: " ", maxSplits: 1).map(String.init)
            let arg = parts.count > 1 ? parts[1] : ""
            switch parts.first ?? "" {
            case "tap":
                guard let n = find(arg) else { continue }
                if n.kind == .toggle {
                    n.on.toggle()
                    let on = n.on
                    rt.emit(n.id, .toggled) { AsukaEvents.toggled(n.id, on) }
                    n.rev &+= 1
                } else {
                    rt.emit(n.id, .press) { AsukaEvents.press(n.id) }
                }
            case "switch":
                let toggles = shown().filter { $0.kind == .toggle }
                guard let i = Int(arg), i < toggles.count else { continue }
                let n = toggles[i]
                n.on.toggle()
                let on = n.on
                rt.emit(n.id, .toggled) { AsukaEvents.toggled(n.id, on) }
                n.rev &+= 1
            case "scroll":
                guard let l = shown().first(where: { $0.kind == .list }), let i = Int(arg) else { continue }
                l.list?.scroll(to: i, animated: true)
                l.rev &+= 1
            case "alert":
                guard let a = rt.scene.alert else { continue }
                let b = a.actions.first { $0.title == arg }
                rt.alertAnswered(a, b?.index)
            case "back":
                guard let s = shown().last(where: { $0.kind == .stack && $0.shownChildren.count > 1 }) else { continue }
                rt.userPopped(s, to: s.shownChildren.count - 1)
                s.rev &+= 1
            case "tree":
                // Each node shown: its kind, ID, frame, and whether it's
                // hidden, faded or not placed yet.
                let lines = shown().map { n -> String in
                    var depth = 0
                    var p = n.parent
                    while let v = p { depth += 1; p = v.parent }
                    let f = n.frame
                    var s = String(repeating: "  ", count: depth) + "\(n.kind?.name ?? "Cell") \(n.id) \(Int(f.minX)),\(Int(f.minY)) \(Int(f.width))x\(Int(f.height))"
                    if !n.placed { s += " unplaced" }
                    if n.hidden { s += " hidden" }
                    if n.opacity < 1 { s += " opacity \(n.opacity)" }
                    if !n.text.isEmpty { s += " \"\(n.text)\"" }
                    if !n.title.isEmpty { s += " [\(n.title)]" }
                    return s
                }
                try? lines.joined(separator: "\n").write(to: tmp.appendingPathComponent("asuka-tree.txt"), atomically: true, encoding: .utf8)
            case "dump":
                let lines = shown().compactMap { n -> String? in
                    switch n.kind {
                    case .text: return n.text
                    case .button: return n.title.isEmpty ? nil : n.title
                    case .screen: return n.title.isEmpty ? nil : "[" + n.title + "]"
                    default: return n.a11yLabel.isEmpty ? nil : n.a11yLabel
                    }
                }
                try? lines.joined(separator: "\n").write(to: tmp.appendingPathComponent("asuka-dump.txt"), atomically: true, encoding: .utf8)
            default:
                break
            }
        }
    }

    /// The nodes shown: the root's and the sheets' trees, depth first,
    /// leaving out screens under the top of their stack and hidden views.
    static func shown() -> [AsukaNode] {
        var out: [AsukaNode] = []
        func walk(_ n: AsukaNode) {
            if n.hidden || n.dying { return }
            out.append(n)
            if n.kind == .stack {
                if let top = n.shownChildren.last { walk(top) }
                return
            }
            if n.kind == .tabs {
                let tabs = n.shownChildren
                if n.selected >= 0, n.selected < tabs.count { walk(tabs[n.selected]) }
                return
            }
            for c in n.shownChildren { walk(c) }
        }
        let rt = AsukaRuntime.shared
        if let r = rt.scene.root { walk(r) }
        for s in rt.scene.sheets { walk(s) }
        // A list's bound rows.
        for n in rt.nodes.values where n.cellOf != nil && n.cellIndex >= 0 { walk(n) }
        return out
    }

    static func find(_ text: String) -> AsukaNode? {
        for n in shown() {
            switch n.kind {
            case .button where n.title == text: return n
            case .toggle where n.a11yLabel == text: return n
            default:
                if n.a11yLabel == text && n.listens(.press) { return n }
            }
        }
        // A pressable view whose text is it (a row's).
        for n in shown() where n.kind == .text && n.text == text {
            var p = n.parent
            while let v = p {
                if v.listens(.press) { return v }
                p = v.parent
            }
        }
        return nil
    }
}
#endif
