// Navigation as the watch has it: a Stack is SwiftUI's NavigationStack (its
// back button, the edge swipe back), Tabs a vertical page TabView (the
// Digital Crown pages), a Sheet SwiftUI's sheet, an Alert its alert or
// confirmation dialog. Screens and sheets are environment providers, as on
// the other hosts: each reports its own size (below the navigation bar),
// and the core lays its content out at it.

import Foundation
import SwiftUI
#if canImport(AsukaOps)
import AsukaOps
#endif

/// The app's scene: the root view at the whole screen, the sheets and the
/// alert over it, asuka dev's error over everything.
public struct AsukaRootView: View {
    public init() {}

    public var body: some View {
        let rt = AsukaRuntime.shared
        let scene = rt.scene
        ZStack(alignment: .topLeading) {
            // The safe area (the time, the screen's rounded corners): a
            // view that ignores it has none, so it's read from one inside
            // it, which SwiftUI gives the insets around it.
            GeometryReader { s in
                Color.clear
                    .onAppear { rt.reportRoot(safe: s.safeAreaInsets) }
                    .onChange(of: s.safeAreaInsets) { rt.reportRoot(safe: s.safeAreaInsets) }
            }
            GeometryReader { g in
                ZStack(alignment: .topLeading) {
                    if let root = scene.root {
                        AsukaRootNode(node: root)
                    }
                }
                .frame(width: g.size.width, height: g.size.height, alignment: .topLeading)
                .onAppear { rt.reportRoot(size: g.size) }
                .onChange(of: g.size) { rt.reportRoot(size: g.size) }
                .onChange(of: scene.root?.id) { rt.reportRoot() }
            }
            .ignoresSafeArea()
        }
        .modifier(AsukaSheets(level: 0))
        .modifier(AsukaAlert())
        .overlay { if let e = scene.devError { AsukaDevErrorView(error: e) } }
        .tint(scene.accent)
        .onAppear { rt.start() }
    }
}

/// The root: a navigator fills the screen as SwiftUI's do (its bars in the
/// safe area); a view is drawn at the core's frames.
struct AsukaRootNode: View {
    let node: AsukaNode

    var body: some View {
        let _ = node.rev
        switch node.kind {
        case .stack:
            AsukaStackView(node: node)
        case .tabs:
            AsukaTabsView(node: node)
        default:
            AsukaNodeView(node: node)
        }
    }
}

/// A Stack: its first screen at the root, the others pushed, as SwiftUI's
/// path. The user's pop (the back button, the edge swipe) sets the path
/// shorter: reported as Popped, the core destroys the screens.
struct AsukaStackView: View {
    let node: AsukaNode

    var body: some View {
        let _ = node.rev
        let rt = AsukaRuntime.shared
        let screens = node.shownChildren
        let path = Binding<[UInt32]>(
            get: { Array(screens.dropFirst().map(\.id)) },
            set: { new in rt.userPopped(node, to: new.count + 1) }
        )
        NavigationStack(path: path) {
            Group {
                if let first = screens.first { AsukaScreenView(node: first) }
            }
            .navigationDestination(for: UInt32.self) { id in
                if let s = rt.nodes[id] { AsukaScreenView(node: s) }
            }
        }
    }
}

/// A Screen: its title in the navigation bar, its bar buttons in the
/// toolbar, its content laid out by the core at the size it reports.
struct AsukaScreenView: View {
    let node: AsukaNode

    var body: some View {
        let _ = node.rev
        let rt = AsukaRuntime.shared
        let n = node
        AsukaProvider(node: n)
            .navigationTitle(n.title)
            .toolbar {
                if let first = n.actions.first(where: { !$0.submenu }) {
                    ToolbarItem(placement: .asukaTrailing) {
                        Button {
                            rt.emit(n.id, .action) { AsukaEvents.action(n.id, first.index) }
                        } label: {
                            if first.icon.isEmpty { Text(first.title) } else { Label(first.title, systemImage: first.icon) }
                        }
                        .disabled(first.disabled)
                    }
                }
            }
    }
}

