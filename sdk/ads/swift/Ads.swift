// platform/ads on iOS: Google's Mobile Ads SDK (AdMob) and its User
// Messaging Platform. The SDK reads the app's ID from Info.plist's
// GADApplicationIdentifier (asuka.toml's ads.ios). Its types that share
// a name with the package's (Request, ConsentStatus, DebugGeography) are
// named with their modules here.
import Foundation
import UIKit
import Asuka
import GoogleMobileAds
import UserMessagingPlatform

final class GoogleMobile: Mobile {
    func start() async throws {
        await Starter.shared.start()
    }

    func configure(settings: Settings) throws {
        let c = MobileAds.shared.requestConfiguration
        c.testDeviceIdentifiers = settings.testDevices
        switch settings.age {
        case .child: c.ageRestrictedTreatment = .child
        case .teen: c.ageRestrictedTreatment = .teen
        default: c.ageRestrictedTreatment = .unspecified
        }
        switch settings.maxRating {
        case .general: c.maxAdContentRating = .general
        case .parentalGuidance: c.maxAdContentRating = .parentalGuidance
        case .teen: c.maxAdContentRating = .teen
        case .matureAudience: c.maxAdContentRating = .matureAudience
        default: c.maxAdContentRating = nil
        }
        switch settings.personalized {
        case true?: c.publisherPrivacyPersonalizationState = .enabled
        case false?: c.publisherPrivacyPersonalizationState = .disabled
        case nil: c.publisherPrivacyPersonalizationState = .default
        }
        MobileAds.shared.isApplicationMuted = settings.muted
        MobileAds.shared.applicationVolume = Float(min(max(settings.volume, 0), 1))
    }

    func consent(options: ConsentOptions) async throws -> Consent {
        let p = RequestParameters()
        p.isTaggedForUnderAgeOfConsent = options.underAgeOfConsent
        if options.debugGeography != .disabled || !options.testDevices.isEmpty {
            let d = DebugSettings()
            d.testDeviceIdentifiers = options.testDevices
            switch options.debugGeography {
            case .eea: d.geography = .EEA
            case .regulatedUSState: d.geography = .regulatedUSState
            case .other: d.geography = .other
            default: d.geography = .disabled
            }
            p.debugSettings = d
        }
        try await Consents.update(p)
        try await Consents.gather()
        return Consents.now()
    }

    func currentConsent() throws -> Consent { Consents.now() }

    func privacyOptions() async throws -> Consent {
        try await Consents.privacyOptions()
        return Consents.now()
    }

    func resetConsent() throws { ConsentInformation.shared.reset() }

    func load(format: Format, unit: String, request: Request) async throws -> any Loaded {
        try await FullScreen.load(format, unit, request)
    }
}

/// Starts the SDK once: later calls wait for the first.
@MainActor final class Starter {
    static let shared = Starter()
    private var done = false
    private var waiting: [CheckedContinuation<Void, Never>] = []

    func start() async {
        if done { return }
        await withCheckedContinuation { c in
            waiting.append(c)
            if waiting.count > 1 { return }
            MobileAds.shared.start { _ in
                DispatchQueue.main.async {
                    MainActor.assumeIsolated {
                        let s = Starter.shared
                        s.done = true
                        let w = s.waiting
                        s.waiting = []
                        for c in w { c.resume() }
                    }
                }
            }
        }
    }
}

/// The User Messaging Platform's consent.
enum Consents {
    static func now() -> Consent {
        let i = ConsentInformation.shared
        let status: ConsentStatus
        switch i.consentStatus {
        case .required: status = .required
        case .notRequired: status = .notRequired
        case .obtained: status = .obtained
        default: status = .unknown
        }
        return Consent(status: status, canRequestAds: i.canRequestAds, privacyOptionsRequired: i.privacyOptionsRequirementStatus == .required)
    }

