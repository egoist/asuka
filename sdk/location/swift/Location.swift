// platform/location on iOS and macOS: Core Location. One hub on the main
// thread holds three managers: one for what the user allowed, one for
// positions on screen (current, watch), one for what goes on with the
// app out of sight (background updates, significant changes, regions).
// The background's events are kept (UserDefaults) until the app says
// it handled them, so a launch the system made for one, or one that
// ended before it was read, loses none.
//
// Apple TV allows when in use only, and gives one position at a time
// (requestLocation): no background updates or regions (the package
// computes those from the positions), and positions() asks again every
// few minutes, for a TV that stays where it is.
//
// Apple Watch has updates in the background (the location background
// mode, while the app runs) but no significant changes, regions or
// relaunches: significant updates are coarse ones, and the package
// computes the regions from the positions.
import Foundation
import CoreLocation
import Asuka
#if os(watchOS)
import WatchKit
#elseif canImport(UIKit)
import UIKit
#else
import AppKit
#endif

final class CoreLocator: Locator {
    func authorization() throws -> Authorization { onMain { Hub.shared.authorization } }

    func watchAuthorization(changed: @escaping (Authorization) -> Void) async throws {
        let send = Box(changed)
        let id = await MainActor.run { Hub.shared.onAuthorization(send) }
        defer { Task { @MainActor in Hub.shared.unlisten(id) } }
        await untilCancelled()
    }

    func request(always: Bool) async throws -> Authorization {
        await withCheckedContinuation { c in
            Task { @MainActor in Hub.shared.request(always: always) { c.resume(returning: $0) } }
        }
    }

    func requestPrecise(purpose: String) async throws -> Authorization {
        await withCheckedContinuation { c in
            Task { @MainActor in Hub.shared.requestPrecise(purpose) { c.resume(returning: $0) } }
        }
    }

    func current() async throws -> Fix {
        try await withCheckedThrowingContinuation { c in
            Task { @MainActor in Hub.shared.current { c.resume(with: $0) } }
        }
    }

    func watch(moved: @escaping (Fix) -> Void) async throws {
        let send = Box(moved)
        let id = try await withCheckedThrowingContinuation { (c: CheckedContinuation<Int, Error>) in
            Task { @MainActor in Hub.shared.watch(send) { c.resume(with: $0) } }
        }
        defer { Task { @MainActor in Hub.shared.unwatch(id) } }
        await untilCancelled()
    }

    func placeName(latitude: Double, longitude: Double) async throws -> String {
        let places = try await CLGeocoder().reverseGeocodeLocation(CLLocation(latitude: latitude, longitude: longitude))
        guard let p = places.first else { throw AsukaNativeError.unsupported("location: no name for this place") }
        var seen = Set<String>()
        return [p.subLocality ?? p.name, p.locality ?? p.administrativeArea ?? p.country].compactMap { $0 }
            .filter { seen.insert($0).inserted }.joined(separator: ", ")
    }

    @MainActor func openSettings() throws {
        #if os(watchOS)
        throw LocationError.unavailable // the watch's Settings have no page for an app
        #elseif canImport(UIKit)
        if let u = URL(string: UIApplication.openSettingsURLString) { UIApplication.shared.open(u) }
        #else
        if let u = URL(string: "x-apple.systempreferences:com.apple.preference.security?Privacy_LocationServices") { NSWorkspace.shared.open(u) }
        #endif
    }

    func support() throws -> Support {
        #if os(tvOS)
        return Support(updates: false, regions: false, maxRegions: 0, maxRadius: 0, dwell: false, relaunch: false)
        #elseif os(watchOS)
        return Support(updates: true, regions: false, maxRegions: 0, maxRadius: 0, dwell: false, relaunch: false)
        #else
        let regions = CLLocationManager.isMonitoringAvailable(for: CLCircularRegion.self)
        #if os(iOS)
        return Support(updates: true, regions: regions, maxRegions: 20, maxRadius: onMain { Hub.shared.maxRadius }, dwell: false, relaunch: true)
        #else
        return Support(updates: false, regions: regions, maxRegions: 20, maxRadius: onMain { Hub.shared.maxRadius }, dwell: false, relaunch: false)
        #endif
        #endif
    }

    func startUpdates(options: Updates) async throws {
        #if os(tvOS)
        throw LocationError.unavailable
        #endif
        let a = try await request(always: false)
        guard a.level == .whenInUse || a.level == .always else { throw LocationError.denied }
        await MainActor.run { Hub.shared.startUpdates(options) }
    }

