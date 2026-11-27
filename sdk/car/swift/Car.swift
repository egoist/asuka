// platform/car on iOS: CarPlay. The app's CarPlay scene (Info.plist's
// CPTemplateApplicationSceneSessionRoleApplication, which the CLI writes
// for asuka.toml's [car]) has AsukaCarSceneDelegate for its delegate: it
// hands CarHub the interface controller, starting the app's code if the
// car launched the app, and CarHub shows the templates Asuka sends,
// changing in place the ones it shows (by their ids), pushing the ones
// it doesn't and popping the ones gone. What the driver chooses goes back
// as its handler's number; the car's back button, as how many are left.
// A navigation app's scene comes with the CarPlay window too, whose root
// view controller is the map under its CPMapTemplate (Navigation.swift,
// CarMap.swift).
import Asuka
import CarPlay
import Foundation
import MapKit
import UIKit

/// The CarPlay scene's delegate, by the name Info.plist gives it.
@objc(AsukaCarSceneDelegate)
public final class AsukaCarSceneDelegate: UIResponder, CPTemplateApplicationSceneDelegate {
    public func templateApplicationScene(_ scene: CPTemplateApplicationScene, didConnect controller: CPInterfaceController) {
        MainActor.assumeIsolated { CarHub.shared.connect(controller) }
    }

    public func templateApplicationScene(_ scene: CPTemplateApplicationScene, didDisconnectInterfaceController controller: CPInterfaceController) {
        MainActor.assumeIsolated { CarHub.shared.disconnect(controller) }
    }

    // A navigation app's (the maps entitlement): the window its map draws in.
    public func templateApplicationScene(_ scene: CPTemplateApplicationScene, didConnect controller: CPInterfaceController, to window: CPWindow) {
        MainActor.assumeIsolated { CarHub.shared.connect(controller, window: window) }
    }

    public func templateApplicationScene(_ scene: CPTemplateApplicationScene, didDisconnect controller: CPInterfaceController, from window: CPWindow) {
        MainActor.assumeIsolated { CarHub.shared.disconnect(controller) }
    }
}

/// A template CarHub shows: what it was made from, and CarPlay's.
private struct Entry {
    var shown: Shown
    var template: CPTemplate
}

@MainActor final class CarHub: NSObject, Head, CPInterfaceControllerDelegate, CPPointOfInterestTemplateDelegate {
    static let shared = CarHub()

    private var controller: CPInterfaceController?
    private var stack: [Shown] = []
    private var entries: [Entry] = []
    private var connectedWatchers: [UUID: (Bool) -> Void] = [:]
    private var choseWatchers: [UUID: (Int) -> Void] = [:]
    private var backWatchers: [UUID: (Int) -> Void] = [:]
    private var answeredWatchers: [UUID: (Int, Int) -> Void] = [:]
    /// A navigation app's: the CarPlay window, its map, and the root's
    /// map template.
    private var window: CPWindow?
    private(set) lazy var mapController = CarMapController()
    private(set) var nav: CarNavigation?
    private var images: [String: UIImage] = [:]
    private var loading: Set<String> = []
    // The places shown, by their template, for its delegate.
    private var placeRuns: [ObjectIdentifier: [Int]] = [:]

    /// Whether the app has a CarPlay scene (asuka.toml's [car]).
    static var configured: Bool {
        let m = Bundle.main.object(forInfoDictionaryKey: "UIApplicationSceneManifest") as? [String: Any]
        let c = m?["UISceneConfigurations"] as? [String: Any]
        return c?["CPTemplateApplicationSceneSessionRoleApplication"] != nil
    }

    // MARK: The scene

    func connect(_ c: CPInterfaceController, window w: CPWindow? = nil) {
        controller = c
        c.delegate = self
        entries = []
        nav = nil
        window = w
        if let w {
            mapController = CarMapController()
            w.rootViewController = mapController
        }
        // The car launched the app: its code starts now, without a screen
        // on the phone (a window scene that comes later shows it).
        AsukaRuntime.shared.startHeadless()
        render()
        for w in connectedWatchers.values { w(true) }
    }