    static func update(_ p: RequestParameters) async throws {
        try await withCheckedThrowingContinuation { (c: CheckedContinuation<Void, Error>) in
            DispatchQueue.main.async {
                ConsentInformation.shared.requestConsentInfoUpdate(with: p) { e in
                    if let e { c.resume(throwing: e) } else { c.resume() }
                }
            }
        }
    }

    /// Shows the consent form if it's required.
    static func gather() async throws {
        try await withCheckedThrowingContinuation { (c: CheckedContinuation<Void, Error>) in
            DispatchQueue.main.async {
                MainActor.assumeIsolated {
                    guard let vc = AsukaNative.presenter else {
                        c.resume(throwing: AsukaNativeError.unsupported("ads: no screen to show the consent form on"))
                        return
                    }
                    ConsentForm.loadAndPresentIfRequired(from: vc) { e in
                        if let e { c.resume(throwing: e) } else { c.resume() }
                    }
                }
            }
        }
    }

    static func privacyOptions() async throws {
        try await withCheckedThrowingContinuation { (c: CheckedContinuation<Void, Error>) in
            DispatchQueue.main.async {
                MainActor.assumeIsolated {
                    guard let vc = AsukaNative.presenter else {
                        c.resume(throwing: AsukaNativeError.unsupported("ads: no screen to show the privacy options on"))
                        return
                    }
                    ConsentForm.presentPrivacyOptionsForm(from: vc) { e in
                        if let e { c.resume(throwing: e) } else { c.resume() }
                    }
                }
            }
        }
    }
}

/// The SDK's errors as the package's, where one says it (the rest as
/// they are: their domain, code and message).
func adsError(_ e: Error) -> Error {
    let ns = e as NSError
    guard ns.domain == GADErrorDomain else { return e }
    switch ns.code {
    case 1, 9: return AdsError.noFill  // no fill, mediation no fill
    case 2, 5: return AdsError.network  // network, timeout
    case 0, 20: return AdsError.invalidRequest  // invalid request, no app ID
    case 18, 19: return AdsError.alreadyShown
    default: return e
    }
}

/// A full-screen ad, loaded: shown once, from the app's top view
/// controller.
@MainActor final class FullScreen: NSObject, Loaded, FullScreenContentDelegate {
    private enum Ad {
        case interstitial(InterstitialAd)
        case rewarded(RewardedAd)
        case rewardedInterstitial(RewardedInterstitialAd)
        case appOpen(AppOpenAd)

        var presenting: FullScreenPresentingAd {
            switch self {
            case .interstitial(let a): return a
            case .rewarded(let a): return a
            case .rewardedInterstitial(let a): return a
            case .appOpen(let a): return a
            }
        }
    }

    private var ad: Ad?
    private var shown = false
    private var earned: Reward?
    private var done: CheckedContinuation<Reward?, Error>?

    private init(_ ad: Ad) {
        self.ad = ad
        super.init()
        ad.presenting.fullScreenContentDelegate = self
    }

    nonisolated static func load(_ format: Format, _ unit: String, _ request: Request) async throws -> any Loaded {
        try await withCheckedThrowingContinuation { (c: CheckedContinuation<any Loaded, Error>) in
            DispatchQueue.main.async {
                MainActor.assumeIsolated { start(format, unit, request, c) }
            }
        }
    }

