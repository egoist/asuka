// The Video element's player: Media3's ExoPlayer (HLS streams too) in its
// PlayerView. Only the apps that show a Video are built with it (the
// Asuka CLI's asukaModules: the app's sources name Video; asuka dev's
// player always), so the others carry neither Media3 nor the permissions
// its manifest declares.
plugins {
    id("com.android.library")
    id("org.jetbrains.kotlin.android")
}

android {
    namespace = "dev.asuka.video"
    compileSdk = 36
    defaultConfig {
        minSdk = 24
        consumerProguardFiles("consumer-rules.pro")
    }
    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
}

kotlin { jvmToolchain(17) }

dependencies {
    implementation(project(":asuka"))
    implementation("androidx.media3:media3-exoplayer:1.11.1")
    implementation("androidx.media3:media3-exoplayer-hls:1.11.1")
    implementation("androidx.media3:media3-ui:1.11.1")
}