    func disconnect(_ c: CPInterfaceController) {
        guard controller === c else { return }
        controller = nil
        entries = []
        placeRuns = [:]
        nav = nil
        window?.rootViewController = nil
        window = nil
        for w in connectedWatchers.values { w(false) }
    }

    // MARK: Head

    func show(stack: [Shown]) throws {
        self.stack = stack
        render()
    }

    func connections(connected: @escaping (Bool) -> Void) async throws {
        let id = UUID()
        connectedWatchers[id] = connected
        defer { connectedWatchers[id] = nil }
        connected(controller != nil)
        while !Task.isCancelled {
            try? await Task.sleep(nanoseconds: 3_600_000_000_000)
        }
    }

    func alert(alert: ShownAlert) throws {
        if let n = nav, controller != nil {
            n.present(alert)
        } else {
            // No map to show it over: it goes at once.
            answered(alert.id, -1)
        }
    }

    func dismiss(id: Int) throws {
        nav?.dismissAlert(id)
    }

    func events(chose: @escaping (Int) -> Void, back: @escaping (Int) -> Void, answered: @escaping (Int, Int) -> Void) async throws {
        let id = UUID()
        choseWatchers[id] = chose
        backWatchers[id] = back
        answeredWatchers[id] = answered
        defer {
            choseWatchers[id] = nil
            backWatchers[id] = nil
            answeredWatchers[id] = nil
        }
        while !Task.isCancelled {
            try? await Task.sleep(nanoseconds: 3_600_000_000_000)
        }
    }

    private func chose(_ n: Int) {
        guard n > 0 else { return }
        for w in choseWatchers.values { w(n) }
    }

    private func answered(_ id: Int, _ n: Int) {
        for w in answeredWatchers.values { w(id, n) }
    }

    // MARK: Showing

    private var appName: String {
        Bundle.main.object(forInfoDictionaryKey: "CFBundleDisplayName") as? String
            ?? Bundle.main.object(forInfoDictionaryKey: "CFBundleName") as? String ?? ""
    }

    /// Shows the stack on the car: the root, changed in place or made
    /// again; the pushed ones it shows already changed in place; the ones
    /// gone popped; the new ones pushed.
    private func render() {
        guard let c = controller else { return }
        var want = stack
        if want.isEmpty || want[0].kind == .nowPlaying {
            // Nothing yet (the app's Screen isn't mounted): its name, until
            // it is; a navigation app's map (its root must be one).
            want = [window != nil ? Shown(id: 1, kind: .navigation) : Shown(id: 1, kind: .list, title: appName)] + want.dropFirst()
        }
        if let root = entries.first, compatible(root.shown, want[0]), update(root.template, root.shown, want[0]) {
            entries[0].shown = want[0]
        } else {
            let t = make(want[0])
            entries = [Entry(shown: want[0], template: t)]
            c.setRootTemplate(t, animated: false, completion: nil)
        }
        var i = 1
        while i < entries.count && i < want.count && entries[i].shown.id == want[i].id && compatible(entries[i].shown, want[i])
            && update(entries[i].template, entries[i].shown, want[i]) {
            entries[i].shown = want[i]
            i += 1
        }
        if i < entries.count {
            let to = entries[i - 1].template
            entries.removeLast(entries.count - i)
            if c.templates.contains(where: { $0 === to }) {
                c.pop(to: to, animated: true, completion: nil)
            }
        }
        while i < want.count {
            let t = make(want[i])
            entries.append(Entry(shown: want[i], template: t))
            if t === CPNowPlayingTemplate.shared, c.topTemplate === t {
                // The driver opened it already.
            } else if c.templates.contains(where: { $0 === t }) {
                c.pop(to: t, animated: true, completion: nil)
            } else {
                c.pushTemplate(t, animated: true, completion: nil)
            }
            i += 1
        }
    }

