// The health package's Kotlin for Android: a library module the Asuka
// CLI builds into the apps that import the package (asuka native). Its
// Kotlin is in the package's namespace; its dependencies go here.
plugins {
    id("com.android.library")
    id("org.jetbrains.kotlin.android")
}

android {
    namespace = "platform.health"
}

dependencies {
    // Health Connect's client (Android 8 and later at run time; the
    // package says notAvailable before).
    implementation("androidx.health.connect:connect-client:1.1.0")
}
