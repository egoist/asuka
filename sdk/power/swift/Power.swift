// platform/power on iOS and the Mac. iOS: UIDevice's battery (monitored
// while it's read or watched; unknown on the simulator), ProcessInfo's
// Low Power Mode and thermal state; with a passcode, the screen locks as
// protected data goes and unlocks as it comes back; no sleep, no idle
// time. The Mac: IOKit's power sources (the internal battery), followed
// through IOPSNotificationCreateRunLoopSource; ProcessInfo's Low Power
// Mode and thermal state; NSWorkspace's sleep and wake; loginwindow's
// screenIsLocked and screenIsUnlocked; Quartz's seconds since the last
// input. Apple TV runs on mains: no battery, its thermal state only.
import Foundation
#if canImport(UIKit)
import UIKit
#else
import AppKit
import IOKit.ps
import CoreGraphics
#endif

final class SystemPower: Monitor {
    func current() throws -> Status {
        if Thread.isMainThread { return MainActor.assumeIsolated { Self.status() } }
        return DispatchQueue.main.sync { MainActor.assumeIsolated { Self.status() } }
    }

    func watch(changed: @escaping (Status) -> Void) async throws {
        let w = await MainActor.run { PowerWatch(status: changed) }
        defer { Task { @MainActor in w.stop() } }
        await Self.untilCancelled()
    }

    func events(happened: @escaping (Event) -> Void) async throws {
        let w = await MainActor.run { PowerWatch(events: happened) }
        defer { Task { @MainActor in w.stop() } }
        await Self.untilCancelled()
    }

    func idle() async throws -> TimeInterval {
        #if canImport(UIKit)
        throw PowerError.idleUnknown
        #else
        // Any input, the session's.
        let s = CGEventSource.secondsSinceLastEventType(.combinedSessionState, eventType: CGEventType(rawValue: ~0)!)
        guard s.isFinite, s >= 0 else { throw PowerError.idleUnknown }
        return s
        #endif
    }

    static func untilCancelled() async {
        while !Task.isCancelled {
            try? await Task.sleep(nanoseconds: 3_600_000_000_000)
        }
    }

    @MainActor static func status() -> Status {
        var s = Status()
        let p = ProcessInfo.processInfo
        if #available(macOS 12, *) { s.lowPower = p.isLowPowerModeEnabled }
        switch p.thermalState {
        case .nominal: s.thermal = .nominal
        case .fair: s.thermal = .fair
        case .serious: s.thermal = .serious
        default: s.thermal = .critical
        }
        #if os(tvOS)
        // No battery: on the mains.
        s.level = -1
        #elseif canImport(UIKit)
        let d = UIDevice.current
        let was = d.isBatteryMonitoringEnabled
        if !was { d.isBatteryMonitoringEnabled = true }
        defer { if !was { d.isBatteryMonitoringEnabled = false } }
        let st = d.batteryState
        s.battery = st != .unknown
        s.onBattery = st == .unplugged
        s.charging = st == .charging
        s.level = s.battery && d.batteryLevel >= 0 ? Double(d.batteryLevel) : -1
        #else
        guard let blob = IOPSCopyPowerSourcesInfo()?.takeRetainedValue(),
              let list = IOPSCopyPowerSourcesList(blob)?.takeRetainedValue() as? [CFTypeRef] else { return s }
        for ps in list {
            guard let d = IOPSGetPowerSourceDescription(blob, ps)?.takeUnretainedValue() as? [String: Any],
                  d[kIOPSTypeKey] as? String == kIOPSInternalBatteryType,
                  d[kIOPSIsPresentKey] as? Bool ?? true else { continue }
            s.battery = true
            s.onBattery = d[kIOPSPowerSourceStateKey] as? String == kIOPSBatteryPowerValue
            if let cur = d[kIOPSCurrentCapacityKey] as? Int, let max = d[kIOPSMaxCapacityKey] as? Int, max > 0 {
                s.level = min(Double(cur) / Double(max), 1)
            }
            s.charging = d[kIOPSIsChargingKey] as? Bool ?? false
            break
        }
        #endif
        return s
    }
}

