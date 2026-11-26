// swift-tools-version:5.9
// The platform/ads package's Swift, for iOS (Google's Mobile Ads SDK has
// none for the Mac): the Asuka CLI builds it with the apps that import
// the package (asuka native).
import PackageDescription

let package = Package(
    name: "AdsNative",
    platforms: [.iOS("16.0")],
    products: [.library(name: "AdsNative", targets: ["AdsNative"])],
    dependencies: [
        .package(url: "https://github.com/egoist/asuka", from: "0.1.0"),
        // Google Mobile Ads, with its User Messaging Platform.
        .package(url: "https://github.com/googleads/swift-package-manager-google-mobile-ads", from: "13.0.0"),
    ],
    targets: [
        .target(name: "AdsNative", dependencies: [
            .product(name: "Asuka", package: "asuka"),
            .product(name: "GoogleMobileAds", package: "swift-package-manager-google-mobile-ads"),
        ], path: "swift"),
    ]
)
