// Languages and direction on a Mac.
//
// The language the app shows comes from the core (the Locale command), and
// frames are always physical: the host only mirrors what it draws itself.
//
// AppKit's own idea of the direction, NSApplication's
// userInterfaceLayoutDirection, is read once and never changes. It decides
// what the host can't set: the traffic lights, the title, the toolbar's
// order and NSTabView's. So the host works it out before the application
// starts, as AppKit's own -NSForceRightToLeftWritingDirection would, and an
// app whose language reads right to left is a right to left app from its
// first frame (appRTL). A language that changes direction later reaches
// what AppKit lets a view take (userInterfaceLayoutDirection), and the host
// orders the toolbar and the tabs itself; the traffic lights stay until the
// next launch.
import AppKit
#if canImport(AsukaOps)
import AsukaOps
#endif

@MainActor extension AsukaRuntime {
    /// Reads what the environment reports of the user's languages and
    /// formats. They're kept: the window reports its environment as it's
    /// resized, and they change rarely, with a notification.
    func refreshLocale() {
        languages = asukaPreferredLanguages().joined(separator: ",")
        firstWeekday = 0
        let d = Calendar.current.firstWeekday
        if d >= 1 && d <= 7 { firstWeekday = UInt8(d) }
        hourCycle = asukaHourCycle(DateFormatter.dateFormat(fromTemplate: "j", options: 0, locale: .autoupdatingCurrent) ?? "")
    }

    /// The Locale command: the language the app shows, and the way it
    /// reads.
    func setLocale(_ args: UnsafeRawBufferPointer) {
        guard args.count > 0 else { return }
        let newRTL = args[0] & 1 != 0
        let tag = String(decoding: UnsafeRawBufferPointer(rebasing: args[1...]), as: UTF8.self)
        let dirChanged = newRTL != rtl, tagChanged = tag != locale
        rtl = newRTL
        locale = tag
        if dirChanged {
            for n in nodes.values { applyDirection(n) }
        }
        guard dirChanged || tagChanged else { return }
        // The host's own words: the toolbar and the menus are made of them.
        AsukaShell.shared.makeMenuBar()
        updateWindowChrome()
    }

    /// The SaveLocale command: AppKit reads AppleLanguages of the app's
    /// defaults at launch, as System Settings' language of an app writes
    /// it. Without it, NSLocale's are the system's again at once.
    func saveLocale(_ tag: String) {
        let d = UserDefaults.standard
        if tag.isEmpty { d.removeObject(forKey: "AppleLanguages") } else { d.set([tag], forKey: "AppleLanguages") }
        d.synchronize()
        refreshLocale()
        afterCore { [weak self] in self?.environmentChanged() }
    }

    /// Whether n reads right to left: its own direction (a
    /// WithDirection's), or the app's.
    func isRTL(_ n: AsukaNode) -> Bool {
        switch n.direction {
        case .leftToRight: return false
        case .rightToLeft: return true
        default: return rtl
        }
    }

    /// Makes n's view read its way: what it lays out itself, and its text.
    /// A view doesn't take the direction from its parent: each has its own,
    /// which starts as the application's.
    func applyDirection(_ n: AsukaNode) {
        guard let v = n.view else { return }
        let r = isRTL(n)
        let changed = r != n.rtl
        n.rtl = r
        v.userInterfaceLayoutDirection = r ? .rightToLeft : .leftToRight
        switch n.kind {
        case .text, .textField:
            applyTextAlign(n)
        case .button:
            if n.textAlign != 0 { applyTextAlign(n) }
        case .wheel, .picker:
            if let p = v as? NSPopUpButton { setMenuDirection(p.menu, r) }
        case .scroll:
            if changed && n.horizontal { mirrorScroll(n, r) }
        default:
            break
        }
        directionChanged(n, changed)
        if let m = n.menu { setMenuDirection(m, r) }
    }

    /// The NSTextAlignment of n's TextAlign, whichever way the app reads.
    /// Natural isn't used: it follows the app's direction and its text, not
    /// the view's.
    func alignment(_ n: AsukaNode) -> NSTextAlignment {
        var r = isRTL(n)
        switch AsukaTextAlign(rawValue: n.textAlign) {
        case .center: return .center
        case .justify: return .justified
        case .end: r.toggle()
        default: break
        }
        return r ? .right : .left
    }

