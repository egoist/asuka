// The maps package's Kotlin for Android: a library module the Asuka
// CLI builds into the apps that import the package (asuka native). Its
// Kotlin is in the package's namespace; its dependencies go here.
plugins {
    id("com.android.library")
    id("org.jetbrains.kotlin.android")
}

android {
    namespace = "platform.maps"
}

dependencies {
    // Google's Maps SDK, which reads the app's key (asuka.toml's
    // android.mapsKey) from its manifest.
    implementation("com.google.android.gms:play-services-maps:20.1.0")
    // Its clustering (ClusterManager) and MarkerManager: 3.14, built
    // with the Kotlin, coroutines and AndroidX the host has.
    implementation("com.google.maps.android:android-maps-utils:3.14.0")
}
