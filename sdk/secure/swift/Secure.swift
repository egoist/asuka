// platform/secure on iOS and the Mac: the Keychain, a generic password per
// key under the app's ID (its share extension's too: AsukaAppID), kept
// on this device only, readable once it's been unlocked after a restart.
// A protected one carries an access control (the user's presence: Face ID
// or Touch ID, else the passcode), so the keychain itself won't give it
// up until the user proves it's them; the device must have a passcode.
import Foundation
import LocalAuthentication
import Security

final class SecKeychain: Keychain {
    let service = Bundle.main.object(forInfoDictionaryKey: "AsukaAppID") as? String
        ?? Bundle.main.bundleIdentifier ?? ProcessInfo.processInfo.processName

    /// A key's item. On the Mac, an unprotected one is in the login
    /// keychain, which an app signed ad hoc can use too (asuka dev's).
    func query(_ key: String) -> [String: Any] {
        [kSecClass as String: kSecClassGenericPassword, kSecAttrService as String: service, kSecAttrAccount as String: key]
    }

    /// A key's protected item. Access control needs the data protection
    /// keychain, which on the Mac is the one iOS has, and wants the app's
    /// identifier (a provisioning profile's entitlement); on iOS it's the
    /// only keychain, the same as query's.
    func protectedQuery(_ key: String) -> [String: Any] {
        var q = query(key)
        #if os(macOS)
        q[kSecUseDataProtectionKeychain as String] = true
        #endif
        return q
    }

    /// The queries that find key's item, each with whether what it finds
    /// may be protected: on the Mac the data protection keychain's, then
    /// the login keychain's.
    func queries(_ key: String) -> [(query: [String: Any], protectable: Bool)] {
        #if os(macOS)
        return [(protectedQuery(key), true), (query(key), false)]
        #else
        return [(query(key), true)]
        #endif
    }

    func get(key: String, prompt: String) async throws -> String? {
        for (base, protectable) in queries(key) {
            var q = base
            if protectable {
                // Whether it's protected: its attributes, which ask nothing.
                var a = base
                a[kSecReturnAttributes as String] = true
                a[kSecMatchLimit as String] = kSecMatchLimitOne
                let (st, attrs) = try await Self.offMain { () -> (OSStatus, [String: Any]?) in
                    var out: CFTypeRef?
                    let st = SecItemCopyMatching(a as CFDictionary, &out)
                    return (st, out as? [String: Any])
                }
                switch st {
                case errSecSuccess:
                    break
                case errSecItemNotFound:
                    continue // on the Mac, maybe in the login keychain
                case errSecMissingEntitlement where Self.dataProtection(a):
                    continue // the Mac's, for an app without a profile: nothing protected there
                default:
                    throw Self.error(st)
                }
                // A protected one: the user proves it's them, asked in the
                // app's words, and the keychain takes that proof (asking
                // nothing more). No words: the keychain asks in its own.
                if attrs?[kSecAttrAccessControl as String] != nil, !prompt.isEmpty {
                    let ctx = LAContext()
                    do {
                        _ = try await ctx.evaluatePolicy(.deviceOwnerAuthentication, localizedReason: prompt)
                    } catch let e as LAError {
                        switch e.code {
                        case .passcodeNotSet, .biometryNotAvailable, .biometryNotEnrolled:
                            throw SecureError.notAvailable
                        case .userCancel, .appCancel, .systemCancel, .authenticationFailed, .userFallback, .biometryLockout:
                            throw SecureError.cancelled
                        default:
                            throw e
                        }
                    }
                    q[kSecUseAuthenticationContext as String] = ctx
                }
            }
            q[kSecReturnData as String] = true
            q[kSecMatchLimit as String] = kSecMatchLimitOne
            // SecItemCopyMatching waits while the system asks the user:
            // never on the main thread, nor on one of Swift's.
            let (st, out) = try await Self.offMain { () -> (OSStatus, Data?) in
                var out: CFTypeRef?
                let st = SecItemCopyMatching(q as CFDictionary, &out)
                return (st, out as? Data)
            }
            switch st {
            case errSecSuccess:
                guard let d = out else { throw Self.error(st) }
                return String(decoding: d, as: UTF8.self)
            case errSecItemNotFound:
                continue
            case errSecMissingEntitlement where Self.dataProtection(q):
                continue
            case errSecUserCanceled, errSecAuthFailed:
                throw SecureError.cancelled
            default:
                throw Self.error(st)
            }
        }
        return nil
    }