    /// The base writing direction of n's text: the text's own (its first
    /// strong character), so English in a right to left app reads "Hello,
    /// Ada!" at the right, unless a WithDirection says which.
    func writingDirection(_ n: AsukaNode) -> NSWritingDirection {
        switch n.direction {
        case .leftToRight: return .leftToRight
        case .rightToLeft: return .rightToLeft
        default: return .natural
        }
    }

    /// Sets the alignment and the base writing direction of a text, a
    /// field or a multiline field's text view.
    func applyTextAlign(_ n: AsukaNode) {
        if let tv = n.textView {
            tv.alignment = alignment(n)
            tv.baseWritingDirection = writingDirection(n)
            // What's typed next takes the paragraph style of the text view's.
            let p = NSMutableParagraphStyle()
            p.alignment = alignment(n)
            p.baseWritingDirection = writingDirection(n)
            tv.defaultParagraphStyle = p
            tv.userInterfaceLayoutDirection = isRTL(n) ? .rightToLeft : .leftToRight
            return
        }
        guard let c = n.view as? NSControl else { return }
        c.alignment = alignment(n)
        if n.kind == .text || n.kind == .textField { c.baseWritingDirection = writingDirection(n) }
        if n.styledLabel { applyStyledText(n) } // its paragraph style's (TextStyle.swift)
    }

    /// A horizontal Scroll going another way: once the core has laid it
    /// out again, what was the distance from one end is that from the
    /// other.
    func mirrorScroll(_ n: AsukaNode, _ r: Bool) {
        let gap = r ? (n.view as? NSScrollView)?.contentView.bounds.origin.x ?? 0 : endGap(n)
        afterCore { [weak self] in
            guard let self, self.alive(n) else { return }
            self.scrollFromStart(n, gap)
        }
    }

    /// The horizontal Scroll that a frame set on n changes the content or
    /// the width of (nil: none). A left to right one only matters where
    /// AppKit reads right to left: it would move to the content's end.
    func horizontalScroll(_ n: AsukaNode) -> AsukaNode? {
        let s = n.kind == .scroll ? n : n.parent
        guard let s, s.kind == .scroll, s.horizontal, isRTL(s) || appRTL else { return nil }
        return s
    }

    /// How far a horizontal Scroll is from the end it starts at: the
    /// right of its content reading right to left, else the left.
    func startGap(_ s: AsukaNode) -> CGFloat {
        if isRTL(s) { return endGap(s) }
        return (s.view as? NSScrollView)?.contentView.bounds.origin.x ?? 0
    }

    /// Scrolls a horizontal Scroll to that distance.
    func scrollFromStart(_ s: AsukaNode, _ gap: CGFloat) {
        guard let sv = s.view as? NSScrollView, let doc = sv.documentView else { return }
        let clip = sv.contentView.bounds
        let x = isRTL(s) ? max(doc.frame.width - clip.width - gap, 0) : gap
        guard clip.origin.x != x else { return }
        sv.contentView.scroll(to: CGPoint(x: x, y: clip.origin.y))
        sv.reflectScrolledClipView(sv.contentView)
    }

    /// How far a horizontal Scroll is from the end of its content (0: it
    /// shows the end).
    func endGap(_ s: AsukaNode) -> CGFloat {
        guard let sv = s.view as? NSScrollView, let doc = sv.documentView else { return 0 }
        let clip = sv.contentView.bounds
        return max(doc.frame.width - clip.width - clip.origin.x, 0)
    }

    /// Whether the host orders the toolbar's items: AppKit orders them by
    /// the app's direction.
    var toolbarReversed: Bool { rtl != appRTL }

    /// The back button's symbol: it's on the side the app reads from, and
    /// points out of the window.
    var backSymbol: String { rtl ? "chevron.right" : "chevron.left" }