    func stopUpdates() throws { onMain { Hub.shared.stopUpdates() } }

    func updating() async throws -> Bool { await MainActor.run { Hub.shared.updatesOn } }

    func monitor(region: Region) async throws {
        try await MainActor.run { try Hub.shared.monitor(region) }
    }

    func stopMonitoring(id: String) async throws {
        await MainActor.run { Hub.shared.stopMonitoring { $0 == id } }
    }

    func stopMonitoringAll() async throws {
        await MainActor.run { Hub.shared.stopMonitoring { _ in true } }
    }

    func monitored() async throws -> [Region] { await MainActor.run { Hub.shared.monitored } }

    func events(event: @escaping (Event) -> Void) async throws {
        let send = Box(event)
        let id = await MainActor.run { Hub.shared.onEvents(send) }
        defer { Task { @MainActor in Hub.shared.unlisten(id) } }
        await untilCancelled()
    }

    func hold(id: Int) throws { onMain { Hub.shared.hold(id) } }

    func done(id: Int) throws { onMain { Hub.shared.done(id) } }
}

/// A callback, called on the main thread.
final class Box<T>: @unchecked Sendable {
    let f: (T) -> Void
    init(_ f: @escaping (T) -> Void) { self.f = f }
}

func untilCancelled() async {
    while !Task.isCancelled {
        try? await Task.sleep(nanoseconds: 3_600_000_000_000)
    }
}

func onMain<T>(_ body: @MainActor () -> T) -> T {
    if Thread.isMainThread { return MainActor.assumeIsolated(body) }
    return DispatchQueue.main.sync { MainActor.assumeIsolated(body) }
}

@MainActor final class Hub: NSObject, CLLocationManagerDelegate {
    static let shared = Hub()
    private let access = CLLocationManager()
    private let fg = CLLocationManager()
    private let bg = CLLocationManager()
    private let defaults = UserDefaults.standard
    private var servicesOn = true
    private var answering: [() -> Void] = []
    private var resigned = false
    private var authReaders: [Int: Box<Authorization>] = [:]
    private var eventReaders: [Int: Box<Event>] = [:]
    private var watchers: [Int: Box<Fix>] = [:]
    private var currents: [(Result<Fix, Error>) -> Void] = []
    private var next = 0
    #if os(tvOS)
    private var watchTimer: Timer?
    #endif
    #if os(iOS)
    private var holds: [Int: UIBackgroundTaskIdentifier] = [:]
    #endif

    override init() {
        super.init()
        access.delegate = self
        fg.delegate = self
        fg.desiredAccuracy = kCLLocationAccuracyBest
        bg.delegate = self
        refreshServices()
        #if canImport(UIKit) || os(watchOS)
        let nc = NotificationCenter.default
        #if os(watchOS)
        let resign = WKApplication.willResignActiveNotification, active = WKApplication.didBecomeActiveNotification
        #else
        let resign = UIApplication.willResignActiveNotification, active = UIApplication.didBecomeActiveNotification
        #endif
        nc.addObserver(forName: resign, object: nil, queue: .main) { _ in
            MainActor.assumeIsolated { Hub.shared.resigned = true }
        }
        nc.addObserver(forName: active, object: nil, queue: .main) { _ in
            MainActor.assumeIsolated {
                let h = Hub.shared
                h.refreshServices()
                // A question was answered (or dismissed) without a change.
                if h.resigned { h.resigned = false; h.answered() }
            }
        }
        #endif
        // Updates started at an earlier launch go on (the system started
        // the app for them, perhaps).
        if let o = savedUpdates { apply(o) }
    }

    // MARK: What the user allowed

    #if os(tvOS) || os(watchOS)
    var maxRadius: Double { 0 }
    #else
    var maxRadius: Double { bg.maximumRegionMonitoringDistance }
    #endif

    private func refreshServices() {
        DispatchQueue.global().async {
            let on = CLLocationManager.locationServicesEnabled()
            DispatchQueue.main.async {
                MainActor.assumeIsolated {
                    let h = Hub.shared
                    if h.servicesOn != on { h.servicesOn = on; h.authorizationChanged() }
                }
            }
        }
    }

