// swift-tools-version:5.9
// The platform/imageedit package's Swift, for iOS and macOS: the Asuka CLI builds it
// with the apps that import the package (asuka native).
import PackageDescription

let package = Package(
    name: "ImageeditNative",
    platforms: [.iOS("16.0"), .tvOS("17.0"), .macOS("13.0")],
    products: [.library(name: "ImageeditNative", targets: ["ImageeditNative"])],
    dependencies: [
        .package(url: "https://github.com/egoist/asuka", from: "0.1.0"),
    ],
    targets: [
        .target(name: "ImageeditNative", dependencies: [.product(name: "Asuka", package: "asuka")], path: "swift"),
    ]
)