    func set(key: String, value: String, protected: Bool) async throws {
        try await Self.offMain { [self] in
            var add: [String: Any]
            if protected {
                // Without a passcode (or with nothing to ask with), the
                // keychain can't keep it protected: say so before the
                // old value's gone.
                var why: NSError?
                guard LAContext().canEvaluatePolicy(.deviceOwnerAuthentication, error: &why) else {
                    throw SecureError.notAvailable
                }
                var cfErr: Unmanaged<CFError>?
                guard let access = SecAccessControlCreateWithFlags(nil, kSecAttrAccessibleWhenPasscodeSetThisDeviceOnly, .userPresence, &cfErr) else {
                    throw cfErr?.takeRetainedValue() ?? Self.error(errSecParam)
                }
                add = protectedQuery(key)
                add[kSecAttrAccessControl as String] = access
                #if os(macOS)
                // The data protection keychain wants the app's identifier (a
                // provisioning profile's entitlement): an app signed without
                // one (ad hoc, asuka dev's player) can't keep a protected
                // secret. Asked before the old value's deleted, so it stays.
                var probe = protectedQuery(key)
                probe[kSecReturnAttributes as String] = true
                probe[kSecUseAuthenticationUI as String] = kSecUseAuthenticationUIFail
                if SecItemCopyMatching(probe as CFDictionary, nil) == errSecMissingEntitlement {
                    throw SecureError.notAvailable
                }
                #endif
            } else {
                add = query(key)
                add[kSecAttrAccessible as String] = kSecAttrAccessibleAfterFirstUnlockThisDeviceOnly
            }
            add[kSecValueData as String] = Data(value.utf8)
            // A key has one item, protected or not: the old one goes
            // (deleting a protected item asks nothing), then the new one.
            try removeAll(key)
            let st = SecItemAdd(add as CFDictionary, nil)
            switch st {
            case errSecSuccess:
                return
            case errSecNotAvailable where protected, errSecAuthFailed where protected, errSecMissingEntitlement where protected:
                throw SecureError.notAvailable
            default:
                throw Self.error(st)
            }
        }
    }

    func remove(key: String) async throws {
        try await Self.offMain { [self] in try removeAll(key) }
    }

    /// Deletes key's items wherever they are.
    func removeAll(_ key: String) throws {
        for (q, _) in queries(key) {
            let st = SecItemDelete(q as CFDictionary)
            guard st == errSecSuccess || st == errSecItemNotFound || (st == errSecMissingEntitlement && Self.dataProtection(q)) else { throw Self.error(st) }
        }
    }

    /// Whether q is the Mac's data protection keychain's.
    static func dataProtection(_ q: [String: Any]) -> Bool {
        q[kSecUseDataProtectionKeychain as String] != nil
    }

    /// Runs work on a thread of its own (the keychain's calls block), and
    /// gives back what it gave.
    static func offMain<T>(_ work: @escaping () throws -> T) async throws -> T {
        try await withCheckedThrowingContinuation { cont in
            DispatchQueue.global(qos: .userInitiated).async {
                cont.resume(with: Result { try work() })
            }
        }
    }

    static func error(_ st: OSStatus, _ why: String? = nil) -> Error {
        let msg = why ?? SecCopyErrorMessageString(st, nil) as String? ?? "status \(st)"
        return NSError(domain: NSOSStatusErrorDomain, code: Int(st), userInfo: [NSLocalizedDescriptionKey: "the keychain: \(msg)"])
    }
}

func makeKeychain() throws -> any Keychain { SecKeychain() }