    private static func start(_ format: Format, _ unit: String, _ request: Request, _ c: CheckedContinuation<any Loaded, Error>) {
        let r = gadRequest(request)
        func ssv<T>(_ ad: T?, _ set: (T, ServerSideVerificationOptions) -> Void) {
            guard let ad, !request.userId.isEmpty || !request.customData.isEmpty else { return }
            let o = ServerSideVerificationOptions()
            if !request.userId.isEmpty { o.userIdentifier = request.userId }
            if !request.customData.isEmpty { o.customRewardText = request.customData }
            set(ad, o)
        }
        func answer(_ ad: Ad?, _ e: Error?) {
            if let ad {
                c.resume(returning: FullScreen(ad))
            } else {
                c.resume(throwing: adsError(e ?? AdsError.noFill))
            }
        }
        switch format {
        case .rewarded:
            RewardedAd.load(with: unit, request: r) { ad, e in
                ssv(ad) { $0.serverSideVerificationOptions = $1 }
                answer(ad.map { .rewarded($0) }, e)
            }
        case .rewardedInterstitial:
            RewardedInterstitialAd.load(with: unit, request: r) { ad, e in
                ssv(ad) { $0.serverSideVerificationOptions = $1 }
                answer(ad.map { .rewardedInterstitial($0) }, e)
            }
        case .appOpen:
            AppOpenAd.load(with: unit, request: r) { ad, e in answer(ad.map { .appOpen($0) }, e) }
        case .interstitial:
            InterstitialAd.load(with: unit, request: r) { ad, e in answer(ad.map { .interstitial($0) }, e) }
        default:
            c.resume(throwing: AdsError.invalidRequest)
        }
    }

    nonisolated func show() async throws -> Reward? {
        try await withCheckedThrowingContinuation { (c: CheckedContinuation<Reward?, Error>) in
            DispatchQueue.main.async {
                MainActor.assumeIsolated { self.present(c) }
            }
        }
    }

    private func present(_ c: CheckedContinuation<Reward?, Error>) {
        guard let ad, !shown else {
            c.resume(throwing: AdsError.alreadyShown)
            return
        }
        guard let vc = AsukaNative.presenter else {
            c.resume(throwing: AsukaNativeError.unsupported("ads: no screen to show the ad on"))
            return
        }
        shown = true
        done = c
        switch ad {
        case .interstitial(let a): a.present(from: vc)
        case .appOpen(let a): a.present(from: vc)
        case .rewarded(let a):
            a.present(from: vc) { [weak self] in self?.earned = Reward(type: a.adReward.type, amount: a.adReward.amount.intValue) }
        case .rewardedInterstitial(let a):
            a.present(from: vc) { [weak self] in self?.earned = Reward(type: a.adReward.type, amount: a.adReward.amount.intValue) }
        }
    }

    nonisolated func close() throws {
        DispatchQueue.main.async {
            MainActor.assumeIsolated {
                // Kept while it shows: let go once it's closed.
                if self.done == nil { self.ad = nil }
            }
        }
    }

    func ad(_ ad: FullScreenPresentingAd, didFailToPresentFullScreenContentWithError error: Error) {
        finish(.failure(adsError(error)))
    }

    func adDidDismissFullScreenContent(_ ad: FullScreenPresentingAd) {
        finish(.success(earned))
    }

    private func finish(_ r: Swift.Result<Reward?, Error>) {
        let c = done
        done = nil
        ad = nil
        c?.resume(with: r)
    }
}

/// A request as the SDK takes it.
@MainActor func gadRequest(_ r: Request) -> GoogleMobileAds.Request {
    let g = GoogleMobileAds.Request()
    if !r.keywords.isEmpty { g.keywords = r.keywords }
    if !r.contentURL.isEmpty { g.contentURL = r.contentURL }
    g.scene = AsukaNative.window?.windowScene
    return g
}

/// A banner: Google's BannerView in a box Asuka lays out, loaded once
/// it's in a window and has its width (again when the width, the unit,
/// the size or the request changes).
@MainActor final class GoogleBanner: NSObject, Banner, BannerViewDelegate, AsukaDestroying {
    let events: BannerEvents
    let box = BannerBox()
    var view: AsukaPlatformView { box }
    private var unit = ""
    private var size = BannerSize.adaptive
    private var request = Request()
    private var banner: BannerView?
    private var stale = true
    private var loadedFor: CGFloat = -1

    init(_ events: BannerEvents) {
        self.events = events
        super.init()
        box.laidOut = { [weak self] in self?.loadIfNeeded() }
        box.measure = { [weak self] in self?.sizeThatFits($0) ?? .zero }
    }

