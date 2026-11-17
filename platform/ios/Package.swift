// swift-tools-version:5.9
// Asuka's iOS host. The Asuka CLI compiles these sources straight into the
// app (with App/); this package is for editing them in Xcode, for the
// decoder's tests (`swift test`, on macOS) and for the host's (on the
// simulator: AsukaHostTests).
import PackageDescription

let package = Package(
    name: "AsukaHost",
    platforms: [.iOS(.v16), .macOS(.v13)],
    products: [.library(name: "AsukaHost", targets: ["AsukaHost"])],
    targets: [
        .target(name: "CAsuka"),
        .target(name: "AsukaOps"),
        .target(name: "AsukaHost", dependencies: ["AsukaOps", "CAsuka"]),
        .testTarget(name: "AsukaOpsTests", dependencies: ["AsukaOps"]),
        // The UIKit host's tests, on the iOS simulator (xcodebuild test
        // -scheme AsukaHost-Package -destination 'platform=iOS Simulator,...');
        // on macOS they build to nothing. The core is a stub that keeps the
        // events the host sends.
        .target(name: "CAsukaStub", path: "Tests/CAsukaStub"),
        .testTarget(name: "AsukaHostTests", dependencies: ["AsukaHost", "AsukaOps", "CAsukaStub"]),
    ]
)
