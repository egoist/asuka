// The pdf package's Kotlin for Android: a library module the Asuka CLI
// builds into the apps that import the package (asuka native). Its
// Kotlin is in the package's namespace; its dependencies go here.
plugins {
    id("com.android.library")
    id("org.jetbrains.kotlin.android")
}

android {
    namespace = "platform.pdf"
}

dependencies {
    implementation("androidx.recyclerview:recyclerview:1.4.0")
}