/// A screen's or a sheet's content: what the core lays out at the size
/// the provider reports (Environment).
struct AsukaProvider: View {
    let node: AsukaNode

    var body: some View {
        let _ = node.rev
        let rt = AsukaRuntime.shared
        let n = node
        let kids = n.shownChildren
        if kids.count == 1, kids[0].kind == .stack || kids[0].kind == .tabs {
            // A navigator in a sheet or a tab: it fills it.
            AsukaRootNode(node: kids[0])
        } else if kids.count == 1, kids[0].kind == .screen, n.kind == .sheet {
            // A sheet's screen: its title and bar buttons in a bar of its
            // own, as a sheet's screen has on a phone.
            NavigationStack { AsukaScreenView(node: kids[0]) }
        } else {
            // The area SwiftUI leaves its content (below the bar, inside
            // the screen's rounded corners): a screen's whole, with no
            // safe area of its own, as a phone's screen under its bar. A
            // pushed screen is laid out once before it's placed, at the
            // whole stack's size: that isn't its size, and isn't reported
            // (the core has it at the size of the screen under it).
            GeometryReader { g in
                let placed = asukaPlaced(g.frame(in: .global))
                AsukaChildren(node: n)
                    .frame(width: g.size.width, height: g.size.height, alignment: .topLeading)
                    .onAppear { if placed { rt.report(n, size: g.size, safe: EdgeInsets()) } }
                    .onChange(of: g.size) { if placed { rt.report(n, size: g.size, safe: EdgeInsets()) } }
                    .onChange(of: placed) { if placed { rt.report(n, size: g.size, safe: EdgeInsets()) } }
            }
        }
    }
}

/// Whether SwiftUI has placed a view whose frame on the screen is f: it
/// lays a navigation destination out before it places it, its frame then
/// centered on the screen's top left corner, where no screen shown is.
func asukaPlaced(_ f: CGRect) -> Bool {
    !(f.midX == 0 && f.midY == 0)
}

/// Tabs: a vertical page TabView, as watchOS 10's apps page (the Digital
/// Crown turns the pages), inside one NavigationStack, as watchOS wants
/// it (a stack inside a page doesn't push): each page is its tab's first
/// screen, and the stack's path is the chosen tab's stack's other
/// screens. A user's pop is that stack's Popped.
struct AsukaTabsView: View {
    let node: AsukaNode

    var body: some View {
        let _ = node.rev
        let rt = AsukaRuntime.shared
        let n = node
        let tabs = n.shownChildren
        let _ = tabs.map(\.rev) // a tab's stack pushing or popping
        let sel = tabs.isEmpty ? 0 : Swift.min(Swift.max(n.selected, 0), tabs.count - 1)
        let stack = tabs.isEmpty ? nil : tabStack(tabs[sel])
        let _ = stack?.rev
        let screens = stack?.shownChildren ?? []
        let path = Binding<[UInt32]>(
            get: { Array(screens.dropFirst().map(\.id)) },
            set: { new in if let stack { rt.userPopped(stack, to: new.count + 1) } }
        )
        NavigationStack(path: path) {
            TabView(selection: Binding(get: { sel }, set: { i in
                n.selected = i
                n.rev &+= 1
                rt.emit(n.id, .selected) { AsukaEvents.selected(n.id, Int64(i)) }
            })) {
                ForEach(Array(tabs.enumerated()), id: \.element.id) { i, t in
                    Group {
                        if let st = tabStack(t) {
                            if let first = st.shownChildren.first { AsukaScreenView(node: first) }
                        } else if t.kind == .screen {
                            AsukaScreenView(node: t)
                        } else {
                            AsukaProvider(node: t)
                        }
                    }
                    .tag(i)
                }
            }
            .asukaPages()
            .navigationDestination(for: UInt32.self) { id in
                if let s = rt.nodes[id] { AsukaScreenView(node: s) }
            }
        }
    }
}