    /// Whether a template made from was can show now (its kind, and what
    /// CarPlay can't change of it, its tabs' kinds).
    private func compatible(_ was: Shown, _ now: Shown) -> Bool {
        guard was.kind == now.kind else { return false }
        switch now.kind {
        case .list:
            return was.title == now.title
        case .tabs:
            return was.tabs.count == now.tabs.count && zip(was.tabs, now.tabs).allSatisfy { compatible($0, $1) && $0.text == $1.text && $0.symbol == $1.symbol }
        default:
            return true
        }
    }

    /// Changes t, made from was, to show now; false when it can't.
    private func update(_ t: CPTemplate, _ was: Shown, _ now: Shown) -> Bool {
        if same(was, now) { return true }
        switch (t, now.kind) {
        case (let l as CPListTemplate, .list):
            l.updateSections(sections(now))
            l.trailingNavigationBarButtons = barButtons(now.actions)
            l.emptyViewTitleVariants = now.empty.isEmpty ? [] : [now.empty]
            return true
        case (let g as CPGridTemplate, .grid):
            g.updateTitle(now.title)
            g.updateGridButtons(gridButtons(now))
            g.trailingNavigationBarButtons = barButtons(now.actions)
            return true
        case (let tb as CPTabBarTemplate, .tabs):
            var ok = true
            for (j, child) in tb.templates.enumerated() where j < now.tabs.count && j < was.tabs.count {
                if !update(child, was.tabs[j], now.tabs[j]) { ok = false }
            }
            if !ok { tb.updateTemplates(now.tabs.map { tab($0) }) }
            return true
        case (let info as CPInformationTemplate, .info), (let info as CPInformationTemplate, .message):
            info.title = now.title
            info.items = informationItems(now)
            info.actions = textButtons(now.actions)
            return true
        case (let p as CPPointOfInterestTemplate, .places):
            p.title = now.title
            p.setPointsOfInterest(points(now, p), selectedIndex: NSNotFound)
            return true
        case (let m as CPMapTemplate, .navigation):
            guard let n = nav, n.template === m else { return false }
            n.update(now)
            return true
        case (_, .nowPlaying):
            return true
        default:
            return false
        }
    }

    /// Whether a and b show the same, their handlers' numbers too (their
    /// places on the stack aside).
    private func same(_ a: Shown, _ b: Shown) -> Bool {
        var x = a, y = b
        x.id = 0
        y.id = 0
        return describe(x) == describe(y)
    }

    private func describe(_ s: Shown) -> String { String(describing: s) }

    private func make(_ s: Shown) -> CPTemplate {
        switch s.kind {
        case .grid:
            let g = CPGridTemplate(title: s.title, gridButtons: gridButtons(s))
            g.trailingNavigationBarButtons = barButtons(s.actions)
            return g
        case .tabs:
            return CPTabBarTemplate(templates: s.tabs.map { tab($0) })
        case .info, .message:
            return CPInformationTemplate(title: s.title, layout: s.kind == .message ? .leading : .twoColumn, items: informationItems(s), actions: textButtons(s.actions))
        case .places:
            let p = CPPointOfInterestTemplate(title: s.title, pointsOfInterest: [], selectedIndex: NSNotFound)
            p.pointOfInterestDelegate = self
            p.setPointsOfInterest(points(s, p), selectedIndex: NSNotFound)
            return p
        case .nowPlaying:
            return CPNowPlayingTemplate.shared
        case .navigation where s.id == 1:
            let n = CarNavigation(s, map: mapController, chose: { [weak self] in self?.chose($0) }, answered: { [weak self] in self?.answered($0, $1) })
            nav = n
            return n.template
        case .navigation:
            // A map goes at the root only.
            return CPListTemplate(title: s.title, sections: [])
        default:
            let l = CPListTemplate(title: s.title, sections: sections(s))
            l.trailingNavigationBarButtons = barButtons(s.actions)
            if !s.empty.isEmpty { l.emptyViewTitleVariants = [s.empty] }
            return l
        }
    }