    var authorization: Authorization {
        let level: Level
        switch access.authorizationStatus {
        case .notDetermined: level = .notAsked
        case .denied: level = .denied
        case .restricted: level = .restricted
        #if !os(macOS)
        case .authorizedWhenInUse: level = .whenInUse
        #endif
        case .authorizedAlways: level = .always
        @unknown default: level = .denied
        }
        return Authorization(level: level, precise: access.accuracyAuthorization == .fullAccuracy, off: !servicesOn)
    }

    private var allowed: Bool { authorization.level == .whenInUse || authorization.level == .always }

    func onAuthorization(_ f: Box<Authorization>) -> Int {
        next += 1
        authReaders[next] = f
        f.f(authorization)
        return next
    }

    func unlisten(_ id: Int) {
        authReaders[id] = nil
        eventReaders[id] = nil
    }

    private func authorizationChanged() {
        let a = authorization
        for r in authReaders.values { r.f(a) }
    }

    private func answered() {
        let ws = answering
        answering = []
        for w in ws { w() }
    }

    /// Waits for the user's answer to a question just asked: the
    /// authorization's change, or the question going without one; a
    /// question that doesn't show (asked before) answers within a second.
    private func waitForAnswer(_ then: @escaping () -> Void) {
        answering.append(then)
        DispatchQueue.main.asyncAfter(deadline: .now() + 1) {
            MainActor.assumeIsolated {
                if !Hub.shared.resigned { Hub.shared.answered() }
            }
        }
    }

    func request(always: Bool, _ done: @escaping (Authorization) -> Void) {
        switch access.authorizationStatus {
        case .notDetermined:
            waitForAnswer { [unowned self] in
                if always && self.authorization.level == .whenInUse {
                    self.request(always: true, done)
                } else {
                    done(self.authorization)
                }
            }
            #if os(iOS) || os(tvOS) || os(watchOS)
            access.requestWhenInUseAuthorization()
            #else
            access.requestAlwaysAuthorization()
            #endif
        #if os(iOS) || os(watchOS)
        case .authorizedWhenInUse where always:
            waitForAnswer { [unowned self] in done(self.authorization) }
            access.requestAlwaysAuthorization()
        #endif
        default:
            done(authorization)
        }
    }

    func requestPrecise(_ purpose: String, _ done: @escaping (Authorization) -> Void) {
        guard allowed, access.accuracyAuthorization != .fullAccuracy else { done(authorization); return }
        access.requestTemporaryFullAccuracyAuthorization(withPurposeKey: purpose.isEmpty ? "default" : purpose) { _ in
            DispatchQueue.main.async { MainActor.assumeIsolated { done(Hub.shared.authorization) } }
        }
    }

    // MARK: Positions on screen

    /// Asks the first time; then whether it's allowed.
    private func authorize(_ then: @escaping (Bool) -> Void) {
        if access.authorizationStatus == .notDetermined {
            request(always: false) { [unowned self] _ in then(self.allowed) }
        } else {
            then(allowed)
        }
    }

    func current(_ done: @escaping (Result<Fix, Error>) -> Void) {
        authorize { [unowned self] ok in
            guard ok else { done(.failure(LocationError.denied)); return }
            self.currents.append(done)
            self.fg.requestLocation()
        }
    }

    func watch(_ f: Box<Fix>, _ started: @escaping (Result<Int, Error>) -> Void) {
        authorize { [unowned self] ok in
            guard ok else { started(.failure(LocationError.denied)); return }
            self.next += 1
            self.watchers[self.next] = f
            #if os(tvOS)
            // One position at a time on tvOS: now, and again every five
            // minutes while someone watches.
            if self.watchers.count == 1 {
                self.fg.requestLocation()
                self.watchTimer = Timer.scheduledTimer(withTimeInterval: 300, repeats: true) { _ in
                    MainActor.assumeIsolated { Hub.shared.fg.requestLocation() }
                }
            }
            #else
            if self.watchers.count == 1 { self.fg.startUpdatingLocation() }
            #endif
            if let l = self.fg.location { f.f(Self.position(l)) }
            started(.success(self.next))
        }
    }

    func unwatch(_ id: Int) {
        guard watchers.removeValue(forKey: id) != nil else { return }
        #if os(tvOS)
        if watchers.isEmpty { watchTimer?.invalidate(); watchTimer = nil }
        #else
        if watchers.isEmpty { fg.stopUpdatingLocation() }
        #endif
    }

