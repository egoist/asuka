// platform/splash on iOS and Apple TV: the host's copy of the launch screen over the app
// (AsukaSplash). The Mac shows no splash.
import Foundation
import Asuka

#if os(iOS) || os(tvOS)
@MainActor final class HostLaunch: Launch {
    func hold() { AsukaSplash.shared.hold() }
    func hide() { AsukaSplash.shared.release() }
}

@MainActor func makeLaunch() throws -> any Launch { HostLaunch() }
#else
@MainActor func makeLaunch() throws -> any Launch {
    throw AsukaNativeError.unsupported("splash: a Mac app shows no splash")
}
#endif