    /// The symbol a toolbar item shows of name. A forward or backward
    /// symbol mirrors with the app's direction, which AppKit's toolbar
    /// takes from the application: where the two differ, it's the other.
    func barSymbol(_ name: String) -> NSImage? {
        if toolbarReversed {
            var other = ""
            if name.contains("forward") { other = name.replacingOccurrences(of: "forward", with: "backward") }
            else if name.contains("backward") { other = name.replacingOccurrences(of: "backward", with: "forward") }
            if !other.isEmpty, let img = symbolImage(other) { return img }
        }
        return symbolImage(name)
    }
}

func setMenuDirection(_ m: NSMenu?, _ rtl: Bool) {
    guard let m else { return }
    m.userInterfaceLayoutDirection = rtl ? .rightToLeft : .leftToRight
    for it in m.items { setMenuDirection(it.submenu, rtl) }
}

/// The languages the user reads, most preferred first. NSLocale's are the
/// app's own alone when its defaults have an AppleLanguages (SaveLocale's,
/// or the language System Settings gives the app), which isn't all the user
/// reads: the system's follow them.
func asukaPreferredLanguages() -> [String] {
    var out: [String] = []
    func add(_ list: [String]) { for t in list where !t.isEmpty && !out.contains(t) { out.append(t) } }
    add(NSLocale.preferredLanguages)
    if let g = UserDefaults.standard.persistentDomain(forName: UserDefaults.globalDomain), let l = g["AppleLanguages"] as? [String] { add(l) }
    return out
}

/// The hour cycle of a date format pattern: 12 for h and K, 24 for H and
/// k, 0 for none. Quoted text isn't a field.
func asukaHourCycle(_ pattern: String) -> UInt8 {
    var quoted = false
    for c in pattern {
        if c == "'" { quoted.toggle(); continue }
        if quoted { continue }
        if c == "h" || c == "K" { return 12 }
        if c == "H" || c == "k" { return 24 }
    }
    return 0
}

/// Whether the app's first language reads right to left, worked out
/// before there's a core to ask: asuka dev's --lang and --dir (asuka-app.json's
/// lang and direction, or ASUKA_LOCALE and ASUKA_DIRECTION), else the
/// user's languages matched against the app's (its Info.plist's
/// CFBundleLocalizations, its catalogs', the CLI writes).
/// It may be wrong (the app sets its locale as it starts): that's a
/// language changing later.
func asukaLaunchRTL(_ c: AsukaConfig) -> Bool {
    let env = ProcessInfo.processInfo.environment
    let dir = env["ASUKA_DIRECTION"] ?? c.direction
    if dir == "rtl" { return true }
    if dir == "ltr" { return false }
    var lang = env["ASUKA_LOCALE"] ?? c.lang
    if lang.isEmpty {
        let info = Bundle.main.infoDictionary ?? [:]
        var have = info["CFBundleLocalizations"] as? [String] ?? []
        if have.isEmpty { have = [info["CFBundleDevelopmentRegion"] as? String ?? "en"] }
        lang = Bundle.preferredLocalizations(from: have, forPreferences: asukaPreferredLanguages()).first ?? have[0]
    }
    // Pseudo locales: ar-XB reads right to left, en-XA doesn't.
    if lang.hasSuffix("-XB") { return true }
    let base = Locale(identifier: lang).languageCode ?? lang
    return NSLocale.characterDirection(forLanguage: base) == .rightToLeft
}

private var directionLatched = false

/// Makes AppKit a right to left app if the app's language reads that way.
/// AppKit reads it once, before the first view, from the defaults' argument
/// domain among others, and the window's title bar reads it again as each
/// window is made, so it stays there. Before the application is made.
func asukaLatchDirection(_ c: AsukaConfig) {
    guard !directionLatched else { return }
    directionLatched = true
    guard asukaLaunchRTL(c) else { return }
    let d = UserDefaults.standard
    var forced = d.volatileDomain(forName: UserDefaults.argumentDomain)
    d.removeVolatileDomain(forName: UserDefaults.argumentDomain)
    forced["AppleTextDirection"] = true
    forced["NSForceRightToLeftWritingDirection"] = true
    d.setVolatileDomain(forced, forName: UserDefaults.argumentDomain)
}
