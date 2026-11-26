// swift-tools-version:5.9
// The platform/analytics package's Swift, for iOS and macOS: the Asuka CLI
// builds it with the apps that import the package (asuka native), with
// Google Analytics for Firebase.
import PackageDescription

let package = Package(
    name: "AnalyticsNative",
    platforms: [.iOS("16.0"), .macOS("11.0")],
    products: [.library(name: "AnalyticsNative", targets: ["AnalyticsNative"])],
    dependencies: [
        .package(url: "https://github.com/egoist/asuka", from: "0.1.0"),
        .package(url: "https://github.com/firebase/firebase-ios-sdk", "13.2.0"..<"14.0.0"),
    ],
    targets: [
        .target(name: "AnalyticsNative", dependencies: [
            .product(name: "Asuka", package: "asuka"),
            .product(name: "FirebaseAnalytics", package: "firebase-ios-sdk"),
            .product(name: "FirebaseCore", package: "firebase-ios-sdk"),
        ], path: "swift"),
    ]
)
