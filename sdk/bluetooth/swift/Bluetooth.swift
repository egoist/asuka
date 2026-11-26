// platform/bluetooth on iOS and macOS: Core Bluetooth. One central manager
// and one peripheral manager, made as the app first needs them (making
// the central asks for the permission), their delegates on the main
// queue. Scans share the central's one scan; a connection holds its
// peripheral by handle; reads and notifications both come as
// didUpdateValue: a read waiting takes it, else the subscriptions do.
//
// On iOS, with the app's bluetooth-central or bluetooth-peripheral
// background mode, the managers have restore identifiers: iOS keeps their
// connections and scan when it ends the app, and starts the app again for
// them, handing them to managers made before launch ends (the launch
// class, AsukaBluetoothLaunch). What it restored waits for watchRestored.
import Foundation
import CoreBluetooth
import Asuka
#if os(iOS)
import UIKit
#endif

final class CoreBluetoothAdapter: Adapter {
    func state() throws -> State { onMain { Hub.shared.state } }

    func watchState(changed: @escaping (State) -> Void) async throws {
        let f = Box(changed)
        let id = await MainActor.run { Hub.shared.listenState(f) }
        defer { Task { @MainActor in Hub.shared.unlisten(id) } }
        await untilCancelled()
    }

    func request() async throws -> State {
        await withCheckedContinuation { c in
            Task { @MainActor in Hub.shared.request { c.resume(returning: $0) } }
        }
    }

    func scan(options: ScanOptions, found: @escaping (Heard) -> Void) async throws {
        let f = Box(found)
        let id = try await MainActor.run { try Hub.shared.startScan(options, f) }
        defer { Task { @MainActor in Hub.shared.stopScan(id) } }
        await untilCancelled()
    }

    func choose(filters: [Filter], optionalServices: [String]) async throws -> Device { throw BluetoothError.notAvailable }

    func connect(device: Device, options: ConnectOptions, changed: @escaping (Link) -> Void) async throws {
        let f = Box(changed)
        let (h, ended) = try await MainActor.run { try Hub.shared.connect(device, options, f) }
        defer { Task { @MainActor in Hub.shared.disconnect(h) } }
        // Until the task is cancelled, or the connection is lost.
        while !Task.isCancelled {
            if await MainActor.run(body: { ended.done }) { return }
            try? await Task.sleep(nanoseconds: 200_000_000)
        }
    }

    func services(connection: Int) async throws -> [FoundService] {
        try await withCheckedThrowingContinuation { c in
            Task { @MainActor in Hub.shared.services(connection) { c.resume(with: $0) } }
        }
    }

    func mtu(connection: Int) async throws -> Int {
        try await MainActor.run { try Hub.shared.mtu(connection) }
    }

    func rssi(connection: Int) async throws -> Int {
        try await withCheckedThrowingContinuation { c in
            Task { @MainActor in Hub.shared.rssi(connection) { c.resume(with: $0) } }
        }
    }

    func read(connection: Int, service: String, characteristic: String) async throws -> Data {
        try await withCheckedThrowingContinuation { c in
            Task { @MainActor in Hub.shared.read(connection, service, characteristic) { c.resume(with: $0) } }
        }
    }

    func write(connection: Int, service: String, characteristic: String, value: Data, response: Bool) async throws {
        try await withCheckedThrowingContinuation { (c: CheckedContinuation<Void, Error>) in
            Task { @MainActor in Hub.shared.write(connection, service, characteristic, value, response) { c.resume(with: $0) } }
        }
    }

    func subscribe(connection: Int, service: String, characteristic: String, value: @escaping (Data) -> Void) async throws {
        let f = Box(value)
        let id = try await withCheckedThrowingContinuation { (c: CheckedContinuation<Int, Error>) in
            Task { @MainActor in Hub.shared.subscribe(connection, service, characteristic, f) { c.resume(with: $0) } }
        }
        defer { Task { @MainActor in Hub.shared.unsubscribe(id) } }
        await untilCancelled()
    }

    func serve(services: [ServedService], request: @escaping (Request) -> Void) async throws {
        let f = Box(request)
        try await withCheckedThrowingContinuation { (c: CheckedContinuation<Void, Error>) in
            Task { @MainActor in Hub.shared.serve(services, f) { c.resume(with: $0) } }
        }
        defer { Task { @MainActor in Hub.shared.stopServing() } }
        await untilCancelled()
    }

    func respond(request: Int, value: Data, error: Int) throws {
        Task { @MainActor in Hub.shared.respond(request, value, error) }
    }

    func notify(service: String, characteristic: String, value: Data) throws -> Int {
        onMain { Hub.shared.notify(characteristic, value) }
    }

