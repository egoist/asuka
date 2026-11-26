// platform/purchase on iOS and macOS: StoreKit 2. It listens to
// Transaction.updates, Transaction.unfinished and Storefront.updates from
// the package's object's making (as the app starts: the package makes it
// at its top), keeping what comes for the events() that reads it, so a
// purchase completed while the app doesn't read (Ask to Buy approved, a
// renewal, a refund, an offer code redeemed in the App Store) isn't lost.
// StoreKit's Product and Transaction are StoreKit.Product and
// StoreKit.Transaction here: Product and StoreTransaction are the package's.
// StoreKit 2 needs macOS 12: before, a Mac has no store.
import Foundation
import StoreKit
import Asuka
#if canImport(UIKit)
import UIKit
#else
import AppKit
#endif

@available(macOS 12.0, *)
final class AppStoreStore: Store {
    init() {
        Task { @MainActor in Hub.shared.start() }
    }

    func available() throws -> Bool { AppStore.canMakePayments }

    func declare(items: [Item]) throws {}

    func products(items: [Item]) async throws -> [Product] {
        try await Hub.shared.products(items.map(\.id))
    }

    func buy(item: Item, options: StoreOptions) async throws -> StoreResult {
        try await Hub.shared.buy(item.id, options)
    }

    func finish(transaction: StoreTransaction) async throws {
        try await Hub.shared.finish(transaction.id)
    }

    func entitlements() async throws -> [StoreTransaction] { await Hub.shared.entitlements() }

    func restore() async throws -> [StoreTransaction] {
        do {
            try await AppStore.sync()
        } catch {
            throw Hub.failure(error)
        }
        return await Hub.shared.entitlements()
    }

    func statuses(group: String) async throws -> [StoreStatus] { try await Hub.shared.statuses(group) }

    func manageSubscriptions(productId: String) async throws { try await Hub.shared.manage(productId) }

    func requestRefund(transactionId: String) async throws -> Bool {
        guard let id = UInt64(transactionId) else { throw AsukaNativeError.unsupported("purchase: no transaction \(transactionId)") }
        return try await Hub.shared.refund(id)
    }

    func redeemCode() async throws { try await Hub.shared.redeem() }

    func storefront() async throws -> Storefront {
        guard let s = await StoreKit.Storefront.current else { throw PurchaseError.notAvailable }
        return Storefront(country: s.countryCode, id: s.id)
    }

    func events(event: @escaping (StoreEvent) -> Void) async throws {
        let send = Send(event)
        let id = await MainActor.run { Hub.shared.listen(send) }
        defer { Task { @MainActor in Hub.shared.unlisten(id) } }
        while !Task.isCancelled {
            try? await Task.sleep(nanoseconds: 3_600_000_000_000)
        }
    }
}

/// An events() reader, called on the main thread.
final class Send: @unchecked Sendable {
    let f: (StoreEvent) -> Void
    init(_ f: @escaping (StoreEvent) -> Void) { self.f = f }
}

@available(macOS 12.0, *)
@MainActor final class Hub {
    static let shared = Hub()

    /// Products asked for, by ID: purchase needs StoreKit's.
    private var products: [String: StoreKit.Product] = [:]
    /// Transactions not yet finished, by ID.
    private var unfinished: [UInt64: StoreKit.Transaction] = [:]
    /// Transactions given, by ID and revocation: StoreKit gives an
    /// unfinished one both in Transaction.unfinished and updates.
    private var given = Set<String>()
    private var kept: [StoreEvent] = []
    private var readers: [Int: Send] = [:]
    private var next = 0
    private var started = false

    #if canImport(UIKit)
    /// The scene the sheets show in: the app's.
    var scene: UIWindowScene? {
        let scenes = UIApplication.shared.connectedScenes.compactMap { $0 as? UIWindowScene }
        return scenes.first { $0.activationState == .foregroundActive } ?? scenes.first
    }
    #else
    /// The window the sheets show on: the app's key window.
    var window: NSWindow? { NSApp.keyWindow ?? NSApp.mainWindow ?? NSApp.windows.first { $0.isVisible } }
    #endif

    /// Starts listening (Apple's advice: as the app launches; an update
    /// that came before is given once the listener starts).
    func start() {
        guard !started else { return }
        started = true
        #if targetEnvironment(simulator)
        StoreKitTest.start()
        #endif
        Task { @MainActor in
            for await v in StoreKit.Transaction.updates { self.updated(v) }
        }
        Task { @MainActor in
            for await v in StoreKit.Transaction.unfinished { self.updated(v) }
        }
        Task { @MainActor in
            for await s in StoreKit.Storefront.updates {
                self.send(StoreEvent(type: .storefrontChanged, storefront: Storefront(country: s.countryCode, id: s.id)))
            }
        }
    }

