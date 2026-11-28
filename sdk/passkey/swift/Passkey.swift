// platform/passkey on iOS and the Mac: AuthenticationServices' passkeys
// (ASAuthorizationPlatformPublicKeyCredentialProvider: iCloud Keychain, or
// the password manager the user chose, and another device's by its QR
// code) and security keys, the system's sheet over the app's window. The
// relying party's domain must list the app in its
// apple-app-site-association's webcredentials, and the app the domain in
// its associated domains (asuka.toml's passkeys).
import AuthenticationServices
import Foundation
import Asuka

final class SystemPasskeys: Authenticator {
    func available() async throws -> Bool {
        if #available(iOS 16.0, macOS 13.0, *) { return true }
        return false
    }

    @MainActor func register(c: Creation) async throws -> Response {
        guard #available(iOS 16.0, macOS 13.0, *) else { throw PasskeyError.notAvailable }
        var requests: [ASAuthorizationRequest] = []
        if c.attachment != "cross-platform" {
            let p = ASAuthorizationPlatformPublicKeyCredentialProvider(relyingPartyIdentifier: c.rpId)
            let r = p.createCredentialRegistrationRequest(challenge: c.challenge, name: c.userName, userID: c.userId)
            r.displayName = c.userDisplayName
            r.userVerificationPreference = ASAuthorizationPublicKeyCredentialUserVerificationPreference(rawValue: c.userVerification)
            r.attestationPreference = ASAuthorizationPublicKeyCredentialAttestationKind(rawValue: c.attestation)
            if #available(iOS 17.4, macOS 14.4, *), !c.exclude.isEmpty {
                r.excludedCredentials = c.exclude.map { ASAuthorizationPlatformPublicKeyCredentialDescriptor(credentialID: $0) }
            }
            requests.append(r)
        }
        #if !os(tvOS)
        if c.attachment != "platform" {
            let p = ASAuthorizationSecurityKeyPublicKeyCredentialProvider(relyingPartyIdentifier: c.rpId)
            let r = p.createCredentialRegistrationRequest(challenge: c.challenge, displayName: c.userDisplayName, name: c.userName, userID: c.userId)
            r.credentialParameters = c.algorithms.map { ASAuthorizationPublicKeyCredentialParameters(algorithm: ASCOSEAlgorithmIdentifier(rawValue: $0)) }
            r.residentKeyPreference = ASAuthorizationPublicKeyCredentialResidentKeyPreference(rawValue: c.residentKey)
            r.userVerificationPreference = ASAuthorizationPublicKeyCredentialUserVerificationPreference(rawValue: c.userVerification)
            r.attestationPreference = ASAuthorizationPublicKeyCredentialAttestationKind(rawValue: c.attestation)
            r.excludedCredentials = c.exclude.map { ASAuthorizationSecurityKeyPublicKeyCredentialDescriptor(credentialID: $0, transports: ASAuthorizationSecurityKeyPublicKeyCredentialDescriptor.Transport.allSupported) }
            requests.append(r)
        }
        #endif
        let a = try await PasskeySheet.run(requests)
        switch a.credential {
        case let r as ASAuthorizationPlatformPublicKeyCredentialRegistration:
            var attachment = "platform"
            if #available(iOS 16.6, macOS 13.5, *), r.attachment == .crossPlatform { attachment = "cross-platform" }
            return Response(id: r.credentialID, clientData: r.rawClientDataJSON, attestationObject: r.rawAttestationObject ?? Data(),
                            transports: attachment == "platform" ? ["internal", "hybrid"] : ["hybrid"], attachment: attachment)
        #if !os(tvOS)
        case let r as ASAuthorizationSecurityKeyPublicKeyCredentialRegistration:
            var transports: [String] = []
            if #available(iOS 17.5, macOS 14.5, *) { transports = r.transports.map(\.rawValue) }
            return Response(id: r.credentialID, clientData: r.rawClientDataJSON, attestationObject: r.rawAttestationObject ?? Data(),
                            transports: transports, attachment: "cross-platform")
        #endif
        default:
            throw AsukaNativeError.unsupported("passkey: not a passkey's credential")
        }
    }

    @MainActor func signIn(r q: Request) async throws -> Response {
        guard #available(iOS 16.0, macOS 13.0, *) else { throw PasskeyError.notAvailable }
        let p = ASAuthorizationPlatformPublicKeyCredentialProvider(relyingPartyIdentifier: q.rpId)
        let r = p.createCredentialAssertionRequest(challenge: q.challenge)
        r.allowedCredentials = q.allow.map { ASAuthorizationPlatformPublicKeyCredentialDescriptor(credentialID: $0) }
        r.userVerificationPreference = ASAuthorizationPublicKeyCredentialUserVerificationPreference(rawValue: q.userVerification)
        var requests: [ASAuthorizationRequest] = [r]
        #if !os(tvOS)
        let k = ASAuthorizationSecurityKeyPublicKeyCredentialProvider(relyingPartyIdentifier: q.rpId).createCredentialAssertionRequest(challenge: q.challenge)
        k.allowedCredentials = q.allow.map { ASAuthorizationSecurityKeyPublicKeyCredentialDescriptor(credentialID: $0, transports: ASAuthorizationSecurityKeyPublicKeyCredentialDescriptor.Transport.allSupported) }
        k.userVerificationPreference = ASAuthorizationPublicKeyCredentialUserVerificationPreference(rawValue: q.userVerification)
        requests.append(k)
        #endif
        let a = try await PasskeySheet.run(requests)
        guard let c = a.credential as? ASAuthorizationPublicKeyCredentialAssertion else {
            throw AsukaNativeError.unsupported("passkey: not a passkey's assertion")
        }
        var attachment = "cross-platform"
        if let pc = c as? ASAuthorizationPlatformPublicKeyCredentialAssertion {
            attachment = "platform"
            if #available(iOS 16.6, macOS 13.5, *), pc.attachment == .crossPlatform { attachment = "cross-platform" }
        }
        return Response(id: c.credentialID, clientData: c.rawClientDataJSON, authenticatorData: c.rawAuthenticatorData ?? Data(),
                        signature: c.signature ?? Data(), userHandle: c.userID ?? Data(), attachment: attachment)
    }
}