    static func position(_ l: CLLocation) -> Fix {
        Fix(latitude: l.coordinate.latitude, longitude: l.coordinate.longitude, accuracy: l.horizontalAccuracy,
                 altitude: l.verticalAccuracy >= 0 ? l.altitude : 0, speed: l.speedAccuracy >= 0 ? l.speed : -1,
                 course: l.courseAccuracy >= 0 ? l.course : -1, time: Int(l.timestamp.timeIntervalSince1970 * 1e9))
    }

    // MARK: Background updates

    private var savedUpdates: Updates? {
        guard let d = defaults.dictionary(forKey: "asuka.location.updates") else { return nil }
        return Updates(accuracy: Accuracy(rawValue: d["accuracy"] as? Int ?? 0), distance: d["distance"] as? Double ?? 0,
                       activity: Activity(rawValue: d["activity"] as? Int ?? 0), delay: d["delay"] as? Double ?? 0,
                       significant: d["significant"] as? Bool ?? false, autoPause: d["autoPause"] as? Bool ?? false,
                       indicator: d["indicator"] as? Bool ?? false)
    }

    var updatesOn: Bool { savedUpdates != nil }

    func startUpdates(_ o: Updates) {
        defaults.set(["accuracy": o.accuracy.rawValue, "distance": o.distance, "activity": o.activity.rawValue, "delay": o.delay,
                      "significant": o.significant, "autoPause": o.autoPause, "indicator": o.indicator], forKey: "asuka.location.updates")
        apply(o)
    }

    private func apply(_ o: Updates) {
        #if os(watchOS)
        bg.stopUpdatingLocation()
        // No significant changes on the watch: coarse updates, every 500
        // meters or so, as theirs come.
        let accuracy = o.significant ? .low : o.accuracy
        switch accuracy {
        case .navigation: bg.desiredAccuracy = kCLLocationAccuracyBestForNavigation
        case .balanced: bg.desiredAccuracy = kCLLocationAccuracyHundredMeters
        case .low: bg.desiredAccuracy = kCLLocationAccuracyKilometer
        case .passive: bg.desiredAccuracy = kCLLocationAccuracyThreeKilometers
        default: bg.desiredAccuracy = kCLLocationAccuracyBest
        }
        bg.distanceFilter = o.significant ? 500 : o.distance > 0 ? o.distance : kCLDistanceFilterNone
        switch o.activity {
        case .automotive: bg.activityType = .automotiveNavigation
        case .otherNavigation: bg.activityType = .otherNavigation
        case .fitness: bg.activityType = .fitness
        case .airborne: bg.activityType = .airborne
        default: bg.activityType = .other
        }
        bg.allowsBackgroundLocationUpdates = true
        bg.startUpdatingLocation()
        #elseif !os(tvOS)
        bg.stopUpdatingLocation()
        bg.stopMonitoringSignificantLocationChanges()
        if o.significant {
            bg.startMonitoringSignificantLocationChanges()
            return
        }
        switch o.accuracy {
        case .navigation: bg.desiredAccuracy = kCLLocationAccuracyBestForNavigation
        case .balanced: bg.desiredAccuracy = kCLLocationAccuracyHundredMeters
        case .low: bg.desiredAccuracy = kCLLocationAccuracyKilometer
        case .passive: bg.desiredAccuracy = kCLLocationAccuracyThreeKilometers
        default: bg.desiredAccuracy = kCLLocationAccuracyBest
        }
        bg.distanceFilter = o.distance > 0 ? o.distance : kCLDistanceFilterNone
        #if os(iOS)
        switch o.activity {
        case .automotive: bg.activityType = .automotiveNavigation
        case .otherNavigation: bg.activityType = .otherNavigation
        case .fitness: bg.activityType = .fitness
        case .airborne: bg.activityType = .airborne
        default: bg.activityType = .other
        }
        bg.pausesLocationUpdatesAutomatically = o.autoPause
        bg.allowsBackgroundLocationUpdates = true
        bg.showsBackgroundLocationIndicator = o.indicator
        #endif
        bg.startUpdatingLocation()
        #endif
    }

    func stopUpdates() {
        defaults.removeObject(forKey: "asuka.location.updates")
        #if os(watchOS)
        bg.stopUpdatingLocation()
        #elseif !os(tvOS)
        bg.stopUpdatingLocation()
        bg.stopMonitoringSignificantLocationChanges()
        #endif
    }

    // MARK: Regions

