plugins {
    id("com.android.application") version "8.12.0" apply false
    id("com.android.library") version "8.12.0" apply false
    id("org.jetbrains.kotlin.android") version "2.1.20" apply false
}

// asuka build --platform android --aar: the optional modules (asuka-*) an embedded app
// uses are published beside it, their dependency on :asuka as the app's
// coordinates.
providers.gradleProperty("asukaMavenRepo").orNull?.let { repo ->
    subprojects {
        if (!name.startsWith("asuka-")) return@subprojects
        apply(plugin = "maven-publish")
        plugins.withId("com.android.library") {
            extensions.configure<com.android.build.gradle.LibraryExtension> { publishing { singleVariant("release") } }
            // After the Android plugin's own: its components exist by then.
            afterEvaluate {
                extensions.configure<PublishingExtension> {
                    publications {
                        create<MavenPublication>("release") {
                            from(components["release"])
                            groupId = providers.gradleProperty("asukaGroup").orNull ?: "dev.asuka"
                            artifactId = project.name
                            version = providers.gradleProperty("asukaVersion").orNull ?: "1.0"
                        }
                    }
                    repositories { maven { name = "embed"; url = uri(repo) } }
                }
            }
        }
    }
}

// The CLI's AARs of the host's modules and the std and sdk packages'
// (asukaPublishRepo: a local Maven repository): each module named in
// asukaPublishVersions (module=version) is published as dev.asuka:<its
// name, or asukaPublishArtifacts' (module=artifact)>:<version>, a
// package's depending on the host's at its version.
providers.gradleProperty("asukaPublishRepo").orNull?.let { repo ->
    fun map(p: String) = (providers.gradleProperty(p).orNull ?: "").split(';').filter { it.contains('=') }
        .associate { it.substringBefore('=') to it.substringAfter('=') }
    val versions = map("asukaPublishVersions")
    val artifacts = map("asukaPublishArtifacts")
    subprojects {
        val v = versions[name] ?: return@subprojects
        group = "dev.asuka"
        version = v
        apply(plugin = "maven-publish")
        plugins.withId("com.android.library") {
            extensions.configure<com.android.build.gradle.LibraryExtension> { publishing { singleVariant("release") } }
            afterEvaluate {
                extensions.configure<PublishingExtension> {
                    publications {
                        create<MavenPublication>("prebuilt") {
                            from(components["release"])
                            groupId = "dev.asuka"
                            artifactId = artifacts[project.name] ?: project.name
                            version = v
                        }
                    }
                    repositories { maven { name = "prebuilt"; url = uri(repo) } }
                }
            }
        }
    }
}

// The app's native packages' modules (settings.gradle.kts): built where
// the host is (not in the package, which may be read-only), with the
// host's SDK levels and Java, and its Kotlin to call (dev.asuka): a
// package's build.gradle.kts says only its namespace and its own
// dependencies. Each package's directory builds in a directory of its own:
// the host's build is shared by every checkout of a package (another
// clone's, a worktree's std/), and Kotlin's incremental caches keep its
// sources' paths, so another's would compile against what they recorded.
subprojects {
    if (!name.startsWith("native-")) return@subprojects
    val source = java.security.MessageDigest.getInstance("SHA-256").digest(projectDir.absolutePath.toByteArray())
        .take(4).joinToString("") { "%02x".format(it) }
    layout.buildDirectory.set(rootProject.layout.buildDirectory.dir("native/$name-$source"))
    plugins.withId("com.android.library") {
        dependencies.add("coreLibraryDesugaring", "com.android.tools:desugar_jdk_libs:2.1.5")
        extensions.configure<com.android.build.gradle.LibraryExtension> {
            compileSdk = 36
            defaultConfig {
                minSdk = 24
                testInstrumentationRunner = "androidx.test.runner.AndroidJUnitRunner"
            }
            compileOptions {
                isCoreLibraryDesugaringEnabled = true
                sourceCompatibility = JavaVersion.VERSION_17
                targetCompatibility = JavaVersion.VERSION_17
            }
            // Its instrumented tests (src/androidTest, run on a device by
            // connectedDebugAndroidTest) read its package's tests/ as assets.
            sourceSets.getByName("androidTest").assets.srcDir(projectDir.resolve("../tests"))
            // Its unit tests (src/test, on this computer's JVM by
            // testDebugUnitTest): Android's classes stubs that answer
            // defaults, the test server's URLs and its test authority's
            // files (asukaTestServer, asukaTestServerTLS, asukaTestPKI,
            // when the CLI's tests started it) system properties.
            testOptions.unitTests.isReturnDefaultValues = true
        }
        dependencies {
            // The host's AAR when the app's build depends on it.
            val host = providers.gradleProperty("asukaPrebuiltHost").orNull
            add("implementation", if (host != null) host else project(":asuka"))
            add("androidTestImplementation", "androidx.test:runner:1.6.2")
            add("androidTestImplementation", "androidx.test.ext:junit:1.2.1")
            add("testImplementation", "junit:junit:4.13.2")
        }
        tasks.withType<Test>().configureEach {
            providers.gradleProperty("asukaTestServer").orNull?.let { systemProperty("asuka.testserver", it) }
            providers.gradleProperty("asukaTestServerTLS").orNull?.let { systemProperty("asuka.testserver.tls", it) }
            providers.gradleProperty("asukaTestPKI").orNull?.let { systemProperty("asuka.testserver.pki", it) }
        }
    }
    plugins.withId("org.jetbrains.kotlin.android") {
        extensions.configure<org.jetbrains.kotlin.gradle.dsl.KotlinAndroidProjectExtension> { jvmToolchain(17) }
    }
}
