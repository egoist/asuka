// The car package's Kotlin for Android: a library module the Asuka CLI
// builds into the apps that import the package (asuka native). Android
// Auto's templates are the Car App Library's (androidx.car.app 1.7, built
// with Kotlin 1.8, which the host's Kotlin reads).
plugins {
    id("com.android.library")
    id("org.jetbrains.kotlin.android")
}

android {
    namespace = "platform.car"
    testOptions { unitTests.isIncludeAndroidResources = true }
}

dependencies {
    implementation("androidx.car.app:app:1.7.0")
    testImplementation("androidx.car.app:app-testing:1.7.0")
    testImplementation("junit:junit:4.13.2")
    testImplementation("androidx.test:core:1.6.1")
    testImplementation("org.robolectric:robolectric:4.14")
}
