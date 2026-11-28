// The device package's Kotlin for Android: a library module the Asuka
// CLI builds into the apps that import the package (asuka native), with
// AndroidX's activity results (the pickers) and Custom Tabs.
plugins {
    id("com.android.library")
    id("org.jetbrains.kotlin.android")
}

android {
    namespace = "platform.device"
}

dependencies {
    implementation("androidx.activity:activity:1.10.1")
    implementation("androidx.browser:browser:1.9.0")
    implementation("androidx.core:core-ktx:1.16.0")
    implementation("androidx.exifinterface:exifinterface:1.4.1") // a picked photo's orientation
}
