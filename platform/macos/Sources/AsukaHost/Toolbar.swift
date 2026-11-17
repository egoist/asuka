// A window's title and toolbar: its innermost shown stack's. The toolbar
// has a back button while the stack has a screen to go back to, then the
// top screen's bar buttons, its Toolbar's items and its Primary last,
// prominent, and its search field; the window's title is the screen's (or
// the selected tab's). Back (the button, ⌘[ and the View menu's item)
// pops that stack's top screen, or asks the core (OnBack).
import AppKit
#if canImport(AsukaOps)
import AsukaOps
#endif

private let backID = NSToolbarItem.Identifier("asuka.back")
private let searchID = NSToolbarItem.Identifier("asuka.search")
private let actionPrefix = "asuka.action."

@MainActor extension AsukaRuntime {
    /// The innermost stack on screen under root: through each stack's top
    /// screen and each tabs' selected page.
    func activeStack(_ root: AsukaNode?) -> AsukaNode? {
        var found: AsukaNode?
        func walk(_ n: AsukaNode) {
            switch n.kind {
            case .stack:
                found = n
                if let t = topScreen(n) { walk(t) }
                return
            case .tabs:
                if n.selected >= 0 && n.selected < n.children.count { walk(n.children[n.selected]) }
                return
            case .sheet, .alert, .window, .popover:
                if n !== root { return }
            default:
                break
            }
            for c in n.children { walk(c) }
        }
        if let root { walk(root) }
        return found
    }

    /// The selected tab's screen under root, the innermost.
    func selectedTab(_ root: AsukaNode?) -> AsukaNode? {
        var found: AsukaNode?
        func walk(_ n: AsukaNode) {
            if n.kind == .tabs && n.selected >= 0 && n.selected < n.children.count {
                found = n.children[n.selected]
                walk(found!)
                return
            }
            if n !== root && (n.kind == .sheet || n.kind == .window) { return }
            for c in n.children { walk(c) }
        }
        if let root { walk(root) }
        return found
    }

    /// Makes every window's title and toolbar again, once the call into
    /// the core in progress has returned.
    func updateWindowChrome() {
        guard main != nil else { return }
        if depth > 0 {
            if !navTarget.chromeQueued {
                navTarget.chromeQueued = true
                afterCore { [weak self] in
                    guard let self else { return }
                    self.navTarget.chromeQueued = false
                    for w in self.allWins { self.updateChrome(w) }
                }
            }
            return
        }
        for w in allWins { updateChrome(w) }
    }

    func updateChrome(_ w: AsukaWin) {
        let c = w.chrome
        let stack = activeStack(w.root)
        let screen = stack.flatMap { topScreen($0) }
        var title = w.base
        if let s = screen, !s.title.isEmpty {
            title = s.title
        } else if let t = selectedTab(w.root), !t.title.isEmpty {
            title = t.title
        }
        if w.window.title != title { w.window.title = title }
        c.stack = stack
        c.screen = screen
        guard let stack else {
            if c.toolbar != nil {
                w.window.toolbar = nil
                c.toolbar = nil
                releaseBarMenus(c)
                c.searchField = nil
                if w.isMain { environmentChanged() } else { windowContentResized(w) }
            }
            return
        }
        var ids: [NSToolbarItem.Identifier] = []
        c.back = liveScreens(stack) > 1
        if c.back { ids.append(backID) }
        ids.append(.flexibleSpace)
        c.actions = []
        c.search = ""
        if let s = screen, let nav = s.nav {
            // SwiftUI's bottomBar is iOS's: a Mac's toolbar has the screen's
            // Toolbar after its bar buttons, and its Primary last, prominent.
            c.actions = s.actions + nav.toolbar + nav.primary
            c.search = nav.search
        }
        if !c.search.isEmpty { ids.append(searchID) }
        for i in c.actions.indices { ids.append(NSToolbarItem.Identifier(actionPrefix + String(i))) }
        if toolbarReversed { ids.reverse() }
        if c.toolbar == nil {
            // Its own identifier: toolbars that share one keep the same
            // items, which the windows' don't.
            let t = NSToolbar(identifier: "asuka.\(w.window.windowNumber).\(ObjectIdentifier(w).hashValue)")
            t.delegate = navTarget
            t.displayMode = .iconOnly
            t.allowsUserCustomization = false
            c.toolbar = t
            if #available(macOS 11.0, *) { w.window.toolbarStyle = .unified } // the title beside the back button
            c.ids = []
            w.window.toolbar = t
            if w.isMain { environmentChanged() } else { windowContentResized(w) }
        }
        // The items again: they're the screen's.
        guard let t = c.toolbar else { return }
        while !t.items.isEmpty { t.removeItem(at: 0) }
        releaseBarMenus(c)
        c.searchField = nil
        c.ids = ids
        for (i, id) in ids.enumerated() { t.insertItem(withItemIdentifier: id, at: i) }
    }

