// swift-tools-version:5.9
// The platform/purchase package's Swift, for iOS and macOS: the Asuka CLI builds it
// with the apps that import the package (asuka native).
import PackageDescription

let package = Package(
    name: "PurchaseNative",
    platforms: [.iOS("16.0"), .tvOS("17.0"), .macOS("11.0")],
    products: [.library(name: "PurchaseNative", targets: ["PurchaseNative"])],
    dependencies: [
        .package(url: "https://github.com/egoist/asuka", from: "0.1.0"),
    ],
    targets: [
        .target(name: "PurchaseNative", dependencies: [.product(name: "Asuka", package: "asuka")], path: "swift"),
    ]
)
