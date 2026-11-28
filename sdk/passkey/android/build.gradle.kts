// The passkey package's Kotlin for Android: a library module the Asuka CLI
// builds into the apps that import the package (asuka native), with
// AndroidX's Credential Manager (Google Play services' passkeys below
// Android 14). Each one's minSdk is 24 or lower.
plugins {
    id("com.android.library")
    id("org.jetbrains.kotlin.android")
}

android {
    namespace = "platform.passkey"
}

dependencies {
    implementation("androidx.credentials:credentials:1.6.0")
    implementation("androidx.credentials:credentials-play-services-auth:1.6.0")
}