    /// The window a toolbar is in.
    func toolbarWin(_ t: NSToolbar?) -> AsukaWin? {
        guard let t else { return nil }
        return allWins.first { $0.chrome.toolbar === t }
    }

    /// Makes the item for an identifier of a window's chrome.
    func toolbarItem(_ w: AsukaWin, _ ident: NSToolbarItem.Identifier) -> NSToolbarItem? {
        let c = w.chrome
        if ident == backID {
            let it = NSToolbarItem(itemIdentifier: ident)
            let back = asukaWord("back")
            it.image = symbolImage(backSymbol)
            it.label = back
            it.toolTip = back
            it.target = navTarget
            it.action = #selector(AsukaNavTarget.back(_:))
            // A navigational item goes on the app's leading side, wherever
            // it's put: not where the host orders the toolbar itself.
            if #available(macOS 11.0, *), !toolbarReversed { it.isNavigational = true }
            it.isBordered = true
            return it
        }
        if ident == searchID {
            let f: NSSearchField
            let it: NSToolbarItem
            if #available(macOS 11.0, *) {
                let s = NSSearchToolbarItem(itemIdentifier: ident)
                f = s.searchField
                it = s
            } else {
                it = NSToolbarItem(itemIdentifier: ident)
                f = NSSearchField(frame: CGRect(x: 0, y: 0, width: 200, height: 22))
                it.view = f
            }
            c.searchField = f
            (f as NSTextField).delegate = self
            f.userInterfaceLayoutDirection = rtl ? .rightToLeft : .leftToRight
            if rtl { f.alignment = .right }
            f.placeholderString = c.search
            if let s = c.screen { f.stringValue = s.nav?.searchText ?? "" }
            return it
        }
        guard ident.rawValue.hasPrefix(actionPrefix), let i = Int(ident.rawValue.dropFirst(actionPrefix.count)),
              i >= 0, i < c.actions.count else { return nil }
        let a = c.actions[i]
        let it: NSToolbarItem
        if a.submenu && !a.items.isEmpty {
            // A bar button with children opens them as a menu.
            let mi = NSMenuToolbarItem(itemIdentifier: ident)
            let m = makeMenuOf(a.items)
            mi.menu = m
            mi.showsIndicator = true
            c.menus.append(m)
            if let s = c.screen { byMenu[ObjectIdentifier(m)] = s }
            it = mi
        } else {
            it = NSToolbarItem(itemIdentifier: ident)
            it.target = navTarget
            it.action = #selector(AsukaNavTarget.barItem(_:))
        }
        it.label = a.title
        it.toolTip = a.title
        if let img = barSymbol(a.icon) { it.image = img } else { it.title = a.title }
        it.isBordered = true
        it.tag = Int(a.index)
        if a.index >= AsukaActions.primaryBase, #available(macOS 26.0, *) {
            // The screen's main action: tinted with the accent.
            it.style = .prominent
        }
        if a.disabled {
            // Dimmed, and kept so: validation would enable it again.
            it.autovalidates = false
            it.isEnabled = false
        }
        return it
    }

    /// Lets go of the menus of a window's bar buttons.
    func releaseBarMenus(_ c: AsukaChrome) {
        for m in c.menus { byMenu.removeValue(forKey: ObjectIdentifier(m)) }
        c.menus = []
    }

    /// A screen's bar button.
    func barItem(_ sender: Any?) {
        guard let it = sender as? NSToolbarItem, let w = toolbarWin(it.toolbar), let s = w.chrome.screen else { return }
        let tag = Int64(it.tag)
        emit(s, .action) { AsukaEvents.action($0, tag) }
    }

    /// The window an action came from: a toolbar item's, else the one the
    /// user works in (a menu item, a shortcut).
    func senderWin(_ sender: Any?) -> AsukaWin {
        if let it = sender as? NSToolbarItem, let w = toolbarWin(it.toolbar) { return w }
        return keyWin
    }

    /// Pops the innermost shown stack's top screen in w, as its back
    /// button.
    func back(_ w: AsukaWin) {
        if overlayBack(w) { return } // an overlay over w takes it first (Overlay.swift)
        guard let stack = activeStack(w.root), let top = topScreen(stack), liveScreens(stack) >= 2 else { return }
        if top.interceptBack {
            emit(top, .back) { AsukaEvents.back($0) }
            return
        }
        top.dying = true
        if let below = topScreen(stack) {
            below.view?.isHidden = false
            // The core destroys it; its views go once the slide is done.
            slide(stack, below, top, false)
        }
        updateWindowChrome()
        let id = Int64(top.id)
        emit(stack, .popped) { AsukaEvents.popped($0, id) }
    }

    /// Whether w's back button would pop (the Back menu item's state).
    func canGoBack(_ w: AsukaWin?) -> Bool {
        guard let w, let stack = activeStack(w.root) else { return false }
        return liveScreens(stack) > 1
    }

    func searchWin(_ f: NSTextField) -> AsukaWin? {
        allWins.first { $0.chrome.searchField != nil && $0.chrome.searchField === f }
    }

    func searchChanged(_ f: NSTextField) {
        guard let w = searchWin(f), let s = w.chrome.screen else { return }
        let text = f.stringValue
        s.nav?.searchText = text
        emit(s, .textChanged) { AsukaEvents.textChanged($0, text) }
    }

    func searchCommand(_ c: NSControl, _ cmd: Selector) -> Bool {
        guard let f = c as? NSTextField, let w = searchWin(f), let s = w.chrome.screen else { return false }
        if cmd == #selector(NSResponder.insertNewline(_:)) {
            emit(s, .submit) { AsukaEvents.submit($0) }
            return true
        }
        return false
    }

    /// Presses the toolbar item labeled label in the window in use (or any
    /// window showing it), as a click would: a bar button's action, the
    /// back button. A menu's item is the "pick" step's.
    func tapToolbar(_ label: String) -> Bool {
        // AppKit's own buttons that aren't nodes': an alert's, a
        // snackbar's.
        if tapAlertButton(label) || tapSnack(label) { return true }
        let ws = [keyWin] + allWins.filter { $0 !== keyWin }
        for w in ws where w.window.isVisible {
            guard let t = w.chrome.toolbar else { continue }
            for it in t.items where it.label == label {
                if it is NSMenuToolbarItem { return false }
                if !it.isEnabled { return false }
                if it.itemIdentifier == backID { back(w) } else { barItem(it) }
                return true
            }
        }
        return false
    }

    /// Types text into the toolbar's search field of the window in use, as
    /// the user would: its text set, its change reported.
    func typeSearch(_ text: String) -> Bool {
        let ws = [keyWin] + allWins.filter { $0 !== keyWin }
        for w in ws where w.window.isVisible {
            guard let f = w.chrome.searchField else { continue }
            f.stringValue = text
            searchChanged(f)
            return true
        }
        return false
    }

    /// Presses return in the toolbar's search field of the window in use,
    /// as the user would: the screen's searchSubmit.
    func submitSearch() -> Bool {
        let ws = [keyWin] + allWins.filter { $0 !== keyWin }
        for w in ws where w.window.isVisible {
            guard let f = w.chrome.searchField else { continue }
            return searchCommand(f, #selector(NSResponder.insertNewline(_:)))
        }
        return false
    }
}

