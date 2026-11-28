// The webrtc package's Kotlin for Android: a library module the Asuka
// CLI builds into the apps that import the package (asuka native). Its
// Kotlin is in the package's namespace; its dependencies go here.
plugins {
    id("com.android.library")
    id("org.jetbrains.kotlin.android")
}

android {
    namespace = "platform.webrtc"
}

dependencies {
    // Google's WebRTC (libwebrtc), built for Android by Stream: the
    // org.webrtc API, for Android 5 and later.
    implementation("io.getstream:stream-webrtc-android:1.3.10")
}
