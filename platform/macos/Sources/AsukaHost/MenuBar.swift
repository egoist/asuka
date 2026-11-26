// The main menu: the app's, File, Edit (text fields' copy and paste go
// through it), View, Window and Help, in the language shown, with the
// app's own menus (MenuBar). The menu bar is made again with them each
// time they change. A chosen item, by a click or its shortcut, sends
// Action to the root with its number among all the menus' items
// (AsukaMenuItem.index); a submenu is an NSMenu of its own, a role's item
// does what AppKit does (Quit: terminate:).
import AppKit
#if canImport(AsukaOps)
import AsukaOps
#endif

// MARK: Menus on the wire (ops/menu.go)

/// A menu's item, as the core sends menus: the app's menu bar
/// (MenuBar), and the menus of its tray icons and of its Dock icon.
struct AsukaMenuItem {
    /// The item's number in its menu (or menu bar), depth first: what a
    /// chosen item is reported by.
    var index = 0
    var flags: UInt8 = 0
    var title = ""
    /// A character, or enter, escape, tab, space, delete, backspace, up,
    /// down, left, right, home, end, pageup, pagedown, f1-f12; empty: no
    /// shortcut.
    var key = ""
    /// 1 primary (⌘), 2 shift, 4 option, 8 control.
    var mods: UInt8 = 0
    var role: UInt8 = 0
    /// Its submenu's items (submenu).
    var items: [AsukaMenuItem] = []

    static let separator: UInt8 = 1
    static let disabled: UInt8 = 2
    static let checked: UInt8 = 4
    static let destructive: UInt8 = 8
    static let submenu: UInt8 = 16
    static let inline: UInt8 = 32

    // MenuRole: what an item does that the host does itself.
    static let roleNone: UInt8 = 0
    /// Quits the app: "Quit <app>", ⌘Q.
    static let roleQuit: UInt8 = 1
    /// Closes the window in use as its close button does: "Close", ⌘W.
    static let roleClose: UInt8 = 2
    /// The host's own Open Recent submenu, NSDocumentController's, in its
    /// place.
    static let roleOpenRecent: UInt8 = 3

    func `is`(_ f: UInt8) -> Bool { flags & f == f }
}

/// One of a menu bar's menus.
struct AsukaMenuBarMenu {
    var title = ""
    var items: [AsukaMenuItem] = []
}

/// Decodes menus (ops.DecodeMenu, ops.DecodeMenuBar). A menu is u32
/// count, then its items: u8 flags, a u32-length title, a u32-length key,
/// u8 modifiers and u8 role; with the submenu flag, its submenu follows,
/// a menu. A menu bar is u32 count, then per menu a u32-length title and a
/// menu. Items are numbered depth first, each submenu's items after the
/// item that opens it, separators and submenus too. A short menu ends
/// where it's cut.
enum AsukaMenus {
    /// Submenus' nesting is bounded: deeper ones are cut.
    static let depth = 16

    static func decode(_ b: [UInt8]) -> [AsukaMenuItem] {
        var r = AsukaByteReader(b)
        var n = 0
        return menu(&r, &n, 0)
    }

    static func decodeBar(_ b: [UInt8]) -> [AsukaMenuBarMenu] {
        var r = AsukaByteReader(b)
        let count = r.u32()
        var menus: [AsukaMenuBarMenu] = []
        var n = 0
        var i: UInt32 = 0
        while i < count && !r.bad {
            var m = AsukaMenuBarMenu(title: r.string())
            m.items = menu(&r, &n, 0)
            if r.bad && m.items.isEmpty { break }
            menus.append(m)
            i += 1
        }
        return menus
    }

    private static func menu(_ r: inout AsukaByteReader, _ n: inout Int, _ depth: Int) -> [AsukaMenuItem] {
        let count = r.u32()
        var items: [AsukaMenuItem] = []
        var i: UInt32 = 0
        while i < count && !r.bad {
            i += 1
            var it = AsukaMenuItem(index: n, flags: r.u8())
            it.title = r.string()
            it.key = r.string()
            it.mods = r.u8()
            it.role = r.u8()
            if r.bad { break }
            n += 1
            if it.is(AsukaMenuItem.submenu) {
                if depth >= Self.depth {
                    r.bad = true
                    break
                }
                it.items = menu(&r, &n, depth + 1)
            }
            items.append(it)
        }
        return items
    }