    func advertise(options: Advertising) async throws {
        try await withCheckedThrowingContinuation { (c: CheckedContinuation<Void, Error>) in
            Task { @MainActor in Hub.shared.advertise(options) { c.resume(with: $0) } }
        }
        defer { Task { @MainActor in Hub.shared.stopAdvertising() } }
        await untilCancelled()
    }

    func watchRestored(restored: @escaping (Brought) -> Void) async throws {
        #if os(iOS)
        let f = Box(restored)
        let id = await MainActor.run { Hub.shared.listenRestored(f) }
        defer { Task { @MainActor in Hub.shared.unlistenRestored(id) } }
        await untilCancelled()
        #endif
        // macOS doesn't start an app for Bluetooth: nothing is restored.
    }

    func stopBackgroundScan() throws {
        Task { @MainActor in Hub.shared.stopBackgroundScan() }
    }
}

/// A callback, called on the main thread.
final class Box<T>: @unchecked Sendable {
    let f: (T) -> Void
    init(_ f: @escaping (T) -> Void) { self.f = f }
}

/// Whether a connection ended, read on the main thread.
final class Ended: @unchecked Sendable { var done = false }

func untilCancelled() async {
    while !Task.isCancelled {
        try? await Task.sleep(nanoseconds: 3_600_000_000_000)
    }
}

func onMain<T>(_ body: @MainActor () -> T) -> T {
    if Thread.isMainThread { return MainActor.assumeIsolated(body) }
    return DispatchQueue.main.sync { MainActor.assumeIsolated(body) }
}

/// A UUID as the package writes it: lowercase, a SIG one in four digits.
func uuidString(_ u: CBUUID) -> String { u.uuidString.lowercased() }

@MainActor final class Hub: NSObject, CBCentralManagerDelegate, CBPeripheralDelegate, CBPeripheralManagerDelegate {
    static let shared = Hub()

    /// The managers' restore identifiers, which iOS's launch options name.
    static let centralRestoreID = "dev.asuka.bluetooth.central"
    static let peripheralRestoreID = "dev.asuka.bluetooth.peripheral"
    private static let backgroundModes = Bundle.main.object(forInfoDictionaryKey: "UIBackgroundModes") as? [String] ?? []

    private var centralManager: CBCentralManager?
    private var central: CBCentralManager { centralManager ?? makeCentral() }
    private var peripheralManager: CBPeripheralManager?
    #if !os(tvOS)
    private var pm: CBPeripheralManager { peripheralManager ?? makePeripheralManager() }
    #endif

    /// The central manager, made once; with its restore identifier only
    /// when the app has the bluetooth-central mode (one without it is
    /// Core Bluetooth's API misuse). alert: iOS's alert when Bluetooth is
    /// off (not for a launch in the background).
    @discardableResult func makeCentral(alert: Bool = true) -> CBCentralManager {
        if let c = centralManager { return c }
        var o: [String: Any] = [:]
        if !alert { o[CBCentralManagerOptionShowPowerAlertKey] = false }
        #if os(iOS)
        if Self.backgroundModes.contains("bluetooth-central") { o[CBCentralManagerOptionRestoreIdentifierKey] = Self.centralRestoreID }
        #endif
        let c = CBCentralManager(delegate: self, queue: .main, options: o)
        centralManager = c
        return c
    }

    #if !os(tvOS)
    /// The peripheral manager, made once; with its restore identifier only
    /// with the bluetooth-peripheral mode. Apple TV is a central only.
    @discardableResult func makePeripheralManager() -> CBPeripheralManager {
        if let p = peripheralManager { return p }
        var o: [String: Any] = [:]
        #if os(iOS)
        if Self.backgroundModes.contains("bluetooth-peripheral") { o[CBPeripheralManagerOptionRestoreIdentifierKey] = Self.peripheralRestoreID }
        #endif
        let p = CBPeripheralManager(delegate: self, queue: .main, options: o)
        peripheralManager = p
        return p
    }
    #endif

    // What iOS restored, until watchRestored reads it, and who does.
    private var restoredNow: Brought?
    private var restoreReaders: [Int: Box<Brought>] = [:]
    // The scan iOS restored, which goes on (what it hears restored too)
    // until the app scans again or stops it (stopBackgroundScan).
    private var restoredScan: (services: [CBUUID], duplicates: Bool)?
    /// At most this many advertisements wait for a reader.
    private static let keep = 1000

    private var stateReaders: [Int: Box<State>] = [:]
    private var stateWaiters: [(State) -> Void] = []
    private var scanners: [Int: (ScanOptions, Box<Heard>)] = [:]
    private var known: [UUID: CBPeripheral] = [:]
    private var seen = Set<UUID>()