/// Follows the power's notifications: its status (reported at once, then
/// as it changes) or its events.
@MainActor final class PowerWatch: NSObject {
    private var status: ((Status) -> Void)?
    private var events: ((Event) -> Void)?
    private var last: Status?
    #if os(macOS)
    private var source: CFRunLoopSource?
    private static var watches: [ObjectIdentifier: PowerWatch] = [:]
    #endif

    init(status: @escaping (Status) -> Void) {
        self.status = status
        super.init()
        let nc = NotificationCenter.default
        var names: [Notification.Name] = [ProcessInfo.thermalStateDidChangeNotification]
        if #available(macOS 12, *) { names.append(.NSProcessInfoPowerStateDidChange) }
        #if os(tvOS)
        #elseif canImport(UIKit)
        UIDevice.current.isBatteryMonitoringEnabled = true
        names += [UIDevice.batteryStateDidChangeNotification, UIDevice.batteryLevelDidChangeNotification]
        #else
        // A power source changed: plugged in, its capacity, charging.
        let ctx = Unmanaged.passUnretained(self).toOpaque()
        if let src = IOPSNotificationCreateRunLoopSource({ ctx in
            guard let ctx else { return }
            let w = Unmanaged<PowerWatch>.fromOpaque(ctx).takeUnretainedValue()
            MainActor.assumeIsolated { w.changed() }
        }, ctx)?.takeRetainedValue() {
            CFRunLoopAddSource(CFRunLoopGetMain(), src, .commonModes)
            source = src
        }
        #endif
        for n in names { nc.addObserver(self, selector: #selector(changedAnywhere), name: n, object: nil) }
        changed()
    }

    init(events: @escaping (Event) -> Void) {
        self.events = events
        super.init()
        #if canImport(UIKit)
        let nc = NotificationCenter.default
        nc.addObserver(self, selector: #selector(locked), name: UIApplication.protectedDataWillBecomeUnavailableNotification, object: nil)
        nc.addObserver(self, selector: #selector(unlocked), name: UIApplication.protectedDataDidBecomeAvailableNotification, object: nil)
        #else
        let ws = NSWorkspace.shared.notificationCenter
        ws.addObserver(self, selector: #selector(sleeping), name: NSWorkspace.willSleepNotification, object: nil)
        ws.addObserver(self, selector: #selector(woke), name: NSWorkspace.didWakeNotification, object: nil)
        let dc = DistributedNotificationCenter.default()
        dc.addObserver(self, selector: #selector(locked), name: Notification.Name("com.apple.screenIsLocked"), object: nil)
        dc.addObserver(self, selector: #selector(unlocked), name: Notification.Name("com.apple.screenIsUnlocked"), object: nil)
        #endif
    }

    // ProcessInfo's notifications come on the thread that changed it.
    @objc nonisolated func changedAnywhere() { DispatchQueue.main.async { MainActor.assumeIsolated { self.changed() } } }

    func changed() {
        guard let status else { return }
        let s = SystemPower.status()
        if let l = last, l.battery == s.battery, l.onBattery == s.onBattery, l.level == s.level, l.charging == s.charging,
           l.lowPower == s.lowPower, l.thermal == s.thermal { return }
        last = s
        status(s)
    }

    @objc func sleeping() { events?(.suspend) }
    @objc func woke() { events?(.resume) }
    @objc func locked() { events?(.lock) }
    @objc func unlocked() { events?(.unlock) }

    func stop() {
        NotificationCenter.default.removeObserver(self)
        #if os(tvOS)
        #elseif canImport(UIKit)
        if status != nil { UIDevice.current.isBatteryMonitoringEnabled = false }
        #else
        NSWorkspace.shared.notificationCenter.removeObserver(self)
        DistributedNotificationCenter.default().removeObserver(self)
        if let src = source { CFRunLoopRemoveSource(CFRunLoopGetMain(), src, .commonModes) }
        source = nil
        #endif
        status = nil
        events = nil
    }
}

func makeMonitor() throws -> any Monitor { SystemPower() }
