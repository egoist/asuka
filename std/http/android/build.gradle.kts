// The http package's Kotlin for Android: a library module the Asuka
// CLI builds into the apps that import the package (asuka native). Its
// Kotlin is in the package's namespace; its dependencies go here.
plugins {
    id("com.android.library")
    id("org.jetbrains.kotlin.android")
}

android {
    namespace = "std.http"
}

dependencies {
    // Background uploads (Background.kt): WorkManager, as platform/periodic's.
    implementation("androidx.work:work-runtime:2.12.0")
    testImplementation("org.json:json:20240303") // the real org.json: android.jar's is a stub
}

// The unit tests' JVM holds far less than the uploads they send (a
// 256 MiB file): one whose body were held whole would run out of memory.
tasks.withType<Test>().configureEach {
    maxHeapSize = "160m"
}