    /// A tab bar's tab: its template, with the tab's title and symbol.
    private func tab(_ s: Shown) -> CPTemplate {
        var inner = s
        if inner.kind == .tabs || inner.kind == .nowPlaying || inner.kind == .navigation { inner.kind = .list } // a tab bar holds neither
        let t = make(inner)
        t.tabTitle = s.text
        t.tabImage = UIImage(systemName: s.symbol.isEmpty ? "circle" : s.symbol)
        return t
    }

    // MARK: Parts

    private func sections(_ s: Shown) -> [CPListSection] {
        var out: [CPListSection] = []
        var left = Int(CPListTemplate.maximumItemCount)
        for sec in s.sections.prefix(Int(CPListTemplate.maximumSectionCount)) where left > 0 {
            let items = sec.items.prefix(left).map { listItem($0) }
            left -= items.count
            out.append(CPListSection(items: items, header: sec.title.isEmpty ? nil : sec.title, sectionIndexTitle: nil))
        }
        return out
    }

    private func listItem(_ i: ShownItem) -> CPListItem {
        let item = CPListItem(text: i.title, detailText: i.detail.isEmpty ? nil : i.detail, image: image(i.image, i.symbol))
        item.accessoryType = i.browsable ? .disclosureIndicator : .none
        item.isPlaying = i.playing
        if i.progress >= 0 { item.playbackProgress = CGFloat(i.progress) }
        item.isEnabled = i.enabled
        let run = i.run
        item.handler = { [weak self] _, done in
            MainActor.assumeIsolated { self?.chose(run) }
            done()
        }
        if !i.image.isEmpty, images[i.image] == nil {
            load(i.image) { [weak item] img in item?.setImage(img) }
        }
        return item
    }

