// swift-tools-version:5.9
// The platform/webrtc package's Swift, for iOS and macOS: the Asuka CLI builds it
// with the apps that import the package (asuka native). WebRTC is Google's,
// built as an XCFramework by stasel/WebRTC, pinned to one milestone's build
// (M157) and its checksum. It's a binary target of this package's, not
// stasel/WebRTC as a package: SwiftPM knows a package by its URL's or its
// directory's last name, and both would be "webrtc".
import PackageDescription

let package = Package(
    name: "WebrtcNative",
    platforms: [.iOS("16.0"), .macOS("13.0")],
    products: [.library(name: "WebrtcNative", targets: ["WebrtcNative"])],
    dependencies: [
        .package(url: "https://github.com/egoist/asuka", from: "0.1.0"),
    ],
    targets: [
        .binaryTarget(
            name: "WebRTC",
            url: "https://github.com/stasel/WebRTC/releases/download/157.0.0/WebRTC-M157.xcframework.zip",
            checksum: "ffb1221be2138ecc6bc92300ae4f0157e18694d652db9921e4251992ea91a328"
        ),
        .target(name: "WebrtcNative", dependencies: [
            .product(name: "Asuka", package: "asuka"),
            "WebRTC",
        ], path: "swift"),
    ]
)
