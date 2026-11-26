// The ads package's Kotlin for Android: a library module the Asuka
// CLI builds into the apps that import the package (asuka native). Its
// Kotlin is in the package's namespace; its dependencies go here.
plugins {
    id("com.android.library")
    id("org.jetbrains.kotlin.android")
}

android {
    namespace = "platform.ads"
}

dependencies {
    // Google Mobile Ads, which reads the app's ID (asuka.toml's
    // ads.android) from its manifest, and its User Messaging Platform.
    // 24.x: 25's Kotlin metadata (2.3) is newer than the apps' compiler reads.
    implementation("com.google.android.gms:play-services-ads:24.9.0")
    implementation("com.google.android.ump:user-messaging-platform:3.2.0")
}