    private func gridButtons(_ s: Shown) -> [CPGridButton] {
        let items = s.sections.flatMap { $0.items }.prefix(Int(CPGridTemplateMaximumItems))
        return items.map { i in
            let run = i.run
            let b = CPGridButton(titleVariants: [i.title], image: image(i.image, i.symbol.isEmpty ? "square.grid.2x2" : i.symbol) ?? UIImage()) { [weak self] _ in
                MainActor.assumeIsolated { self?.chose(run) }
            }
            b.isEnabled = i.enabled && run > 0
            if !i.image.isEmpty, images[i.image] == nil {
                load(i.image) { [weak self, weak b] img in
                    if #available(iOS 26.0, *) {
                        b?.updateImage(img)
                    } else {
                        self?.gridsChanged()
                    }
                }
            }
            return b
        }
    }

    private func barButtons(_ actions: [ShownAction]) -> [CPBarButton] {
        actions.prefix(2).map { a in
            let run = a.run
            let handler: (CPBarButton) -> Void = { [weak self] _ in MainActor.assumeIsolated { self?.chose(run) } }
            let b: CPBarButton
            if !a.symbol.isEmpty, let img = UIImage(systemName: a.symbol) {
                b = CPBarButton(image: img, handler: handler)
            } else {
                b = CPBarButton(title: a.title, handler: handler)
            }
            b.isEnabled = a.enabled && run > 0
            return b
        }
    }

    private func textButtons(_ actions: [ShownAction]) -> [CPTextButton] {
        actions.prefix(3).map { a in
            let run = a.run
            let style: CPTextButtonStyle = a.style == 1 ? .cancel : a.style == 2 ? .confirm : .normal
            let b = CPTextButton(title: a.title, textStyle: style) { [weak self] _ in
                MainActor.assumeIsolated { self?.chose(run) }
            }
            return b
        }
    }

    /// The grids shown, made again (an image came in: before iOS 26 a
    /// grid button's can't change).
    private func gridsChanged() {
        func refresh(_ t: CPTemplate, _ s: Shown) {
            if let g = t as? CPGridTemplate, s.kind == .grid { g.updateGridButtons(gridButtons(s)) }
            if let tb = t as? CPTabBarTemplate {
                for (child, sub) in zip(tb.templates, s.tabs) { refresh(child, sub) }
            }
        }
        for e in entries { refresh(e.template, e.shown) }
    }

    private func informationItems(_ s: Shown) -> [CPInformationItem] {
        if s.kind == .message {
            return [CPInformationItem(title: nil, detail: s.text)]
        }
        return s.fields.map { CPInformationItem(title: $0.title, detail: $0.detail.isEmpty ? nil : $0.detail) }
    }

    private func points(_ s: Shown, _ t: CPPointOfInterestTemplate) -> [CPPointOfInterest] {
        placeRuns[ObjectIdentifier(t)] = s.places.map { $0.run }
        return s.places.prefix(12).map { p in
            let item = MKMapItem(placemark: MKPlacemark(coordinate: CLLocationCoordinate2D(latitude: p.latitude, longitude: p.longitude)))
            item.name = p.title
            // Its distance in the user's units, then what's said of it.
            let far = p.distance >= 0 ? MKDistanceFormatter().string(fromDistance: p.distance) : nil
            let subtitle = [far, p.detail.isEmpty ? nil : p.detail].compactMap { $0 }.joined(separator: " · ")
            return CPPointOfInterest(location: item, title: p.title, subtitle: subtitle.isEmpty ? nil : subtitle, summary: nil,
                                     detailTitle: p.title, detailSubtitle: subtitle.isEmpty ? nil : subtitle, detailSummary: nil, pinImage: nil)
        }
    }

    // MARK: CPPointOfInterestTemplateDelegate

    nonisolated func pointOfInterestTemplate(_ t: CPPointOfInterestTemplate, didChangeMapRegion region: MKCoordinateRegion) {}

    nonisolated func pointOfInterestTemplate(_ t: CPPointOfInterestTemplate, didSelectPointOfInterest p: CPPointOfInterest) {
        MainActor.assumeIsolated {
            guard let i = t.pointsOfInterest.firstIndex(where: { $0 === p }), let runs = placeRuns[ObjectIdentifier(t)], i < runs.count else { return }
            chose(runs[i])
        }
    }

    // MARK: Images

    /// The image for source if it's in, else the symbol's.
    private func image(_ source: String, _ symbol: String) -> UIImage? {
        if !source.isEmpty, let img = images[source] { return img }
        if !symbol.isEmpty { return UIImage(systemName: symbol) }
        return nil
    }

    /// Loads an image (an asset, a file, an http(s) URL) off the main
    /// thread, then hands it to done (and keeps it for what's made later).
    private func load(_ source: String, _ done: @escaping @MainActor (UIImage) -> Void) {
        guard let url = AsukaAssets.url(source) else { return }
        URLSession.shared.dataTask(with: url) { data, _, _ in
            guard let data else { return }
            DispatchQueue.main.async {
                MainActor.assumeIsolated {
                    guard let img = UIImage(data: data) else { return }
                    CarHub.shared.images[source] = img
                    done(img)
                }
            }
        }.resume()
    }

    // MARK: CPInterfaceControllerDelegate

    /// A template went: if the driver took the app's away (the back
    /// button), Asuka hears how many are left.
    nonisolated func templateDidDisappear(_ t: CPTemplate, animated: Bool) {
        MainActor.assumeIsolated { wentBack() }
    }

    private func wentBack() {
        guard let c = controller else { return }
        let on = c.templates
        var left = 0
        while left < entries.count && on.contains(where: { $0 === entries[left].template }) { left += 1 }
        guard left < entries.count, left > 0 else { return }
        entries.removeLast(entries.count - left)
        for w in backWatchers.values { w(left) }
    }
}

@MainActor func makeHead() throws -> any Head {
    guard CarHub.configured else {
        throw AsukaNativeError.unsupported("no CarPlay scene: asuka.toml's [car] says the app's category")
    }
    return CarHub.shared
}