    func listen(_ f: Send) -> Int {
        start()
        next += 1
        readers[next] = f
        let k = kept
        kept = []
        for e in k { f.f(e) }
        return next
    }

    func unlisten(_ id: Int) { readers[id] = nil }

    private func send(_ e: StoreEvent) {
        if readers.isEmpty {
            kept.append(e)
        } else {
            for r in readers.values { r.f(e) }
        }
    }

    private func key(_ t: StoreKit.Transaction) -> String { "\(t.id):\(t.revocationDate == nil ? "" : "revoked")" }

    /// A transaction outside a purchase: given to events().
    private func updated(_ v: VerificationResult<StoreKit.Transaction>) {
        let t = v.unsafePayloadValue
        guard given.insert(key(t)).inserted else { return }
        if t.revocationDate == nil { unfinished[t.id] = t }
        send(StoreEvent(type: .transactionUpdated, transaction: Self.wire(v)))
    }

    // MARK: Calls

    /// An error as the package says it.
    nonisolated static func failure(_ e: Error) -> Error {
        if let e = e as? StoreKit.Product.PurchaseError {
            switch e {
            case .productUnavailable: return PurchaseError.unknownProduct
            case .purchaseNotAllowed: return PurchaseError.notAllowed
            default: return e
            }
        }
        if let e = e as? StoreKitError, case .notAvailableInStorefront = e { return PurchaseError.unknownProduct }
        return e
    }

    func products(_ ids: [String]) async throws -> [Product] {
        start()
        let got: [StoreKit.Product]
        do {
            got = try await StoreKit.Product.products(for: ids)
        } catch {
            throw Self.failure(error)
        }
        for p in got { products[p.id] = p }
        // In the order asked for: StoreKit's is its own.
        var out: [Product] = []
        for id in ids {
            if let p = got.first(where: { $0.id == id }) { out.append(await wire(p)) }
        }
        return out
    }

    private func product(_ id: String) async throws -> StoreKit.Product {
        if let p = products[id] { return p }
        guard let p = try await StoreKit.Product.products(for: [id]).first else { throw PurchaseError.unknownProduct }
        products[id] = p
        return p
    }