    private final class Conn {
        let peripheral: CBPeripheral
        let changed: Box<Link>
        let ended: Ended
        var services: [CBUUID: CBService] = [:]
        var discovering: [(Swift.Result<[FoundService], Error>) -> Void] = []
        var left = 0
        var reads: [CBCharacteristic: [(Swift.Result<Data, Error>) -> Void]] = [:]
        var writes: [CBCharacteristic: [(Swift.Result<Void, Error>) -> Void]] = [:]
        var notifying: [CBCharacteristic: (Swift.Result<Void, Error>) -> Void] = [:]
        var rssi: [(Swift.Result<Int, Error>) -> Void] = []
        var timeout: DispatchWorkItem?
        init(_ p: CBPeripheral, _ c: Box<Link>, _ e: Ended) { peripheral = p; changed = c; ended = e }
    }

    private var conns: [Int: Conn] = [:]
    private var subs: [Int: (Int, CBCharacteristic, Box<Data>)] = [:]
    private var next = 0

    // The peripheral's side.
    private var serving: Box<Request>?
    private var servedChars: [CBUUID: CBMutableCharacteristic] = [:]
    private var pendingAdds = 0
    private var serveDone: ((Swift.Result<Void, Error>) -> Void)?
    private var requests: [Int: CBATTRequest] = [:]
    private var advertiseDone: ((Swift.Result<Void, Error>) -> Void)?
    private var pmWaiters: [() -> Void] = []

    // MARK: The adapter

    var state: State {
        if CBManager.authorization == .denied || CBManager.authorization == .restricted { return .unauthorized }
        guard let c = centralManager else { return CBManager.authorization == .notDetermined ? .unknown : centralState() }
        return Self.state(c.state)
    }

    /// The central's state, made to know it.
    private func centralState() -> State { Self.state(central.state) }

    static func state(_ s: CBManagerState) -> State {
        switch s {
        case .poweredOn: return .on
        case .poweredOff: return .off
        case .unauthorized: return .unauthorized
        case .unsupported: return .unsupported
        case .resetting: return .resetting
        default: return .unknown
        }
    }

    func listenState(_ f: Box<State>) -> Int {
        next += 1
        stateReaders[next] = f
        _ = central
        f.f(state)
        return next
    }

    func unlisten(_ id: Int) { stateReaders[id] = nil }

    func request(_ done: @escaping (State) -> Void) {
        let c = central
        if c.state != .unknown { done(state); return }
        stateWaiters.append(done)
    }

    nonisolated func centralManagerDidUpdateState(_ c: CBCentralManager) {
        MainActor.assumeIsolated {
            let s = state
            for r in stateReaders.values { r.f(s) }
            let ws = stateWaiters
            stateWaiters = []
            for w in ws { w(s) }
            if c.state == .poweredOn { rescan() }
            if c.state != .poweredOn {
                for (h, conn) in conns { lost(h, conn, "bluetooth: disconnected: Bluetooth is \(s)") }
            }
        }
    }

    private func check() throws {
        switch state {
        case .on: return
        case .off: throw BluetoothError.off
        case .unauthorized: throw BluetoothError.denied
        case .unsupported: throw BluetoothError.unsupported
        default: return // not known yet: Core Bluetooth starts it once on
        }
    }

    // MARK: Scanning

    func startScan(_ o: ScanOptions, _ f: Box<Heard>) throws -> Int {
        _ = central
        try check()
        next += 1
        scanners[next] = (o, f)
        rescan()
        return next
    }

    func stopScan(_ id: Int) {
        scanners[id] = nil
        rescan()
    }

    private func rescan() {
        guard let c = centralManager, c.state == .poweredOn else { return }
        if scanners.isEmpty, let r = restoredScan {
            // iOS's restored scan, for the app to take over.
            if !c.isScanning { c.scanForPeripherals(withServices: r.services, options: [CBCentralManagerScanOptionAllowDuplicatesKey: r.duplicates]) }
            return
        }
        c.stopScan()
        guard !scanners.isEmpty else { return }
        restoredScan = nil
        seen = []
        // Every scanner's services, or all: filtered again per scanner.
        let any = scanners.values.contains { $0.0.services.isEmpty }
        let services = any ? nil : Array(Set(scanners.values.flatMap { $0.0.services })).map { CBUUID(string: $0) }
        let dup = scanners.values.contains { $0.0.duplicates }
        c.scanForPeripherals(withServices: services, options: [CBCentralManagerScanOptionAllowDuplicatesKey: dup])
    }

