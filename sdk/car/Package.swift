// swift-tools-version:5.9
// The platform/car package's Swift, for iOS (CarPlay): the Asuka CLI builds it
// with the apps that import the package (asuka native).
import PackageDescription

let package = Package(
    name: "CarNative",
    platforms: [.iOS("16.0")],
    products: [.library(name: "CarNative", targets: ["CarNative"])],
    dependencies: [
        .package(url: "https://github.com/egoist/asuka", from: "0.1.0"),
    ],
    targets: [
        .target(name: "CarNative", dependencies: [.product(name: "Asuka", package: "asuka")], path: "swift",
                linkerSettings: [.linkedFramework("CarPlay"), .linkedFramework("MapKit")]),
    ]
)
