// swift-tools-version:5.9
// The std/http package's Swift, for iOS and macOS: the Asuka CLI builds it
// with the apps that import the package (asuka native).
import PackageDescription

let package = Package(
    name: "HttpNative",
    platforms: [.iOS("16.0"), .tvOS("17.0"), .watchOS("10.0"), .macOS("11.0")],
    products: [.library(name: "HttpNative", targets: ["HttpNative"])],
    dependencies: [
        .package(url: "https://github.com/egoist/asuka", from: "0.1.0"),
    ],
    targets: [
        .target(name: "HttpNative", dependencies: [.product(name: "Asuka", package: "asuka")], path: "swift"),
    ]
)