    nonisolated func centralManager(_ c: CBCentralManager, didDiscover p: CBPeripheral, advertisementData d: [String: Any], rssi: NSNumber) {
        MainActor.assumeIsolated {
            known[p.identifier] = p
            let first = seen.insert(p.identifier).inserted
            var a = Heard(device: Device(id: p.identifier.uuidString, name: p.name ?? (d[CBAdvertisementDataLocalNameKey] as? String ?? "")),
                                  rssi: rssi.intValue, localName: d[CBAdvertisementDataLocalNameKey] as? String ?? "",
                                  services: (d[CBAdvertisementDataServiceUUIDsKey] as? [CBUUID] ?? []).map(uuidString),
                                  solicited: (d[CBAdvertisementDataSolicitedServiceUUIDsKey] as? [CBUUID] ?? []).map(uuidString),
                                  connectable: (d[CBAdvertisementDataIsConnectable] as? NSNumber)?.boolValue ?? true,
                                  time: Int(Date().timeIntervalSince1970 * 1e9))
            for (k, v) in d[CBAdvertisementDataServiceDataKey] as? [CBUUID: Data] ?? [:] { a.serviceData[uuidString(k)] = v }
            if let m = d[CBAdvertisementDataManufacturerDataKey] as? Data, m.count >= 2 {
                let company = Int(m[m.startIndex]) | Int(m[m.startIndex + 1]) << 8
                a.manufacturerData[String(format: "%04x", company)] = m.subdata(in: m.startIndex + 2..<m.endIndex)
            }
            if let t = d[CBAdvertisementDataTxPowerLevelKey] as? NSNumber {
                a.txPower = t.intValue
                a.hasTxPower = true
            }
            for (o, f) in scanners.values {
                if !o.duplicates && !first { continue }
                if !o.services.isEmpty && !o.services.contains(where: { a.services.contains($0) }) { continue }
                f.f(a)
            }
            // The restored scan's, before the app scans again.
            if scanners.isEmpty, let r = restoredScan, r.duplicates || first {
                deliverRestored(Brought(advertisements: [a]))
            }
        }
    }

    func stopBackgroundScan() {
        guard restoredScan != nil else { return }
        restoredScan = nil
        if scanners.isEmpty, let c = centralManager, c.state == .poweredOn { c.stopScan() }
    }

    // MARK: Restoration

    func listenRestored(_ f: Box<Brought>) -> Int {
        next += 1
        restoreReaders[next] = f
        if let r = restoredNow {
            restoredNow = nil
            f.f(r)
        }
        return next
    }

    func unlistenRestored(_ id: Int) { restoreReaders[id] = nil }

    /// To the readers, or kept (merged) for the first.
    private func deliverRestored(_ r: Brought) {
        guard restoreReaders.isEmpty else {
            for f in restoreReaders.values { f.f(r) }
            return
        }
        var k = restoredNow ?? Brought()
        for d in r.devices where !k.devices.contains(where: { $0.id == d.id }) { k.devices.append(d) }
        if !r.scanServices.isEmpty { k.scanServices = r.scanServices }
        for a in r.advertisements {
            if !(restoredScan?.duplicates ?? false) { k.advertisements.removeAll { $0.device.id == a.device.id } }
            k.advertisements.append(a)
        }
        if k.advertisements.count > Self.keep { k.advertisements.removeFirst(k.advertisements.count - Self.keep) }
        restoredNow = k
    }

    #if os(iOS)
    /// iOS brought back the connections and the scan it kept for the app
    /// (before centralManagerDidUpdateState): its peripherals are the
    /// package's again, its scan goes on.
    nonisolated func centralManager(_ c: CBCentralManager, willRestoreState dict: [String: Any]) {
        MainActor.assumeIsolated {
            var r = Brought()
            for p in dict[CBCentralManagerRestoredStatePeripheralsKey] as? [CBPeripheral] ?? [] {
                known[p.identifier] = p
                p.delegate = self
                r.devices.append(Device(id: p.identifier.uuidString, name: p.name ?? ""))
            }
            if let s = dict[CBCentralManagerRestoredStateScanServicesKey] as? [CBUUID], !s.isEmpty {
                r.scanServices = s.map(uuidString)
                let o = dict[CBCentralManagerRestoredStateScanOptionsKey] as? [String: Any]
                restoredScan = (s, (o?[CBCentralManagerScanOptionAllowDuplicatesKey] as? NSNumber)?.boolValue ?? false)
            }
            guard !r.devices.isEmpty || !r.scanServices.isEmpty else { return }
            deliverRestored(r)
        }
    }

    /// iOS brought back the services and the advertising the app's last
    /// run published: they go on until the app serves or advertises again
    /// (serve replaces them).
    nonisolated func peripheralManager(_ p: CBPeripheralManager, willRestoreState dict: [String: Any]) {}
    #endif

    // MARK: Connections

