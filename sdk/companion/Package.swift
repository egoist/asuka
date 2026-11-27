// swift-tools-version:5.9
// The platform/companion package's Swift, for iOS and watchOS: the Asuka CLI builds it
// with the apps that import the package (asuka native).
import PackageDescription

let package = Package(
    name: "CompanionNative",
    platforms: [.iOS("16.0"), .watchOS("10.0")],
    products: [.library(name: "CompanionNative", targets: ["CompanionNative"])],
    dependencies: [
        .package(url: "https://github.com/egoist/asuka", from: "0.1.0"),
    ],
    targets: [
        .target(name: "CompanionNative", dependencies: [.product(name: "Asuka", package: "asuka")], path: "swift"),
    ]
)
