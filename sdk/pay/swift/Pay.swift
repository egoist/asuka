// platform/pay on iOS and macOS: Apple Pay, with
// PKPaymentAuthorizationController. Its delegate's callbacks are asked
// of the app (asked), each completed when the app answers (answer); a
// callback the app has no handler for keeps the sheet as it is. The
// button is PKPaymentButton, made again when its type or variant changes
// (they're fixed at init).
import Foundation
import PassKit
import Contacts
import Asuka
#if canImport(UIKit)
import UIKit
#else
import AppKit
#endif

final class ApplePayWallets: Wallets {
    func available(request: Sheet) async throws -> Availability {
        let can = PKPaymentAuthorizationController.canMakePayments()
        let card = can && PKPaymentAuthorizationController.canMakePayments(usingNetworks: Requests.networks(request), capabilities: Requests.capabilities(request))
        return Availability(wallet: .applePay, canPay: can, hasCard: card)
    }

    func pay(request: Sheet, asked: @escaping (Ask) -> Void) async throws -> Payment {
        let ask = Asker(asked)
        return try await withCheckedThrowingContinuation { c in
            Task { @MainActor in Sheets.shared.present(request, ask) { c.resume(with: $0) } }
        }
    }

    func answer(id: Int, answer: Answer) throws {
        let a = answer
        Task { @MainActor in Sheets.shared.answered(id, a) }
    }

    func setUp() async throws {
        #if os(iOS)
        await MainActor.run { PKPassLibrary().openPaymentSetup() }
        #else
        throw PayError.notAvailable
        #endif
    }
}

/// The app's question callback, called from the main thread.
final class Asker: @unchecked Sendable {
    let f: (Ask) -> Void
    init(_ f: @escaping (Ask) -> Void) { self.f = f }
}

@MainActor final class Sheets: NSObject, @preconcurrency PKPaymentAuthorizationControllerDelegate {
    static let shared = Sheets()

    private var controller: PKPaymentAuthorizationController?
    private var request = Sheet()
    private var ask: Asker?
    private var done: ((Swift.Result<Payment, Error>) -> Void)?
    private var paid: Payment?
    private var next = 0
    /// The sheet's summary items and methods now, for the callbacks the
    /// app has no handler for.
    private var items: [PKPaymentSummaryItem] = []
    private var methods: [PKShippingMethod] = []
    private var pending: [Int: (Answer) -> Void] = [:]

    func present(_ r: Sheet, _ ask: Asker, _ done: @escaping (Swift.Result<Payment, Error>) -> Void) {
        if controller != nil { done(.failure(PayError.busy)); return }
        guard PKPaymentAuthorizationController.canMakePayments() else { done(.failure(PayError.notAvailable)); return }
        request = r
        self.ask = ask
        self.done = done
        paid = nil
        pending = [:]
        let pr = Requests.paymentRequest(r)
        items = pr.paymentSummaryItems
        methods = pr.shippingMethods ?? []
        let c = PKPaymentAuthorizationController(paymentRequest: pr)
        c.delegate = self
        controller = c
        c.present { ok in
            DispatchQueue.main.async {
                MainActor.assumeIsolated {
                    guard !ok, let s = Sheets.shared.done else { return }
                    Sheets.shared.controller = nil
                    Sheets.shared.done = nil
                    s(.failure(AsukaNativeError.unsupported("pay: the payment sheet didn't show (check the merchant ID and the networks)")))
                }
            }
        }
    }

    /// Asks the app; then runs with its answer.
    private func send(_ a: Ask, _ then: @escaping (Answer) -> Void) {
        next += 1
        var q = a
        q.id = next
        pending[next] = then
        ask?.f(q)
    }

    func answered(_ id: Int, _ a: Answer) {
        pending.removeValue(forKey: id)?(a)
    }

    private func asks(_ k: AskKind) -> Bool { request.asks.contains(k) }

    /// keep takes an answer's items and methods as the sheet's now.
    private func keep(_ a: Answer) {
        if let i = a.update.items {
            var all = i
            all.append(a.update.total.map { merge(request.total, $0) } ?? request.total)
            items = all.map(Requests.item)
            request.items = i
        } else if let t = a.update.total {
            request.total = merge(request.total, t)
            items = (request.items + [request.total]).map(Requests.item)
        }
        if let t = a.update.total { request.total = merge(request.total, t) }
        if let m = a.update.shippingMethods { methods = m.map(Requests.method) }
    }