    /// Items and their submenus' items depth first, so that an item's
    /// index is its place in the list.
    static func flatten(_ items: [AsukaMenuItem]) -> [AsukaMenuItem] {
        var out: [AsukaMenuItem] = []
        func walk(_ items: [AsukaMenuItem]) {
            for it in items {
                out.append(it)
                walk(it.items)
            }
        }
        walk(items)
        return out
    }
}

// MARK: The main menu

@MainActor extension AsukaShell: NSMenuItemValidation {
    /// Makes the main menu: the app's, File, Edit, View, Window and Help,
    /// in the language shown, with the app's own menus (MenuBar): one
    /// titled as a standard one, in English or in that language, adds its
    /// items to it (one titled as the app, to the app's menu after About:
    /// "Check for Updates…"), the others go between View and Window, as
    /// macOS orders them.
    func makeMenuBar() {
        let app = NSApplication.shared
        let bar = NSMenu()
        let name = rt.appName
        var own: [String: Int] = [:] // the standard menus' app menus, by index
        var joined = Set<Int>()
        for (i, m) in appMenus.enumerated() {
            if !name.isEmpty && m.title == name {
                if let old = own["App"] { joined.remove(old) }
                own["App"] = i
                joined.insert(i)
                continue
            }
            for s in ["File", "Edit", "View", "Window", "Help"] {
                if m.title.caseInsensitiveCompare(s) == .orderedSame || m.title.caseInsensitiveCompare(menuWord(s)) == .orderedSame {
                    if let old = own[s] { joined.remove(old) }
                    own[s] = i
                    joined.insert(i)
                    break
                }
            }
        }
        func ownMenu(_ s: String) -> AsukaMenuBarMenu? { own[s].map { appMenus[$0] } }

        let appMenu = NSMenu(title: "")
        asukaAddItem(appMenu, asukaWord("about", ["app": name]), #selector(NSApplication.orderFrontStandardAboutPanel(_:)), "")
        if let m = ownMenu("App") { addAppItems(appMenu, m) }
        appMenu.addItem(.separator())
        let services = NSMenu(title: menuWord("Services"))
        asukaAddSubmenu(appMenu, menuWord("Services"), services)
        app.servicesMenu = services
        appMenu.addItem(.separator())
        asukaAddItem(appMenu, asukaWord("hide", ["app": name]), #selector(NSApplication.hide(_:)), "h")
        asukaAddItem(appMenu, asukaWord("hideOthers"), #selector(NSApplication.hideOtherApplications(_:)), "h", [.option])
        asukaAddItem(appMenu, menuWord("Show All"), #selector(NSApplication.unhideAllApplications(_:)), "")
        appMenu.addItem(.separator())
        asukaAddItem(appMenu, asukaWord("quit", ["app": name]), #selector(NSApplication.terminate(_:)), "q")
        asukaAddSubmenu(bar, name, appMenu)

        let file = NSMenu(title: menuWord("File"))
        if let m = ownMenu("File") {
            addAppItems(file, m)
            file.addItem(.separator())
        }
        // The app's File menu with Open Recent and Close of its own (a
        // document app's) has them where it puts them.
        if !hasRole(ownMenu("File"), AsukaMenuItem.roleOpenRecent) && hasOpenRecent() { // Files.swift
            addOpenRecent(file)
            file.addItem(.separator())
        }
        if !hasRole(ownMenu("File"), AsukaMenuItem.roleClose) {
            asukaAddItem(file, menuWord("Close Window"), #selector(NSWindow.performClose(_:)), "w")
        } else if let last = file.items.last, last.isSeparatorItem {
            file.removeItem(last)
        }
        asukaAddSubmenu(bar, menuWord("File"), file)

        let edit = NSMenu(title: menuWord("Edit"))
        asukaAddItem(edit, menuWord("Undo"), Selector(("undo:")), "z")
        asukaAddItem(edit, menuWord("Redo"), Selector(("redo:")), "z", [.shift])
        edit.addItem(.separator())
        asukaAddItem(edit, menuWord("Cut"), #selector(NSText.cut(_:)), "x")
        asukaAddItem(edit, menuWord("Copy"), #selector(NSText.copy(_:)), "c")
        asukaAddItem(edit, menuWord("Paste"), #selector(NSText.paste(_:)), "v")
        asukaAddItem(edit, menuWord("Select All"), #selector(NSText.selectAll(_:)), "a")
        if let m = ownMenu("Edit") {
            edit.addItem(.separator())
            addAppItems(edit, m)
        }
        asukaAddSubmenu(bar, menuWord("Edit"), edit)

        let view = NSMenu(title: menuWord("View"))
        if let m = ownMenu("View") {
            addAppItems(view, m)
            view.addItem(.separator())
        }
        // Back goes to the window in use (rt.keyWin), as its back button
        // does, enabled while it can go back (AsukaNavTarget's
        // validation), not to whatever window is key: one shows when no
        // window is key, the app being in the background.
        asukaAddItem(view, asukaWord("back"), #selector(AsukaNavTarget.back(_:)), "[").target = AsukaNavTarget.shared
        view.addItem(.separator())
        asukaAddItem(view, menuWord("Enter Full Screen"), #selector(NSWindow.toggleFullScreen(_:)), "f", [.control])
        asukaAddSubmenu(bar, menuWord("View"), view)

        for (i, m) in appMenus.enumerated() where !joined.contains(i) {
            let mm = NSMenu(title: m.title)
            addAppItems(mm, m)
            asukaAddSubmenu(bar, m.title, mm)
        }

        let win = NSMenu(title: menuWord("Window"))
        asukaAddItem(win, menuWord("Minimize"), #selector(NSWindow.performMiniaturize(_:)), "m")
        asukaAddItem(win, asukaWord("zoom"), #selector(NSWindow.performZoom(_:)), "")
        if let m = ownMenu("Window") {
            win.addItem(.separator())
            addAppItems(win, m)
        }
        win.addItem(.separator())
        asukaAddItem(win, menuWord("Bring All to Front"), #selector(NSApplication.arrangeInFront(_:)), "")
        asukaAddSubmenu(bar, menuWord("Window"), win)
        app.windowsMenu = win

        if let m = ownMenu("Help") {
            let help = NSMenu(title: menuWord("Help"))
            addAppItems(help, m)
            asukaAddSubmenu(bar, menuWord("Help"), help)
            app.helpMenu = help
        }

        setMenuDirection(bar, rt.rtl)
        app.mainMenu = bar
    }

    /// The MenuBar command: the app's menus, and the menu bar made again.
    func setMenuBar(_ b: [UInt8]) {
        appMenus = AsukaMenus.decodeBar(b)
        appItems = appMenus.flatMap { AsukaMenus.flatten($0.items) }
        makeMenuBar()
    }

    /// Adds an app menu's items to m.
    func addAppItems(_ m: NSMenu, _ am: AsukaMenuBarMenu) {
        addMenuItems(m, am.items, #selector(appMenuItem(_:)), 0, true)
    }

    /// Adds items to m, their submenus too: chosen, an item sends action
    /// to the shell, its tag tagBase plus its number. keys says whether
    /// they show their shortcuts (a tray icon's and the Dock's menus show
    /// none). A role's item does what AppKit does.
    func addMenuItems(_ m: NSMenu, _ items: [AsukaMenuItem], _ action: Selector, _ tagBase: Int, _ keys: Bool) {
        for it in items {
            if it.is(AsukaMenuItem.separator) {
                m.addItem(.separator())
                continue
            }
            var title = it.title
            var (key, mods): (String, NSEvent.ModifierFlags) = keys ? asukaKeyEquivalent(it.key, it.mods) : ("", [])
            var act: Selector? = action
            switch it.role {
            case AsukaMenuItem.roleQuit:
                if title.isEmpty { title = asukaWord("quit", ["app": rt.appName]) }
                if key.isEmpty && keys { (key, mods) = ("q", [.command]) }
                act = #selector(NSApplication.terminate(_:))
            case AsukaMenuItem.roleClose:
                if title.isEmpty { title = menuWord("Close") }
                if key.isEmpty && keys { (key, mods) = ("w", [.command]) }
                act = #selector(NSWindow.performClose(_:))
            case AsukaMenuItem.roleOpenRecent:
                // The host's own, NSDocumentController's recent documents.
                addOpenRecent(m)
                continue
            default:
                break
            }
            let isSub = it.is(AsukaMenuItem.submenu)
            if isSub {
                act = nil
                (key, mods) = ("", [])
            }
            let mi = m.addItem(withTitle: title, action: act, keyEquivalent: key)
            mi.keyEquivalentModifierMask = mods
            if act == action && it.role == AsukaMenuItem.roleNone {
                mi.target = action == #selector(appMenuItem(_:)) ? self : AsukaTray.shared
                mi.tag = tagBase + it.index
            }
            if it.is(AsukaMenuItem.checked) && !isSub { mi.state = .on }
            if it.is(AsukaMenuItem.destructive) { asukaDestructiveTitle(mi, title) } // red (Menus.swift)
            if isSub {
                let s = NSMenu(title: title)
                s.autoenablesItems = m.autoenablesItems
                addMenuItems(s, it.items, action, tagBase, keys)
                mi.submenu = s
                if it.is(AsukaMenuItem.disabled) { mi.isEnabled = false }
            } else if !m.autoenablesItems {
                mi.isEnabled = !it.is(AsukaMenuItem.disabled)
            }
        }
    }

    /// Whether the menu has an item of role, in a submenu too.
    func hasRole(_ m: AsukaMenuBarMenu?, _ role: UInt8) -> Bool {
        guard let m else { return false }
        return AsukaMenus.flatten(m.items).contains { $0.role == role }
    }

    /// An app menu item chosen: Action on the root.
    @objc func appMenuItem(_ sender: NSMenuItem) {
        guard let r = rt.root else { return }
        let i = sender.tag
        rt.emit(r, .action) { AsukaEvents.action($0, Int64(i)) }
    }

    /// Enables the app's items as the app says (Back is the window's,
    /// which enables it when there's a screen to go back from).
    func validateMenuItem(_ mi: NSMenuItem) -> Bool {
        if mi.action == #selector(appMenuItem(_:)) {
            let i = mi.tag
            if i >= 0 && i < appItems.count { return !appItems[i].is(AsukaMenuItem.disabled) }
            return false
        }
        return true
    }

    // MARK: Words

    /// A word of the standard menus in the language shown, as AppKit says
    /// it: it doesn't translate a main menu made at run time. English where
    /// its tables don't have the language or the word.
    func menuWord(_ english: String) -> String {
        let locale = rt.locale
        if locale.isEmpty { return english }
        if let p = asukaPseudo(english, locale) { return p }
        guard let src = asukaMenuWords[english] else { return english }
        let bundle = Bundle(for: NSApplication.self)
        let sel = NSSelectorFromString("localizedStringForKey:value:table:localizations:")
        guard bundle.responds(to: sel), let imp = bundle.method(for: sel) else { return english }
        typealias F = @convention(c) (AnyObject, Selector, NSString, NSString, NSString, NSArray) -> NSString?
        let f = unsafeBitCast(imp, to: F.self)
        let s = f(bundle, sel, src.key as NSString, english as NSString, src.table as NSString, [locale] as NSArray) as String? ?? english
        return s.replacingOccurrences(of: "&", with: "")
    }
}

/// The words of the standard menus that AppKit has in its own tables, in
/// the languages macOS speaks: each one's table and key. Asuka's catalog
/// has the others (asukaWord).
private let asukaMenuWords: [String: (table: String, key: String)] = [
    "Services": ("Services", "Services"),
    "Show All": ("Common", "Show All"),
    "File": ("MenuCommands", "File"),
    "Close Window": ("MenuCommands", "Close Window"),
    "Close": ("MenuCommands", "Close"),
    "Open Recent": ("MenuCommands", "Open Recent"),
    "Clear Menu": ("MenuCommands", "Clear Menu"),
    "Edit": ("Document", "Edit"),
    "Undo": ("Undo", "&Undo"), // marked for an access key
    "Redo": ("Undo", "&Redo"),
    "Cut": ("MenuCommands", "Cut"),
    "Copy": ("MenuCommands", "Copy"),
    "Paste": ("MenuCommands", "Paste"),
    "Select All": ("MenuCommands", "Select All"),
    "View": ("MenuCommands", "View"),
    "Enter Full Screen": ("MenuCommands", "Enter Full Screen"),
    "Window": ("MenuCommands", "Window"),
    "Minimize": ("MenuCommands", "Minimize"),
    "Bring All to Front": ("MenuCommands", "Bring All to Front"),
    "Help": ("MenuCommands", "Help"),
]

@discardableResult
@MainActor func asukaAddItem(_ m: NSMenu, _ title: String, _ action: Selector?, _ key: String, _ mods: NSEvent.ModifierFlags = []) -> NSMenuItem {
    let it = m.addItem(withTitle: title, action: action, keyEquivalent: key)
    if !mods.isEmpty { it.keyEquivalentModifierMask = mods.union(.command) }
    return it
}

@MainActor func asukaAddSubmenu(_ m: NSMenu, _ title: String, _ s: NSMenu) {
    let it = m.addItem(withTitle: title, action: nil, keyEquivalent: "")
    it.submenu = s
}

/// A shortcut as AppKit's key equivalent and modifier mask: primary is ⌘.
func asukaKeyEquivalent(_ key: String, _ mods: UInt8) -> (String, NSEvent.ModifierFlags) {
    if key.isEmpty { return ("", []) }
    var mask: NSEvent.ModifierFlags = []
    if mods & 1 != 0 { mask.insert(.command) }
    if mods & 2 != 0 { mask.insert(.shift) }
    if mods & 4 != 0 { mask.insert(.option) }
    if mods & 8 != 0 { mask.insert(.control) }
    let named: [String: Int] = [
        "enter": 0x0D, "escape": 0x1B, "tab": 0x09, "space": 0x20, "backspace": 0x08, "delete": 0x7F,
        "up": 0xF700, "down": 0xF701, "left": 0xF702, "right": 0xF703,
        "home": 0xF729, "end": 0xF72B, "pageup": 0xF72C, "pagedown": 0xF72D,
    ]
    let k = key.lowercased()
    if let c = named[k], let u = Unicode.Scalar(c) { return (String(Character(u)), mask) }
    if k.count >= 2, k.hasPrefix("f"), let n = Int(k.dropFirst()), n >= 1, n <= 12, let u = Unicode.Scalar(0xF704 + n - 1) {
        return (String(Character(u)), mask)
    }
    // A letter: AppKit reads an uppercase one as shifted.
    return (k, mask)
}

// MARK: Pseudo locales

/// english as a pseudo locale shows it (i18n's en-XA and ar-XB, as
/// Android's): nil for a real locale.
func asukaPseudo(_ english: String, _ locale: String) -> String? {
    let tag = locale.split(separator: "-", omittingEmptySubsequences: false)
    guard tag.count >= 2 else { return nil }
    let base = "\(tag[0].lowercased())-\(tag[1].uppercased())"
    switch base {
    case "en-XA":
        // The letters accented, the text 40% longer and in brackets.
        var out = "["
        var length = 0
        for c in english {
            out += asukaAccented[c] ?? String(c)
            length += 1
        }
        let pad = (length * 2 + 4) / 5
        var n = 0, i = 0
        while n < pad {
            let w = asukaPadWords[i % asukaPadWords.count]
            out += " " + w
            n += 1 + w.count
            i += 1
        }
        return out + "]"
    case "ar-XB":
        // Each word right to left: an override reverses its letters, and
        // marks around it set it in a right-to-left run.
        var out = ""
        var word = ""
        for c in english {
            if c.isWhitespace {
                if !word.isEmpty { out += "\u{200F}\u{202E}" + word + "\u{202C}\u{200F}"; word = "" }
                out.append(c)
            } else {
                word.append(c)
            }
        }
        if !word.isEmpty { out += "\u{200F}\u{202E}" + word + "\u{202C}\u{200F}" }
        return out
    default:
        return nil
    }
}

private let asukaAccented: [Character: String] = [
    "a": "å", "b": "ƀ", "c": "ç", "d": "ð", "e": "é", "f": "ƒ", "g": "ĝ", "h": "ĥ", "i": "î",
    "j": "ĵ", "k": "ķ", "l": "ļ", "m": "ɱ", "n": "ñ", "o": "ö", "p": "þ", "q": "ǫ", "r": "ŕ",
    "s": "š", "t": "ţ", "u": "û", "v": "ṽ", "w": "ŵ", "x": "ẋ", "y": "ý", "z": "ž",
    "A": "Å", "B": "Ɓ", "C": "Ç", "D": "Ð", "E": "É", "F": "Ƒ", "G": "Ĝ", "H": "Ĥ", "I": "Î",
    "J": "Ĵ", "K": "Ķ", "L": "Ļ", "M": "Ṁ", "N": "Ñ", "O": "Ö", "P": "Þ", "Q": "Ǫ", "R": "Ŕ",
    "S": "Š", "T": "Ţ", "U": "Û", "V": "Ṽ", "W": "Ŵ", "X": "Ẋ", "Y": "Ý", "Z": "Ž",
]

private let asukaPadWords = "one two three four five six seven eight nine ten eleven twelve thirteen fourteen fifteen sixteen seventeen eighteen nineteen twenty".split(separator: " ").map(String.init)
