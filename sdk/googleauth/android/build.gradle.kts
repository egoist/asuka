// The googleauth package's Kotlin for Android: a library module the
// Asuka CLI builds into the apps that import the package (asuka native).
// Its Kotlin is in the package's namespace; its dependencies go here.
plugins {
    id("com.android.library")
    id("org.jetbrains.kotlin.android")
}

android {
    namespace = "platform.googleauth"
}

dependencies {
    // Credential Manager and Google's Sign in with Google for it (Google
    // Play services' below Android 14); Google Play services' Authorization
    // API for scopes, its Tasks awaited. Each one's minSdk is 24 or lower.
    // googleid 1.2.1 is built with Kotlin 2.4's metadata, which the host's
    // Kotlin (2.1) can't read: 1.2.0 until it moves on.
    implementation("androidx.credentials:credentials:1.6.0")
    implementation("androidx.credentials:credentials-play-services-auth:1.6.0")
    implementation("com.google.android.libraries.identity.googleid:googleid:1.2.0")
    implementation("com.google.android.gms:play-services-auth:22.0.0")
    implementation("org.jetbrains.kotlinx:kotlinx-coroutines-play-services:1.10.2")
}