    func monitor(_ r: Region) throws {
        #if os(tvOS) || os(watchOS)
        throw LocationError.unavailable
        #else
        #if os(iOS)
        guard authorization.level == .always else { throw LocationError.needsAlways }
        #endif
        let existing = bg.monitoredRegions.filter { $0.identifier != r.id }
        guard existing.count < 20 else { throw LocationError.tooManyRegions }
        let c = CLCircularRegion(center: CLLocationCoordinate2D(latitude: r.latitude, longitude: r.longitude),
                                 radius: min(r.radius, bg.maximumRegionMonitoringDistance), identifier: r.id)
        c.notifyOnEntry = r.events.contains(.enter)
        c.notifyOnExit = r.events.contains(.exit)
        bg.startMonitoring(for: c)
        // An enter if the device is inside as monitoring starts.
        var asked = defaults.stringArray(forKey: "asuka.location.asked") ?? []
        if !asked.contains(r.id) { asked.append(r.id) }
        defaults.set(asked, forKey: "asuka.location.asked")
        bg.requestState(for: c)
        var saved = defaults.dictionary(forKey: "asuka.location.regions") as? [String: [String: Any]] ?? [:]
        saved[r.id] = ["events": r.events.map(\.rawValue), "dwell": r.dwell]
        defaults.set(saved, forKey: "asuka.location.regions")
        #endif
    }

    func stopMonitoring(_ which: (String) -> Bool) {
        #if !os(tvOS) && !os(watchOS)
        var saved = defaults.dictionary(forKey: "asuka.location.regions") as? [String: [String: Any]] ?? [:]
        for r in bg.monitoredRegions where which(r.identifier) {
            bg.stopMonitoring(for: r)
            saved[r.identifier] = nil
        }
        defaults.set(saved, forKey: "asuka.location.regions")
        #endif
    }

    var monitored: [Region] {
        #if os(tvOS) || os(watchOS)
        return []
        #else
        let saved = defaults.dictionary(forKey: "asuka.location.regions") as? [String: [String: Any]] ?? [:]
        return bg.monitoredRegions.compactMap { r in
            guard let c = r as? CLCircularRegion else { return nil }
            let events = (saved[r.identifier]?["events"] as? [Int])?.map { Transition(rawValue: $0) } ?? [.enter, .exit]
            return Region(id: r.identifier, latitude: c.center.latitude, longitude: c.center.longitude, radius: c.radius,
                          events: events, dwell: saved[r.identifier]?["dwell"] as? Double ?? 300)
        }
        #endif
    }

    // MARK: Events, kept until done

    private var kept: [[String: Any]] {
        get { defaults.array(forKey: "asuka.location.events") as? [[String: Any]] ?? [] }
        set { defaults.set(newValue, forKey: "asuka.location.events") }
    }

    private static func encode(_ p: Fix) -> [String: Any] {
        ["lat": p.latitude, "lon": p.longitude, "acc": p.accuracy, "alt": p.altitude, "spd": p.speed, "crs": p.course, "t": p.time]
    }

    private static func decode(_ d: [String: Any]?) -> Fix {
        guard let d else { return Fix() }
        return Fix(latitude: d["lat"] as? Double ?? 0, longitude: d["lon"] as? Double ?? 0, accuracy: d["acc"] as? Double ?? 0,
                        altitude: d["alt"] as? Double ?? 0, speed: d["spd"] as? Double ?? -1, course: d["crs"] as? Double ?? -1, time: d["t"] as? Int ?? 0)
    }

    private static func event(_ d: [String: Any]) -> Event {
        Event(id: d["id"] as? Int ?? 0, positions: (d["positions"] as? [[String: Any]] ?? []).map { decode($0) },
              crossing: d["crossing"] as? Bool ?? false, region: d["region"] as? String ?? "",
              kind: Transition(rawValue: d["kind"] as? Int ?? 0), at: decode(d["at"] as? [String: Any]), time: d["time"] as? Int ?? 0)
    }

    private func add(_ d: [String: Any]) {
        let id = defaults.integer(forKey: "asuka.location.nextEvent") + 1
        defaults.set(id, forKey: "asuka.location.nextEvent")
        var e = d
        e["id"] = id
        kept.append(e)
        // The system gives the app a few seconds for it.
        hold(id)
        DispatchQueue.main.asyncAfter(deadline: .now() + 5) { MainActor.assumeIsolated { Hub.shared.release(id) } }
        let ev = Self.event(e)
        for r in eventReaders.values { r.f(ev) }
    }

