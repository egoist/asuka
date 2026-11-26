// The secure package's Kotlin for Android: a library module the Asuka
// CLI builds into the apps that import the package (asuka native). Its
// Kotlin is in the package's namespace; its dependencies go here:
// AndroidX's BiometricPrompt, which unlocks a protected secret's key.
plugins {
    id("com.android.library")
    id("org.jetbrains.kotlin.android")
}

android {
    namespace = "platform.secure"
}

dependencies {
    implementation("androidx.biometric:biometric:1.1.0")
}