extension AsukaNavTarget: NSToolbarDelegate {
    func toolbar(_ toolbar: NSToolbar, itemForItemIdentifier itemIdentifier: NSToolbarItem.Identifier, willBeInsertedIntoToolbar flag: Bool) -> NSToolbarItem? {
        guard let w = rt.toolbarWin(toolbar) else { return nil }
        return rt.toolbarItem(w, itemIdentifier)
    }

    func toolbarDefaultItemIdentifiers(_ toolbar: NSToolbar) -> [NSToolbarItem.Identifier] {
        rt.toolbarWin(toolbar)?.chrome.ids ?? []
    }

    func toolbarAllowedItemIdentifiers(_ toolbar: NSToolbar) -> [NSToolbarItem.Identifier] {
        rt.toolbarWin(toolbar)?.chrome.ids ?? []
    }

    /// The back button, ⌘[ and the View menu's Back.
    @objc func back(_ sender: Any?) { rt.back(rt.senderWin(sender)) }

    @objc func barItem(_ sender: Any?) { rt.barItem(sender) }
}

// The View menu's Back (back:, sent along the responder chain) goes to the
// window it's in: its delegate. It's enabled while that window's stack has
// a screen to go back to.
extension AsukaWin: NSMenuItemValidation {
    @objc func back(_ sender: Any?) { MainActor.assumeIsolated { AsukaRuntime.shared.back(self) } }

    func validateMenuItem(_ item: NSMenuItem) -> Bool {
        if item.action == #selector(back(_:)) { return MainActor.assumeIsolated { AsukaRuntime.shared.canGoBack(self) } }
        return true
    }
}