    func setUnit(_ value: String) { unit = value; changed() }
    func setSize(_ value: BannerSize) { size = value; changed() }
    func setRequest(_ value: Request) { request = value; changed() }

    private func changed() {
        stale = true
        box.setNeedsLayout()
    }

    /// The ad's size for a width.
    private func adSize(_ width: CGFloat) -> AdSize {
        switch size {
        case .banner: return AdSizeBanner
        case .largeBanner: return AdSizeLargeBanner
        case .mediumRectangle: return AdSizeMediumRectangle
        case .fullBanner: return AdSizeFullBanner
        case .leaderboard: return AdSizeLeaderboard
        default: return largeAnchoredAdaptiveBanner(width: width)
        }
    }

    /// The width it's laid out at, or would be: at most the screen's (a
    /// measure without a width proposes no limit).
    private func width(_ w: CGFloat) -> CGFloat {
        let screen = box.window?.bounds.width ?? AsukaNative.window?.bounds.width ?? 320
        return w.isFinite && w > 0 ? min(w, screen) : screen
    }

    func sizeThatFits(_ s: CGSize) -> CGSize {
        let w = width(s.width)
        let a = cgSize(for: adSize(w))
        return CGSize(width: size == .adaptive ? w : a.width, height: a.height)
    }

    private func loadIfNeeded() {
        let w = box.bounds.width
        guard box.window != nil, w > 0, !unit.isEmpty else { return }
        if size == .adaptive && abs(w - loadedFor) > 0.5 { stale = true }
        if let b = banner {
            let a = cgSize(for: b.adSize)
            b.frame = CGRect(x: ((w - a.width) / 2).rounded(), y: 0, width: a.width, height: a.height)
        }
        guard stale else { return }
        stale = false
        loadedFor = w
        banner?.delegate = nil
        banner?.removeFromSuperview()
        let b = BannerView(adSize: adSize(w))
        b.adUnitID = unit
        b.rootViewController = AsukaNative.presenter
        b.delegate = self
        let a = cgSize(for: b.adSize)
        b.frame = CGRect(x: ((w - a.width) / 2).rounded(), y: 0, width: a.width, height: a.height)
        box.addSubview(b)
        banner = b
        b.load(gadRequest(request))
    }

    func destroy() {
        banner?.delegate = nil
        banner?.removeFromSuperview()
        banner = nil
    }

    func bannerViewDidReceiveAd(_ bannerView: BannerView) { events.loaded() }
    func bannerView(_ bannerView: BannerView, didFailToReceiveAdWithError error: Error) {
        events.error(message: (adsError(error) as? AdsError)?.errorDescription ?? error.localizedDescription)
    }
    func bannerViewDidRecordImpression(_ bannerView: BannerView) { events.impression() }
    func bannerViewDidRecordClick(_ bannerView: BannerView) { events.click() }
    func bannerViewWillPresentScreen(_ bannerView: BannerView) { events.open() }
    func bannerViewDidDismissScreen(_ bannerView: BannerView) { events.close() }
}

/// The banner's box: it measures as the banner's size, and says when
/// it's laid out or put in a window, for the banner to load then.
final class BannerBox: UIView {
    var laidOut: (() -> Void)?
    var measure: ((CGSize) -> CGSize)?

    override func sizeThatFits(_ size: CGSize) -> CGSize { measure?(size) ?? .zero }

    override func layoutSubviews() {
        super.layoutSubviews()
        laidOut?()
    }

    override func didMoveToWindow() {
        super.didMoveToWindow()
        if window != nil { setNeedsLayout() }
    }
}

func makeMobile() throws -> any Mobile { GoogleMobile() }

func makeLoaded() throws -> any Loaded {
    throw AsukaNativeError.unsupported("ads: a full-screen ad is loaded with interstitial(), rewarded()...")
}

@MainActor func makeBanner(_ events: BannerEvents) -> any Banner { GoogleBanner(events) }