/// A tab's stack: the tab itself, or the one its page holds (a routed
/// tab is a screen with a stack in it).
@MainActor func tabStack(_ t: AsukaNode) -> AsukaNode? {
    if t.kind == .stack { return t }
    let kids = t.shownChildren
    if t.kind == .screen, kids.count == 1, kids[0].kind == .stack { return kids[0] }
    return nil
}

/// The sheets, each over the one below it: SwiftUI presents one sheet per
/// view, so each level's sheet holds the next.
struct AsukaSheets: ViewModifier {
    let level: Int

    func body(content: Content) -> some View {
        let rt = AsukaRuntime.shared
        let scene = rt.scene
        let sheet = level < scene.sheets.count ? scene.sheets[level] : nil
        content.sheet(isPresented: Binding(get: { sheet != nil }, set: { shown in
            if !shown, let s = sheet { rt.userDismissed(s) }
        })) {
            if let s = sheet {
                AsukaProvider(node: s)
                    .modifier(AsukaSheets(level: level + 1))
                    .interactiveDismissDisabled(s.noDismiss)
            }
        }
    }
}

/// The alert showing (Present): an alert, or an action sheet as a
/// confirmation dialog. A button reports Action with its index; dismissing
/// it otherwise, Dismissed.
struct AsukaAlert: ViewModifier {
    func body(content: Content) -> some View {
        let rt = AsukaRuntime.shared
        let a = rt.scene.alert
        let shown = Binding(get: { a != nil }, set: { v in
            if !v, let a, rt.scene.alert === a { rt.alertAnswered(a, nil) }
        })
        let title = a?.title ?? ""
        let buttons = a?.actions ?? []
        if let a, AsukaAlertStyle(rawValue: a.alertStyle) == .actionSheet {
            content.confirmationDialog(title, isPresented: shown, titleVisibility: title.isEmpty ? .hidden : .visible) {
                alertButtons(a, buttons)
            } message: {
                if !a.message.isEmpty { Text(a.message) }
            }
        } else {
            content.alert(title, isPresented: shown) {
                if let a { alertButtons(a, buttons) }
            } message: {
                if let a, !a.message.isEmpty { Text(a.message) }
            }
        }
    }

    @ViewBuilder private func alertButtons(_ a: AsukaNode, _ buttons: [AsukaActionItem]) -> some View {
        let rt = AsukaRuntime.shared
        ForEach(buttons, id: \.index) { b in
            Button(b.title, role: role(b.role)) { rt.alertAnswered(a, b.index) }
                .disabled(b.disabled)
        }
    }

    private func role(_ r: Int32) -> ButtonRole? {
        switch AsukaAlertButtonRole(rawValue: r) {
        case .cancel: return .cancel
        case .destructive: return .destructive
        default: return nil
        }
    }
}

/// asuka dev's error over the app: its title, message, source and
/// details, scrolled with the Digital Crown, and Dismiss and Reload.
struct AsukaDevErrorView: View {
    let error: AsukaDevError

    var body: some View {
        let rt = AsukaRuntime.shared
        ScrollView {
            VStack(alignment: .leading, spacing: 8) {
                Text(error.title).font(.headline).foregroundStyle(.red)
                Text(error.message).font(.footnote)
                if !error.source.isEmpty { Text(error.source).font(.system(.caption2, design: .monospaced)) }
                if !error.details.isEmpty { Text(error.details).font(.system(.caption2, design: .monospaced)).foregroundStyle(.secondary) }
                Button("Dismiss") { rt.send(AsukaEvents.devErrorAction(rt.rootID, Int64(AsukaDevErrorAction.dismiss.rawValue))) }
                if error.reload {
                    Button("Reload") { rt.send(AsukaEvents.devErrorAction(rt.rootID, Int64(AsukaDevErrorAction.reload.rawValue))) }
                        .buttonStyle(.borderedProminent)
                }
            }
            .padding(.horizontal, 4)
        }
        .background(Color.black)
    }
}