    func onEvents(_ f: Box<Event>) -> Int {
        next += 1
        eventReaders[next] = f
        for e in kept { f.f(Self.event(e)) }
        return next
    }

    func hold(_ id: Int) {
        #if os(iOS)
        guard holds[id] == nil else { return }
        holds[id] = UIApplication.shared.beginBackgroundTask(withName: "asuka.location") {
            MainActor.assumeIsolated { Hub.shared.release(id) }
        }
        #endif
    }

    private func release(_ id: Int) {
        #if os(iOS)
        if let t = holds.removeValue(forKey: id) { UIApplication.shared.endBackgroundTask(t) }
        #endif
    }

    func done(_ id: Int) {
        kept = kept.filter { ($0["id"] as? Int) != id }
        release(id)
    }

    // MARK: The managers' delegate

    nonisolated func locationManagerDidChangeAuthorization(_ m: CLLocationManager) {
        MainActor.assumeIsolated {
            guard m === access else { return }
            authorizationChanged()
            if m.authorizationStatus != .notDetermined { answered() }
        }
    }

    nonisolated func locationManager(_ m: CLLocationManager, didUpdateLocations locations: [CLLocation]) {
        MainActor.assumeIsolated {
            if m === bg {
                guard updatesOn, !locations.isEmpty else { return }
                add(["positions": locations.map { Self.encode(Self.position($0)) }])
                return
            }
            guard let l = locations.last else { return }
            let p = Self.position(l)
            let cs = currents
            currents = []
            for c in cs { c(.success(p)) }
            for w in watchers.values { w.f(p) }
        }
    }

    nonisolated func locationManager(_ m: CLLocationManager, didFailWithError error: Error) {
        MainActor.assumeIsolated {
            guard m === fg else { return }
            // Not known yet: Core Location keeps trying.
            if (error as? CLError)?.code == .locationUnknown { return }
            let e: Error = (error as? CLError)?.code == .denied ? LocationError.denied : error
            let cs = currents
            currents = []
            for c in cs { c(.failure(e)) }
        }
    }

    private func crossed(_ r: CLRegion, _ kind: Transition) {
        let saved = defaults.dictionary(forKey: "asuka.location.regions") as? [String: [String: Any]] ?? [:]
        let events = (saved[r.identifier]?["events"] as? [Int]) ?? [Transition.enter.rawValue, Transition.exit.rawValue]
        guard events.contains(kind.rawValue) else { return }
        var e: [String: Any] = ["crossing": true, "region": r.identifier, "kind": kind.rawValue, "time": Int(Date().timeIntervalSince1970 * 1e9)]
        if let l = bg.location { e["at"] = Self.encode(Self.position(l)) }
        add(e)
    }

    #if !os(tvOS) && !os(watchOS)
    nonisolated func locationManager(_ m: CLLocationManager, didEnterRegion region: CLRegion) {
        MainActor.assumeIsolated { crossed(region, .enter) }
    }

    nonisolated func locationManager(_ m: CLLocationManager, didExitRegion region: CLRegion) {
        MainActor.assumeIsolated { crossed(region, .exit) }
    }

    nonisolated func locationManager(_ m: CLLocationManager, didDetermineState state: CLRegionState, for region: CLRegion) {
        // The state requested as monitoring starts: inside is an enter.
        MainActor.assumeIsolated {
            var asked = defaults.stringArray(forKey: "asuka.location.asked") ?? []
            guard let i = asked.firstIndex(of: region.identifier) else { return }
            asked.remove(at: i)
            defaults.set(asked, forKey: "asuka.location.asked")
            if state == .inside { crossed(region, .enter) }
        }
    }
    #endif
}

func makeLocator() throws -> any Locator { CoreLocator() }

#if os(iOS)
/// The background part's managers as the app launches (the manifest's
/// ios.launch, for an app that calls startUpdates or monitor): iOS
/// relaunches the app for a significant change or a region and hands
/// the event to a manager made before launch ends. The hub keeps it until
/// the app reads it; for such a launch the app's code starts at once,
/// without a screen.
@objc(AsukaLocationLaunch) final class LocationLaunch: NSObject, AsukaLaunching {
    @MainActor static func launch(_ options: [UIApplication.LaunchOptionsKey: Any]) -> Bool {
        _ = Hub.shared
        return options[.location] != nil
    }
}
#endif
