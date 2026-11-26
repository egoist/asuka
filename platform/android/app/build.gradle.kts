// The shell app around a Asuka app. The Asuka CLI passes the app's ID, name,
// version, signing key and (for release builds) the Go library for each
// ABI, the assets and the icons; in development the Go library is pushed
// to the app's files directory instead of being packaged (see
// AsukaNative.load).
plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
}

fun prop(name: String) = providers.gradleProperty(name).orNull

android {
    namespace = "dev.asuka.app"
    compileSdk = 36
    // The NDK the CLI built the Go libraries with (the SDK's ndk/<version>):
    // Gradle strips them with it, and keeps their symbols
    // (native-debug-symbols.zip).
    prop("asukaNdkVersion")?.let { ndkVersion = it }
    defaultConfig {
        applicationId = prop("asukaApplicationId") ?: "dev.asuka.app"
        minSdk = 24
        targetSdk = 36
        versionCode = (prop("asukaVersionCode") ?: "1").toInt()
        versionName = prop("asukaVersionName") ?: "1.0"
        manifestPlaceholders["asukaAppName"] = prop("asukaAppName") ?: "asuka"
        manifestPlaceholders["asukaIcon"] = prop("asukaIcon") ?: "@android:drawable/sym_def_app_icon"
    }
    signingConfigs {
        create("release") {
            prop("asukaKeystore")?.let {
                storeFile = file(it)
                storePassword = prop("asukaKeystorePassword")
                keyAlias = prop("asukaKeyAlias")
                keyPassword = prop("asukaKeyPassword") ?: prop("asukaKeystorePassword")
            }
        }
    }
    buildTypes {
        release {
            isMinifyEnabled = true
            isShrinkResources = true
            proguardFiles(getDefaultProguardFile("proguard-android-optimize.txt"))
            // Go's stack traces don't need the libraries' symbols (they're
            // in its own tables), a native crash's frames do: Gradle strips
            // the libraries it packages and keeps their symbol tables, in
            // the .aab for Google Play (and native-debug-symbols.zip).
            ndk { debugSymbolLevel = "SYMBOL_TABLE" }
            if (prop("asukaKeystore") != null) signingConfig = signingConfigs.getByName("release")
        }
    }
    // Language splits would leave out the Material strings of a language the
    // app chooses in-app (ui.SetLocale) rather than the device's.
    bundle { language { enableSplit = false } }
    sourceSets {
        // The app's manifest (asuka.json's permissions, scheme and links),
        // over the debug one in development.
        getByName("debug") {
            // The core: a player's (asuka dev sends it the app's bytecode),
            // or one with the app compiled in.
            prop("asukaJniLibs")?.let { jniLibs.srcDir(it) }
            prop("asukaManifest")?.let { manifest.srcFile(it) }
            prop("asukaRes")?.let { res.srcDir(it) } // the splash, Firebase's settings
        }
        getByName("release") {
            prop("asukaJniLibs")?.let { jniLibs.srcDir(it) }
            prop("asukaAssets")?.let { assets.srcDir(it) }
            prop("asukaRes")?.let { res.srcDir(it) }
            prop("asukaManifest")?.let { manifest.srcFile(it) }
        }
    }
    compileOptions {
        // java.time and the rest of Java 8's library on Android 7 (API 24,
        // 25), which hasn't it: desugared into the app.
        isCoreLibraryDesugaringEnabled = true
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
}

kotlin { jvmToolchain(17) }

dependencies {
    coreLibraryDesugaring("com.android.tools:desugar_jdk_libs:2.1.5")
    val prebuilt = prop("asukaPrebuiltDeps")
    if (prebuilt != null) {
        // The host's, its optional modules' and the std and sdk packages'
        // AARs (the CLI's asukaPrebuiltDeps: coordinates).
        prebuilt.split(',').filter { it.isNotEmpty() }.forEach { implementation(it) }
    } else {
        implementation(project(":asuka"))
        // The optional modules the app's packages need (the CLI's asukaModules).
        prop("asukaModules")?.split(',')?.filter { it.isNotEmpty() }?.forEach { implementation(project(":asuka-$it")) }
    }
    // The app's native packages' modules (the CLI's asukaNativeModules).
    prop("asukaNativeModules")?.split(';')?.filter { it.isNotEmpty() }?.forEach { implementation(project(":" + it.substringBefore('='))) }
}