    private func merge(_ old: Item, _ new: Item) -> Item {
        Item(label: new.label.isEmpty ? old.label : new.label, amount: new.amount.isEmpty ? old.amount : new.amount, pending: new.pending, kind: new.kind)
    }

    private func errors(_ a: Answer) -> [Error] { a.update.errors.map(Requests.error) }

    // The delegate.

    func paymentAuthorizationController(_ c: PKPaymentAuthorizationController, didAuthorizePayment payment: PKPayment,
                                        handler completion: @escaping (PKPaymentAuthorizationResult) -> Void) {
        let p = Requests.payment(payment)
        send(Ask(kind: .authorize, payment: p)) { [unowned self] a in
            let errs = self.errors(a)
            if errs.isEmpty && !a.failed { self.paid = p }
            completion(PKPaymentAuthorizationResult(status: errs.isEmpty && !a.failed ? .success : .failure, errors: errs))
        }
    }

    func paymentAuthorizationController(_ c: PKPaymentAuthorizationController, didSelectShippingContact contact: PKContact,
                                        handler completion: @escaping (PKPaymentRequestShippingContactUpdate) -> Void) {
        guard asks(.shippingContact) else {
            completion(PKPaymentRequestShippingContactUpdate(errors: nil, paymentSummaryItems: items, shippingMethods: methods))
            return
        }
        send(Ask(kind: .shippingContact, contact: Requests.contact(contact))) { [unowned self] a in
            self.keep(a)
            completion(PKPaymentRequestShippingContactUpdate(errors: self.errors(a), paymentSummaryItems: self.items, shippingMethods: self.methods))
        }
    }

    func paymentAuthorizationController(_ c: PKPaymentAuthorizationController, didSelectShippingMethod method: PKShippingMethod,
                                        handler completion: @escaping (PKPaymentRequestShippingMethodUpdate) -> Void) {
        guard asks(.shippingMethod) else { completion(PKPaymentRequestShippingMethodUpdate(paymentSummaryItems: items)); return }
        let m = request.shippingMethods.first { $0.id == method.identifier } ?? ShippingMethod(id: method.identifier ?? "", label: method.label)
        send(Ask(kind: .shippingMethod, method: m)) { [unowned self] a in
            self.keep(a)
            let u = PKPaymentRequestShippingMethodUpdate(paymentSummaryItems: self.items)
            if !a.update.errors.isEmpty { u.status = .failure }
            completion(u)
        }
    }

    func paymentAuthorizationController(_ c: PKPaymentAuthorizationController, didSelectPaymentMethod method: PKPaymentMethod,
                                        handler completion: @escaping (PKPaymentRequestPaymentMethodUpdate) -> Void) {
        guard asks(.paymentMethod) else { completion(PKPaymentRequestPaymentMethodUpdate(errors: nil, paymentSummaryItems: items)); return }
        send(Ask(kind: .paymentMethod, card: Requests.card(method))) { [unowned self] a in
            self.keep(a)
            completion(PKPaymentRequestPaymentMethodUpdate(errors: self.errors(a), paymentSummaryItems: self.items))
        }
    }

    @available(iOS 15.0, macOS 12.0, *)
    func paymentAuthorizationController(_ c: PKPaymentAuthorizationController, didChangeCouponCode code: String,
                                        handler completion: @escaping (PKPaymentRequestCouponCodeUpdate) -> Void) {
        guard asks(.coupon) else {
            completion(PKPaymentRequestCouponCodeUpdate(errors: nil, paymentSummaryItems: items, shippingMethods: methods))
            return
        }
        send(Ask(kind: .coupon, coupon: code)) { [unowned self] a in
            self.keep(a)
            completion(PKPaymentRequestCouponCodeUpdate(errors: self.errors(a), paymentSummaryItems: self.items, shippingMethods: self.methods))
        }
    }