    func buy(_ id: String, _ o: StoreOptions) async throws -> StoreResult {
        start()
        let p = try await product(id)
        guard AppStore.canMakePayments else { throw PurchaseError.notAllowed }
        var options = Set<StoreKit.Product.PurchaseOption>()
        if o.quantity > 1 { options.insert(.quantity(o.quantity)) }
        if !o.accountToken.isEmpty {
            guard let u = UUID(uuidString: o.accountToken) else {
                throw AsukaNativeError.unsupported("purchase: Options.accountToken must be a UUID on Apple's platforms")
            }
            options.insert(.appAccountToken(u))
        }
        let offerID = o.offer?.id ?? ""
        if let sig = o.signature, !offerID.isEmpty {
            if !sig.jws.isEmpty {
                if #available(iOS 18.4, macOS 15.4, tvOS 18.4, *) {
                    options.formUnion(StoreKit.Product.PurchaseOption.promotionalOffer(offerID, compactJWS: sig.jws))
                }
            } else {
                guard let nonce = UUID(uuidString: sig.nonce), let data = Data(base64Encoded: sig.signature) else {
                    throw AsukaNativeError.unsupported("purchase: a promotional offer's signature needs its key ID, nonce (a UUID) and signature")
                }
                options.insert(.promotionalOffer(offerID: offerID, keyID: sig.keyId, nonce: nonce, signature: data, timestamp: sig.timestamp))
            }
        } else if o.offer?.type == .winBack, #available(iOS 18.0, macOS 15.0, tvOS 18.0, *) {
            if let w = p.subscription?.winBackOffers.first(where: { $0.id == offerID }) { options.insert(.winBackOffer(w)) }
        }
        let result: StoreKit.Product.PurchaseResult
        do {
            #if canImport(UIKit)
            if #available(iOS 17.0, *), let scene {
                result = try await p.purchase(confirmIn: scene, options: options)
            } else {
                result = try await p.purchase(options: options)
            }
            #else
            if #available(macOS 15.2, *), let window {
                result = try await p.purchase(confirmIn: window, options: options)
            } else {
                result = try await p.purchase(options: options)
            }
            #endif
        } catch {
            throw Self.failure(error)
        }
        switch result {
        case .success(let v):
            let t = v.unsafePayloadValue
            given.insert(key(t))
            unfinished[t.id] = t
            return StoreResult(status: .bought, transaction: Self.wire(v))
        case .pending:
            return StoreResult(status: .pendingApproval)
        default:
            return StoreResult(status: .cancelled)
        }
    }

    func finish(_ s: String) async throws {
        guard let id = UInt64(s) else { throw AsukaNativeError.unsupported("purchase: no transaction \(s)") }
        if let t = unfinished.removeValue(forKey: id) {
            await t.finish()
            return
        }
        for await v in StoreKit.Transaction.unfinished {
            let t = v.unsafePayloadValue
            if t.id == id { await t.finish() }
        }
    }

    func entitlements() async -> [StoreTransaction] {
        var out: [StoreTransaction] = []
        for await v in StoreKit.Transaction.currentEntitlements { out.append(Self.wire(v)) }
        return out
    }

    func statuses(_ group: String) async throws -> [StoreStatus] {
        var statuses: [StoreKit.Product.SubscriptionInfo.Status] = []
        if #available(iOS 17.0, macOS 14.0, *) {
            statuses = try await StoreKit.Product.SubscriptionInfo.status(for: group)
        } else if let p = products.values.first(where: { $0.subscription?.subscriptionGroupID == group }) {
            statuses = try await p.subscription?.status ?? []
        }
        return statuses.map { s in
            let r = s.renewalInfo.unsafePayloadValue
            var renewal = 0
            if #available(iOS 17.0, macOS 14.0, *), let at = r.renewalDate { renewal = Self.ns(at) }
            return StoreStatus(
                productId: s.transaction.unsafePayloadValue.productID, state: Self.state(s.state), willRenew: r.willAutoRenew,
                renewsTo: r.autoRenewPreference ?? "", renewal: renewal, graceExpires: r.gracePeriodExpirationDate.map(Self.ns) ?? 0,
                expirationReason: r.expirationReason.map(Self.reason) ?? "", renewalJWS: s.renewalInfo.jwsRepresentation,
                transaction: Self.wire(s.transaction))
        }
    }

    func manage(_ productID: String) async throws {
        #if os(tvOS)
        // tvOS has no sheet of the user's subscriptions: they're in
        // Settings › Users and Accounts › Subscriptions.
        throw AsukaNativeError.unsupported("purchase: tvOS has no subscriptions sheet")
        #elseif canImport(UIKit)
        guard let scene else { throw AsukaNativeError.unsupported("purchase: no screen") }
        if #available(iOS 17.0, *), let g = products[productID]?.subscription?.subscriptionGroupID {
            try await AppStore.showManageSubscriptions(in: scene, subscriptionGroupID: g)
        } else {
            try await AppStore.showManageSubscriptions(in: scene)
        }
        #else
        // The Mac has no sheet: the App Store's subscriptions page.
        NSWorkspace.shared.open(URL(string: "macappstore://apps.apple.com/account/subscriptions")!)
        #endif
    }

    func refund(_ id: UInt64) async throws -> Bool {
        #if os(tvOS)
        throw AsukaNativeError.unsupported("purchase: tvOS has no refund sheet")
        #elseif canImport(UIKit)
        guard let scene else { throw AsukaNativeError.unsupported("purchase: no screen") }
        let s = try await StoreKit.Transaction.beginRefundRequest(for: id, in: scene)
        #else
        guard let c = window?.contentViewController else { throw AsukaNativeError.unsupported("purchase: no window") }
        let s = try await StoreKit.Transaction.beginRefundRequest(for: id, in: c)
        #endif
        #if !os(tvOS)
        return s == .success
        #endif
    }

    func redeem() async throws {
        #if os(tvOS)
        throw AsukaNativeError.unsupported("purchase: tvOS has no offer code sheet")
        #elseif canImport(UIKit)
        guard let scene else { throw AsukaNativeError.unsupported("purchase: no screen") }
        try await AppStore.presentOfferCodeRedeemSheet(in: scene)
        #else
        guard #available(macOS 15.0, *) else { throw AsukaNativeError.unsupported("purchase: offer codes need macOS 15") }
        guard let c = window?.contentViewController else { throw AsukaNativeError.unsupported("purchase: no window") }
        try await AppStore.presentOfferCodeRedeemSheet(from: c)
        #endif
    }

    // MARK: The package's types

    nonisolated static func ns(_ d: Date) -> Int { Int((d.timeIntervalSince1970 * 1e9).rounded()) }

    nonisolated static func micros(_ d: Decimal) -> Int { NSDecimalNumber(decimal: d * 1_000_000).intValue }

    nonisolated static func kind(_ t: StoreKit.Product.ProductType) -> Kind {
        switch t {
        case .consumable: return .consumable
        case .autoRenewable: return .autoRenewable
        case .nonRenewable: return .nonRenewing
        default: return .nonConsumable
        }
    }

    nonisolated static func period(_ p: StoreKit.Product.SubscriptionPeriod) -> Period {
        let unit: PeriodUnit
        switch p.unit {
        case .day: unit = .day
        case .week: unit = .week
        case .month: unit = .month
        default: unit = .year
        }
        return Period(unit: unit, count: p.value)
    }

    nonisolated static func mode(_ m: StoreKit.Product.SubscriptionOffer.PaymentMode) -> Mode {
        switch m {
        case .freeTrial: return .freeTrial
        case .payAsYouGo: return .payAsYouGo
        case .payUpFront: return .payUpFront
        default: return .recurring
        }
    }

    nonisolated static func state(_ s: StoreKit.Product.SubscriptionInfo.RenewalState) -> SubscriptionState {
        switch s {
        case .subscribed: return .subscribed
        case .inBillingRetryPeriod: return .billingRetry
        case .inGracePeriod: return .gracePeriod
        case .revoked: return .revoked
        default: return .expired
        }
    }

    nonisolated static func reason(_ r: StoreKit.Product.SubscriptionInfo.RenewalInfo.ExpirationReason) -> String {
        switch r {
        case .autoRenewDisabled: return "autoRenewDisabled"
        case .billingError: return "billingError"
        case .didNotConsentToPriceIncrease: return "didNotConsentToPriceIncrease"
        case .productUnavailable: return "productUnavailable"
        default: return "unknown"
        }
    }

    nonisolated static func offerType(_ t: StoreKit.Transaction.OfferType) -> OfferType {
        switch t {
        case .introductory: return .introductory
        case .code: return .code
        default:
            if #available(iOS 18.0, macOS 15.0, tvOS 18.0, *), t == .winBack { return .winBack }
            return .promotional
        }
    }

    private func offer(_ o: StoreKit.Product.SubscriptionOffer, _ type: OfferType, _ currency: String) -> Offer {
        Offer(id: o.id ?? "", type: type, phases: [Phase(price: o.displayPrice, priceMicros: Self.micros(o.price), currency: currency,
                                                         period: Self.period(o.period), cycles: o.periodCount, mode: Self.mode(o.paymentMode),
                                                         amount: o.price)])
    }

    private func wire(_ p: StoreKit.Product) async -> Product {
        let currency = p.priceFormatStyle.currencyCode
        var sub: Subscription?
        if let s = p.subscription {
            var offers: [Offer] = []
            if let i = s.introductoryOffer { offers.append(offer(i, .introductory, currency)) }
            for o in s.promotionalOffers { offers.append(offer(o, .promotional, currency)) }
            if #available(iOS 18.0, macOS 15.0, tvOS 18.0, *) {
                for o in s.winBackOffers { offers.append(offer(o, .winBack, currency)) }
            }
            var level = 0
            if #available(iOS 16.4, macOS 13.3, *) { level = s.groupLevel }
            sub = Subscription(group: s.subscriptionGroupID, level: level, period: Self.period(s.subscriptionPeriod), offers: offers,
                               introEligible: await s.isEligibleForIntroOffer)
        }
        return Product(id: p.id, kind: Self.kind(p.type), title: p.displayName, description: p.description, price: p.displayPrice,
                       priceMicros: Self.micros(p.price), currency: currency, subscription: sub, familyShareable: p.isFamilyShareable,
                       amount: p.price)
    }

    nonisolated static func wire(_ v: VerificationResult<StoreKit.Transaction>) -> StoreTransaction {
        let t = v.unsafePayloadValue
        var verified = false
        if case .verified = v { verified = true }
        var env = "", store = "", offerID = "", offerType = OfferType.base
        if #available(iOS 16.0, macOS 13.0, *) { env = t.environment.rawValue }
        if #available(iOS 17.0, macOS 14.0, *) { store = t.storefront.countryCode } else { store = t.storefrontCountryCode }
        if #available(iOS 17.2, macOS 14.2, tvOS 17.2, *) {
            if let o = t.offer {
                offerID = o.id ?? ""
                offerType = Self.offerType(o.type)
            }
        } else if let ty = t.offerType {
            offerID = t.offerID ?? ""
            offerType = Self.offerType(ty)
        }
        return StoreTransaction(
            id: String(t.id), originalId: String(t.originalID), productId: t.productID, kind: kind(t.productType),
            state: t.revocationDate == nil ? .purchased : .revoked, quantity: t.purchasedQuantity, date: ns(t.purchaseDate),
            expires: t.expirationDate.map(ns) ?? 0, revoked: t.revocationDate.map(ns) ?? 0,
            revocationReason: t.revocationReason.map { $0 == .developerIssue ? "refundedForIssue" : "refundedForOther" } ?? "",
            upgraded: t.isUpgraded, familyShared: t.ownershipType == .familyShared, accountToken: t.appAccountToken?.uuidString.lowercased() ?? "",
            environment: env, storefront: store, offerId: offerID, offerType: offerType,
            verification: Verification(jws: v.jwsRepresentation, verified: verified))
    }
}

