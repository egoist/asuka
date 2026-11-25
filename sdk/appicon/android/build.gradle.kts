// The appicon package's Kotlin for Android: a library module the Asuka
// CLI builds into the apps that import the package (asuka native).
plugins {
    id("com.android.library")
    id("org.jetbrains.kotlin.android")
}

android {
    namespace = "platform.appicon"
}
