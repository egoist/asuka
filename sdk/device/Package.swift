// swift-tools-version:5.9
// The platform/device package's Swift, for iOS and macOS: the Asuka CLI builds it
// with the apps that import the package (asuka native).
import PackageDescription

let package = Package(
    name: "DeviceNative",
    platforms: [.iOS("16.0"), .tvOS("17.0"), .macOS("11.0")],
    products: [.library(name: "DeviceNative", targets: ["DeviceNative"])],
    dependencies: [
        .package(url: "https://github.com/egoist/asuka", from: "0.1.0"),
    ],
    targets: [
        .target(name: "DeviceNative", dependencies: [.product(name: "Asuka", package: "asuka")], path: "swift"),
    ]
)