/// One sheet: its controller, over the app's window, answered once;
/// cancelling the task closes it.
@MainActor final class PasskeySheet: NSObject, ASAuthorizationControllerDelegate, ASAuthorizationControllerPresentationContextProviding {
    private static var running = Set<PasskeySheet>()
    private var controller: ASAuthorizationController?
    private var done: CheckedContinuation<ASAuthorization, Error>?
    private var window: ASPresentationAnchor?

    static func run(_ requests: [ASAuthorizationRequest]) async throws -> ASAuthorization {
        let s = PasskeySheet()
        return try await withTaskCancellationHandler {
            try await withCheckedThrowingContinuation { c in s.start(requests, c) }
        } onCancel: {
            Task { @MainActor in s.cancel() }
        }
    }

    private func start(_ requests: [ASAuthorizationRequest], _ c: CheckedContinuation<ASAuthorization, Error>) {
        done = c
        guard let w = AsukaNative.window else {
            return finish(.failure(AsukaNativeError.unsupported("passkey: no window to show the sheet over")))
        }
        window = w
        Self.running.insert(self)
        let ctl = ASAuthorizationController(authorizationRequests: requests)
        ctl.delegate = self
        ctl.presentationContextProvider = self
        controller = ctl
        ctl.performRequests()
    }

    private func cancel() {
        if #available(iOS 16.0, macOS 13.0, *) { controller?.cancel() }
        finish(.failure(CancellationError()))
    }

    private func finish(_ r: Result<ASAuthorization, Error>) {
        guard let d = done else { return }
        done = nil
        controller = nil
        Self.running.remove(self)
        d.resume(with: r)
    }

    nonisolated func presentationAnchor(for controller: ASAuthorizationController) -> ASPresentationAnchor {
        MainActor.assumeIsolated { window ?? ASPresentationAnchor() }
    }

    nonisolated func authorizationController(controller: ASAuthorizationController, didCompleteWithAuthorization authorization: ASAuthorization) {
        MainActor.assumeIsolated { finish(.success(authorization)) }
    }

    nonisolated func authorizationController(controller: ASAuthorizationController, didCompleteWithError error: Error) {
        MainActor.assumeIsolated { finish(.failure(passkeyError(error))) }
    }
}

/// The platform's error as the package's, where it's one of them.
func passkeyError(_ error: Error) -> Error {
    let e = error as NSError
    guard e.domain == ASAuthorizationError.errorDomain else { return error }
    switch e.code {
    case ASAuthorizationError.canceled.rawValue: return PasskeyError.canceled
    case 1006: return PasskeyError.exists // matchedExcludedCredential (iOS 18, macOS 15)
    case ASAuthorizationError.failed.rawValue, ASAuthorizationError.unknown.rawValue:
        // "Application with identifier … is not associated with domain …",
        // "Unable to verify webcredentials association of … with domain
        // …": the domain's apple-app-site-association, or the entitlement.
        let text = "\(e.localizedDescription) \(e.userInfo)"
        if text.contains("not associated") || text.contains("webcredentials association") { return PasskeyError.notAssociated }
        return error
    default: return error
    }
}

func makeAuthenticator() throws -> any Authenticator { SystemPasskeys() }