    func paymentAuthorizationControllerDidFinish(_ c: PKPaymentAuthorizationController) {
        c.dismiss(completion: nil)
        guard c === controller else { return }
        controller = nil
        pending = [:]
        let d = done
        done = nil
        if let p = paid { d?(.success(p)) } else { d?(.failure(PayError.canceled)) }
    }

    #if os(iOS)
    func presentationWindow(for c: PKPaymentAuthorizationController) -> UIWindow? {
        let scenes = UIApplication.shared.connectedScenes.compactMap { $0 as? UIWindowScene }
        return scenes.flatMap(\.windows).first(where: \.isKeyWindow) ?? scenes.first?.windows.first
    }
    #else
    func presentationWindow(for c: PKPaymentAuthorizationController) -> NSWindow? {
        NSApp.keyWindow ?? NSApp.mainWindow
    }
    #endif
}

/// The package's types as PassKit's, and back.
enum Requests {
    static let networkNames: [(Network, PKPaymentNetwork)] = {
        var n: [(Network, PKPaymentNetwork)] = [
            (.visa, .visa), (.mastercard, .masterCard), (.amex, .amex), (.discover, .discover), (.jcb, .JCB),
            (.interac, .interac), (.unionPay, .chinaUnionPay), (.maestro, .maestro), (.electron, .electron),
            (.elo, .elo), (.cartesBancaires, .cartesBancaires), (.eftpos, .eftpos), (.mada, .mada),
            (.girocard, .girocard), (.privateLabel, .privateLabel),
        ]
        if #available(iOS 14.5, macOS 11.5, *) { n.append((.mir, .mir)) }
        if #available(iOS 14.5, macOS 12.1, *) { n.append((.dankort, .dankort)) }
        if #available(iOS 16.0, macOS 13.0, *) { n.append((.bancontact, .bancontact)) }
        return n
    }()

    static func networks(_ r: Sheet) -> [PKPaymentNetwork] {
        let ns = r.networks.isEmpty ? [.visa, .mastercard, .amex, .discover] : r.networks
        return ns.compactMap { n in networkNames.first { $0.0 == n }?.1 }
    }

    static func network(_ n: PKPaymentNetwork?) -> Network {
        guard let n else { return .other }
        return networkNames.first { $0.1 == n }?.0 ?? .other
    }

    static func capabilities(_ r: Sheet) -> PKMerchantCapability {
        var c: PKMerchantCapability = .threeDSecure
        if r.apple.emv { c.insert(.emv) }
        if r.cards.contains(.credit) { c.insert(.credit) }
        if r.cards.contains(.debit) || r.cards.contains(.prepaid) { c.insert(.debit) }
        return c
    }

    static func fields(_ fs: [ContactField]) -> Set<PKContactField> {
        var s = Set<PKContactField>()
        for f in fs {
            switch f {
            case .name: s.insert(.name)
            case .email: s.insert(.emailAddress)
            case .phone: s.insert(.phoneNumber)
            case .address: s.insert(.postalAddress)
            case .phoneticName: s.insert(.phoneticName)
            default: break
            }
        }
        return s
    }

    static func decimal(_ s: String) -> NSDecimalNumber {
        let d = NSDecimalNumber(string: s.isEmpty ? "0" : s, locale: Locale(identifier: "en_US_POSIX"))
        return d == .notANumber ? .zero : d
    }

    /// "2026-11-20" as date components, and as a date.
    static func day(_ s: String) -> DateComponents? {
        let p = s.split(separator: "-").compactMap { Int($0) }
        guard p.count == 3 else { return nil }
        return DateComponents(calendar: Calendar(identifier: .gregorian), year: p[0], month: p[1], day: p[2])
    }

    static func date(_ s: String) -> Date? { day(s)?.date }

    static func item(_ i: Item) -> PKPaymentSummaryItem {
        PKPaymentSummaryItem(label: i.label, amount: decimal(i.amount), type: i.pending ? .pending : .final)
    }