#if targetEnvironment(simulator)
/// A StoreKit configuration file (asuka.toml's ios.storekit) in asuka
/// dev's simulator build: the App Store, local, as Xcode's scheme sets it
/// up. The CLI puts the file in the bundle (AsukaStoreKit.storekit) and
/// launches the app as a test run is, Xcode's XCTest found, which
/// StoreKitTest's session (SKTestSession) needs.
enum StoreKitTest {
    @MainActor static var session: NSObject?

    @MainActor static func start() {
        guard session == nil, let file = Bundle.main.url(forResource: "AsukaStoreKit", withExtension: "storekit") else { return }
        // StoreKitTest loads XCTest from there, and ends the app if it can't.
        guard dlopen("/System/Library/Frameworks/XCTest.framework/XCTest", RTLD_NOW) != nil else {
            NSLog("asuka: the StoreKit configuration file is used when asuka dev launches the app (XCTest is Xcode's)")
            return
        }
        guard dlopen("/Developer/Library/Frameworks/StoreKitTest.framework/StoreKitTest", RTLD_NOW) != nil,
              let cls = NSClassFromString("SKTestSession") as? NSObject.Type else {
            NSLog("asuka: no StoreKitTest in this simulator: the StoreKit configuration file isn't used")
            return
        }
        // [[SKTestSession alloc] initWithContentsOfURL:error:], by their
        // implementations: alloc's object goes to init, whose answer is ours.
        typealias Alloc = @convention(c) (AnyClass, Selector) -> UnsafeMutableRawPointer?
        typealias Init = @convention(c) (UnsafeMutableRawPointer, Selector, NSURL, AutoreleasingUnsafeMutablePointer<NSError?>) -> Unmanaged<NSObject>?
        let allocSel = NSSelectorFromString("alloc"), initSel = NSSelectorFromString("initWithContentsOfURL:error:")
        guard let am = class_getClassMethod(cls, allocSel), let im = class_getInstanceMethod(cls, initSel),
              let obj = unsafeBitCast(method_getImplementation(am), to: Alloc.self)(cls, allocSel) else { return }
        var err: NSError?
        guard let s = unsafeBitCast(method_getImplementation(im), to: Init.self)(obj, initSel, file as NSURL, &err)?.takeRetainedValue() else {
            NSLog("asuka: the StoreKit configuration file: %@", err?.localizedDescription ?? "?")
            return
        }
        s.setValue(false, forKey: "disableDialogs")
        session = s
        NSLog("asuka: selling from the StoreKit configuration file")
    }
}
#endif

func makeStore() throws -> any Store {
    // StoreKit 2 is macOS 12's: before, a Mac has no store for the app.
    if #available(macOS 12.0, *) { return AppStoreStore() }
    throw PurchaseError.notAvailable
}

#if os(iOS)
/// StoreKit's transaction listener from the app's launch (the manifest's
/// ios.launch), as Apple asks: a transaction that comes before the app's
/// code reads (Ask to Buy approved, a renewal, a refund, an offer code
/// redeemed in the App Store) is kept for events().
@objc(AsukaPurchaseLaunch) final class PurchaseLaunch: NSObject, AsukaLaunching {
    @MainActor static func launch(_ options: [UIApplication.LaunchOptionsKey: Any]) -> Bool {
        Hub.shared.start()
        return false
    }
}
#endif
