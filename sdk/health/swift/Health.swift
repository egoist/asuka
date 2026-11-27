// platform/health on iOS: HealthKit. Each kind is a HealthKit type and
// its unit (one table, Kinds); queries run on HealthKit's own queue and
// are awaited through continuations, so nothing blocks the main thread.
// The Mac (and an iPad without Health) has no store: notAvailable. Apple
// Watch's HealthKit is the watch's own store: what it measured, and
// the recent days of what the iPhone has.
import Foundation
import Asuka
#if os(iOS) || os(watchOS)
import HealthKit

/// A kind as HealthKit has it: a quantity type and its unit, or a
/// category type (sleep, mindfulness) whose samples read as value 1.
private enum Kinds {
    case quantity(HKQuantityTypeIdentifier, HKUnit)
    case category(HKCategoryTypeIdentifier)

    static let perMinute = HKUnit.count().unitDivided(by: .minute())

    static let table: [String: Kinds] = [
        "steps": .quantity(.stepCount, .count()),
        "distance": .quantity(.distanceWalkingRunning, .meter()),
        "activeEnergy": .quantity(.activeEnergyBurned, .kilocalorie()),
        "floors": .quantity(.flightsClimbed, .count()),
        "heartRate": .quantity(.heartRate, perMinute),
        "restingHeartRate": .quantity(.restingHeartRate, perMinute),
        "oxygenSaturation": .quantity(.oxygenSaturation, .percent()), // 0–1
        "respiratoryRate": .quantity(.respiratoryRate, perMinute),
        "weight": .quantity(.bodyMass, .gramUnit(with: .kilo)),
        "height": .quantity(.height, .meter()),
        "bodyFat": .quantity(.bodyFatPercentage, .percent()), // 0–1
        "water": .quantity(.dietaryWater, .liter()),
        "sleep": .category(.sleepAnalysis),
        "mindfulness": .category(.mindfulSession),
    ]

    static func of(_ name: String) throws -> Kinds {
        guard let k = table[name] else { throw failure("health: no kind \(name)") }
        return k
    }

    var type: HKSampleType {
        switch self {
        case .quantity(let id, _): return HKQuantityType(id)
        case .category(let id): return HKCategoryType(id)
        }
    }
}

final class HealthKitStore: Store {
    private let store = HKHealthStore()

    func available() throws -> Bool { HKHealthStore.isHealthDataAvailable() }

    func request(read: [String], write: [String]) async throws -> Bool {
        try ready()
        let r = Set(try read.map { try Kinds.of($0).type as HKObjectType })
        let w = Set(try write.map { try Kinds.of($0).type })
        if r.isEmpty && w.isEmpty { return true }
        return try await withCheckedThrowingContinuation { (c: CheckedContinuation<Bool, Error>) in
            store.requestAuthorization(toShare: w, read: r) { ok, error in
                if let error { c.resume(throwing: mapped(error)) } else { c.resume(returning: ok) }
            }
        }
    }

    func samples(kind: String, from: Int, to: Int, limit: Int) async throws -> [NativeSample] {
        try ready()
        let k = try Kinds.of(kind)
        var predicate: NSPredicate = HKQuery.predicateForSamples(withStart: date(from), end: date(to), options: [])
        if case .category(.sleepAnalysis) = k {
            // Asleep (any stage), not in bed or awake.
            let asleep = HKCategoryValueSleepAnalysis.allAsleepValues.map { NSNumber(value: $0.rawValue) }
            predicate = NSCompoundPredicate(andPredicateWithSubpredicates: [
                predicate, NSPredicate(format: "%K IN %@", HKPredicateKeyPathCategoryValue, asleep),
            ])
        }
        let sort = [NSSortDescriptor(key: HKSampleSortIdentifierEndDate, ascending: false)]
        let found = try await withCheckedThrowingContinuation { (c: CheckedContinuation<[HKSample], Error>) in
            let q = HKSampleQuery(sampleType: k.type, predicate: predicate, limit: limit > 0 ? limit : HKObjectQueryNoLimit, sortDescriptors: sort) { _, samples, error in
                if let error { c.resume(throwing: mapped(error)) } else { c.resume(returning: samples ?? []) }
            }
            store.execute(q)
        }
        return found.map { s in
            var value = 1.0
            if case .quantity(_, let unit) = k, let q = s as? HKQuantitySample {
                value = q.quantity.doubleValue(for: unit)
            }
            return NativeSample(value: value, start: millis(s.startDate), end: millis(s.endDate), source: s.sourceRevision.source.name)
        }
    }

    func total(kind: String, from: Int, to: Int) async throws -> Double {
        try ready()
        guard case .quantity(let id, let unit) = try Kinds.of(kind), HKQuantityType(id).aggregationStyle == .cumulative else {
            throw failure("health: \(kind) doesn't add up: read its samples")
        }
        let predicate = HKQuery.predicateForSamples(withStart: date(from), end: date(to), options: [])
        return try await withCheckedThrowingContinuation { (c: CheckedContinuation<Double, Error>) in
            let q = HKStatisticsQuery(quantityType: HKQuantityType(id), quantitySamplePredicate: predicate, options: .cumulativeSum) { _, stats, error in
                if let error {
                    // No samples in the range: HealthKit says no data, which is 0.
                    if (error as? HKError)?.code == .errorNoData { c.resume(returning: 0) } else { c.resume(throwing: mapped(error)) }
                    return
                }
                c.resume(returning: stats?.sumQuantity()?.doubleValue(for: unit) ?? 0)
            }
            store.execute(q)
        }
    }

    func write(kind: String, value: Double, from: Int, to: Int) async throws {
        try ready()
        let start = date(from), end = date(max(from, to))
        let sample: HKSample
        switch try Kinds.of(kind) {
        case .quantity(let id, let unit):
            sample = HKQuantitySample(type: HKQuantityType(id), quantity: HKQuantity(unit: unit, doubleValue: value), start: start, end: end)
        case .category(let id):
            let v = id == .sleepAnalysis ? HKCategoryValueSleepAnalysis.asleepUnspecified.rawValue : HKCategoryValue.notApplicable.rawValue
            sample = HKCategorySample(type: HKCategoryType(id), value: v, start: start, end: end)
        }
        try await withCheckedThrowingContinuation { (c: CheckedContinuation<Void, Error>) in
            store.save(sample) { _, error in
                if let error { c.resume(throwing: mapped(error)) } else { c.resume() }
            }
        }
    }

    private func ready() throws {
        guard HKHealthStore.isHealthDataAvailable() else { throw HealthError.notAvailable }
    }
}

/// HealthKit's errors as the package declares them where they say the
/// user didn't allow it, or there's no store; others as they are.
private func mapped(_ error: Error) -> Error {
    guard let e = error as? HKError else { return error }
    switch e.code {
    case .errorAuthorizationDenied, .errorAuthorizationNotDetermined: return HealthError.notAllowed
    case .errorHealthDataUnavailable, .errorHealthDataRestricted: return HealthError.notAvailable
    default: return error
    }
}

private func date(_ ms: Int) -> Date { Date(timeIntervalSince1970: Double(ms) / 1000) }

private func millis(_ d: Date) -> Int { Int((d.timeIntervalSince1970 * 1000).rounded()) }

private func failure(_ message: String) -> Error {
    NSError(domain: "health", code: 1, userInfo: [NSLocalizedDescriptionKey: message])
}

func makeStore() throws -> any Store { HealthKitStore() }
#else
// HealthKit's store is iOS's here: the Mac has none.
func makeStore() throws -> any Store { throw HealthError.notAvailable }
#endif