    static func method(_ m: ShippingMethod) -> PKShippingMethod {
        let p = PKShippingMethod(label: m.label, amount: decimal(m.amount))
        p.identifier = m.id
        p.detail = m.detail
        if #available(iOS 15.0, macOS 12.0, *), let f = day(m.from), let t = day(m.to.isEmpty ? m.from : m.to) {
            p.dateComponentsRange = PKDateComponentsRange(start: f, end: t)
        }
        return p
    }

    @available(iOS 15.0, macOS 12.0, *)
    static func recurring(_ b: Billing) -> PKRecurringPaymentSummaryItem {
        let r = PKRecurringPaymentSummaryItem(label: b.label, amount: decimal(b.amount), type: b.pending ? .pending : .final)
        r.startDate = date(b.start)
        r.endDate = date(b.end)
        switch b.interval {
        case .day: r.intervalUnit = .day
        case .week: r.intervalUnit = .weekOfYear
        case .year: r.intervalUnit = .year
        default: r.intervalUnit = .month
        }
        r.intervalCount = max(b.every, 1)
        return r
    }

    static func paymentRequest(_ s: Sheet) -> PKPaymentRequest {
        let r = PKPaymentRequest()
        r.merchantIdentifier = s.apple.merchantId
        r.countryCode = s.country
        r.currencyCode = s.currency
        r.supportedNetworks = networks(s)
        r.merchantCapabilities = capabilities(s)
        if !s.cardCountries.isEmpty { r.supportedCountries = Set(s.cardCountries) }
        r.paymentSummaryItems = (s.items + [s.total]).map(item)
        r.requiredShippingContactFields = fields(s.shipping)
        r.requiredBillingContactFields = fields(s.billing)
        switch s.shippingType {
        case .delivery: r.shippingType = .delivery
        case .storePickup: r.shippingType = .storePickup
        case .servicePickup: r.shippingType = .servicePickup
        default: r.shippingType = .shipping
        }
        if !s.shippingMethods.isEmpty { r.shippingMethods = s.shippingMethods.map(method) }
        if #available(iOS 15.0, macOS 12.0, *) {
            r.shippingContactEditingMode = s.shippingType == .storePickup ? .storePickup : .available
            r.supportsCouponCode = s.coupons
            if !s.coupon.isEmpty { r.couponCode = s.coupon }
        }
        if !s.apple.applicationData.isEmpty { r.applicationData = s.apple.applicationData }
        if #available(iOS 16.0, macOS 13.0, *) {
            if let x = s.recurring, let url = URL(string: x.managementURL) {
                let rr = PKRecurringPaymentRequest(paymentDescription: x.description, regularBilling: recurring(x.regular), managementURL: url)
                rr.trialBilling = x.trial.map(recurring)
                if !x.billingAgreement.isEmpty { rr.billingAgreement = x.billingAgreement }
                rr.tokenNotificationURL = URL(string: x.tokenNotificationURL)
                r.recurringPaymentRequest = rr
            }
            if let x = s.automaticReload, let url = URL(string: x.managementURL) {
                let b = PKAutomaticReloadPaymentSummaryItem(label: x.billing.label, amount: decimal(x.billing.amount), type: .final)
                b.thresholdAmount = decimal(x.threshold)
                let rr = PKAutomaticReloadPaymentRequest(paymentDescription: x.description, automaticReloadBilling: b, managementURL: url)
                if !x.billingAgreement.isEmpty { rr.billingAgreement = x.billingAgreement }
                rr.tokenNotificationURL = URL(string: x.tokenNotificationURL)
                r.automaticReloadPaymentRequest = rr
            }
        }
        if #available(iOS 17.0, macOS 14.0, *), let x = s.deferred, let url = URL(string: x.managementURL), let at = date(x.date) {
            let b = PKDeferredPaymentSummaryItem(label: x.billing.label, amount: decimal(x.billing.amount), type: .final)
            b.deferredDate = at
            let d = PKDeferredPaymentRequest(paymentDescription: x.description, deferredBilling: b, managementURL: url)
            if !x.billingAgreement.isEmpty { d.billingAgreement = x.billingAgreement }
            d.tokenNotificationURL = URL(string: x.tokenNotificationURL)
            if let f = date(x.freeCancellation) {
                d.freeCancellationDate = f
                d.freeCancellationDateTimeZone = .current
            }
            r.deferredPaymentRequest = d
        }
        return r
    }

    /// PassKit's error for a field, which the sheet shows by it.
    static func error(_ e: FieldError) -> Error {
        let m = e.message
        switch e.field {
        case .shippingStreet: return PKPaymentRequest.paymentShippingAddressInvalidError(withKey: CNPostalAddressStreetKey, localizedDescription: m)
        case .shippingCity: return PKPaymentRequest.paymentShippingAddressInvalidError(withKey: CNPostalAddressCityKey, localizedDescription: m)
        case .shippingState: return PKPaymentRequest.paymentShippingAddressInvalidError(withKey: CNPostalAddressStateKey, localizedDescription: m)
        case .shippingPostalCode: return PKPaymentRequest.paymentShippingAddressInvalidError(withKey: CNPostalAddressPostalCodeKey, localizedDescription: m)
        case .shippingCountry: return PKPaymentRequest.paymentShippingAddressInvalidError(withKey: CNPostalAddressISOCountryCodeKey, localizedDescription: m)
        case .billingAddress, .billingStreet: return PKPaymentRequest.paymentBillingAddressInvalidError(withKey: CNPostalAddressStreetKey, localizedDescription: m)
        case .billingCity: return PKPaymentRequest.paymentBillingAddressInvalidError(withKey: CNPostalAddressCityKey, localizedDescription: m)
        case .billingState: return PKPaymentRequest.paymentBillingAddressInvalidError(withKey: CNPostalAddressStateKey, localizedDescription: m)
        case .billingPostalCode: return PKPaymentRequest.paymentBillingAddressInvalidError(withKey: CNPostalAddressPostalCodeKey, localizedDescription: m)
        case .billingCountry: return PKPaymentRequest.paymentBillingAddressInvalidError(withKey: CNPostalAddressISOCountryCodeKey, localizedDescription: m)
        case .shippingAddress: return PKPaymentRequest.paymentContactInvalidError(withContactField: .postalAddress, localizedDescription: m)
        case .shippingName: return PKPaymentRequest.paymentContactInvalidError(withContactField: .name, localizedDescription: m)
        case .shippingEmail: return PKPaymentRequest.paymentContactInvalidError(withContactField: .emailAddress, localizedDescription: m)
        case .shippingPhone: return PKPaymentRequest.paymentContactInvalidError(withContactField: .phoneNumber, localizedDescription: m)
        case .unserviceable: return PKPaymentRequest.paymentShippingAddressUnserviceableError(withLocalizedDescription: m)
        case .billingName:
            return NSError(domain: PKPaymentErrorDomain, code: PKPaymentError.billingContactInvalidError.rawValue,
                           userInfo: [PKPaymentErrorKey.contactFieldUserInfoKey.rawValue: PKContactField.name, NSLocalizedDescriptionKey: m])
        case .couponInvalid:
            if #available(iOS 15.0, macOS 12.0, *) { return PKPaymentRequest.paymentCouponCodeInvalidError(localizedDescription: m) }
        case .couponExpired:
            if #available(iOS 15.0, macOS 12.0, *) { return PKPaymentRequest.paymentCouponCodeExpiredError(localizedDescription: m) }
        default: break
        }
        return NSError(domain: PKPaymentErrorDomain, code: PKPaymentError.unknownError.rawValue, userInfo: [NSLocalizedDescriptionKey: m])
    }

    static func payment(_ p: PKPayment) -> Payment {
        Payment(wallet: .applePay, token: String(decoding: p.token.paymentData, as: UTF8.self), transactionId: p.token.transactionIdentifier,
                card: card(p.token.paymentMethod), shipping: p.shippingContact.map(contact) ?? Contact(),
                billing: p.billingContact.map(contact) ?? Contact(), shippingMethod: p.shippingMethod?.identifier ?? "")
    }

    static func card(_ m: PKPaymentMethod) -> Card {
        let kind: CardKind
        switch m.type {
        case .debit: kind = .debit
        case .credit: kind = .credit
        case .prepaid: kind = .prepaid
        case .store: kind = .store
        case .eMoney: kind = .eMoney
        default: kind = .unknown
        }
        let name = m.displayName ?? ""
        var billing = Contact()
        if let b = m.billingAddress {
            billing = Contact(givenName: b.givenName, familyName: b.familyName, address: b.postalAddresses.first.map { address($0.value) } ?? PostalAddress())
        }
        return Card(network: network(m.network), kind: kind, name: name, last4: String(name.suffix(4)).allSatisfy(\.isNumber) ? String(name.suffix(4)) : "", billing: billing)
    }

    static func contact(_ c: PKContact) -> Contact {
        var r = Contact(email: c.emailAddress ?? "", phone: c.phoneNumber?.stringValue ?? "")
        if let n = c.name {
            r.givenName = n.givenName ?? ""
            r.familyName = n.familyName ?? ""
            r.name = PersonNameComponentsFormatter().string(from: n)
        }
        if let a = c.postalAddress { r.address = address(a) }
        return r
    }

    static func address(_ a: CNPostalAddress) -> PostalAddress {
        PostalAddress(lines: a.street.split(separator: "\n").map(String.init), city: a.city, subLocality: a.subLocality, state: a.state,
                      subAdministrativeArea: a.subAdministrativeArea, postalCode: a.postalCode, countryCode: a.isoCountryCode.uppercased(), country: a.country)
    }
}

