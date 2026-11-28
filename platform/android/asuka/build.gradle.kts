plugins {
    id("com.android.library")
    id("org.jetbrains.kotlin.android")
    `maven-publish`
}

fun prop(name: String) = providers.gradleProperty(name).orNull

// asuka build --platform android --aar: an app to embed in another, this library with
// the app's Go code (asukaJniLibs) and assets, and AsukaActivity declared,
// published to a Maven repository (asukaMavenRepo) so its dependencies come
// with it.
val embed = prop("asukaMavenRepo") != null

android {
    namespace = "dev.asuka"
    compileSdk = 36
    defaultConfig {
        minSdk = 24
        consumerProguardFiles("consumer-rules.pro")
    }
    compileOptions {
        // java.time and the rest of Java 8's library on Android 7 (API 24,
        // 25), which hasn't it: desugared into the app.
        isCoreLibraryDesugaringEnabled = true
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
    testOptions.unitTests.isReturnDefaultValues = true
    testOptions.unitTests.isIncludeAndroidResources = true
    if (embed) {
        sourceSets.getByName("main") {
            prop("asukaJniLibs")?.let { jniLibs.srcDir(it) }
            prop("asukaAssets")?.let { assets.srcDir(it) }
            prop("asukaManifest")?.let { manifest.srcFile(it) }
        }
        publishing { singleVariant("release") }
    }
}

if (embed) afterEvaluate {
    publishing {
        publications {
            create<MavenPublication>("release") {
                from(components["release"])
                groupId = prop("asukaGroup") ?: "dev.asuka"
                artifactId = prop("asukaArtifact") ?: "app"
                version = prop("asukaVersion") ?: "1.0"
            }
        }
        repositories { maven { name = "embed"; url = uri(prop("asukaMavenRepo")!!) } }
    }
}

kotlin { jvmToolchain(17) }

dependencies {
    coreLibraryDesugaring("com.android.tools:desugar_jdk_libs:2.1.5")
    api("com.google.android.material:material:1.14.0")
    api("androidx.appcompat:appcompat:1.7.1")
    implementation("androidx.core:core-ktx:1.16.0")
    // Typed native modules' methods that take a context are suspend funs.
    api("org.jetbrains.kotlinx:kotlinx-coroutines-android:1.10.2")
    implementation("androidx.recyclerview:recyclerview:1.4.0")
    implementation("androidx.activity:activity:1.10.1") // predictive back, edge to edge, the result launchers
    implementation("androidx.swiperefreshlayout:swiperefreshlayout:1.1.0")
    implementation("androidx.exifinterface:exifinterface:1.4.1") // an image's orientation
    implementation("androidx.webkit:webkit:1.14.0") // WebView's document-start scripts
    // What a std package does (the camera, biometrics, the in-app browser,
    // Play's libraries...) is its own Gradle module's
    // (std/<name>/android), with its libraries: only in the apps that
    // import it. Video's player (Media3) is asuka-video's: only in the
    // apps that show one.
    testImplementation("junit:junit:4.13.2")
    testImplementation("org.json:json:20240303") // the real org.json: android.jar's is a stub
    // Android's own classes, for the tests that need them (the scheduled
    // notifications' alarms and their receiver after a reboot).
    testImplementation("androidx.test:core:1.6.1")
    testImplementation("org.robolectric:robolectric:4.14")
}
