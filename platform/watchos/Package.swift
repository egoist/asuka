// swift-tools-version:5.9
// Asuka's watchOS host: SwiftUI (watchOS has no UIKit views), the C core
// linked in. The Asuka CLI compiles these sources straight into the app
// (with App/, its entry point) and links the core; this package is for
// editing them in Xcode and for the host's tests (`swift test`, on this
// Mac), which link CAsuka's stand-ins for the core.
import PackageDescription

let package = Package(
    name: "AsukaWatch",
    platforms: [.watchOS("10.0"), .macOS("14.0")],
    products: [.library(name: "AsukaHost", targets: ["AsukaHost"])],
    targets: [
        .target(name: "CAsuka"),
        // The decoder (AsukaOps, iOS's: go generate ./ops writes it here
        // too) and the host in one module, as the CLI builds them.
        .target(name: "AsukaHost", dependencies: ["CAsuka"], path: "Sources", exclude: ["CAsuka"], sources: ["AsukaHost", "AsukaOps"]),
        .testTarget(name: "AsukaWatchTests", dependencies: ["AsukaHost"]),
    ]
)
