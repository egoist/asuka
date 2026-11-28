// The camera package's Kotlin for Android: a library module the Asuka
// CLI builds into the apps that import the package (asuka native). Its
// Kotlin is in the package's namespace; its dependencies go here.
plugins {
    id("com.android.library")
    id("org.jetbrains.kotlin.android")
}

android {
    namespace = "platform.camera"
}

dependencies {
    implementation("androidx.camera:camera-camera2:1.6.2")
    implementation("androidx.camera:camera-lifecycle:1.6.2")
    implementation("androidx.camera:camera-view:1.6.2")
    implementation("androidx.camera:camera-video:1.6.2")
    implementation("androidx.exifinterface:exifinterface:1.4.1") // a photo's orientation, written again (capture's maxSize)
    // Reading codes in what it sees: ML Kit, with its model, so it works
    // without Google Play services.
    implementation("com.google.mlkit:barcode-scanning:17.3.0")
}
