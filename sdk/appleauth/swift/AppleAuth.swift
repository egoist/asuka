// platform/appleauth on iOS and macOS: AuthenticationServices' Apple ID
// provider and its controller (the system's sheet), and
// ASAuthorizationAppleIDButton, made again when its type or variant
// changes (they're fixed at init).
import Foundation
import AuthenticationServices
import Asuka
#if canImport(UIKit)
import UIKit
#else
import AppKit
#endif

final class AppleIDProvider: Provider {
    func available() throws -> Bool { true }

    func signIn(scopes: [Scope], nonce: String, state: String) async throws -> Credential {
        try await withCheckedThrowingContinuation { c in
            Task { @MainActor in
                Sheets.shared.show(scopes: scopes, nonce: nonce, state: state) { c.resume(with: $0) }
            }
        }
    }

    func credentialState(user: String) async throws -> CredentialState {
        try await withCheckedThrowingContinuation { c in
            ASAuthorizationAppleIDProvider().getCredentialState(forUserID: user) { s, error in
                if let error, s != .notFound { c.resume(throwing: error); return }
                switch s {
                case .revoked: c.resume(returning: .revoked)
                case .authorized: c.resume(returning: .authorized)
                case .transferred: c.resume(returning: .transferred)
                default: c.resume(returning: .notFound)
                }
            }
        }
    }
}

/// The sheets shown, each its controller's.
@MainActor final class Sheets: NSObject, ASAuthorizationControllerDelegate, ASAuthorizationControllerPresentationContextProviding {
    static let shared = Sheets()
    private var pending: [ObjectIdentifier: (ASAuthorizationController, (Result<Credential, Error>) -> Void)] = [:]

    func show(scopes: [Scope], nonce: String, state: String, _ done: @escaping (Result<Credential, Error>) -> Void) {
        let req = ASAuthorizationAppleIDProvider().createRequest()
        var s: [ASAuthorization.Scope] = []
        if scopes.contains(.fullName) { s.append(.fullName) }
        if scopes.contains(.email) { s.append(.email) }
        req.requestedScopes = s
        if !nonce.isEmpty { req.nonce = nonce }
        if !state.isEmpty { req.state = state }
        let c = ASAuthorizationController(authorizationRequests: [req])
        c.delegate = self
        c.presentationContextProvider = self
        pending[ObjectIdentifier(c)] = (c, done)
        c.performRequests()
    }

    nonisolated func presentationAnchor(for controller: ASAuthorizationController) -> ASPresentationAnchor {
        MainActor.assumeIsolated {
            #if canImport(UIKit)
            let scenes = UIApplication.shared.connectedScenes.compactMap { $0 as? UIWindowScene }
            return scenes.flatMap(\.windows).first(where: \.isKeyWindow) ?? scenes.first?.windows.first ?? ASPresentationAnchor()
            #else
            return NSApp.keyWindow ?? NSApp.windows.first ?? ASPresentationAnchor()
            #endif
        }
    }

    nonisolated func authorizationController(controller: ASAuthorizationController, didCompleteWithAuthorization authorization: ASAuthorization) {
        MainActor.assumeIsolated {
            guard let (_, done) = pending.removeValue(forKey: ObjectIdentifier(controller)) else { return }
            guard let c = authorization.credential as? ASAuthorizationAppleIDCredential else {
                done(.failure(AsukaNativeError.unsupported("appleauth: not an Apple ID credential")))
                return
            }
            done(.success(Credential(
                user: c.user,
                identityToken: c.identityToken.map { String(decoding: $0, as: UTF8.self) } ?? "",
                authorizationCode: c.authorizationCode.map { String(decoding: $0, as: UTF8.self) } ?? "",
                state: c.state ?? "",
                email: c.email ?? "",
                givenName: c.fullName?.givenName ?? "",
                familyName: c.fullName?.familyName ?? "",
                realUser: c.realUserStatus == .likelyReal)))
        }
    }

    nonisolated func authorizationController(controller: ASAuthorizationController, didCompleteWithError error: Error) {
        MainActor.assumeIsolated {
            guard let (_, done) = pending.removeValue(forKey: ObjectIdentifier(controller)) else { return }
            let code = (error as? ASAuthorizationError)?.code
            done(.failure(code == .canceled ? AppleAuthError.canceled : error))
        }
    }
}

/// The button: Apple's, in a box that holds it (it's made again as its
/// type, its variant or, for automatic, the appearance changes).
@MainActor final class AppleSignInButton: SignInButton {
    let events: SignInButtonEvents
    let box = Box()
    var view: AsukaPlatformView { box }
    private var type = ButtonType.signIn, variant = ButtonVariant.automatic
    private var radius: Double = -1

    init(_ events: SignInButtonEvents) {
        self.events = events
        box.appearanceChanged = { [weak self] in if self?.variant == .automatic { self?.make() } }
        make()
    }

    func setType(_ value: ButtonType) { type = value; make() }
    func setVariant(_ value: ButtonVariant) { variant = value; make() }
    func setCornerRadius(_ value: Double) {
        radius = value
        #if !os(tvOS) // tvOS's button keeps its own shape
        if radius >= 0 { box.button?.cornerRadius = radius }
        #endif
    }

    private func make() {
        let t: ASAuthorizationAppleIDButton.ButtonType = type == .continueWithApple ? .continue : type == .signUp ? .signUp : .signIn
        // Automatic: white in dark mode, black in light.
        let s: ASAuthorizationAppleIDButton.Style
        switch variant {
        case .white: s = .white
        case .whiteOutline: s = .whiteOutline
        case .black: s = .black
        default: s = box.dark ? .white : .black
        }
        let b = ASAuthorizationAppleIDButton(authorizationButtonType: t, authorizationButtonStyle: s)
        #if os(tvOS)
        // The remote's Select, on the focused button.
        b.addTarget(self, action: #selector(pressed), for: .primaryActionTriggered)
        #elseif canImport(UIKit)
        if radius >= 0 { b.cornerRadius = radius }
        b.addTarget(self, action: #selector(pressed), for: .touchUpInside)
        #else
        if radius >= 0 { b.cornerRadius = radius }
        b.target = self
        b.action = #selector(pressed)
        #endif
        box.button = b
    }

    @objc private func pressed() { events.press() }
}

/// Holds the button, which fills it; as wide as it's given (at least
/// Apple's 140), 44 points tall (32 on the Mac).
final class Box: AsukaPlatformView {
    var appearanceChanged: (() -> Void)?
    var button: ASAuthorizationAppleIDButton? {
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
    var dark: Bool { traitCollection.userInterfaceStyle == .dark }
    override func traitCollectionDidChange(_ previous: UITraitCollection?) {
        super.traitCollectionDidChange(previous)
        if previous?.userInterfaceStyle != traitCollection.userInterfaceStyle { appearanceChanged?() }
    }
    override func sizeThatFits(_ size: CGSize) -> CGSize { CGSize(width: max(min(size.width, 375), 140), height: 44) }
    #else
    var dark: Bool { effectiveAppearance.bestMatch(from: [.darkAqua, .aqua]) == .darkAqua }
    override func viewDidChangeEffectiveAppearance() {
        super.viewDidChangeEffectiveAppearance()
        appearanceChanged?()
    }
    override var fittingSize: NSSize { NSSize(width: 200, height: 32) }
    #endif
}

func makeProvider() throws -> any Provider { AppleIDProvider() }

@MainActor func makeSignInButton(_ events: SignInButtonEvents) -> any SignInButton { AppleSignInButton(events) }