    private func peripheral(_ d: Device) -> CBPeripheral? {
        guard let id = UUID(uuidString: d.id) else { return nil }
        if let p = known[id] { return p }
        return central.retrievePeripherals(withIdentifiers: [id]).first
    }

    func connect(_ d: Device, _ o: ConnectOptions, _ f: Box<Link>) throws -> (Int, Ended) {
        _ = central
        try check()
        guard let p = peripheral(d) else { throw BluetoothError.notFound }
        known[p.identifier] = p
        next += 1
        let ended = Ended()
        let conn = Conn(p, f, ended)
        conns[next] = conn
        p.delegate = self
        f.f(Link(state: .connecting))
        // Connected already: iOS kept it (restored), or another connection
        // of the app's has it.
        if p.state == .connected {
            f.f(Link(state: .connected, handle: next))
            return (next, ended)
        }
        central.connect(p, options: nil)
        if o.timeout > 0 {
            let h = next
            let w = DispatchWorkItem { [weak self] in
                MainActor.assumeIsolated {
                    guard let self, let c = self.conns[h], c.peripheral.state != .connected else { return }
                    self.central.cancelPeripheralConnection(c.peripheral)
                    self.lost(h, c, "bluetooth: timeout")
                }
            }
            conn.timeout = w
            DispatchQueue.main.asyncAfter(deadline: .now() + o.timeout, execute: w)
        }
        return (next, ended)
    }

    func disconnect(_ h: Int) {
        guard let c = conns.removeValue(forKey: h) else { return }
        c.timeout?.cancel()
        if !conns.values.contains(where: { $0.peripheral === c.peripheral }) { centralManager?.cancelPeripheralConnection(c.peripheral) }
        subs = subs.filter { $0.value.0 != h }
    }

    private func lost(_ h: Int, _ c: Conn, _ why: String) {
        guard !c.ended.done else { return }
        c.ended.done = true
        c.timeout?.cancel()
        c.changed.f(Link(state: .disconnected, error: why))
        let e = BluetoothError.disconnected
        for d in c.discovering { d(.failure(e)) }
        for (_, rs) in c.reads { for r in rs { r(.failure(e)) } }
        for (_, ws) in c.writes { for w in ws { w(.failure(e)) } }
        for (_, n) in c.notifying { n(.failure(e)) }
        for r in c.rssi { r(.failure(e)) }
        c.discovering = []
        c.reads = [:]
        c.writes = [:]
        c.notifying = [:]
        c.rssi = []
    }

    private func conns(of p: CBPeripheral) -> [(Int, Conn)] { conns.filter { $0.value.peripheral === p }.map { ($0.key, $0.value) } }

    nonisolated func centralManager(_ c: CBCentralManager, didConnect p: CBPeripheral) {
        MainActor.assumeIsolated {
            for (h, conn) in conns(of: p) {
                conn.timeout?.cancel()
                conn.changed.f(Link(state: .connected, handle: h))
            }
        }
    }