/// The button, in a box that holds it.
@MainActor final class ApplePayButton: PayButton {
    let events: PayButtonEvents
    let box = ButtonBox()
    var view: AsukaPlatformView { box }
    private var type = ButtonType.buy, variant = ButtonVariant.automatic
    private var radius: Double = -1

    init(_ events: PayButtonEvents) {
        self.events = events
        make()
    }

    func setType(_ value: ButtonType) { type = value; make() }
    func setVariant(_ value: ButtonVariant) { variant = value; make() }
    func setCornerRadius(_ value: Double) {
        radius = value
        if radius >= 0 { box.button?.cornerRadius = radius }
    }
    func setNetworks(_ value: [Network]) {}

    private func make() {
        let t: PKPaymentButtonType
        switch type {
        case .plain, .pay: t = .plain
        case .checkout: t = .checkout
        case .book: t = .book
        case .donate: t = .donate
        case .order: t = .order
        case .subscribe: t = .subscribe
        case .continueWith:
            if #available(iOS 15.0, macOS 12.0, *) { t = .continue } else { t = .plain }
        case .setUp: t = .setUp
        case .reload: t = .reload
        case .addMoney: t = .addMoney
        case .topUp: t = .topUp
        case .rent: t = .rent
        case .support: t = .support
        case .contribute: t = .contribute
        case .tip: t = .tip
        default: t = .buy
        }
        // Automatic follows the appearance; the rest are fixed.
        let s: PKPaymentButtonStyle
        switch variant {
        case .black: s = .black
        case .white: s = .white
        case .whiteOutline: s = .whiteOutline
        default: s = .automatic
        }
        let b = PKPaymentButton(paymentButtonType: t, paymentButtonStyle: s)
        if radius >= 0 { b.cornerRadius = radius }
        #if canImport(UIKit)
        b.addTarget(self, action: #selector(pressed), for: .touchUpInside)
        #else
        b.target = self
        b.action = #selector(pressed)
        #endif
        box.button = b
    }

    @objc private func pressed() { events.press() }
}

/// Holds the button, which fills it: as wide as it's given (at least
/// Apple's 140), 44 points tall (30 on the Mac).
final class ButtonBox: AsukaPlatformView {
    var button: PKPaymentButton? {
        didSet {
            oldValue?.removeFromSuperview()
            guard let b = button else { return }
            b.frame = bounds
            #if canImport(UIKit)
            b.autoresizingMask = [.flexibleWidth, .flexibleHeight]
            #else
            b.autoresizingMask = [.width, .height]
            #endif
            addSubview(b)
        }
    }

    #if canImport(UIKit)
    override func sizeThatFits(_ size: CGSize) -> CGSize { CGSize(width: max(min(size.width, 600), 140), height: 44) }
    #else
    override var fittingSize: NSSize { NSSize(width: 200, height: 30) }
    #endif
}

func makeWallets() throws -> any Wallets { ApplePayWallets() }

@MainActor func makePayButton(_ events: PayButtonEvents) -> any PayButton { ApplePayButton(events) }
