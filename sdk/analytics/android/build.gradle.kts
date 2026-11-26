// The analytics package's Kotlin for Android: a library module the Asuka
// CLI builds into the apps that import the package (asuka native), with
// Google Analytics for Firebase. The CLI writes the app's
// google-services.json (asuka.toml's android.googleServices) as the
// resources Firebase starts from.
plugins {
    id("com.android.library")
    id("org.jetbrains.kotlin.android")
}

android {
    namespace = "platform.analytics"
}

dependencies {
    implementation(platform("com.google.firebase:firebase-bom:34.19.0"))
    implementation("com.google.firebase:firebase-analytics")
}