    nonisolated func centralManager(_ c: CBCentralManager, didFailToConnect p: CBPeripheral, error: Error?) {
        MainActor.assumeIsolated {
            for (h, conn) in conns(of: p) { lost(h, conn, "bluetooth: disconnected: \(error?.localizedDescription ?? "couldn't connect")") }
        }
    }

    nonisolated func centralManager(_ c: CBCentralManager, didDisconnectPeripheral p: CBPeripheral, error: Error?) {
        MainActor.assumeIsolated {
            for (h, conn) in conns(of: p) { lost(h, conn, error.map { "bluetooth: disconnected: \($0.localizedDescription)" } ?? "bluetooth: disconnected") }
        }
    }

    private func conn(_ h: Int) throws -> Conn {
        guard let c = conns[h], !c.ended.done, c.peripheral.state == .connected else { throw BluetoothError.disconnected }
        return c
    }

    // MARK: GATT

    func services(_ h: Int, _ done: @escaping (Swift.Result<[FoundService], Error>) -> Void) {
        let c: Conn
        do { c = try conn(h) } catch { done(.failure(error)); return }
        if !c.services.isEmpty && c.discovering.isEmpty { done(.success(found(c))); return }
        c.discovering.append(done)
        if c.discovering.count == 1 { c.peripheral.discoverServices(nil) }
    }

    private func found(_ c: Conn) -> [FoundService] {
        (c.peripheral.services ?? []).map { s in
            FoundService(uuid: uuidString(s.uuid), characteristics: (s.characteristics ?? []).map { ch in
                FoundCharacteristic(uuid: uuidString(ch.uuid), properties: Self.properties(ch.properties), descriptors: (ch.descriptors ?? []).map { uuidString($0.uuid) })
            })
        }
    }

    static func properties(_ p: CBCharacteristicProperties) -> [Property] {
        var out: [Property] = []
        if p.contains(.broadcast) { out.append(.broadcast) }
        if p.contains(.read) { out.append(.read) }
        if p.contains(.writeWithoutResponse) { out.append(.writeWithoutResponse) }
        if p.contains(.write) { out.append(.write) }
        if p.contains(.notify) { out.append(.notify) }
        if p.contains(.indicate) { out.append(.indicate) }
        if p.contains(.authenticatedSignedWrites) { out.append(.authenticatedSignedWrites) }
        if p.contains(.extendedProperties) { out.append(.extendedProperties) }
        return out
    }

    private func discovered(_ p: CBPeripheral, _ error: Error?) {
        for (_, c) in conns(of: p) where !c.discovering.isEmpty {
            if let error {
                let ds = c.discovering
                c.discovering = []
                for d in ds { d(.failure(error)) }
                continue
            }
            if c.left == 0 {
                let fs = found(c)
                for s in p.services ?? [] { c.services[s.uuid] = s }
                let ds = c.discovering
                c.discovering = []
                for d in ds { d(.success(fs)) }
            }
        }
    }

    nonisolated func peripheral(_ p: CBPeripheral, didDiscoverServices error: Error?) {
        MainActor.assumeIsolated {
            let ss = p.services ?? []
            for (_, c) in conns(of: p) { c.left = ss.count }
            if error != nil || ss.isEmpty { discovered(p, error); return }
            for s in ss { p.discoverCharacteristics(nil, for: s) }
        }
    }

    nonisolated func peripheral(_ p: CBPeripheral, didDiscoverCharacteristicsFor s: CBService, error: Error?) {
        MainActor.assumeIsolated {
            for (_, c) in conns(of: p) { c.left = max(c.left - 1, 0) }
            discovered(p, nil)
        }
    }

    private func characteristic(_ h: Int, _ service: String, _ char: String) throws -> (Conn, CBCharacteristic) {
        let c = try conn(h)
        let su = CBUUID(string: service), cu = CBUUID(string: char)
        guard let s = c.peripheral.services?.first(where: { $0.uuid == su }),
              let ch = s.characteristics?.first(where: { $0.uuid == cu }) else { throw BluetoothError.notFound }
        return (c, ch)
    }

    func mtu(_ h: Int) throws -> Int {
        let c = try conn(h)
        return c.peripheral.maximumWriteValueLength(for: .withoutResponse) + 3
    }

    func rssi(_ h: Int, _ done: @escaping (Swift.Result<Int, Error>) -> Void) {
        do {
            let c = try conn(h)
            c.rssi.append(done)
            c.peripheral.readRSSI()
        } catch { done(.failure(error)) }
    }

    nonisolated func peripheral(_ p: CBPeripheral, didReadRSSI rssi: NSNumber, error: Error?) {
        MainActor.assumeIsolated {
            for (_, c) in conns(of: p) {
                let rs = c.rssi
                c.rssi = []
                for r in rs { r(error.map { .failure($0) } ?? .success(rssi.intValue)) }
            }
        }
    }

    func read(_ h: Int, _ service: String, _ char: String, _ done: @escaping (Swift.Result<Data, Error>) -> Void) {
        do {
            let (c, ch) = try characteristic(h, service, char)
            c.reads[ch, default: []].append(done)
            c.peripheral.readValue(for: ch)
        } catch { done(.failure(error)) }
    }

    func write(_ h: Int, _ service: String, _ char: String, _ v: Data, _ response: Bool, _ done: @escaping (Swift.Result<Void, Error>) -> Void) {
        do {
            let (c, ch) = try characteristic(h, service, char)
            if response {
                c.writes[ch, default: []].append(done)
                c.peripheral.writeValue(v, for: ch, type: .withResponse)
            } else {
                c.peripheral.writeValue(v, for: ch, type: .withoutResponse)
                done(.success(()))
            }
        } catch { done(.failure(error)) }
    }

    func subscribe(_ h: Int, _ service: String, _ char: String, _ f: Box<Data>, _ done: @escaping (Swift.Result<Int, Error>) -> Void) {
        do {
            let (c, ch) = try characteristic(h, service, char)
            next += 1
            let id = next
            subs[id] = (h, ch, f)
            if ch.isNotifying { done(.success(id)); return }
            c.notifying[ch] = { r in done(r.map { id }) }
            c.peripheral.setNotifyValue(true, for: ch)
        } catch { done(.failure(error)) }
    }

    func unsubscribe(_ id: Int) {
        guard let (h, ch, _) = subs.removeValue(forKey: id) else { return }
        if !subs.values.contains(where: { $0.1 === ch }), let c = conns[h], c.peripheral.state == .connected {
            c.peripheral.setNotifyValue(false, for: ch)
        }
    }

    nonisolated func peripheral(_ p: CBPeripheral, didUpdateNotificationStateFor ch: CBCharacteristic, error: Error?) {
        MainActor.assumeIsolated {
            for (_, c) in conns(of: p) {
                if let n = c.notifying.removeValue(forKey: ch) { n(error.map { .failure($0) } ?? .success(())) }
            }
        }
    }

    nonisolated func peripheral(_ p: CBPeripheral, didUpdateValueFor ch: CBCharacteristic, error: Error?) {
        MainActor.assumeIsolated {
            let v = ch.value ?? Data()
            var read = false
            for (_, c) in conns(of: p) {
                if let rs = c.reads.removeValue(forKey: ch) {
                    read = true
                    for r in rs { r(error.map { .failure($0) } ?? .success(v)) }
                }
            }
            if !read && error == nil {
                for (_, ch2, f) in subs.values where ch2 === ch { f.f(v) }
            }
        }
    }

    nonisolated func peripheral(_ p: CBPeripheral, didWriteValueFor ch: CBCharacteristic, error: Error?) {
        MainActor.assumeIsolated {
            for (_, c) in conns(of: p) {
                guard var ws = c.writes[ch], !ws.isEmpty else { continue }
                let w = ws.removeFirst()
                c.writes[ch] = ws.isEmpty ? nil : ws
                w(error.map { .failure($0) } ?? .success(()))
            }
        }
    }

    // MARK: The peripheral's side

    #if !os(tvOS)
    private func whenOn(_ then: @escaping () -> Void) {
        if pm.state == .poweredOn { then(); return }
        pmWaiters.append(then)
    }
    #endif

    nonisolated func peripheralManagerDidUpdateState(_ p: CBPeripheralManager) {
        MainActor.assumeIsolated {
            if p.state == .poweredOn {
                let ws = pmWaiters
                pmWaiters = []
                for w in ws { w() }
            } else if p.state == .unauthorized || p.state == .unsupported || p.state == .poweredOff {
                let e: Error = p.state == .poweredOff ? BluetoothError.off : p.state == .unauthorized ? BluetoothError.denied : BluetoothError.unsupported
                pmWaiters = []
                serveDone?(.failure(e))
                serveDone = nil
                advertiseDone?(.failure(e))
                advertiseDone = nil
            }
        }
    }

    func serve(_ services: [ServedService], _ f: Box<Request>, _ done: @escaping (Swift.Result<Void, Error>) -> Void) {
        #if os(tvOS)
        // Apple TV can't be a peripheral: it serves nothing.
        done(.failure(BluetoothError.unsupported))
        #else
        serving = f
        serveDone = done
        whenOn { [unowned self] in
            self.pm.removeAllServices()
            self.servedChars = [:]
            self.pendingAdds = services.count
            if services.isEmpty { self.serveDone?(.success(())); self.serveDone = nil }
            for s in services {
                let ms = CBMutableService(type: CBUUID(string: s.uuid), primary: s.primary)
                ms.characteristics = s.characteristics.map { c in
                    var props: CBCharacteristicProperties = []
                    for p in c.properties {
                        switch p {
                        case .broadcast: props.insert(.broadcast)
                        case .read: props.insert(.read)
                        case .writeWithoutResponse: props.insert(.writeWithoutResponse)
                        case .write: props.insert(.write)
                        case .notify: props.insert(.notify)
                        case .indicate: props.insert(.indicate)
                        default: break
                        }
                    }
                    var perms: CBAttributePermissions = []
                    for p in c.permissions {
                        switch p {
                        case .readable: perms.insert(.readable)
                        case .writeable: perms.insert(.writeable)
                        case .readEncrypted: perms.insert(.readEncryptionRequired)
                        case .writeEncrypted: perms.insert(.writeEncryptionRequired)
                        default: break
                        }
                    }
                    // A fixed value only for a read-only one; else asked each time.
                    let fixed = !c.dynamic && !c.value.isEmpty && !props.contains(.write) && !props.contains(.notify) && !props.contains(.writeWithoutResponse)
                    let ch = CBMutableCharacteristic(type: CBUUID(string: c.uuid), properties: props, value: fixed ? c.value : nil, permissions: perms)
                    self.servedChars[CBUUID(string: c.uuid)] = ch
                    return ch
                }
                self.pm.add(ms)
            }
        }
        #endif
    }

    nonisolated func peripheralManager(_ p: CBPeripheralManager, didAdd s: CBService, error: Error?) {
        MainActor.assumeIsolated {
            if let error {
                serveDone?(.failure(error))
                serveDone = nil
                return
            }
            pendingAdds -= 1
            if pendingAdds == 0 {
                serveDone?(.success(()))
                serveDone = nil
            }
        }
    }

    func stopServing() {
        serving = nil
        peripheralManager?.removeAllServices()
        servedChars = [:]
    }

    private func ask(_ kind: RequestKind, _ central: CBCentral, _ ch: CBCharacteristic, _ value: Data = Data(), _ offset: Int = 0, _ req: CBATTRequest? = nil) {
        next += 1
        if let req { requests[next] = req }
        serving?.f(Request(id: next, kind: kind, central: central.identifier.uuidString, service: ch.service.map { uuidString($0.uuid) } ?? "",
                           characteristic: uuidString(ch.uuid), value: value, offset: offset))
    }

    nonisolated func peripheralManager(_ p: CBPeripheralManager, didReceiveRead r: CBATTRequest) {
        MainActor.assumeIsolated { ask(.read, r.central, r.characteristic, Data(), r.offset, r) }
    }

    nonisolated func peripheralManager(_ p: CBPeripheralManager, didReceiveWrite rs: [CBATTRequest]) {
        MainActor.assumeIsolated {
            // Answered once, for the first: Core Bluetooth's rule.
            for (i, r) in rs.enumerated() { ask(.write, r.central, r.characteristic, r.value ?? Data(), r.offset, i == 0 ? r : nil) }
        }
    }

    func respond(_ id: Int, _ value: Data, _ error: Int) {
        guard let r = requests.removeValue(forKey: id) else { return }
        if error != 0 {
            peripheralManager?.respond(to: r, withResult: CBATTError.Code(rawValue: error) ?? .unlikelyError)
            return
        }
        if r.value == nil || r.value?.isEmpty == true {
            r.value = r.offset < value.count ? value.subdata(in: value.startIndex + r.offset..<value.endIndex) : Data()
        }
        peripheralManager?.respond(to: r, withResult: .success)
    }

    nonisolated func peripheralManager(_ p: CBPeripheralManager, central: CBCentral, didSubscribeTo ch: CBCharacteristic) {
        MainActor.assumeIsolated { ask(.subscribe, central, ch) }
    }

    nonisolated func peripheralManager(_ p: CBPeripheralManager, central: CBCentral, didUnsubscribeFrom ch: CBCharacteristic) {
        MainActor.assumeIsolated { ask(.unsubscribe, central, ch) }
    }

    func notify(_ char: String, _ value: Data) -> Int {
        guard let pm = peripheralManager, let ch = servedChars[CBUUID(string: char)] else { return 0 }
        let n = ch.subscribedCentrals?.count ?? 0
        _ = pm.updateValue(value, for: ch, onSubscribedCentrals: nil)
        return n
    }

    func advertise(_ o: Advertising, _ done: @escaping (Swift.Result<Void, Error>) -> Void) {
        #if os(tvOS)
        done(.failure(BluetoothError.unsupported))
        #else
        advertiseDone = done
        whenOn { [unowned self] in
            var d: [String: Any] = [:]
            if !o.localName.isEmpty { d[CBAdvertisementDataLocalNameKey] = o.localName }
            if !o.services.isEmpty { d[CBAdvertisementDataServiceUUIDsKey] = o.services.map { CBUUID(string: $0) } }
            self.pm.startAdvertising(d)
        }
        #endif
    }

    nonisolated func peripheralManagerDidStartAdvertising(_ p: CBPeripheralManager, error: Error?) {
        MainActor.assumeIsolated {
            advertiseDone?(error.map { .failure($0) } ?? .success(()))
            advertiseDone = nil
        }
    }

    func stopAdvertising() { peripheralManager?.stopAdvertising() }
}

func makeAdapter() throws -> any Adapter { CoreBluetoothAdapter() }

#if os(iOS)
/// The managers as the app launches (the manifest's ios.launch, for an
/// app with a Bluetooth background mode): iOS starts the app again for the
/// connections, scan or advertising it kept, and hands them to the
/// managers made with their restore identifiers before launch ends. For
/// such a launch the app's code starts at once, without a screen, and
/// reads them (restored()).
@objc(AsukaBluetoothLaunch) final class BluetoothLaunch: NSObject, AsukaLaunching {
    @MainActor static func launch(_ options: [UIApplication.LaunchOptionsKey: Any]) -> Bool {
        var woke = false
        if let ids = options[.bluetoothCentrals] as? [String], ids.contains(Hub.centralRestoreID) {
            Hub.shared.makeCentral(alert: false)
            woke = true
        }
        if let ids = options[.bluetoothPeripherals] as? [String], ids.contains(Hub.peripheralRestoreID) {
            Hub.shared.makePeripheralManager()
            woke = true
        }
        return woke
    }
}
#endif
